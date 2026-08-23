# imcufosphor — Design Document

A realtime spectrum analyzer and waterfall display for SigMF IQ recordings, built on
libscopehal's GPU compute pipeline and ngscopeclient's Vulkan/ImGui rendering primitives.

**Status:** design, pre-implementation.

All upstream claims in this document were verified against:

| Repo | Commit | Date |
|------|--------|------|
| `ngscopeclient/scopehal` | `24dd95fb` | 2026-08-15 |
| `ngscopeclient/scopehal-apps` | `455237b7` | 2026-08-15 |

File:line citations below refer to those commits. Re-verify after a submodule bump.

---

## 1. Goals

Read a SigMF recording, play it back at a controlled rate, and render two vertically
stacked, frequency-aligned panes:

- a spectrum plot (top), and
- a scrolling waterfall (bottom),

both GPU-computed and GPU-rendered, sharing one frequency axis with linked pan/zoom
and a shared cursor.

### Non-goals

- **Any form of demodulation.** No audio output, no decoders, no symbol recovery.
- **Real (non-complex) input.** Complex IQ only.
- **SigMF collections.** Single recordings only.
- **Live hardware.** File playback only. (The architecture does not preclude a live
  source later — see §5.1 — but no driver work is in scope.)

---

## 2. Decisions register

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | IQ enters the graph via a **SigMF source class** owning a `ComplexChannel` — *not* `ComplexImportFilter` | Bounded memory on arbitrarily large recordings; real run/stop/single semantics; provides the center-frequency scalar stream. See §7.1 for why `ComplexImportFilter` was rejected. **Revised:** this was an `Oscilloscope` subclass until the vendoring. None of the three properties above came from that base class, and it cost 47 overrides to use six. |
| D2 | Waterfall pane is driven by the **`Waterfall` filter** (live ring-buffer scroll), not `ComplexSpectrogramFilter` | One FFT feeds both panes, so spectrum and waterfall agree by construction. Matches the playback model. |
| D3 | The complex FFT is an **app-local filter**, not added to `scopeprotocols` | Was: keeps both submodules unmodified. Still true for a better reason — it is ours, so it is in `src/`, where our code lives. |
| D4 | ~~Lifted display primitives are **referenced by path from a `scopehal-apps` submodule**, never copied~~ | **Superseded.** See D7. |
| D7 | scopehal and ngscopeclient are **vendored into `third_party/`**: copied in, trimmed to what this project uses, and modified where needed | D4's premise was that referencing upstream in place makes updates a pointer bump with no merge burden. In practice it made them invisible: one bump removed an API `main.cpp` called and the tree stopped building, and the same bump silently dropped the patch this hardware needs, leaving every GPU transfer faulting the device. Neither was noticed, because nothing built or ran by default. The copy is 26 sources against upstream's 386, with a ten-file delta recorded in `THIRD_PARTY.md`. |
| D5 | Annotation rendering is **phase 2**, core namespace only | Nice-to-have; the enabling machinery (row-history ring) is phase 1 anyway. |
| D6 | True RTSA density ("fosphor-style") is **phase 3**; phase 1 uses upstream's built-in display persistence | Built-in persistence is free and looks right; true density needs the reducer interface designed correctly first. See §10. |

---

## 3. Repository layout

```
imcufosphor/
  CMakeLists.txt              superbuild, modeled on scopehal-apps/CMakeLists.txt
  DESIGN.md                   this file
  cmake/CollectShaders.cmake  gathers every built .spv next to the binary (§7.2)
  THIRD_PARTY.md              what is vendored, from where, and every local change
  licenses/                   upstream licence texts
  lib/                        submodules: pinned, never edited
    libsigmf/                 → deepsig/libsigmf     (header-only)
    imgui/                    → ocornut/imgui        v1.92.8-docking
    VkFFT/                    → ngscopeclient/VkFFT  (header-only)
  third_party/                vendored: copied in, trimmed, ours to modify
    scopehal/                 26 sources of upstream's 386, + 4 shaders of ~105
    ngscopeclient/            VulkanWindow, TextureManager, 2 shaders, 1 ramp
  src/imcufosphor/
    main.cpp
    SigMFSource.{cpp,h}       the recording, played into the graph
    ComplexFFTFilter.{cpp,h}  app-local complex FFT
    SpectrumReducer.{cpp,h}   N-in-1-out accumulator (see §5.3)
    PlayerSession.{cpp,h}     thin Session analogue
    SpectrumArea.{cpp,h}      thin waveform area — spectrum
    WaterfallArea.{cpp,h}     thin waveform area — waterfall
    AnalyzerPane.{cpp,h}      combined stacked view, owns the shared axis
    AnnotationOverlay.{cpp,h} phase 2
    MainWindow.{cpp,h}        subclasses the vendored VulkanWindow
    shaders/                  our own compute shaders, plus their glslc step
      ComplexToLogMagnitudeShifted.glsl   fftshift + dBm for a spectrum trace (§7.2)
```

The superbuild builds `third_party/scopehal` and `third_party/ngscopeclient` directly.
`find_package` set: Vulkan, glfw3, glslang, SPIRV-Tools, ZLIB, PNG, Threads, OpenMP, sigc++.
yaml-cpp, hidapi, liblxi and libtirpc were all dropped by the vendoring — they belonged to
the instrument drivers, the SCPI transports and the graph serialization, none of which this
project has.

`third_party/` keeps upstream's directory shape (`scopehal/{scopehal,scopeprotocols,log,
xptools}`) so that neither our `<scopehal/Filter.h>` includes nor upstream's own internal
relative ones had to change. Both directories' CMakeLists fail the configure if a source file
is present but unbuilt, or built but absent.

`libsigmf` bundles flatbuffers and nlohmann/json as its own submodules and builds `flatc`
on first configure, so the initial build is slower than subsequent ones. Its pre-generated
protocol headers live in `sigmf_protocols/`, but `add_dependencies(libsigmf
libsigmf_genheaders)` regenerates them regardless.

### Build prerequisites

Beyond a Vulkan-capable toolchain, on Ubuntu 24.04:

```
sudo apt install libyaml-cpp-dev libsigc++-3.0-dev glslc libhidapi-dev libpng-dev
```

`glslc` is a hard requirement — the top-level `CMakeLists.txt` fails configuration
without it, since every compute shader in scopehal and scopeprotocols is compiled with it.
`hidapi` is required by `xptools` (`xptools/CMakeLists.txt:26-28`) even though this
project uses no HID instruments.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ln -sf build/compile_commands.json compile_commands.json   # once, for clangd
```

### NVIDIA driver defect breaking all scopehal GPU work (R7)

On this machine (RTX 5060 Ti, driver 580.173.02, `nvidia-driver-580-open`) **every scopehal
Vulkan compute submission faults the device** — `ErrorDeviceLost`, with `NVRM: Xid ... 32`
(corrupted push buffer) in the kernel log.

The GPU and driver are healthy for ordinary compute. A minimal scopehal-free dispatch
passes, including with scopehal's own SPIR-V, push descriptors, device-local memory and
staging copies (`tools/vkmintest/`). Vulkan validation (1.3.275, confirmed loading) reports
**nothing** on the recording or submission path — the usage is spec-legal.

**Root cause.** `tools/vkmintest/xid32_repro.cpp` — 166 lines, includes only
`<vulkan/vulkan.h>`, no shader, no buffers, no memory, no barriers — faults
deterministically when three conditions hold together:

1. The logical device creates queues in **both** the graphics family (0, flags `0x0f`) and
   the async-compute family (2, flags `0x0e`);
2. a command buffer from a **family 2** pool contains `vkCmdSetEvent`;
3. that call's stage mask is **`VK_PIPELINE_STAGE_TRANSFER_BIT`**.

Changing any one of the three makes it pass — a different family, a different stage mask,
or `vkCmdSetEvent2` from `VK_KHR_synchronization2`.

**Why scopehal meets all three on every GPU transfer.** `AcceleratorBuffer.h` calls
`setEvent(..., eTransfer)` in all four copy paths (lines 1458, 1536, 1559, 1587);
`VulkanInit.cpp:871-877` creates queues in every family including graphics; and
`QueueManager` sorts by fewest capability flags, so the compute and transfer handles both
land on family 2.

This is **scopehal-wide, not specific to this project** — upstream's own
`tests/Acceleration` (`Buffers_CpuGpu / MirrorCopy`) fails identically when built from
unmodified sources, and passes on llvmpipe.

**Fix — applied as a tracked local patch.** `patches/scopehal/0001-AcceleratorBuffer-*.patch`,
committed on branch `imcufosphor/xid32-workaround` in `lib/scopehal`. It routes the four
`setEvent` stage masks through one accessor returning `eAllCommands`, which is strictly
more conservative than `eTransfer` — the event signals later, never earlier — so
correctness is preserved at negligible cost, since those command buffers contain only the
copy.

