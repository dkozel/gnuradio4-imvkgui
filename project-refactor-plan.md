# imcufosphor — structural refactor: libraries, ingest, and rate parameters

## Context

The project works: `sigmf-spectrum` plays a 245.76 MS/s recording at ~4.9× realtime with
density, waterfall and traces on screen (DESIGN.md §13–15), and a GNU Radio 4 OOT module
(`gr-imcufosphor/`) drives the same engine from a live flowgraph. Having reached that point,
the structure now has to be made to match what the code has become. Three problems, all of
them consequences of the code arriving faster than the boundaries did:

1. **The layering is nominal, not enforced.** `imcufosphor-core` mixes pure-GPU compute with
   SigMF-specific playback, so `wfbench` and every GR4 unit test link libsigmf and flatbuffers
   they never use. `RenderHost.h:16` includes `ngscopeclient.h`, which drags GLFW, both ImGui
   backends and six ngscopeclient instrument-state headers into every GR4 block header.
   `AnnotationOverlay.h:17` includes `PlayerSession.h` for one type it could get from
   `AnalyzerSource.h`. `qa_IqLayout` — a test that checks `sizeof(std::complex<int16_t>)` —
   links X11.

2. **Host-side copies exist for API-shape reasons.** The GR4 path is
   `port span → IqRing → memcpy into pinned → DMA to VRAM`: three host-visible moves against
   the file path's two. The middle `memcpy` (`IqInjector.cpp:138`) is type-identical,
   layout-identical and contiguous on both ends — it exists only because the engine's ingest
   API takes a `span` to read from instead of handing out a destination to write into.

3. **The waterfall's time scale is set by the wrong knob.** `SpectrumReducer` only emits on
   block boundaries (`SpectrumReducer.cpp:183-187`), so a row is `ceil(k/S)·B` samples where
   `S = floor(B/N)`. At every default (`B`=1 Msample, `N`=8192, `k`=1) that is exactly one
   block, which makes the transfer block size — an implementation detail — the sole
   determinant of the waterfall time axis, and makes the "Spectra/row" slider inert until it
   passes 128. Changing "Block" from 1 Msample to 8192 shortens the visible time span 128×
   with nothing else touched.

Intended outcome: a dependency hierarchy where each target links only what it uses; a mid-level
library that can either create the GLFW/Vulkan/ImGui environment or adopt one owned by a higher-level
ImGui application; a single host copy on the ingest path with the API shaped so a true zero-copy
is expressible; and user-facing parameters that name physical quantities (rows/sec, RBW) with
implementation details (block size, step budget) derived or moved to an advanced section.

## Decisions taken

- **Ingest**: the library hands out the destination. `IqInjector`/`SpectrumEngine` gain a
  `BeginBlock()` → writable pinned span / `CommitBlock()` pair; the producer writes straight
  into pinned memory. The pinned→VRAM DMA **stays** — it is the right thing for a buffer the
  shader streams. Casting a GNU Radio port buffer in place to a `PackedIQWaveform` is designed
  for but scoped separately (§C4), because it needs `AcceleratorBuffer` to adopt external
  memory.
- **Embedding**: support both. The no-patch baseline is "the embedder calls scopehal's
  `VulkanInit()` and owns everything above it"; on top of that, a tracked
  `patches/scopehal/0002-VulkanInitFromExisting.patch` lets a foreign `VkInstance`/`VkDevice`
  be passed down, so an external ImGui application does not have to let scopehal create its
  device. This is a second deliberate exception to DESIGN.md D4, on the same terms as the
  first (branch in the submodule, `format-patch`, message written for upstream).
- **Rate model**: row duration is the user knob; spectra-per-row is derived; the reducer is
  fixed to emit at exact group boundaries so block size stops quantizing the time axis.
- **Order**: layering → embed contract → ingest → parameters.

## Target architecture

```
imcufosphor-model    Annotation, RecordingClock, RowHistory        → scopehal
imcufosphor-dsp      ComplexFFTFilter, SpectrumReducer,            → model, scopehal,
                     SpectrumDensity, SpectrumEngine, IqInjector,    scopeprotocols
                     GpuTimer, PackedIQWaveform, AnalyzerSource
imcufosphor-sigmf    SigMFSource, PlayerSession                    → dsp, libsigmf
imcufosphor-imgui    imgui core (5 TUs)                            [replaceable, §A3]
imcufosphor-imgui-vk imgui_impl_vulkan                             → imgui, Vulkan
imcufosphor-imgui-glfw imgui_impl_glfw                             → imgui, glfw
imcufosphor-textures TextureManager + TextureCompat.h              → imgui, imgui-vk, PNG, ZLIB
imcufosphor-render   PlotAxis, SpectrumArea, WaterfallArea,        → dsp, model, textures
                     AnalyzerPane, AnnotationOverlay, RenderContext
ngscopeclient-window VulkanWindow, Preference*                     → textures, imgui-glfw, glfw
imcufosphor-host     AppWindow, OwnedRenderContext                 → render, ngscopeclient-window
```

