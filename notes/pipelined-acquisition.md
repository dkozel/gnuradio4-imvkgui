# Pipelining acquisition against GPU work

An unimplemented optimization, written down while the measurements that justify it are
fresh. Worth roughly **1.7x** on top of the batching work in DESIGN.md §13, taking the
waterfall pipeline from ~1417 MS/s to an estimated ~2400 MS/s.

Not done, because 1417 MS/s is already 5.8x realtime on the 245.76 MS/s recordings and
nothing currently needs the margin. The thing that would need it is overlapped FFTs for true
100% probability of intercept (DESIGN.md §10), which multiplies the transform rate by the
overlap factor.

| Repo | Commit |
|------|--------|
| `ngscopeclient/scopehal` | `24dd95fb` (v0.2.1), plus the local Xid-32 patch |

## The opportunity

Host work and GPU work are strictly serialized today. `PlayerSession::StepOneBlock` acquires
a block on the host, records a command buffer, then blocks in `SubmitAndBlock` until the GPU
is done, and only then acquires the next block. Measured at 1 Msample blocks, 8192 points,
on an RTX 5060 Ti:

| | % of wall clock |
|---|---|
| blocked on submit | 58.3 |
| sample convert (ci16 → float) | 22.7 |
| file read | 15.5 |
| command recording | 3.4 |

Host total is 41.7% against 58.3% waiting on the GPU, and the two never run at the same
time. Perfect overlap gives `max(41.7, 58.3)` = 58.3% of the current wall clock, i.e.
**1.72x**. That cross-checks against the GPU-bound ceiling implied by the timestamps: 0.387
ms of GPU work per 1 Msample block is 2.71 GS/s, and 1417 × 1.72 = 2437 MS/s sits just under
it, the difference being the ~44 µs of submit overhead per block that does not disappear.

## Scope: only one buffer needs multi-buffering

The important structural point, and the reason this is a much smaller change than it sounds.

`ComplexFFTFilter::m_rdinbuf`, `m_rdoutbuf` and `SpectrumReducer::m_accumulator` are touched
by exactly one submit at a time and can stay single-copy, **provided submits execute in
order**. The reducer's accumulator is a read-modify-write across blocks, so in-order
execution is required anyway.

The only genuine handoff point between host and GPU is the I/Q sample buffers that
`SigMFSource::AcquireData` fills and the window shader reads. Those are what need N copies.

So the shape is:

- 2 (or 3) slots of `{readBuffer, icap, qcap}`, owned by `SigMFSource`
- 2 command buffers, because a command buffer cannot be re-recorded while it executes
- **one** `QueueHandle`, not several

One queue handle is a deliberate choice, not a limitation. `QueueHandle` owns exactly one
fence (`QueueHandle.h:117`), so a second `Submit()` blocks in `_waitFence()` until the first
completes — which is precisely the in-order execution the accumulator depends on, obtained
for free. Several handles would allow concurrent GPU execution of two blocks, which would
then need per-block copies of every intermediate *and* explicit serialization of the reduce
stage. All cost, no benefit: the GPU side is one submit of solid work with no gaps to fill.

Loop shape, replacing `SubmitAndBlock`:

```
Submit(N)                      // non-blocking
fill slot N+1 on the host      // overlaps with the GPU
WaitIdleWithTimeout(...)       // until N completes
record and Submit(N+1)
```

Memory cost is ~12 MB per slot at 1 Msample ci16 (4 MB raw + two 4 MB float waveforms).

## The trap: reuse must be gated on the fence, not on the pop

Two separate hazards, and the second is the one that bites.

**Ownership.** `InstrumentChannel::SetData` deletes the outgoing waveform whenever the
pointer differs (`InstrumentChannel.cpp:149-150`). A rotating set of waveforms pushed
through `m_pendingWaveforms` and `PopPendingWaveform` therefore gets destroyed one per
block. This is exactly why the current code reuses a *single* pair of waveforms and hands
over the same pointers, making `SetData` early out.

The clean fix is already there: `InstrumentChannel::Detach(size_t stream)`
(`InstrumentChannel.h:257-260`) returns the waveform and nulls the slot **without deleting**.
`Oscilloscope::PopPendingWaveform` is virtual (`Oscilloscope.h:899`), so an override can
`Detach` each stream and then `SetData` the new one, keeping ownership with `SigMFSource`
throughout.

**Lifetime.** A slot must not become reusable when it is popped. At that moment the previous
block's submit is still reading it. Reuse has to wait for that submit's fence. A plain free
list — including `WaveformPool`, `WaveformPool.h:73-111` — will hand back a buffer the GPU
is still reading, and the resulting corruption is intermittent and data-dependent.

## Upstream already has the fence-gated pool pattern

Worth knowing before writing a new one. `ScratchBufferManager` (`ScratchBufferManager.h`) is
a pre-allocated pool of `AcceleratorBuffer`s with an RAII handle, and
`QueueHandle::MarkScratchBufferUsed` move-parks that handle into `m_usedScratchBuffers`
(`ScratchBufferManager.h:153-155`). `_waitFence()` clears the set (`QueueHandle.cpp:137`),
running the destructors, which return the buffers to the pool. Acquire, use, release on the
fence — exactly the discipline needed here. `CouplerDeEmbedFilter.cpp:352` and
`JitterSpectrumFilter.cpp:248` use it.

**Borrow the discipline, not the class.** Its pools are keyed by type and purpose and are all
GPU-resident intermediates (`U8_GPU_WAVEFORM`, `F32_GPU_WAVEFORM`, …); ours are long-lived,
fixed in number, host-writable, and hold `UniformAnalogWaveform`s rather than bare
`AcceleratorBuffer`s. A `std::array<SampleSlot, N>` with an index and a fence check is
simpler and makes the dependency explicit.

## This supersedes GPU-side sample conversion

DESIGN.md §13 lists moving the ci16 → float conversion into the window shader as a separate
~1.3x lever. **The two are not additive, and after pipelining the conversion move becomes
counterproductive.**

Conversion is 22.7% of wall clock on the host. Once host and GPU overlap, the host is idle
about 30% of the time and the GPU is the critical path. Moving conversion onto the GPU takes
work off an idle resource and puts it on the busy one:

| | critical path, % of today's wall clock | speedup |
|---|---|---|
| pipeline only | 58.3 | 1.72x |
| GPU convert only | ~79 | ~1.27x |
| both | ~60 | ~1.67x |

The GPU-convert column is an estimate — the cost of an integer unpack added to the window
stage has not been measured — but the direction does not depend on its size. Do the
pipelining; leave conversion on the host.

The one thing that would change this is a source whose host-side decode is much more
expensive than a ci16 unpack, which would make the host the critical path again.

## Verification if this gets built

The failure mode is a slot being overwritten while the GPU reads it, which produces
plausible-looking output. `wfbench --verify` compares batched spectra against unbatched
per segment and would catch a slot reused one block early. Running it against
`lora_uninverted` rather than `tone_signal` matters: consecutive segments of real data
differ from each other, so a stale slot shows up, whereas a stationary tone would hide it.