This is a **deliberate exception to D3/D4**, the first one. See `patches/README.md` for the
rules that keep it tracked: branch in the submodule, exported with `format-patch`, commit
message written for the upstream maintainer, and a re-apply check after every submodule
bump. Without it no GPU work is possible here at all, which outweighs the purity of the
never-modify-submodules rule.

**Verified:** with the patch applied, `ComplexFFTFilter` verification passes on the RTX 5060
Ti with no device override, producing bin-for-bin and dBm-for-dBm identical results to
llvmpipe, and no new Xid in the kernel log.

Still open, for the user to raise with upstream once phase 1 results are in:

1. Upstream may prefer `vkCmdSetEvent2` (`VK_KHR_synchronization2`, core since Vulkan 1.3;
   this device reports 1.4.312), which also avoids the fault but needs the extension
   enabled at device creation — scopehal currently requests 1.2.
2. Upstream may identify a different underlying cause. The patch is a workaround for an
   observed driver defect, not a diagnosis of scopehal doing anything wrong — validation is
   clean and the usage is spec-legal.
3. NVIDIA bug report, using `tools/vkmintest/xid32_repro.cpp` unmodified.

Ruled out with method: pipeline cache, VkFFT, `VK_ENABLE_BETA_EXTENSIONS`, API version,
memory type/heap selection, barriers, layers, header/ABI mismatch. See the agent findings
and `tools/vkbisect/`.

### Warning policy

Upstream's warning flags apply to everything, submodules included, but **`-Werror` is
scoped to our own targets** via `IMCUFOSPHOR_WERROR_FLAGS` and
`target_compile_options()`. `add_compile_options(-Werror)` at the top level would gate the
build on warnings in code we neither own nor can fix — gcc 13 emits a false-positive
`-Wstringop-overflow` in libsigmf's bundled `flatbuffers/src/reflection.cpp`, and a
submodule bump can introduce more at any time.

Separately, libsigmf's interface include directories are promoted to `SYSTEM` after its
`add_subdirectory`, so its heavy template headers do not emit warnings into our
translation units. libsigmf declares them as plain `INTERFACE` includes, so the promotion
happens in our `CMakeLists.txt` rather than by patching the submodule.

---

## 4. Layer 1 — vendored upstream code

Vendored into `third_party/` rather than built from submodules; see D7 and `THIRD_PARTY.md`.
What we actually use of it:

**From scopehal**

- `VulkanInit(bool skipGLFW)` — device, queues, memory heaps. Library-side, so the GUI and
  compute paths share one device and tone-mapped textures never round-trip through host
  memory.
- `AcceleratorBuffer` — 2183 header-only lines, and the reason `VulkanInit` is
  non-negotiable.
- `ComputePipeline`, `PipelineCacheManager`, `QueueManager`, `VulkanFFTPlan`.
- The filter graph: `Filter`, `FlowGraphNode`, `FilterParameter`, `FilterGraphExecutor`,
  `InstrumentChannel`, `OscilloscopeChannel`, `ComplexChannel`, the waveform types.
- `Unit`, `FindDataFile`, `log`.

Not used, and not vendored: every SCPI transport, every instrument driver, the whole
instrument model (`Oscilloscope`, `Instrument`, `Trigger`), the Touchstone and IBIS parsers.

**From scopeprotocols**

- `Waterfall` — the scrolling ring buffer (D2). One filter of 211.
- Complex **window** shaders, used by our app-local FFT. The magnitude shaders are not
  reusable and we supply our own; see §7.2.

`PeakHoldFilter` and `PeakDetectionFilter` were listed here and neither was ever used.
`ComplexFFTFilter` did inherit `PeakDetectionFilter`, but nothing read its output: peak
detection defaults off, and when enabled it refuses any batch of more than one spectrum,
which is the normal case. It also withheld `CommandBufferTailCall` while enabled, so an
unreachable feature was shaping the scheduling of the one that runs.

---

## 5. Layer 2 — the vendored display primitives

Two source files in `third_party/ngscopeclient/`, plus ImGui as its own target. See
`THIRD_PARTY.md` for provenance and the exact local changes.

| File | Why it is worth taking |
|------|------------------------|
| `VulkanWindow.{h,cpp}` | ~690 lines of GLFW + Vulkan + ImGui bring-up, swapchain management and frame loop that we would otherwise have written ourselves, with `virtual DoRender` / `virtual RenderUI` already the extension points. |
| `TextureManager.{h,cpp}` | Vulkan image, view, sampler and ImGui descriptor-set lifetime, including the compute-written texture path `SpectrumArea` and `WaterfallArea` use directly. |
| `shaders/WaterfallToneMap.glsl` | Self-contained: push constants, fp32 input buffer, colour ramp sampler, output image. |
| `shaders/waveform-compute.glsl` | The analog trace rasterizer. **Borrowed deliberately** — display persistence lives inside it (§10). |

The last row is a design commitment, not a convenience: `SpectrumArea` is a thin wrapper
around the upstream rasterizer rather than original rendering code. Writing a simpler line
renderer would silently forfeit phosphor and require rebuilding it later. We build two of its
sixteen compile-time variants.

### What the lift cost, before and after

The original attempt kept these files in place under the submodule, which is what D4
required. That took **twelve files to compile two**: the two above, ngscopeclient's
1604-line preference system, our 70-line schema for it, a 68-line preprocessor shim, and
three compat headers. `notes/R2-lifted-primitives.md` is the account of getting that to
build and link, and it stands as an accurate record of what reference-don't-copy cost.

Three of the four problems it documents were consequences of not being allowed to edit:

1. **`VulkanWindow::GetContentScale()` is declared and defined nowhere upstream.** Both our
   window subclasses carried an identical ten-line `GetDpiScale()` working around it.
2. **`VulkanWindow.h` had no `#include` directives at all**, and `TextureManager.h` named
   `ImTextureID` and `GLFWimage` without declaring either. Both compiled only behind
   `ngscopeclient.h` — 121 lines of umbrella over 1081 lines of instrument-session model,
   which therefore reached every display header here and every GNU Radio block header
   through them.
3. **`PreferenceManager` hardcodes `~/.config/ngscopeclient`** and saves from a static
   destructor, so linking it unmodified meant truncating the real ngscopeclient user's
   preference file at exit. That needed a preprocessor interpose on scopehal's
   `ExpandPath`/`CreateDirectory`, applied to one translation unit, and was **unsolved on
   Windows**.

All three are now one-line edits to files we own. The headers are self-contained, so the
compat headers are gone; the preference system is replaced by a ten-field struct
(`WindowGeometry.h`), so the shim and the schema are gone with it.

The remaining cost: ImGui and libpng.

`window-linkcheck` still exists and is registered with `ctest`. Its original purpose — proving
reference-don't-copy links — is moot, but it is a good canary for the vendored window layer.

---

## 6. Data flow

```
.sigmf-meta ─┐
             ├─► SigMFSource ──► [I, Q, center] ──► ComplexFFTFilter ──► dBm spectrum
.sigmf-data ─┘   (Oscilloscope)                            │                    │
                                                           ▼                    ▼
                                                    SpectrumReducer        SpectrumArea
                                                    (K spectra → 1 row)         │
                                                           ▼                    │
                                                      Waterfall ────────────────┤
                                                    (ring buffer)               │
                                                           ▼                    ▼
                                                    WaterfallArea ────────► AnalyzerPane
```

One FFT feeds both panes, so the spectrum trace and the newest waterfall row are the
same data — they cannot disagree.

---

## 7. Layer 3 — new components

### 7.1 `SigMFSource`

A plain class owning one `ComplexChannel`. It was an `Oscilloscope` subclass until the
vendoring; see below for why it is not one now.

Owns one `ComplexChannel` named "RX", which supplies **I, Q, and a `center` scalar
stream** (`ComplexChannel.h:17`) — the third of these is required by any downstream
complex filter and is the specific reason `ComplexImportFilter` is insufficient.

`AcquireData()` reads N samples at the play cursor via `pread`, converts per
`core:datatype`, and pushes a `SequenceSet` onto `m_pendingWaveforms`. `PlayerSession`
consumes via `AcquireData()` / `PopPendingWaveform()`. Playback pacing, seek, and loop live
here.

**Why it is no longer an `Oscilloscope`.** It inherited 47 overrides to use six, and only
one of those six — `PopPendingWaveform()`, fifteen lines draining a queue this class fills
itself — had a base implementation worth anything. The other 41 were stubs for hardware a
recording does not have.