Consumers: `wfbench` → sigmf. `sigmf-spectrum` → sigmf + host. `gr4-analyzer` → block headers
+ host. `gr4-bench` and the GR4 block headers → render (no libsigmf, no GLFW, no VulkanWindow).
`qa_IqLayout`/`qa_MetaFromTags` → a new `blocks_detail_headers` INTERFACE target (gnuradio4 only).

---

## Phase A — decouple and split (no behaviour change)

Each step builds with `BUILD_GR4_BLOCKS` both `OFF` and `ON`, and `ctest` stays green.

**A1. The two bad includes.**
- `src/imcufosphor/AnnotationOverlay.h:17` — replace `#include "PlayerSession.h"` with
  `#include "AnalyzerSource.h"`. It is a substitution, not a deletion: `AnnotationOverlay.h:63`
  needs `BlockSpan` from `AnalyzerSource.h:62`. Also swap `:14`'s `ngscopeclient.h` for `<imgui.h>`.
- `src/imcufosphor/RenderHost.h:16` — replace `ngscopeclient.h` with a new
  `src/ngscopeclient-compat/TextureCompat.h` (scopehal.h + GLFW headers + imgui.h +
  TextureManager.h), which is the minimal umbrella `TextureManager.h` actually needs. Correct
  the comment at `RenderHost.h:95-96` while there: block `draw()` runs *before* the render pass
  is recorded (`VulkanWindow.cpp:623` vs `:691`), so the real invariant is only "between
  `ImGui::NewFrame()` and `ImGui::Render()`".
- Same substitution in `SpectrumArea.h:14`, `WaterfallArea.h:14`, `PlotAxis.h:14`.

**A2. Split the GR4 detail headers from the drawing.** Move `RenderStreamStatus()` out of
`blocks/display/include/gnuradio-4.0/imcufosphor/detail/StreamStatus.hpp` (drops its
`imgui.h` include at `:18`) into a sibling `ui/StreamStatusUI.hpp`. Add an INTERFACE target
`gr_imcufosphor_blocks_detail_headers` and repoint `qa_IqLayout` and `qa_MetaFromTags` at it.

**A3. Split `ngscopeclient-compat`** into `imcufosphor-imgui` / `-imgui-vk` / `-imgui-glfw` /
`imcufosphor-textures` / `ngscopeclient-window`, per the table above. Move
`IMGUI_DEFINE_MATH_OPERATORS` from `ngscopeclient.h:37` to `imcufosphor-imgui`'s PUBLIC compile
definitions so every TU agrees (inconsistency there is a silent ODR hazard on the `ImVec2`
operators). Add `IMCUFOSPHOR_IMGUI_TARGET` as a cache variable defaulting to our target, so a
source-integrating embedder can point it at their own ImGui. Delete the `if(LINUX) … X11` link
(`src/ngscopeclient-compat/CMakeLists.txt`) — none of the five compiled ngscopeclient sources
references X11; it was copied from upstream's `PLATFORM_LIBS` for files we do not build.
`imcufosphor-textures` takes GLFW's *include* directories (for `GLFWimage`,
`TextureManager.h:114`) without linking GLFW.

**A4. Split `imcufosphor-core`** into `imcufosphor-model` / `-dsp` / `-sigmf`. No file moves and
no `#include` changes — `Annotation.cpp:14-15` and `RecordingClock.cpp:12` are already
libsigmf-free, and the SigMF→`Annotation` conversion is already isolated in
`SigMFSource.cpp`'s `BuildAnnotationSet`. Keep `add_library(imcufosphor-core INTERFACE)` over
all three for one commit, repoint consumers, then delete it.

