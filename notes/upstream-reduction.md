# Reducing the scopehal / ngscopeclient surface

> **Historical.** The measurements here are what motivated the vendoring, and they were
> taken before it. Every reduction identified below has since been made; see DESIGN.md D7
> and THIRD_PARTY.md for the result.

Follow-on to `notes/depgraph/`. That pass measured what is pulled in. This one asks which of
it is load-bearing, and answers with symbol-level evidence rather than include counts.

Method: `nm -uC` over the 17 objects of `imcufosphor-core` and over `main.cpp.o`, intersected
against `nm -DC --defined-only` of `libscopehal.so` (13610 exported symbols) and
`libscopeprotocols.so` (5944). Include-graph claims re-checked by compiling modified copies of
the affected translation units. Everything below was run, not reasoned about.

Baseline: scopehal `v0.2.2`, `-DBUILD_GR4_BLOCKS=OFF`, `main.cpp` patched for the
`GetRenderQueue` breakage (see §0).

---

## 0. The tree does not build

`main.cpp:70` calls `QueueManager::GetRenderQueue`, which does not exist in the checked-out
submodule. It was removed upstream by scopehal `0e925076` ("Initial version of new queue
allocation"), which replaced the per-purpose accessors with a pool API. Fix:

```cpp
shared_ptr<QueueHandle> queue(
    g_vkQueueManager->GetQueueFromPool(QueueManager::QUEUE_POOL_RENDER, "MainWindow.render"));
```

`build-g16/.../main.cpp.o` predates the submodule bump.

---

## 1. scopeprotocols is almost entirely removable — highest value, lowest risk

**The whole application library references exactly two symbols from `libscopeprotocols`:**

```
typeinfo for Waterfall
typeinfo for WaterfallWaveform
```

Both are `dynamic_cast` operands. Every actual `Waterfall` method call goes through the vtable
and costs no link-time reference. `main.cpp` adds one more: `ScopeProtocolStaticInit()`.

That is 3 symbol references into a 211-object library whose umbrella header costs 211 headers
per translation unit that names it.

### The change

1. `main.cpp:15` — `scopeprotocols.h` → `scopeprotocols/Waterfall.h`.
2. `main.cpp:131` — delete `ScopeProtocolStaticInit()`.
3. Replace the four `Filter::CreateFilter(name)` + `dynamic_cast` pairs with direct
   construction (`PlayerSession.cpp:35,42,51,55`; `SpectrumEngine.cpp:56,69,73,83`;
   `Verify.cpp:177,482,677`), and delete the `AddDecoderClass` calls at `main.cpp:134-136`,
   `SpectrumEngine.cpp:30-32` and `wfbench.cpp:475-477`.

### Why (3) is safe

`Filter::CreateFilter` is, in full (`Filter.cpp:111-122`):

```cpp
auto f = m_createprocs[protocol](color);
f->m_instanceNum = (m_instanceCount[protocol] ++);
```

and the registered proc is `PROTOCOL_DECODER_INITPROC` (`Filter.h:1527-1531`), which is
literally `return new T(color);`. The only behavioural difference is `m_instanceNum`, which
the `Filter` constructor already sets to 0 (`Filter.cpp:64`) and which is read in exactly one
place — building a default display name (`Filter.cpp:1474`). Nothing in this application
displays filter names.

This also retires the hazard `SpectrumEngine.cpp:23-24` documents in a comment ("registering
twice would leave a duplicate entry that CreateFilter picks between arbitrarily"): with direct
construction there is no name-keyed map, so there is no once-per-process invariant to get
wrong, and `wfbench` and the GR4 blocks stop needing to remember to call a registration
function before they can build a graph.

### Measured

`main.cpp` compiled both ways with the project's real flags, no PCH:

| | headers read | wall |
|---|---|---|
| as-is | 978 | 4.31 s |
| `scopeprotocols.h` → `Waterfall.h` | 748 | 3.98 s |

The time is unremarkable because `scopehal.h` dominates (§3). The 230 headers matter for a
different reason: they are 211 filter class definitions that no one can tell are unused by
reading the file, which is exactly the legibility problem this exercise is about.

### The part that matters later

`ScopeProtocolStaticInit()` names all 206 filter classes. Any future static link or
`--gc-sections` build keeps every one of them alive through that single function. Deleting the
call is the precondition for `libscopeprotocols` shrinking to `Waterfall.o` — 1 object of 211.

### Residual

`ComplexFFTFilter.h:41` keeps `FFTFilter.h` for `FFTFilter::WindowFunction` and its four
enumerators. That is a compile-time-only dependency (no `FFTFilter` symbol appears in the
undefined set). Replacing it with a local `enum class WindowFunction` would sever
`scopeprotocols` from the headers entirely; the window *shaders* are still borrowed and stay
borrowed. Optional, and worth doing only alongside the vendoring the plan anticipates.

---

## 2. Process startup registers three subsystems this application has none of

`main.cpp:127-130`:

```cpp
TransportStaticInit();
DriverStaticInit();
ScopeProtocolStaticInit();
InitializePlugins();
```

- **`TransportStaticInit()`** — registers 12 SCPI transport classes (socket, TMC, LXI, UART,
  HID, VICP, CAN…). Nothing in this application opens a transport. Delete.
- **`InitializePlugins()`** — `dlopen`s every `.so` in `/usr/lib/scopehal/plugins/`,
  `/usr/local/lib/scopehal/plugins/` and the binary's own directory
  (`scopehal.cpp:420-428`). For a file player this is nondeterminism and attack surface in
  exchange for nothing. Delete.
- **`DriverStaticInit()`** — **do not delete.** Its first three lines are load-bearing and
  everything after them is ~100 `Add*DriverClass` calls:

  ```cpp
  InitializeSearchPaths();     // FindDataFile() -> shaders/*.spv, icons/gradients/*.png
  DetectCPUFeatures();         // g_hasFMA, g_hasAvx2, ... read by inline code in headers
  Unit::InitializeLocales();   // Unit::PrettyPrint, used in main.cpp:64 and PlotAxis
  ```

  Replace the call with those three (declared at `scopehal.h:235`, `scopehal.h:237`,
  `Unit.h:147`). The app gets the initialization it actually depends on, named, and stops
  registering Rigol and Tektronix drivers on the way to opening a file.

Zero-risk, and it makes the startup sequence self-documenting.

---

## 3. `SigMFSource : public Oscilloscope` — half the scopehal surface, six used entry points

This is the largest item and the one that most matches "accommodating a design that is not fit
for purpose with RF signal sources". It is also a documented decision (DESIGN.md D1 / §7.1), so
the argument has to engage with the stated rationale.

### The cost, measured

Of the 195 `libscopehal` symbols the application library references, **98 (50%) are
`Oscilloscope` / `OscilloscopeChannel` / `Instrument` / `InstrumentChannel`**. They are vtable
slots: `SigMFSource`'s vtable must be emitted with an entry for every non-pure virtual in the
base chain, so the linker pulls in the base implementation of each. A representative sample of
what a SigMF file player currently carries:

```
Oscilloscope::AutoZero(unsigned long)          Oscilloscope::Degauss(unsigned long)
Oscilloscope::GetADCMode(unsigned long)        Oscilloscope::GetInputMuxNames(unsigned long)
Oscilloscope::GetProbeName(unsigned long)      Oscilloscope::GetDigitalBanks()
Oscilloscope::GetDigitalHysteresis(...)        OscilloscopeChannel::SetDeskew(long)
Instrument::SerializeConfiguration(IDTable&)   Instrument::LoadConfiguration(int, YAML::Node...)
```

Plus 47 `override` declarations in `SigMFSource.h`, most of them stubs, and the five
`Instrument` virtuals §7.1 notes are easy to miss.

### What is actually used

Grepping every `m_source->` / `source.` call site in `src/` and `tools/wfbench/`, the inherited
API in use is **six entry points**:

`Start()`, `AcquireData()`, `PopPendingWaveform()`, `GetChannel()`, `SetSampleDepth()`,
`GetSampleDepth()`, and `GetSampleRate()` — of which only `PopPendingWaveform()` has a
non-trivial `Oscilloscope` implementation (~15 lines over `m_pendingWaveforms`, which
`SigMFSource.cpp:1031,1099` fills itself). `PollTrigger()` is overridden and never called from
our code. Everything else the app touches — `GetRecordingSampleRate`, `GetPlayCursor`,
`SeekToSample`, `GetAnnotations`, `GetExactCenterFrequency`, `IsUsingPackedPath`,
`GetSamplesPlayed` — is `SigMFSource`'s own.

### Why the D1 rationale does not require the base class

§7.1 argues three things, all of them against `ComplexImportFilter` rather than for
`Oscilloscope`:

| §7.1 requirement | Where it actually comes from |
|---|---|
| bounded memory on large recordings | `SigMFSource`'s own `pread`/mmap loop |
| the `center` scalar stream | `ComplexChannel` (`ComplexChannel.h:63-66`) |
| real run/stop/single semantics | `SigMFSource`'s own play state; nothing external polls it |

None of the three is provided by `Oscilloscope`.

### The existing proof

`IqInjector` is this refactor already done, for the live-sample case. Its class comment states
the position exactly ("wrapping them in a 42-virtual Oscilloscope subclass to get them into the
graph would be ceremony around a memcpy"), it owns a `ComplexChannel(nullptr, ...)`
(`IqInjector.cpp:34`), and `Verify.cpp:478-490` drives a real `ComplexFFTFilter` from it. The
only structural difference from `SigMFSource` is that `SigMFSource` passes `this` at
`SigMFSource.cpp:274`.

### Shape of the change

Keep every line of the file/format/annotation logic — that is the value in the class. Change
the base and the channel's owner:

- `class SigMFSource` (no base), owning `ComplexChannel(nullptr, "RX", …)`.
- Promote the six used entry points to plain public methods.
- Reimplement `PopPendingWaveform()` locally over the `m_pendingWaveforms` deque it already
  maintains.
- Delete the other 41 overrides.

This converges with the refactor plan's `imcufosphor-sigmf` layer, and it is what would let
`imcufosphor-dsp` stop naming `Oscilloscope.h` at all — the plan's dependency table already
says `-dsp` should only need `scopehal` + `scopeprotocols`, and today `SigMFSource.h:15` is
what makes that untrue.

Cost to weigh against it: ~1300 lines of `SigMFSource.cpp` get touched at the seams, and
`Verify.cpp:670` uses `PopPendingWaveform` too. Not a small change, but a mechanical one, and
it is the single biggest reduction available.

---

## 4. `PeakDetectionFilter` is inherited and unreachable

`ComplexFFTFilter : public PeakDetectionFilter` (`ComplexFFTFilter.h:161`), and:

- nothing in `src/` or `tools/` calls `GetPeaks()` or reads the peak stream;
- `m_numpeaks` defaults to 0 (`PeakDetectionFilter.cpp:57`, "peak detection is sloooow");
- when it is non-zero, the batch path — which is the normal path — refuses with
  `AddErrorMessage("Unsupported", …)` (`ComplexFFTFilter.cpp:592-596`).

Meanwhile the feature costs a live branch in `GetExecutionCapabilitiesMask()` that withholds
`CommandBufferTailCall` (`ComplexFFTFilter.cpp:141-157`) — an unreachable feature constraining
GPU scheduling.

Deriving from `Filter` directly drops `PeakDetectionFilter.h` from the direct set and two more
symbols from the link. If peak markers are wanted later, they want a batch-aware
implementation anyway.

Related: **DESIGN.md §4 lists `PeakHoldFilter` as a scopeprotocols dependency. It appears
nowhere in `src/` or `tools/`.** Worth correcting the doc either way.

---

## 5. ngscopeclient: what §5 / R2 / refactor-plan-A1 do *not* already cover

The R2 note and refactor plan A1/A3/A5 cover this surface well. Two things they miss:

### 5.1 `VulkanWindow.h` is not self-contained, and A1 does not close that

`VulkanWindow.h` has **no `#include` directives at all**. It relies on its includer having
already included `ngscopeclient.h` (for ImGui, GLFW, `vk::raii`, `QueueHandle`). That is
precisely why `MainWindow.h:14` includes `ngscopeclient.h`, and it is why the 13 unrequested
instrument-state headers (`BERTState.h`, `OscilloscopeState.h`, `PowerSupplyState.h`,
`MultimeterState.h`, `LoadState.h`, `FunctionGeneratorState.h`, `Marker.h`, `FontManager.h`,
`GuiLogSink.h`, `Event.h`, `ImGuiDisabler.h`) reach the application.

Refactor plan A1 replaces `ngscopeclient.h` in five of our headers, but not in `MainWindow.h` —
it can't, because `VulkanWindow.h` would then not compile. The same `TextureCompat.h` trick A1
proposes works here: a `VulkanWindowCompat.h` in `src/ngscopeclient-compat/` that supplies
GLFW + `imgui.h` + `scopehal.h` and then `VulkanWindow.h`. After that, `ngscopeclient.h` is
named by `PreferenceSchema.cpp` and `LinkCheck.cpp` only — both of which are ours and both of
which are boundary files where naming it is honest.

### 5.2 The preference machinery exists to persist ten integers

`PreferenceManager.cpp` + `Preference.cpp` + `PreferenceTree.cpp` = 1035 upstream lines, plus
our 70-line `PreferenceSchema.cpp`, plus the `ConfigPathShim.h` workaround for upstream writing
to `~/.config/ngscopeclient`. Total consumers: `VulkanWindow.cpp:134` and `VulkanWindow.cpp:875`,
reading and writing window geometry — position, size, fullscreen, maximized, monitor name.

Nothing else in the application has a preference. This is the clearest case in the whole lift
where the wrapper exists only to satisfy upstream, and it is the strongest argument for making
`VulkanWindow.cpp` (918 lines) the *first* file to vendor if vendoring happens: doing so
deletes three upstream translation units, our schema, and the config-path shim in one move —
about 1100 lines of borrowed code and the only file-clobbering hazard in the project.

---

## 6. Runtime data: 8.8 MB and ~220 shaders collected to use one PNG and six SPIR-V

`cmake/RuntimeData.cmake` collects whole directories. Actual usage, from every `"shaders/…spv"`
string literal in the tree and the single `LoadTexture` call:

| Source | Built / copied | Used |
|---|---|---|
| ours (`src/imcufosphor/shaders`) | 8 | 8 |
| `scopeprotocols/shaders` | ~200 | 4 (`ComplexBlackmanHarrisWindow`, `ComplexCosineSumWindow`, `ComplexRectangularWindow`, `WaterfallFilter`) |
| `ngcomputeshaders` | 9 | 1 (`WaterfallToneMap`) |
| `ngrendershaders` | 16 | 2 (`waveform-compute.analog.dense`, `…analog.int64.dense`) |
| `ngscopeclient/icons` | 8.8 MB | one 5.6 KB file, `icons/gradients/eye-gradient-viridis.png` |

The icon copy is the egregious one: `imcufosphor_attach_icons` does a `copy_directory` of 8.8 MB
per executable target, and `MainWindow.cpp:39` loads exactly one file out of it. Narrowing it to
`icons/gradients/` alone is 96 KB and one line; narrowing to the ramps actually offered in the UI
is smaller still.

The `ngrendershaders` target is upstream's and compiles all 16 variants unconditionally, so
trimming it means either selecting outputs on our side or accepting it as build-time-only cost.
The shaders are build-time cost only; the icons ship next to the binary.

---

## Ordering

| | Change | Risk | Payoff |
|---|---|---|---|
| 1 | §2 startup calls | none | startup sequence becomes self-documenting |
| 2 | §6 icon copy narrowing | none | 8.8 MB → 96 KB per target |
| 3 | §1 scopeprotocols + direct filter construction | low, verified | 3 symbols → 0 named deps; kills the registry invariant |
| 4 | §4 drop `PeakDetectionFilter` base | low | removes an unreachable feature that costs a tail-call |
| 5 | §5.1 `VulkanWindowCompat.h` | low | last 13 unrequested ngscopeclient headers |
| 6 | §3 `SigMFSource` stops being an `Oscilloscope` | medium | half the scopehal symbol surface, 41 stub overrides |
| 7 | §5.2 vendor `VulkanWindow.cpp` | medium | ~1100 lines of borrowed code and the config-path hazard |

Items 1-5 are independent of the refactor plan and can land before Phase A. Item 6 belongs with
A4 (`imcufosphor-sigmf`). Item 7 belongs with A3.