They were not free. A subclass's vtable carries a slot for every non-pure virtual in the base
chain, so the linker pulled in the implementation of each: `Oscilloscope::AutoZero`,
`::Degauss`, `::GetADCMode`, `::GetInputMuxNames`, `::GetProbeName`,
`Instrument::SerializeConfiguration`. Measured, that was 98 of the 195 scopehal symbols this
application referenced; removing the base took the total from 195 to 143 and eliminated
`Oscilloscope.cpp`, `Instrument.cpp`, `Trigger.cpp` and `EdgeTrigger.cpp` from the vendored
tree entirely.

The three properties D1 asks for all survive, because none came from the base class: bounded
memory is this class's own `pread` loop, the center-frequency stream is `ComplexChannel`'s,
and run/stop/single is three bools. `IqInjector` had already demonstrated the shape for live
samples — a `ComplexChannel` with a null `Oscilloscope*`, which the verification suite drives
a real `ComplexFFTFilter` from.

`GetSampleDepth()` / `SetSampleDepth()` map to the acquisition block size, which is the knob
that trades FFT headroom against latency. `SetSampleRate()` is gone rather than ignored:
resampling a recording to a requested rate is not something a player should silently do, and
with no base class there is no longer an interface demanding the method exist.

**Why not `ComplexImportFilter`:** it does `fseek(END)` → `new uint8_t[len_bytes]` → one
`fread` of the entire file (`ComplexImportFilter.cpp:92-110`), emitting a single waveform
pair covering the whole recording. A 4 GB capture becomes a 4 GB read plus two float32
arrays. It also exposes only I and Q, with no center-frequency stream. Its per-datatype
conversion loops (`ComplexImportFilter.cpp:165-200`) remain useful as reference.

**libsigmf does more of this than expected.** Header-only C++17,
`find_package(libsigmf REQUIRED)` → `libsigmf::libsigmf`, deps flatbuffers +
nlohmann_json. Verified no collision: **nlohmann/json appears nowhere in scopehal or
scopeprotocols** (they use yaml-cpp), so there is no version or ODR conflict.

| Helper | Status |
|--------|--------|
| `metadata_file_to_json(ifstream)` | **Not usable.** It is *not a template* — it hardcodes a record type of `core+antenna+capture_details+signal`, so using it would dictate ours. We parse into our own type with `nlohmann::json` directly, which is three lines. |
| `get_sample_size(dtype_str)` | Used indirectly; we parse the datatype ourselves because we also need signedness and endianness, which this does not expose. |
| `get_capture_range(captures, i, sample_size)` | Not needed — single-capture recordings only, so far. |
| `get_first_of_sigmf_vector(sample, vec)` | Not used; capture counts are tiny (every demo recording has exactly one) so a linear scan is simpler and tolerates out-of-order `sample_start`. |
| `sort_sigmf_vector(vec)` | Phase 2, for annotation range queries. |

**Accessor form matters.** Use `.get<sigmf::core::DescrT>()`, not `.access<sigmf::core::XT>()`.
`Capture<T>` and `Annotation<T>` expose `get<DescrT>()` which forwards to
`access<CaptureT>` internally; calling `access<>` directly on them fails to parse. `Global`
happens to work either way, which makes the inconsistency easy to trip over.

Keep the record type in a single typedef:

```cpp
using SigMFRecord = sigmf::SigMF<
    sigmf::Global<sigmf::core::DescrT>,
    sigmf::Capture<sigmf::core::DescrT>,
    sigmf::Annotation<sigmf::core::DescrT> >;
```

It is a compile-time template parameter, so adding `sigmf::signal::DescrT` later for
emitter metadata is a one-line edit rather than a refactor.

**Note:** `/data/code/libsigmf_api.md` shows these types unqualified (`core::DescrT`).
That is wrong — the generated headers put them in `sigmf::core`
(`sigmf_protocols/sigmf_core_generated.h:16-17`), and libsigmf's own examples use
`sigmf::core::GlobalT`. The same applies to helper functions: `sigmf::get_sample_size`,
not `get_sample_size`. Includes are flat (`#include "sigmf.h"`), not `<sigmf/sigmf.h>`.

**`get_sample_size()` semantics — resolved.** It returns bytes per **complex sample
pair**, not per component: it derives a component width from the bit-width substring, then
doubles it when the datatype string begins with `c`
(`lib/libsigmf/src/sigmf_helpers.h`). Verified at runtime — `cf32_le` → 8 bytes. So the
read loop is `nsamples * get_sample_size(dtype)` with no extra factor of two. It throws
`std::invalid_argument` on an unrecognized datatype, which needs catching at load.

**Residual risk:** `sample_rate` and `frequency` are `std::optional`; a recording missing
`core:sample_rate` needs a user-supplied fallback, not a crash.

### 7.1.1 What the real recordings taught us

Validated against `/data/deepsig/datasets/demo/` — seven recordings, ~7 GB, including two
2.9 GB files.

- **Everything is `ci16_le`.** Not one `cf32_le` in the set. The integer path is the one
  that matters in practice, contrary to the spec examples.
- **`core:frequency` of 0 is legitimate.** `tone_signal` is a baseband recording. Absent
  and zero must never be conflated.
- **`core:frequency` is a double with fractional Hz** (e.g. `2409999997.615808`), but
  `ComplexChannel::UpdateCenterFrequency()` takes a **float**, whose ULP at 2.4 GHz is
  ~256 Hz. On the narrower recordings that is a substantial fraction of an FFT bin
  (`dect6`: 128 Hz error against a 976 Hz bin). `SigMFSource::GetExactCenterFrequency()`
  keeps the full-precision value; the scalar stream exists only for downstream scopehal
  filters that expect it, and **our own axis labelling must not use it**.
- **Malformed metadata occurs and must not be fatal.** `dect6` has `core::datetime` with a
  doubled colon. libsigmf ignores unknown keys, so it parses fine.
- **Unknown namespaces are everywhere** — `deepsig:`, `csw:`, `spatial:`,
  `traceability:`, `capture_details:`, `antenna:` — including deeply nested objects and
  multi-hundred-element arrays. Declaring only `core` in the record type is safe; libsigmf
  silently ignores the rest.
- **Basename pairing is not always right.** `IQ_800MHz-omnisig.sigmf-meta` is a second,
  richer annotation set (9182 annotations) for `IQ_800MHz.sigmf-data`. Hence the explicit
  data-path override; without it that file reports a clear error rather than failing
  obscurely.
- **Annotation counts are large** — 2000 in `dect6`, 9182 in the omnisig set. Nearly all
  carry frequency edges, so the phase 2 box rendering has real data to work with. But
  `tone_signal`'s single annotation has *no* frequency box, so the degenerate case in §9
  is not hypothetical.

**Verified end to end:** `tone_signal` documents a 1.0 MHz tone at 40 MHz baseband; the
chain reports it at 999.756 kHz, a 244 Hz error against a 610 Hz bin, and **positive** —
which is what proves I and Q are not swapped. Independently, `lora_inverted` and
`lora_uninverted` peak at exactly ∓75.0 MHz about their common 2.4425 GHz center, and
`dect6`'s peak falls inside the DECT6 band its own annotations declare.

**Memory:** peak RSS 284 MB across the whole survey. `ComplexImportFilter` would have
needed ~35 GB for the same set.

### 7.2 `ComplexFFTFilter`

App-local (D3). Modeled on `FFTFilter` but with I/Q/center inputs following
`ComplexSpectrogramFilter.cpp:44-55`.

`FFTFilter` cannot be reused: it creates a **single real input**
(`FFTFilter.cpp:52`). `ComplexSpectrogramFilter` is the only complex-aware spectral
filter upstream, and it produces a whole-file STFT image, not a trace.

Reuses scopeprotocols' complex window shaders unmodified — no copying, no fork:

- `ComplexBlackmanHarrisWindow.spv`
- `ComplexCosineSumWindow.spv`
- `ComplexRectangularWindow.spv`

plus a VkFFT complex-to-complex plan (`numBatches` 1, `TYPE_COMPLEX`). Note the window
shaders bind Q at slot **2**, not 1, so the output binding matches the real-valued window
shaders (`ComplexCosineSumWindow.glsl:43-47`).

Output is a dBm analog stream with x-axis in µHz, because `Waterfall` constrains its
input to `UNIT_MICROHZ` (`Waterfall.cpp:71`). The transform length is the input waveform
length, as in `FFTFilter`; there is deliberately no separate "FFT length" parameter, so
RBW is set by whatever acquisition depth drives the filter.