**A5. Keep the boundary from creeping back.** Extend the existing
`ngscopeclient-compat-linkcheck` pattern (`src/ngscopeclient-compat/LinkCheck.cpp`) with:
a link check on `imcufosphor-textures` alone (proves textures need neither `VulkanWindow` nor
`PreferenceManager`), and a `render-headers-selfcheck` target that compiles each
`imcufosphor-render` public header alone with `${SCOPEHAL_APPS_DIR}/src/ngscopeclient` and the
backends dir deliberately absent from the include path. Rule to write into DESIGN.md:
*`imcufosphor-render`'s public headers may name ImGui core, `vk::`/`vk::raii` and scopehal
types, and nothing else.*

---

## Phase B — render context and the embed contract

**B1. `RenderHost` → `RenderContext`** (`src/imcufosphor/RenderContext.{h,cpp}`, in
`imcufosphor-render`). Concrete `final` class, not an abstract base — both the owned and adopted
paths end at the same `g_vkComputeDevice`, so there is nothing to be polymorphic over, and a
vtable across the embed boundary is the ODR hazard this work exists to reduce.

```cpp
struct AdoptDesc {
    std::shared_ptr<QueueHandle> renderQueue;          // required
    float                        dpiScale      = 1.0f;
    TextureManager*              textures      = nullptr;  // null → we create one
    vk::raii::CommandBuffer*     computeCmdBuf = nullptr;  // null → we allocate a pool
};
class RenderContext final {
    static std::unique_ptr<RenderContext> Adopt(const AdoptDesc&);
    TextureManager* Textures() const;  std::shared_ptr<QueueHandle> RenderQueue() const;
    float DpiScale() const;            vk::raii::CommandBuffer* FrameComputeCommandBuffer() const;
    void BeginFrame(); void EndFrame(); void ReleaseGpuResources();
};
RenderContext* CurrentRenderContext() noexcept;
RenderContext* SetCurrentRenderContext(RenderContext*) noexcept;   // returns previous
class ScopedRenderContext { /* RAII swap, for tests and nested hosts */ };
```

`Available()`/`Publish()`/`Retract()` (`RenderHost.h:69,117,125`) collapse into one atomic
pointer — one release store instead of a flag plus three fields hand-ordered at
`RenderHost.cpp:31-50`. Null is headless, which stays a supported state.

**B2. GR4 blocks keep the global, but gain a typed override.** The reasoning at
`RenderHost.h:26-39` still holds — `emplaceBlock` takes a `property_map` and smuggling a
pointer through it as an integer would be untyped and invisible in the flowgraph. Each sink
gains a plain member `::imcufosphor::RenderContext* renderContext = nullptr;` — deliberately
**not** a `GR_MAKE_REFLECTABLE` setting, since it is not serialisable and `settingsChanged()`
runs on a scheduler thread (`AnalyzerSink.hpp:313-319`). `draw()` opens with
`auto* ctx = renderContext ? renderContext : CurrentRenderContext(); if(!ctx) return status();`,
which also collapses the seven `globalRenderHost().DpiScale()` calls at
`AnalyzerSink.hpp:488-525` into one local.

**B3. `AppWindow`** (`imcufosphor-host`) — a `VulkanWindow` subclass owning the
`TextureManager`, the pre-render-pass command pool/buffer, the DPI query and a `RenderContext`
over them. It absorbs three verbatim duplications between `MainWindow.cpp` and
`Gr4AnalyzerWindow.cpp`: TextureManager + ramp load (`:36-39` ≡ `:26-30`), command pool
(`:49-56` ≡ `:32-40`), and `GetDpiScale()` (`:73-83` ≡ `:50-60`, which reimplements
`VulkanWindow::GetContentScale()` — declared at `VulkanWindow.h:54`, defined nowhere upstream).

**B4. The embedder contract**, written into DESIGN.md as a numbered list, because every item
is something an embedder will otherwise get wrong:
1. Use scopehal's device. `VulkanInit()` writes `g_vkComputeDevice` (`VulkanInit.cpp:953`),
   `g_vkQueueManager` (`:1040`), `g_vkTransferQueue` (`:1045`), read directly by
   `SpectrumArea.cpp:129-151`, `WaterfallArea.cpp:126-148` and all of `TextureManager.cpp`.
   Baseline: the embedder calls `VulkanInit(false)` and builds its window and ImGui backend on
   that device. B5 removes this requirement.
2. Descriptor pool must carry `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` —
   `Texture::~Texture` → `ImGui_ImplVulkan_RemoveTexture` (`TextureManager.cpp:272`) frees
   descriptor sets. Budget: 1 `SAMPLED_IMAGE` per colour ramp plus 3 per `AnalyzerPane`
   (`SpectrumArea.h:311,329`, `WaterfallArea.h:166`), reallocated on every resize.
   `VulkanWindow.cpp:255` uses 1000 of each, which is the number to quote.