**fftshift — resolved. `ComplexToLogMagnitude.spv` is not reusable, but not for the
reason expected.** It is not entangled with the normalization at all: it is a clean
`10·log10(|X|²·scale) + 30` with no normalization and no rotation
(`ComplexToLogMagnitude.glsl:59-64`), and its dBm expression is exactly what we want. The
disqualifying part is the missing rotation — it reads `din[i*2]` with `i` the output
index, which is correct for a real FFT (no negative bins) and wrong for a complex one.

The rotation exists only in `ComplexSpectrogramPostprocess.glsl:70-87`, which is also
unusable, and for *two* reasons rather than the one anticipated: it fuses the 0–1
normalization against Range Min/Max, **and** it writes a transposed, column-major
spectrogram image (`nout = x*nblocks + realy`) rather than a linear trace.

So the outcome is the predicted one — one small app-local shader,
`src/imcufosphor/shaders/ComplexToLogMagnitudeShifted.glsl` — reached by a different
route: it is `ComplexToLogMagnitude.glsl` plus the four-line rotation, not
`ComplexSpectrogramPostprocess.glsl` minus the normalization.

**Amplitude calibration — a discrepancy in upstream.** The filter uses
`scale = 1/(√2·N)` to convert bin magnitude to volts RMS, so a unit-amplitude IQ tone
(I = cos, Q = sin) reads **+10 dBm** into 50 Ω, matching what `FFTFilter` reports for a
1 V amplitude real cosine (verified: upstream's real FFT and this filter both read
10.103 dBm for the same tone with a Hamming window). `ComplexSpectrogramFilter.cpp:146`
uses `scale = 2/N` for the same situation, which is **9.03 dB higher**. The convention
here is the self-consistent one; the spectrogram is left alone.

Residual 0.1 dB errors across windows come from `FFTFilter.cpp:222-240`'s coherent gain
constants being empirical rather than exact reciprocals — Hamming uses 1.862 where
1/(25/46) = 1.840, i.e. +0.103 dB. Inherited behaviour, reused for consistency.

**Runtime shader resolution — §7.2's original claim was wrong.** `FindDataFile`
(`scopehal.cpp:939`) searches `g_searchPaths`, which is the binary's own directory plus a
set of *install* prefixes (`scopehal.cpp:821-848`). scopehal and scopeprotocols only put
their `.spv` on that path via `make install` into `share/ngscopeclient`
(`scopeprotocols/shaders/CMakeLists.txt:19`). imcufosphor has no install step and the
submodules are not installed, so in an uninstalled build tree **nothing resolves** — the
compiled shaders sit in each subproject's own binary directory, where nobody looks. This
is not visible until the first `ComputePipeline` is used, which is why the scaffold did
not catch it.

Fixed in our CMake, not by hardcoding paths: a POST_BUILD step
(`cmake/CollectShaders.cmake`) globs every `.spv` the build produced — scopehal's,
scopeprotocols' and ours — into `$<TARGET_FILE_DIR:sigmf-spectrum>/shaders`, which is
search path entry #0. Verified by running the binary from an unrelated working
directory. Our own shaders compile into `shaders-spv/` rather than `shaders/` so glslc
and the collection step do not share an output directory.

**Verification.** `main.cpp` synthesizes a complex exponential at a known offset and
asserts the peak bin, the absolute frequency read off the output x axis, and the
amplitude. Offsets of ±512 and ±1 bins all land exactly on `N/2 + round(f_off·N/fs)` at
10.000 dBm with a rectangular window. The negative cases are the load-bearing ones: they
fail if I/Q are swapped or the rotation direction is inverted.

**Known environment issue.** On the development machine (RTX 5060 Ti, NVIDIA
580.173.02) any scopehal compute submission loses the Vulkan device with `Xid 32`
(corrupted push buffer). This is **not** specific to this filter: it reproduces with an
unmodified scopeprotocols `FFTFilter`, both through a hand-rolled command buffer and
through `FilterGraphExecutor`. The verification therefore passes under
`SCOPEHAL_VULKAN_DEVICE_OVERRIDE=1` (llvmpipe) and the device loss is caught and
reported rather than left to `std::terminate`.

**Unrelated upstream bug worth knowing about.** `PipelineCacheManager`'s destructor
writes the shader cache and logs while doing so; as a static destructor it can outlive
`g_log_sinks` and segfault on exit. `main()` calls `ScopehalStaticCleanup()` explicitly
to avoid this. Any app here that creates a `ComputePipeline` needs to do the same.

### 7.3 `SpectrumReducer`

N-in-1-out accumulator between `ComplexFFTFilter` and `Waterfall`. This exists to break
a coupling in upstream's waterfall (§8.1).

Accumulates K spectra and emits one output — max, average, or min-max envelope —
where `K = fft_rate ÷ lines_per_sec`. `PlayerSession` runs the FFT sub-graph K times per
waterfall advance.

This makes RBW, waterfall line rate, and playback speed **independently controllable**.
Without it they have only two degrees of freedom (§8.1).

**Design the accumulator interface for phase 3 now.** In phase 1 it holds a 1D array
(one value per frequency bin). In phase 3 it widens to 2D — a hit count per
(frequency bin × amplitude cell) — which is a true RTSA density map. Getting that
interface right up front is cheap; retrofitting it is not.

Existing accumulators to model style on: `AverageFilter`, `MovingAverageFilter`,
`ExponentialMovingAverageFilter`, and `PeakHoldFilter` (which accumulates across
successive waveforms into a persistent buffer, reset by `ClearSweeps()`). None do
N-in-1-out, so the reduction logic itself is new.

### 7.4 `PlayerSession`

The thin `Session` analogue. Owns `SigMFSource`, runs the
`AcquireData`/`PopPendingWaveform` loop, drives `FilterGraphExecutor`, owns filter
instances, and implements the K-executions-per-waterfall-row scheduling from §7.3.

### 7.5 Display

**`SpectrumArea`** — thin wrapper around the lifted analog rasterizer (§5), plus peak
markers modeled on `RenderSpectrumPeaks` (`WaveformArea.cpp:1513`).

**`WaterfallArea`** — tone-map dispatch and blit, following `ToneMapWaterfallWaveform`
(`WaveformArea.cpp:2632`) and `RenderWaterfallWaveform` (`WaveformArea.cpp:1309`).

**`AnalyzerPane`** — stacks them, owns the shared frequency axis (transform modeled on
`WaveformGroup.h:108-115`: `XAxisUnitsToPixels`, `XAxisUnitsToXPosition`), linked
pan/zoom, shared cursor, and the transport / RBW / window / range / line-rate controls.

It also owns the **row-history ring** (§8.1), which both the waterfall time axis and the
phase-2 annotation overlay depend on.

---

## 8. Upstream constraints discovered

These are properties of upstream that shape the design. Each is verified; none is a bug.

### 8.1 The waterfall has no frames-per-second concept

`Waterfall::Refresh()` calls `cap->BumpWriteRow()` unconditionally
(`Waterfall.cpp:144`), and `BumpWriteRow()` is `m_writePtr = (m_writePtr + 1) % m_height`
(`Waterfall.h:70-71`). **One row per filter graph execution**, no rate logic. The filter's
entire parameter surface is a single "Max width" (`Waterfall.cpp:63-64`, default 131072).
Height comes from the pane's pixel height (`WaveformArea.cpp:227-235`), not a time span.

So rows/sec == acquisition rate == graph execution rate, implicitly. Against real
hardware in ngscopeclient, waterfall speed is simply whatever the trigger rate is.

For file playback this couples three quantities that should be independent — RBW, line
rate, and playback speed — down to two degrees of freedom. Concretely: a 10 MHz
recording with a 4096-point FFT at 1× realtime with gap-free coverage demands 2441
lines/sec, which is useless as a display; choosing a readable 20 lines/sec instead means
FFT-ing 4096 of every 500 000 samples, i.e. **looking at 0.8% of the recording**. Unlike
real hardware, there is no reason to drop data from a file.

`SpectrumReducer` (§7.3) is the fix.

### 8.2 The waterfall has no per-row timestamps

`WaterfallWaveform` exposes only `GetWriteRow()`/`BumpWriteRow()` (`Waterfall.h:67-70`).
Rows are acquisitions, not sample indices, and the tone-map shader maps row→screen with
modular arithmetic (`WaterfallToneMap.glsl:66-69`).

`AnalyzerPane` therefore maintains a **row-history ring of the same depth as the
waterfall**: on each waterfall advance, push `(writeRow, cursorSampleStart, sampleCount)`.
This is required for a time axis or timestamp readout at all — without it the waterfall
is a scrolling image with no notion of when any row happened — and it is also exactly
what phase-2 annotation placement needs.

The shortcut ("row k from top = k × block_size samples back") holds only while block
size is constant and no acquisition is dropped. The explicit ring is barely more code
and does not lie.

**Mind the sign:** newest row is at top, so increasing sample index goes *upward*.