3. Frame order, one thread: `ImGui::NewFrame()` → `ctx->BeginFrame()` → our draws →
   `ctx->EndFrame()` → `ImGui::Render()` → the embedder's render pass and present. No barrier
   or semaphore is needed from the embedder, because every submit of ours ends in
   `QueueHandle::SubmitAndBlock`. That stops being true the day `SpectrumEngine::Step()` goes
   non-blocking (already flagged at `AnalyzerSink.hpp:436-439`), so the contract says so.
4. Teardown order: stop the scheduler → destroy panes/`ReleaseGpuResources()` →
   `SetCurrentRenderContext(nullptr)` → drop the context → `ImGui_ImplVulkan_Shutdown()` →
   `ImGui::DestroyContext()` → the embedder's window → `ScopehalStaticCleanup()` **last**,
   because `VulkanCleanup()` calls `glfwTerminate()` (`VulkanInit.cpp:1232`), which is not
   refcounted and destroys every GLFW window in the process.
5. Same ImGui object code. `Adopt()` asserts it cheaply: `IMGUI_CHECKVERSION()`, then
   `ImGui::GetCurrentContext() != nullptr` **and** `ImGui::GetIO().BackendRendererUserData !=
   nullptr` — the second is what distinguishes "you initialised a different copy of
   `imgui_impl_vulkan`" from a mystery crash. Log `ImGui::GetVersion()` against `IMGUI_VERSION`.

**B5. `patches/scopehal/0002-VulkanInitFromExisting.patch`** — adds
`VulkanInitFromExisting(vk::Instance, vk::PhysicalDevice, vk::Device, uint32_t queueFamily)`
which populates the same globals from caller-supplied objects rather than creating them, so an
external ImGui application can pass its Vulkan environment down. Tracked on the existing
`imcufosphor/…` submodule branch, exported with `format-patch`, commit message written for
upstream, re-apply check after every submodule bump — the rules `patches/README.md` already
sets. The no-patch path (B4.1) remains supported and is what `sigmf-spectrum` uses.

**B6. `embedcheck`** — a test executable that plays the embedder by hand: `VulkanInit(false)`,
its own `glfwCreateWindow`, `ImGui::CreateContext`, `ImGui_ImplGlfw_InitForVulkan`,
`ImGui_ImplVulkan_Init` with its own descriptor pool, its own swapchain and render pass, then
`Adopt()` and one `AnalyzerPane` frame over a synthetic `SpectrumEngine`. Windowing gated
behind `--window` the way `LinkCheck.cpp` already does, so it runs on a headless build machine.
Until this exists the embed contract is a document, not a tested capability.

---

## Phase C — ingest: the library provides the destination

**C1. `IqInjector` gains a write-into API.**

```cpp
std::span<std::byte> BeginBlock(size_t nsamples, PackedIQFormat fmt, float scale);
void CommitBlock(size_t nsamples, double sampleRateHz, double centerHz);
void AbortBlock();
```

`BeginBlock` resizes the reused `PackedIQWaveform` if needed and returns its pinned CPU
pointer; the producer writes the shader-layout bytes straight in. The two existing
`SetSamples()` overloads become three-line wrappers (`BeginBlock` + `memcpy` + `CommitBlock`),
so `SigMFSource`, `Verify.cpp` and the tests are untouched.

**C2. A pinned block ring, replacing `IqRing`'s storage.** `BeginBlock` must be callable from
the GR4 scheduler thread while the render thread submits, so `IqInjector` grows from one
waveform to K (default 3) with an SPSC free/ready handoff — the same lock-free discipline
`IqRing` already uses, moved down a layer so there is one ring instead of two.
`gr::CircularBuffer`'s double-mapped reader span is no longer load-bearing, since the writer
is now copying *out of* the port span rather than into a second ring.

Cycling K waveforms through `ComplexChannel` must not free them: use
`InstrumentChannel::Detach(0)` (`InstrumentChannel.h:257`) before `SetData(next, 0)`, because
`SetData` deletes any previous waveform that is not the same pointer
(`InstrumentChannel.cpp:143-151`). The ring owns all K.