### 8.3 Persistence is display-layer, not a density map

See §10.

---

## 9. Phase 2 — annotation rendering

A SigMF annotation is already a rectangle in (time, frequency) space:
`sample_start`/`sample_count` give the time extent, `freq_lower_edge`/`freq_upper_edge`
the frequency extent. The waterfall's axes *are* frequency × time, so an annotation is a
box there with no conceptual translation. The spectrum view is that same box with the
time dimension collapsed — which is why it renders as a vertical span (matplotlib's
`axvspan`).

**Alignment is free.** Both the waterfall box and the spectrum span compute their
left/right edges from absolute Hz through `AnalyzerPane`'s single shared axis transform,
so the sides of the box and the edges of the span line up across the stacked panes by
construction.

**Vertical placement on the waterfall** uses the row-history ring (§8.2): annotation
sample range → row range → y pixels, reproducing the same `(height - outheight)` +
`writerow` wraparound the shader uses (`WaterfallToneMap.glsl:66-69`).

**Drawing is pure ImGui overlay**, no shader involvement — both panes already blit a
texture and then draw primitives on top, with `RenderSpectrumPeaks`
(`WaveformArea.cpp:1513`) as the existing precedent.

- Waterfall: `AddRectFilled` at ~10–15% alpha plus `AddRect` outline, inside a
  `PushClipRect` so boxes extending past the visible row range clip at the pane edges.
  Label at the box's top-left.
- Spectrum: `AddRectFilled` spanning full pane height between the two frequency
  x-pixels, plus optional edge lines at `freq_lower_edge`/`freq_upper_edge`, label along
  the top.

**Which annotations, when.** The two panes want different time queries against the same
sorted list, and libsigmf supplies both:

- Spectrum is instantaneous — only annotations overlapping the currently displayed FFT
  block: `get_sigmf_in_range(record, blockStart, blockCount)`, per frame.
- Waterfall shows everything overlapping the visible row range's sample span — same
  helper, wider range derived from the row-history ring.

The span-vs-box distinction thus falls out of the query range, not out of separate logic.

**Degenerate cases.** Annotations without frequency edges (both optional) become
full-width horizontal bands on the waterfall and a full-width tint or margin marker on
the spectrum.

**Culling.** Sort once at load; range-query per frame; never iterate all annotations.
Drop boxes narrower than ~1 px. For label crowding, reuse the `neighborThresholdPixels`
de-duplication pattern from `WaveformArea.cpp:1524`.

**Color.** Hash `core:label` to a stable hue so the same emitter keeps its color across
every box and span.

**Structure.** One `AnnotationOverlay` owned by `AnalyzerPane`, holding the sorted
annotation list and the row-history ring, exposing `DrawOnSpectrum(drawlist, rect,
xform)` and `DrawOnWaterfall(drawlist, rect, xform)`. This keeps `SpectrumArea` and
`WaterfallArea` thin — they render their own waveform and nothing else.

**Later interaction** comes nearly free once the rects exist: hover → tooltip with
`description`/`comment`/`generator`; click → seek to `sample_start`. Both are ImGui
hit-tests against the same rectangles.

---

## 10. Phosphor and true density

### What upstream already provides

Display persistence exists, in `waveform-compute.glsl:395-402`:

```glsl
float fout = g_workingBuffer[y] * alpha;
uint npix = (windowWidth * y) + gl_GlobalInvocationID.x;
if(persistScale != 0)
    fout += outval[npix] * persistScale;
```

The raster target is fp32 and accumulates new trace intensity plus decayed previous
contents, then `WaveformToneMap.glsl` colorizes. Per-channel toggle
(`WaveformArea.cpp:4451-4453`), global decay slider 0–1 (`MainWindow.cpp:881`, default
0.0). It operates on any `STREAM_TYPE_ANALOG` in screen space, so **it applies to an FFT
trace with no new code.** Phase 1 gets a phosphor-looking spectrum for free — provided
`SpectrumArea` wraps the upstream rasterizer (§5).

### What it is not

1. **Not gap-free.** One FFT per acquisition; probability of intercept is whatever
   fraction of samples were transformed. A real RTSA guarantees 100% POI via overlapping
   gap-free FFTs.
2. **Not a density histogram.** It is a recency-weighted decaying raster, not a hit
   count per (frequency, amplitude) cell. Visually similar, statistically different —
   you cannot read "this signal is present 3% of the time" off it.
3. **Decay is per-frame, not per-second**, so the phosphor time constant drifts with
   render frame rate. It is also global rather than per-channel.
4. It accumulates the rasterized *line*, inheriting line-drawing artifacts rather than
   being a true 2D histogram.

### Phase 3 — true density is achievable here

Because this is file playback, we own the read cursor and can compute genuinely gap-free
or overlapped FFTs covering 100% of samples — something real hardware cannot promise.
Widening `SpectrumReducer`'s accumulator from 1D to a 2D hit count per
(frequency bin × amplitude cell) yields a genuine RTSA density map, in the style fosphor
popularized.

`DensityFunctionWaveform` plus the existing tone-map path is exactly the right output
type for it — the same machinery the waterfall and eye diagram already use.

So `SpectrumReducer` serves both features: line-rate decoupling for the waterfall, and
the density accumulator for the spectrum.

---

## 11. Open risks

| # | Risk | Resolution |
|---|------|------------|
| ~~R1~~ | ~~fftshift/dBm shader may not be reusable from `ComplexToLogMagnitude.spv`~~ | **Resolved** — not reusable: it has the dBm conversion but no rotation, and `ComplexSpectrogramPostprocess.spv` has the rotation but also a normalization *and* a transposed image layout. One app-local shader, `ComplexToLogMagnitudeShifted.glsl`. Also uncovered: uninstalled builds resolve no shaders at all, and upstream's complex spectrogram is 9.03 dB off `FFTFilter`'s dBm convention. See §7.2 |
| ~~R2~~ | ~~`ngscopeclient.h` include graph drags state headers into lifted files~~ | **Resolved by deletion.** It was real, not a non-problem: that umbrella brought 1081 lines of instrument-session model into every display header here and every GNU Radio block header through them. Nothing includes it now — the two vendored headers declare what they use. See §5 and D7 |
| R6 | `ConfigPathShim.h` has no Windows implementation (§5) | Needed before any Windows build; `CreateDirectory` is a `windows.h` macro and the path logic differs |
| ~~R7~~ | ~~Vulkan device loss on this workstation's RTX 5060 Ti~~ | **Resolved locally** — NVIDIA driver defect: `vkCmdSetEvent` with a `TRANSFER` stage mask on an async-compute queue, when a graphics queue also exists, faults the device. 166-line scopehal-free repro in `tools/vkmintest/xid32_repro.cpp`; scopehal-wide, upstream's own `tests/Acceleration` fails identically. Unblocked by a tracked local patch (`patches/README.md`); GPU verification now passes with no device override. **Upstreaming still open** — see §3 |
| ~~R3~~ | ~~`get_sample_size()` complex-pair ambiguity~~ | **Resolved** — returns bytes per complex pair; see §7.1 |
| ~~R4~~ | ~~Missing `core:sample_rate` in real-world recordings~~ | **Resolved** — all seven demo recordings carry one; `SigMFSource` warns and leaves the rate at zero rather than crashing, for a caller-supplied fallback. See §7.1.1 |
| R8 | `ComplexChannel`'s center-frequency stream is a `float`, quantizing GHz carriers to ~256 Hz (§7.1.1) | Use `SigMFSource::GetExactCenterFrequency()` for axis labelling; the scalar stream is only for downstream scopehal filters |
| R5 | Cost of K FFT executions per waterfall row at high sample rates (§7.3) | Measure; overlap factor and K are both tunable |

---

## 12. Phasing

**Phase 1 — playable analyzer**

1. Superbuild skeleton; three submodules; resolve R2.
2. `ComplexFFTFilter` + its shader (resolves R1).
3. `SigMFSource` (resolves R3, R4).
4. `SpectrumReducer`, 1D, with the 2D-capable interface.
5. `PlayerSession`.
6. `SpectrumArea`, `WaterfallArea` over lifted primitives.
7. `AnalyzerPane` — shared axis, row-history ring, waterfall time axis, transport and
   RBW/line-rate controls.
8. Persistence enabled on the spectrum trace.

**Phase 2 — annotations**

9. `AnnotationOverlay`: boxes on waterfall, spans on spectrum, range queries, culling.
10. Hover tooltips and click-to-seek.

**Phase 3 — true density**

11. Widen the reducer to 2D hit counts; gap-free/overlapped FFT coverage.
12. Render via `DensityFunctionWaveform` and the existing tone-map path.

---

## 13. Throughput