**C3. Rewire the three sinks.** `processBulk` does one `memcpy` from the GR4 input port span
into `BeginBlock()`'s pinned span, tracking fill level across calls, and publishes when a block
completes. `detail/IqRing.hpp` keeps its drop/backpressure accounting and its counters — that
policy is the reason it exists — but loses `gr::CircularBuffer` and the `std::copy_n` at
`:98`/`:127`. Net: **the GR4 path goes from three host-visible moves to one**, matching the
file path. `gr4-bench/main.cpp:117`'s per-element `_accum.push_back` loop is deleted; it
duplicates the same job.

Threading note to preserve: `SpectrumEngine`'s "render thread only" contract
(`SpectrumEngine.h:104-110`) is unchanged. `BeginBlock`/`CommitBlock` are explicitly the
*writer*-thread half and must be documented as such at every entry point, since this is the
one thing most likely to be got wrong.

**C4. True zero copy — designed, scoped, not landed in this phase.** Two routes, both needing
`AcceleratorBuffer` to adopt memory it did not allocate (`AcceleratorBuffer.h:866-1010` has no
such constructor):
- *Cast the port buffer in place*: wrap the GR4 reader span in a `PackedIQWaveform` view and
  import its pages with `VK_EXT_external_memory_host`. Needs the pages to meet
  `minImportedHostPointerAlignment` (typically 4096) — a double-mapped circular buffer plausibly
  does, but gnuradio4 does not allocate with import in mind, so this likely needs a custom
  buffer allocator on the port.
- *Hand the ring's pinned blocks to GNU Radio as the port buffer*, i.e. the inverse.

Both are a scopehal patch plus a gnuradio4-side question. Write the investigation up as a
DESIGN.md section with the alignment and format constraints stated; do not block Phase C on it.

**C5. One measured experiment, kept behind a flag.** `UniformWaveform` hardcodes both access
hints to `HINT_LIKELY` (`Waveform.h:417-418`), which forces a mirrored VRAM allocation and a
4 MB/block DMA. Setting the packed input buffer to `HINT_UNLIKELY` + `MEM_TYPE_CPU_DMA_CAPABLE`
makes `AcceleratorBuffer` skip the device buffer entirely (`AcceleratorBuffer.h:963`) and the
shader read pinned memory over PCIe. The window shader reads each word exactly once, so this
*might* win; it also might lose on a discrete GPU. Add it as a `wfbench` A/B flag alongside
`--planar`, measure, and record the number in DESIGN.md §13. **Default stays as it is** — the
DMA is the right call until a measurement says otherwise.

---

## Phase D — parameters that name physical quantities

**D1. The reducer emits at exact group boundaries.** Today `SpectrumReducer::Refresh()` folds
every spectrum in a block and tests `m_accumulated >= k` (`SpectrumReducer.cpp:183-187`), so a
row is `ceil(k/S)·B` samples. Change it to consume the block in group-sized pieces: fill the
current group to exactly `k`, emit, carry the remainder, repeat. The caller
(`PlayerSession::StepOneBlock`, `SpectrumEngine::StepAfterPush`) loops while a group completed,
recording a reducer-finalize + `Waterfall::Refresh()` pair into the *same* command buffer each
time, so a block still costs one submit.

Cost is bounded and small in the configuration that matters: at 245.76 MS/s, 8192 points and 20
rows/s, `k` ≈ 1500 against `S` = 128, so most blocks emit zero rows and a few emit one. The
pathological case (`k` < `S`, e.g. `k`=1 → 128 waterfall bumps per block) is exactly the
behaviour the user asked for and is unreachable once `k` is derived from a row duration.
`GetEffectiveGroupSize()` becomes equal to `GetGroupSize()` and its "a row can represent more
spectra than asked for" caveat (`SpectrumReducer.h:52-59`) is deleted.

**D2. Row duration becomes the user parameter.** `EngineConfig` (`SpectrumEngine.h:48-80`) gains
`double rowsPerSecond` alongside — and in preference to — `groupSize`:

```
k = round(fs / (N · rowsPerSecond))            clamped to ≥ 1
row duration = k·N / fs                        exact, independent of block size
```

Zero or absent `sampleRate` keeps `groupSize` authoritative, which is the existing behaviour and
what the headless tools want. `PlayerSession` gets the same pair. With D1 in place, block size no
longer appears in either formula.

**D3. UI.** `MainWindow.cpp:200-251` and `AnalyzerSink.hpp:485-534` get the same treatment:
- **RBW** ↔ **FFT length** as a linked pair (RBW = fs/N, both editable, N snaps to a power of two).
- **Rows/sec** as the waterfall knob, with the derived `k` and row duration shown read-only.
  The current `SliderInt("Spectra/row", …, 1, 65536)` (`MainWindow.cpp:235`) and its 4096-capped
  GR4 twin (`AnalyzerSink.hpp:494`) move under an "Advanced" collapsing header.