The target is realtime playback of the 245.76 MS/s LoRa recordings in the demo dataset,
gap-free at 8192 points — 30 000 transforms per second. This section records what was
measured, with what change, so that each optimization is justified by a number rather than
by an argument.

Measure with `tools/wfbench`, which drives the real `PlayerSession` headless — no window,
no vsync, no ImGui:

```
./build/tools/wfbench/wfbench --file /data/deepsig/datasets/demo/lora_uninverted.sigmf-meta \
    --duration 5 --warm-cache
```

It reports throughput alongside a host-time breakdown and per-stage GPU timestamps,
because the total alone cannot distinguish a compute-bound pipeline from an
overhead-bound one, and those have opposite fixes.

### Results

Recording `lora_uninverted` (245.76 MS/s, ci16_le), 8192-point, 1 spectrum per row,
RTX 5060 Ti, warm page cache.

| # | Change | Block | Spectra/row | MS/s | ×realtime |
|---|--------|-------|-------------|------|-----------|
| 0 | Baseline: one submit per spectrum, block size = FFT length | 8 192 | 1 | 21.1 | 0.086 |
| 1 | Reuse the I/Q waveforms instead of allocating a pair per block | 8 192 | 1 | 95.8 | 0.390 |
| 2 | Batch the block into one command buffer and one submit | 8 192 | 1 | 138.9 | 0.565 |
| 2 | …same code, with the block decoupled from the transform length | 1 Msample | 1 024 | **1 417** | **5.77** |

Block size sweep at change 2, 1024 spectra per row:

| Block | 8 192 | 65 536 | 262 144 | 1 Msample | 4 Msample |
|-------|-------|--------|---------|-----------|-----------|
| MS/s  | 170.8 | 664.9  | 1 102.2 | **1 359.8** | 1 162.6 |

1 Msample is the peak and is the default. Above it the intermediate buffers stop fitting in
cache and per-block latency grows without anything to overlap it with.

### What the baseline says

The GPU is not the problem, and by a wide margin. Per spectrum it spends **0.033 ms**
total — 0.023 ms FFT, 0.004 ms reduce, 0.006 ms waterfall — while the pipeline as a whole
takes 388 µs. Extrapolating the GPU cost alone gives 30 000 transforms/s, which is 248 MS/s:
**the device can already do the job at the target rate.** Everything between here and there
is host overhead.

Where the 5 seconds of wall clock actually went:

| | ms | % |
|---|---|---|
| file read | 41 | 0.8 |
| sample convert (ci16 → float, scalar) | 24 | 0.5 |
| **waveform allocation and free** | **3 602** | **72.0** |
| command buffer recording | 106 | 2.1 |
| blocked on submit | 1 212 | 24.2 |

Two findings, both of which reorder the planned work:

1. **Waveform allocation dominates at 280 µs per spectrum.** `SigMFSource::AcquireData()`
   calls `AllocateAnalogWaveform` twice per block, and nothing in this application ever
   returns a waveform to the pool — there is no `HistoryManager`, so `m_analogWaveformPool`
   is permanently empty and every acquisition builds two fresh `AcceleratorBuffer`s: a
   `vkAllocateMemory` and `vkMapMemory` for pinned host memory, a device buffer, and two
   `vk::raii::Event`s, then frees the previous pair when `InstrumentChannel::SetData`
   replaces them. This was expected to be a minor cleanup; it is the single largest cost.

2. **Sample conversion is negligible**, at 0.5%. The scalar int16→float deinterleave was
   the other suspect and it is nowhere near the top. Moving it to the GPU is not urgent.

### Change 1 — reuse the waveforms (21.1 → 95.8 MS/s, 4.5×)

`SigMFSource::AcquireData()` now allocates its I and Q waveforms once and hands the channel
the *same* pointers every block, which makes `InstrumentChannel::SetData` early out
(`InstrumentChannel.cpp:146-147`) instead of deleting them. Allocation went from 3 602 ms to
20 ms of a 5 second run.

The cost is that only one block may be in flight, since the next acquisition overwrites the
buffers the previous one handed downstream. `AcquireData()` enforces the queue half of that
with an explicit check; the GPU half is enforced by `PlayerSession` blocking on each submit.
**Any move to overlapped submissions has to give each in-flight block its own buffers.**

With allocation gone the bottleneck moved cleanly to submission: **90% of wall clock is now
spent blocked in `SubmitAndBlock`**, at 38 µs per submit against 14 µs of actual GPU work per
row. That is the fence round trip, paid twice per waterfall row, and it is what batching
addresses.

### Change 2 — batch the block (95.8 → 1417 MS/s, 14.8×)

Three things at once, all of them consequences of one decision: the acquisition block size
is no longer the transform length.

- `SigMFSource` delivers a block of arbitrary size. `ComplexFFTFilter` cuts it into
  `floor(N/L)` consecutive non-overlapping transforms and runs them as **one batched VkFFT
  dispatch** (`numberBatches` = the transform count), with one window dispatch per segment
  varying only `offsetIn`/`offsetOut`, and one postprocess dispatch over the whole batch.
  This is the shape `ComplexSpectrogramFilter.cpp:265-320` already uses; we followed it.
- `SpectrumReducer` folds every spectrum in the block into its accumulator in a single
  dispatch, walking the block at stride `nbins` so the reads coalesce.
- `PlayerSession` records the whole graph - FFT, reducer, and the waterfall when a group
  completes - into **one command buffer and one submit**, with explicit
  `AddComputeMemoryBarrier` between stages to replace the ordering the separate submits used
  to provide for free.

At 1 Msample and 8192 points that is 128 transforms per submit instead of one, and the
waterfall no longer needs a submit of its own. Even at the *old* 8192 block size the merge
of the waterfall submit into the graph's is worth 95.8 → 138.9 MS/s on its own.

**The 250 MS/s target is met with 5.7× of margin**, and the GUI plays `lora_uninverted` at
815 waterfall rows/s and 60 fps with 100% coverage.

Where the time goes now, at 1 Msample:

| | % of wall clock |
|---|---|
| blocked on submit | 58.3 |
| sample convert (ci16 → float) | 22.7 |
| file read | 15.5 |
| command recording | 3.4 |
| waveform allocation | 0.1 |

GPU cost is 0.387 ms per block of 128 transforms — **3.0 µs per transform**, against 33 µs
before. The submit wait is now only ~44 µs more than the GPU work it is waiting for, so the
pipeline is close to GPU bound.

### What is left, and why it is not being done yet

Host work and GPU work are strictly serialized: 2 081 ms of read and convert plus 2 913 ms
of submit wait fills the 5 second run almost exactly. The two obvious remaining levers are
therefore:

1. **Overlap host and GPU** (two `QueueHandle`s, acquisition on a worker thread). Worth up
   to ~1.7× on these numbers. Requires giving each in-flight block its own sample buffers,
   which change 1 explicitly traded away.
2. **Move the ci16 → float conversion into the window shader**, over a pinned zero-copy
   buffer. Worth up to ~1.3× and it halves PCIe traffic.

Both were in the original plan. Neither is being done now, because the target is met with
5.7× of margin and every one of these adds a way for the pipeline to be subtly wrong. They
are the right next steps whenever the margin is actually needed — overlapped FFTs for true
100% probability of intercept (§10) would consume it immediately.

Two smaller items measured as not worth doing: a single batched window dispatch replacing
the 128 per block (the FFT dominates the 0.387 ms, so the window dispatches are noise), and
fusing magnitude+fftshift into the reducer (the reduce stage is 0.006 ms of 0.387 ms).

### Change 3 — the packed ingest path (1304 → 2345 MS/s, 1.80×)

Lever 2 above, done, and worth more than the ~1.3× predicted for it. Measured on a 245.76
MS/s ci16_le recording at 8192 points and a 1 Msample block, `--warm-cache`, same machine,
A/B via `wfbench --planar`:

| | planar | packed | |
|---|---|---|---|
| throughput | 1304.2 MS/s | 2345.1 MS/s | **1.80×** |
| realtime ratio | 5.31× | 9.54× | |
| sample convert | 1025 ms (20.5%) | **0 ms** | gone |
| command recording | 166 ms (3.3%) | 90 ms (1.8%) | 1.85× |
| GPU window+FFT+post | 0.379 ms | 0.208 ms | 1.82× |

The prediction was low because it counted only the CPU conversion and the PCIe traffic. Two
other things came with it:

**The dispatch count.** The old window shaders derive the window coefficient from a flat
global thread index, so one dispatch can only cover one transform — 128 per block, which is
where most of the command recording time went. `PackedComplexWindow.glsl` takes the
within-transform index from x/y and the transform index from **z**, so the whole batch is one
dispatch. This is the item recorded above as "measured as not worth doing"; it was not worth
doing *on its own*, and it costs nothing once the shader is being rewritten anyway.