- **Block size** moves to Advanced, relabelled as what it is — a throughput knob — with the
  derived transforms-per-block shown. Default it from the sample rate rather than the fixed
  1 Msample, keeping 1 Msample as the measured optimum (DESIGN.md §13) for high rates.
  Expose it in the GR4 sink's panel too; it currently has no widget at all.
- **ms/frame** moves to Advanced and is relabelled. It is a per-frame wall-clock CPU budget for
  the drain loop (`MainWindow.cpp:108-117`, `AnalyzerSink.hpp:585`) — it controls how fast
  playback advances in wall-clock terms and touches nothing in the data interpretation. The
  honest user-facing form of it is a playback-speed control (×realtime, or "as fast as
  possible"); add that on the file path where the concept exists, and leave the raw budget as
  the advanced escape hatch. On the GR4 path the rate is whatever the flowgraph delivers, so
  only the budget is meaningful there.

**D4. Note the one remaining honest limitation.** The waterfall does not resample vertically,
so immediately after a rows/sec change the rows already in the ring were produced at the old
quantum and `RowHistory::GetStreamSpan` (`RowHistory.h:165-179`) divides a mixed total linearly
across pixels. D1 removes the *block-size* source of this; the rows/sec source remains. Either
clear the waterfall on a rate change, or record the quantum per row and let the axis integrate
— decide when implementing, and record the choice in DESIGN.md §16's "still open" list.

---

## Verification

Per phase, and all of it before calling any phase done:

```bash
# both configurations, every step
cmake --build build -j$(nproc)
source /path/to/gnuradio4/build/activate.sh
cmake --build build-gr4 -j$(nproc) && ctest --test-dir build-gr4 --output-on-failure
```

- **Phase A**: `ctest` green; `ldd`/`cmake --graphviz` shows `qa_IqLayout` linking neither
  libsigmf nor imgui nor X11, and `gr4-bench` linking no GLFW/`VulkanWindow`. The two new link
  checks and `render-headers-selfcheck` build.
- **Phase B**: `embedcheck --window` renders one pane frame against a hand-built embedder;
  `sigmf-spectrum` and `gr4-analyzer` are visually unchanged. Re-apply check for the new
  scopehal patch documented in `patches/README.md`.
- **Phase C**: `wfbench --file …/lora_uninverted.sigmf-meta --block 1M --verify` passes every
  check; throughput on the file path is unchanged (this phase does not touch it) and the GR4
  path is measured with `gr4-bench` before and after. Record both in DESIGN.md §13 — the point
  of that section is that each change is justified by a number.
- **Phase D**: with `sigmf-spectrum` on a demo recording, set rows/sec to 20 and confirm the
  waterfall time axis reads the same span at block sizes of 8192, 64 k and 1 Msample — the
  regression this phase exists to fix. Confirm the derived `k` and row duration match
  `k·N/fs`. `wfbench --survey /data/deepsig/datasets/demo/*.sigmf-meta` still passes.

## Files that carry the change

`src/imcufosphor/CMakeLists.txt`, `src/ngscopeclient-compat/CMakeLists.txt` (target split);
`RenderHost.{h,cpp}` → `RenderContext.{h,cpp}`, new `src/imcufosphor/host/AppWindow.{h,cpp}`,
new `src/ngscopeclient-compat/TextureCompat.h`; `AnnotationOverlay.h`, `SpectrumArea.h`,
`WaterfallArea.h`, `PlotAxis.h` (include cuts); `IqInjector.{h,cpp}`, `SpectrumEngine.{h,cpp}`,
`PlayerSession.{h,cpp}` (ingest + rate); `SpectrumReducer.{h,cpp}` (exact groups);
`MainWindow.cpp` (UI); `gr-imcufosphor/blocks/display/include/gnuradio-4.0/imcufosphor/`
(`AnalyzerSink.hpp`, `SpectrumSink.hpp`, `WaterfallSink.hpp`, `detail/IqRing.hpp`,
`detail/StreamStatus.hpp`), `gr-imcufosphor/apps/*`; `patches/` and `DESIGN.md` throughout.

DESIGN.md needs new sections for the target hierarchy and the embedder contract, plus updates
to §5 (the compat boundary is now four targets), §7.3 and §8.1 (the reducer's block-boundary
caveat is gone), §13 (new measurements), §16 (the axis limitation), and §17 (the parameter set).