**GPU-side read traffic.** The window pass reads 4 MB per Msample block instead of 8 MB,
because it reads the ci16 words rather than two float arrays. That is most of the 0.171 ms
the FFT stage lost, the FFT and postprocess being unchanged.

The CPU conversion did not move to the GPU so much as stop existing. There is no deinterleave
anywhere in the pipeline now, in either direction: ci16_le on disk is already the interleaved
complex layout VkFFT wants (`bufferSeparateComplexComponents` defaults to 0, and the planar
mode is six special cases in `vkFFT_ReadWrite.h` rather than the fast path), so the file's
bytes go into pinned memory by `pread`, cross the bus unmodified, and one shader unpacks,
converts, windows and writes interleaved floats. The staging `vector<uint8_t>` is gone from
the path too, and `PrepareForCpuAccessIgnoringGpuData()` avoids faulting back a device copy
that is about to be entirely overwritten.

Formats: 8/16/32-bit signed and unsigned integers and float32 are all unpacked in the shader,
with the offset-binary convention and the normalization constants matching `ConvertSamples()`
exactly. 64-bit formats and any file whose byte order is not the host's fall back to the CPU
path, which is unchanged and still covered by its own verification run.

`shaderInt16` is deliberately not used, though scopehal probes for it (`VulkanInit.cpp:817`).
Components are extracted from `uint` words with `bitfieldExtract`, which is one instruction
either way and needs no feature bit, so the shader runs on any Vulkan 1.0 device.

### Where the time goes now

| | % of wall clock |
|---|---|
| blocked on submit | 79.9 |
| file read | 18.2 |
| command recording | 1.8 |
| sample convert | 0.0 |
| waveform allocation | 0.1 |

Host and GPU are still strictly serialized, so **lever 1 is now the whole story**: 80% of
wall clock is a fence round trip. It is also cheaper to do than it was, because there is one
sample buffer per in-flight block to double-buffer instead of two.

File read at 18.2% is second, and is now a real cost rather than a rounding error: 2.3 GS/s
of ci16 is 9.4 GB/s out of the page cache. A reader thread would hide it behind the same
overlap lever 1 needs.

### A window bug found on the way past

scopeprotocols' `BlackmanHarrisWindow.glsl:83` and `ComplexBlackmanHarrisWindow.glsl:94`
evaluate the fourth Blackman-Harris term at `cos(6*num)` where the definition has
`cos(3*num)`. Coherent gain is unaffected — both cosines average to zero over the window, so
the 2.805 amplitude correction and every calibration check still pass — but the four-term
sidelobe cancellation the window exists for is destroyed. Measured over 8192 points:

| | peak sidelobe |
|---|---|
| as shipped, `cos(6*num)` | −35.6 dB |
| correct, `cos(3*num)` | −92.0 dB |

Blackman-Harris is this application's default window (`PlayerSession.cpp:40`), so this was
costing 56 dB of usable dynamic range on every spectrum. `PackedComplexWindow.glsl` uses the
correct coefficients. The planar fallback still calls the upstream shader and still has the
old behaviour, which is why `VerifyPackedIQPath()` compares the two paths under Rectangular,
Hamming and Hann but not Blackman-Harris. **This should be reported upstream**; it affects
ngscopeclient's real-input FFT identically.

---

## 14. RTSA density and percentile traces

Two displays were wanted: the fosphor-style density, where each frequency bin shows a
vertical intensity histogram of the amplitudes it has taken, and a set of min/mean/max-style
traces defined as percentiles rather than extremes. They are the same computation.

A hit histogram over (frequency bin × amplitude cell) is the density map when colormapped,
and a cumulative sum down each column yields any percentile plus the mean. Computing them
separately would cost twice as much and let the two disagree about what the signal did.

`SpectrumDensity` is a **parallel consumer** of `ComplexFFTFilter`'s batch, not a stage after
`SpectrumReducer`. §10 anticipated widening the reducer itself; that was written before the
FFT emitted a batch per block. Now that it does, a second consumer is simpler — the two want
different cadences and produce different output types.

### Structure

```
FFT batch ──┬──► SpectrumReducer ──► Waterfall            every block
            │
            └──► SpectrumDensity                          every block: accumulate hits
                     │                                    every window: fold + traces
                     ├──► density map   (nbins × 256, decayed)
                     └──► mean, median, low, mid, high    (dBm, nbins each)
```

Hits accumulate every block; the expensive per-cell passes run once per window. Traces are
therefore exact over a defined window while the density decays continuously across windows,
and because both come from the same hits they cannot disagree.

### What was taken from gr-fosphor

The decay is fosphor's closed-form batch IIR (`display.cl:241-247`), which is the single most
valuable idea in that codebase:

```
a = hits/N,  b = a/t0r,  c = b + 1/t0d,  d = b/c
h ← (h − d)·(1 − c)^N + d
```

This is the exact result of applying `h ← h + (a/t0r)(1−h) − h/t0d` once per spectrum,
computed in five flops. The density map is touched **once per fold regardless of how many
spectra went in** — cost is O(bins × cells), independent of spectrum rate, which is what
makes an RTSA at 30 000 spectra/s affordable at all.

Three deliberate departures:

- **Time constants in seconds, not spectra.** fosphor hardcodes them as counts
  (`cl.c:714-715`, `t0d = 1024`) so persistence silently changes with sample rate; at our
  rates its 1024 spectra is 34 ms against the ~100 ms it means at the rates fosphor was
  written for.
- **No fast-exit for near-zero cells.** fosphor skips cells below 0.01 with no new hits
  (`display.cl:236`), leaving them frozen at stale values forever — hence its comment about
  the texture never being cleared. We let them decay to zero.
- **No atomics.** One thread owns one frequency column for a whole block, so nothing is
  shared. fosphor needs three build-time atomic variants only because it buckets sixteen
  bins into one workgroup's shared memory.

### Measurements

`wfbench --verify` cross-checks the GPU traces against a CPU computation over the
histogram the GPU actually built, using two passes over identical samples.

**Cell count does not affect accuracy.** Trace values are identical to within 0.1 dB from 128
to 1024 amplitude cells, because the trace pass interpolates within the cell a percentile
lands in. Cell height stops mattering long before it gets small.

**Cell count affects cost sharply, and non-linearly:**

| Histogram cells | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|
| Hit buffer at 8192 bins | 4 MB | 8 MB | 16 MB | 32 MB |
| GPU ms per block | — | 0.073 | 0.110 | 0.567 |
| Throughput, MS/s | — | 1276 | 1217 | 797 |

The jump between 512 and 1024 is 5×, not the 2× the extra cells imply: 32 MB is the L2 size
on this part, and both per-fold passes stream the whole hit buffer. **512 is the default** —
the largest that fits in L2 at 8192 bins. A 16384-point transform would put 512 cells back at
32 MB, so the default is a function of the FFT length, not a constant.

Cost is dominated by the per-fold passes, not the scatter: accumulation alone is 0.084 ms per
block against 0.111 ms for the whole density path at the default fold interval.

**Physical check.** A noise-only bin holds an exponentially distributed power, whose 10th to
95th percentile spread in dB is `10·log10(ln 0.05 / ln 0.90)` = 14.5 dB regardless of noise
level. Measured median across 8192 bins: **14.37 dB**. That confirms the axis mapping and the
percentile math independently of the CPU cross-check, which would agree with itself even if
both were wrong.

### Throughput with density enabled

| Configuration | MS/s | ×realtime |
|---|---|---|
| Waterfall only (§13) | 1417 | 5.77 |
| Waterfall + density + 5 traces | 1199 | 4.88 |

The RTSA path costs 15% of throughput and leaves 4.9× realtime headroom on the 245.76 MS/s
recordings.

### Known limitations

- The **mean is a video average** — the mean of dB values, reading ~2.5 dB low against a true
  power average on Gaussian noise. That is the same convention `SpectrumReducer` uses and the
  same one fosphor uses throughout (`display.cl:136`). Correct for a display, wrong for a
  measurement. A power average cannot be recovered from a dB histogram and would need its own
  linear accumulator ahead of the log.
- Out-of-range values **clamp** into the end cells rather than being dropped, which biases
  percentiles when a signal is off scale. This is deliberate: it keeps every column's total
  equal to the spectrum count, which is what lets the trace pass find percentiles in one
  sweep with no pass to total each column. A saturation indicator would be worth adding.
- Changing the amplitude range **discards** the accumulated map, because every cell index
  refers to the old axis. fosphor does not do this and its display is visibly wrong for a
  decay time after any range change.

---

## 15. SpectrumArea

Two layers over each other, either independently switchable, so the same view covers both
the fosphor-style RTSA display and a conventional multi-detector spectrum plot.

**Density layer.** `SpectrumDensityToneMap.glsl` colourizes the map through a ramp. It is
`WaterfallToneMap.glsl` with the ring-buffer row rotation dropped — a density map is a plain
image, not a scrolling history — and a floor and gain added. The texture is only as tall as
the map has cells, so ImGui magnifies it, which is what gr-fosphor does with its 128-cell
histogram (`gl.c:211,436-438`) and why its display looks smooth rather than banded. It also
means a pane resize never touches the accumulator.

**The floor is not cosmetic.** A noise bin's power is exponentially distributed, so in dB it
has a long tail toward negative infinity against a sharp upper edge. Every cell that tail
visits holds a small value for a decay time, and a continuous ramp renders that as a wide dim
haze that swamps the band shape. gr-fosphor hits the same thing and crushes it in its
palette, whose bottom sixteenth is a dark band with a hard discontinuity at the top
(`gl_cmap_gen.c:150-179`). Doing it in the shader instead keeps the ramp swappable and the
threshold adjustable, which matters because the right threshold depends on the decay constant
and the window length.

**Trace layer.** The five traces go through the lifted analog rasterizer
(`waveform-compute.analog[.int64].dense.spv`), not any line drawing of our own — a design
commitment from §5, since display persistence lives inside that shader. They composite into
**one** texture via `SpectrumTraceToneMap.glsl` rather than getting a texture each as
ngscopeclient does per channel: five always-present traces of identical size is a different
situation from channels coming and going, and one texture is 23 MB against 115 MB at
1600x900.

### Two traps, both found by running it

**The x mapping is not the trigger phase.** `FetchX()` returns the sample index plus
`innerXoff` (`waveform-compute.glsl:163`), so `innerXoff` is the pan position measured *from
the first sample*, not the absolute frequency of that sample. Passing the trigger phase
directly puts sample zero about 12 000 pixels off the right edge and nothing draws at all.
Upstream subtracts it (`WaveformArea.cpp:2547-2551`); so do we. The int64/float split of the
trigger phase is also not optional — 2.3e15 microhertz does not fit in a float to anywhere
near a bin.

**Turning off the last visible trace left it on screen.** The composite was skipped when
nothing was visible, which is the cheap thing to do, but `Render()` still drew the texture —
holding whatever was composited last. Whichever trace was unchecked last stayed up, which
looked like a bug specific to that trace rather than to the ordering. `ToneMap()` now records
what it actually composited and `Render()` skips a texture nothing wrote.

`--traces MASK` sets initial visibility from the command line. It exists because a display
state cannot otherwise be reproduced without clicking, which makes a screenshot comparison
impossible to trust — and it is what isolated the bug above to the toggle path rather than
the compositing.

### Cost

The whole render path is 0.2 ms of tone map per frame against a 6 ms present, and throughput
with everything running is unchanged at **1194 MS/s, 4.9x realtime**.

---

## 16. AnalyzerPane and the axis split

§12 item 7. Where the axis code lives was the design question; the answer follows
ngscopeclient, with one extension.

### Who owns what

**The X axis is shared, so the container owns it.** `WaveformGroup` holds the offset and
scale and draws exactly one timeline for all its areas (`WaveformGroup.cpp:285`), sized to
`clientWidth - GetYAxisWidth() - spacing`.

**The Y axis is per-plot, so the area owns it.** `WaveformArea` holds its own offset, scale
and unit (`WaveformArea.cpp:693-705`). That is right here too, because the two Y axes mean
different things: amplitude in dBm for the spectrum, age in seconds for the waterfall.

**The extension:** upstream's `WaveformArea` cannot be used outside a group. Ours can. Each
area holds a `std::shared_ptr<PlotAxis>` and **creates its own if nobody supplies one**, so a
standalone area draws a full set of rulers. `AnalyzerPane` hands both areas the same
instance, which makes them linked *by construction* — there is no per-frame copying of offset
and scale between panes and therefore no order in which that copying can be wrong.

```
PlotAxis          transform, tick generation, ruler drawing.  Reused by all three axes.
RowHistory        which samples went into each waterfall row (section 8.2)
SpectrumArea      owns a dBm axis, references an X axis
WaterfallArea     owns a time axis, references an X axis
AnalyzerPane      owns the shared X axis, lays out the stack, handles the mouse
```

`PlotAxis` is a separate object rather than a pair of members in each area precisely so that
sharing is possible. Tick generation is split from drawing so a cursor readout, gridlines or
a test can have tick positions without a draw list.

### The waterfall time axis

Upstream records no per-row timestamps, so `RowHistory` records the sample index behind each
row as it is produced. DESIGN §8.2 put that ring in `AnalyzerPane`; it lives in
`PlayerSession` instead, because that is where rows are produced. Pushing at the point of
truth means it is populated exactly once and both a standalone area and a pane can read it.

It records **samples delivered, not the play cursor**. The cursor wraps to zero every time
playback loops, and at these rates the whole recording goes past about once a second, so a
wrapped pair of rows gives a negative span and the axis falls back to unitless rows. That
showed up immediately on screen.

The span is measured rather than assumed. Interpolation between ticks does assume a steady
line rate, which holds whenever the block and group sizes are not being changed.

### Two bugs worth recording

**Two owners fitting one shared axis.** Both the pane and the area auto-fitted the axis to
the full span on their first frame with data, so the area silently undid the pane's fit on
the same frame. Nothing looked wrong at full span, which is exactly why it survived: the
symptom only appears once something else moves the axis. `SetXAxis` now clears an
`m_ownsXAxis` flag, and an area only auto-fits an axis it owns.

**A screenshot at full span proves nothing about panning**, because every pan offset is zero
there. `--zoom N` applies a zoom once after the initial fit, which is what exercised the
offset path in both tone-map shaders and found the bug above. `WaterfallToneMap.glsl:29`
takes its offset as *unsigned*, so a view panned left of the first bin wraps to four billion;
`AnalyzerPane::ClampXAxis` keeps the view inside the data, and the waterfall refuses rather
than rendering garbage if anything else moves it.

### Still open

- Cursors and a readout. `PlotAxis::PositionToUnits` is what they need and is already there.
- Tick labels pick their SI prefix per value, so a ruler spanning decades reads
  "0.000 fs ... -200 ms ... -1.20 s". `Unit::PrettyPrintRange` chooses one scale for a whole
  range but formats a pair, and the helpers it uses are protected, so a consistent-scale
  ruler needs either an upstream addition or a local reimplementation of the SI scaling.
- The waterfall does not resample vertically, so its time axis is linear in rows rather than
  in time. Only visible if the line rate changes while data is on screen.

---

## 17. The application and the tool

The application is **`sigmf-spectrum`**. It takes one positional argument, a `.sigmf-meta`
file, and has no other command line surface. Everything adjustable is adjustable in the UI.

It used to carry a dozen flags. They fell into two groups, and neither belonged in a shipped
binary:

- **A test suite.** `--sigmf`, `--survey` and an unconditional FFT self-check ran on every
  invocation that was not `--gui`. A build that links is still not evidence of a correct FFT,
  so none of that was deleted - it moved to `tools/wfbench/Verify.cpp`, which is where a
  question about correctness belongs.
- **Reproducibility hooks for screenshots.** `--traces`, `--zoom`, `--zoom-delay` and
  `--persist` existed so a given display state could be reached without clicking. They earned
  their keep: `--traces` isolated the trace-clearing bug to the toggle path, and
  `--zoom`/`--zoom-delay`/`--persist` are what produced the before-and-after images proving
  the persistence invalidation works. Having served that purpose they are gone.

`wfbench` keeps its flags; it is a tool, not a product. One `--verify` now runs every check:

```
wfbench --file FILE.sigmf-meta --block 1M --verify [--expect-tone HZ]
wfbench --survey /data/deepsig/datasets/demo/*.sigmf-meta
```

Splitting the checks across several flags was a way to quietly drop one from a gate, and
running them together immediately found a real coupling: each check now sets up the session
state it needs rather than trusting whatever the previous one left behind.

### A teardown ordering bug worth remembering

`ScopehalStaticCleanup()` must run after everything holding Vulkan resources is destroyed.
The application gets this right by construction - its session dies when `RunGui` returns -
but `wfbench` held a `PlayerSession` on `main`'s stack, so cleanup destroyed the device and
the session's destructor then ran against it, faulting inside the driver. `wfbench` now does
its work in a `Run()` function whose scope ends before cleanup.

This replaced an earlier workaround that emptied `g_log_sinks` to stop
`PipelineCacheManager`'s destructor from logging through freed sinks. The real fix was the
explicit cleanup call the application already used; the workaround was treating the symptom.
