# GNU Radio 4 sink blocks for the imcufosphor display

Plan for exposing the Waterfall, Spectrum and combined Analyzer displays as GR4 blocks.

Everything below is grounded in what was actually read on 2026-08-16. Sources:

| Tree | Commit / path | Role |
|------|---------------|------|
| `/data/dkozel/src/gr4-incubator` | working tree | block/example conventions, CMake, GUI example |
| `/data/dkozel/src/gr4/gr4-dev/install/include/gnuradio-4.0` | installed headers, `gnuradio4.pc` at `.../install/lib/pkgconfig/` | **the API the incubator actually compiles against** |
| `/data/dkozel/src/gr4/gr4-dev/src/gnuradio4` | `4661f02`, 2026-03-13 | source for the installed headers |
| `/data/dkozel/src/gnuradio4` | `c946b14`, 2026-04-28 | newer checkout; source of `docs/USER_API_Drawable_UI.md` |
| `/data/dkozel/src/projects/imcufosphor` | working tree, `d56ccb4` | the display |

Two gnuradio4 checkouts exist and they differ (`Block.hpp` is not identical). Nothing in this
plan depends on a difference between them, but **pin one before writing code** — see Risks.

---

## 1. The GR4 block/sink API as observed

### 1.1 Declaration

CRTP over `gr::Block<Derived, Args...>`, reflection via `GR_MAKE_REFLECTABLE`. Not
`ENABLE_REFLECTION_FOR_TEMPLATE` — that variant is gone in this version.

Canonical minimal sink, `blocks/measure/include/gnuradio-4.0/measure/HistogramSink.hpp:17-85`:

```cpp
template<typename T>
struct HistogramSink : Block<HistogramSink<T>> {
    using Description = Doc<"Amplitude/phase histogram sink...">;

    PortIn<std::complex<T>> in;

    Annotated<uint32_t, "n_bins", Visible, Doc<"Number of histogram bins">> n_bins  = 64u;
    Annotated<T, "min_val", Visible, Doc<"Lower edge of histogram range">>  min_val = T(-2);

    GR_MAKE_REFLECTABLE(HistogramSink, in, n_bins, min_val, max_val, mode);

    void start() { _rebuild(); }
    void settingsChanged(const property_map&, const property_map&) noexcept { _rebuild(); }
    void processOne(std::complex<T> x) noexcept { ... }
};
GR_REGISTER_BLOCK("gr::incubator::measure::HistogramSink",
                  gr::incubator::measure::HistogramSink, ([T]), [ float, double ])
```

A sink is simply a block with only `PortIn` members. There is no `Sink` base class.

The `A<>` shorthand is idiomatic in this repo
(`blocks/audio/.../RtAudioSink.hpp:51-52`, `testing/ImChartMonitor.hpp:28-29`):

```cpp
template<typename U, gr::meta::fixed_string description = "", typename... Arguments>
using A = gr::Annotated<U, description, Arguments...>;
```

### 1.2 `processBulk` for sinks

```cpp
[[nodiscard]] constexpr work::Status processBulk(InputSpanLike auto& dataIn) noexcept;
```

`dataIn` is span-like with `.size()`, `.data()`, `operator[]`, `.tags()` and an explicit
`.consume(n)`. If you do not call `consume`, the whole span is consumed by default (see
`ImChartMonitor::processBulk`, which never consumes explicitly). `RtAudioSink` consumes
only what it managed to push, which is how it creates backpressure
(`RtAudioSink.hpp:157-162`).

`dataIn.tags()` yields `(relIndex, reference_wrapper<const property_map>)` pairs
(`ImChartMonitor.hpp:108-115`). `this->inputTagsPresent()` / `this->mergedInputTag()` give the
merged tag at index 0 (`DataSink.hpp:600-604`).

### 1.3 Registration

`GR_REGISTER_BLOCK` is a **no-op marker macro** (`BlockRegistry.hpp:36`) consumed by a
`parse_registrations` codegen step during the plugin build. gr4-incubator's CMake policy is
`ENABLE_PLUGINS=OFF` and the README says `ENABLE_PLUGINS=ON` is an error. For a
statically-linked application you include the header and call `graph.emplaceBlock<Block<T>>(...)`
directly — no registration involved. `gr::globalBlockRegistry()` exists
(`BlockRegistry.hpp:156`) but is only needed for name-based/YAML construction.

**Conclusion: keep the `GR_REGISTER_BLOCK` marker for future plugin/YAML use, but do not
depend on it.**

### 1.4 Lifecycle

`LifeCycle.hpp:74` — `IDLE, INITIALISED, RUNNING, REQUESTED_PAUSE, PAUSED, REQUESTED_STOP,
STOPPED, ERROR`. `LifeCycle.hpp:212-240` dispatches, if the derived class defines them:

| Hook | Fires on |
|------|----------|
| `init()` | IDLE → INITIALISED |
| `start()` | INITIALISED → RUNNING |
| `stop()` | → REQUESTED_STOP |
| `pause()` | → REQUESTED_PAUSE |
| `resume()` | (REQUESTED_PAUSE\|PAUSED) → RUNNING |
| `reset()` | (not IDLE) → INITIALISED |
| `settingsChanged(const property_map& old, property_map& newSettings)` | any staged settings apply |

`lifecycle::isShuttingDown(this->state())` is the idiom for a `draw()` to report `DONE`
(`ImChartMonitor.hpp:162,175`).

Settings are staged from another thread with
`blockModel->settings().setStaged(property_map{...})` and read back with
`settings().get()` — see `examples/fm_demodulator_imgui.cpp:388-421`. That is the supported
UI→flowgraph control path.

### 1.5 The `Drawable` UI interface — this is the important one

GR4 has a first-class UI-block mechanism. `annotated.hpp:181-218`:

```cpp
enum class UICategory { None, MenuBar, Toolbar, StatusBar, Content, Panel,
                        Overlay, ContextMenu, Dialog, Notification };

template<UICategory category_, gr::meta::fixed_string toolkit_ = "">
struct Drawable { static constexpr UICategory kCategory = category_;
                  static constexpr gr::meta::fixed_string kToolkit = toolkit_; };
```

A block annotated `Block<Derived, Drawable<UICategory::Content, "ImGui">>` **must** implement
`work::Status draw(const property_map& config = {})` — `Block.hpp:2017-2022` static_asserts
otherwise. `BlockModel` exposes `virtual work::Status draw(const property_map&)`
(`BlockModel.hpp:475`, dispatch at `:860-862`) and `virtual UICategory uiCategory() const`
(`BlockModel.hpp:481`, override at `:869`), so a render loop can walk `sched.blocks()` and
call `draw()` on every `UICategory::Content` block without knowing its type.

The reference implementation is `gnuradio-4.0/testing/ImChartMonitor.hpp` (console toolkit):
`processBulk` writes into `HistoryBuffer`s under `std::mutex _drawMutex`
(`:103-146`), `draw()` takes the same lock and renders (`:155-176`), with a `timeout_ms`
rate limiter so `draw()` cheaply no-ops when called faster than the display rate.

`docs/USER_API_Drawable_UI.md` (in `/data/dkozel/src/gnuradio4`) is the full spec. Its
threading section states explicitly that sync between `process*()` and `draw()` is
**application responsibility**; the framework imposes nothing.

### 1.6 Existing ImGui work in gr4-incubator

There is exactly one: `examples/fm_demodulator_imgui.cpp` (476 lines). It is **not** a
Drawable block. Its architecture:

- ImGui/ImPlot/GLFW/**OpenGL 3.3** on the main thread; `ImGui_ImplGlfw_InitForOpenGL` +
  `ImGui_ImplOpenGL3_Init("#version 330")` (`:325-336`).
- Scheduler on a `std::jthread` running `sched.runAndWait()` (`:353-358`).
- Data crosses via a plain `gr::basic::DataSink<float>` in the graph plus
  `globalDataSinkRegistry().getStreamingPoller<float>(DataSinkQuery::signalName("audio"), cfg)`
  with `PollerConfig{.overflowPolicy = Drop, .minRequiredSamples = 64, .maxRequiredSamples = 1024}`
  (`:338-344`). The render loop calls `poller->process([&](std::span<const float>){...})` once
  per frame (`:366-380`).
- Shutdown: `sched.changeStateTo(lifecycle::State::REQUESTED_STOP)` then join (`:465-466`).

`StreamingPoller` (`basic/DataSink.hpp:76-124`) is a `gr::CircularBuffer<T>` (lock-free SPSC)
plus a parallel `gr::CircularBuffer<Tag>`; `process()` optionally hands you
`(std::span<const T>, std::span<const Tag>)` with tag indices rebased to the span.

There are **no** ImPlot-based blocks, no `chart`/`plot`/`dashboard` blocks, no Vulkan anywhere
in gr4-incubator. imgui 1.92.5 and implot 0.17 exist under `subprojects/` (meson leftovers);
the CMake path finds them as system packages and gates on `ENABLE_GUI_EXAMPLES` +
`GR4I_GUI_READY` (`examples/CMakeLists.txt:33-58`, `cmake/Dependencies.cmake:194+`).

### 1.7 Tag / metadata conventions for a spectrum display

`Tag.hpp:196-215` defines the default tag set. Relevant:

- `sample_rate` — `float`, Hz. `gr::tag::SAMPLE_RATE`, aliased as `gr::tag::SIGNAL_RATE`
  (same key string; they are the same tag).
- `signal_name`, `signal_unit`, `signal_min`, `signal_max`, `n_dropped_samples`,
  `trigger_time` (uint64 ns), `trigger_meta_info` (nested `property_map`).

**There is no `center_frequency` tag in gr4.** `SoapyRx` has `center_frequency` only as a
*block setting* (`blocks/soapysdr/.../SoapyRx.hpp:36,46,86-88`), never published as a tag.
`SigMfSource` folds all SigMF global/capture metadata into `tag::TRIGGER_META_INFO` as a nested
map keyed by the raw SigMF names (`blocks/sigmf/.../detail/SigMfTagSchedule.hpp:32-51`), so
center frequency reaches downstream as `trigger_meta_info["core:frequency"]`.

Consequence for us: we must define the convention. See §4.4.

### 1.8 CMake shape of a block module

Per `blocks/sigmf/CMakeLists.txt` and `blocks/measure/CMakeLists.txt`, a module is:

```cmake
add_library(gr4_incubator_blocks_X_headers INTERFACE)
add_library(gr4_incubator::blocks_X_headers ALIAS gr4_incubator_blocks_X_headers)
target_include_directories(... INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> ...)
target_link_libraries(... INTERFACE ${GR4I_GNURADIO4_TARGET} ...)
install(DIRECTORY include/gnuradio-4.0/X DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gnuradio-4.0)
if(ENABLE_PLUGINS) gr4_incubator_add_block_plugin(...) endif()
if(ENABLE_TESTING) add_subdirectory(test) endif()
```

Header-only INTERFACE libraries throughout, registered in `blocks/CMakeLists.txt` and
aggregated into `gr4_incubator::blocks_headers`. Tests are Boost.UT suites
(`blocks/measure/test/qa_HistogramSink.cpp`) linking `${GR4I_GNURADIO4_TARGET}` and
`${GR4I_BOOST_UT_TARGET}`, registered with `gr4_incubator_add_ut_test()`
(`blocks/CMakeLists.txt:1-5`). Many tests drive the block directly (`sink.processOne(...)`)
without a graph; `gr::testing` proper is used only in gnuradio4's own tests.

gnuradio4 is consumed via **pkg-config**, not a CMake package
(`cmake/Dependencies.cmake:31-37`), and the tree compiles at **C++23** with a forced
`-include cmake/gr4_incubator_stdfloat_compat.hpp` shim.

---

## 2. What imcufosphor actually is, and what does not travel

Read: `DESIGN.md` (1170 lines), `src/imcufosphor/*.h`, `main.cpp`, `src/imcufosphor/CMakeLists.txt`.

### 2.1 The pipeline

```
SigMFSource (Oscilloscope) ─► [I, Q, center] ─► ComplexFFTFilter ─┬─► SpectrumDensity ─► SpectrumArea
                                                                   └─► SpectrumReducer ─► Waterfall ─► WaterfallArea
                                                                                     both ─► AnalyzerPane
```

Driven by `PlayerSession::StepOneBlock()` / `StepOneRow()`, which records one command buffer
per block and does one `SubmitAndBlock()`.

### 2.2 Correction to the brief

**It is Vulkan, not CUDA.** There is no CUDA or cuFFT anywhere in the tree. `ComplexFFTFilter`
is a batched **VkFFT** complex-to-complex plan
(`-DVKFFT_BACKEND=0`, `src/imcufosphor/CMakeLists.txt:41`) plus scopehal `ComputePipeline`
dispatches of SPIR-V shaders. "imcufosphor" is a name, not a backend. Every plan decision
below follows from that.

### 2.3 Data interfaces into the two areas

Neither area takes a buffer. Both take a **pointer to a scopehal `Filter`** and read its
output waveforms on the GPU:

- `WaterfallArea(Waterfall* waterfall, TextureManager*, const std::string& colorRamp)`
  (`WaterfallArea.h:66`). `ToneMap(vk::raii::CommandBuffer&)` dispatches
  `WaterfallToneMap.spv` over the filter's fp32 ring buffer into a `Texture`;
  `Render(ImVec2)` draws that texture with ImGui. `UpdateSize()` resizes the texture *and*
  the `Waterfall` filter's row count. Needs `SetTimebase(const RowHistory*, double sampleRate)`
  because upstream records no per-row timestamps (DESIGN §8.2).
- `SpectrumArea(SpectrumDensity* density, TextureManager*, const std::string& colorRamp)`
  (`SpectrumArea.h:88`). Two layers: the density map tone-mapped through
  `SpectrumDensityToneMap.spv` into a texture only as tall as the histogram has cells, and up
  to 5 percentile/mean traces rasterized through the **lifted ngscopeclient analog rasterizer**
  (`waveform-compute.analog[.int64].dense.spv`) into `AcceleratorBuffer<float>` rasters, then
  composited by `SpectrumTraceToneMap.spv` into one texture.
- `AnalyzerPane(PlayerSession*, TextureManager*, colorRamp)` owns a
  `std::shared_ptr<PlotAxis>` handed to both areas, so linkage is by construction; each area
  owns its own Y axis.

Buffer formats/dimensions that matter:

- `ComplexFFTFilter` output: one `UniformAnalogWaveform` holding `floor(N/L)` back-to-back
  spectra of `L` bins each, **dBm into 50 Ω**, fftshifted, x-axis in **µHz** (because
  `Waterfall` constrains its input to `UNIT_MICROHZ`), `m_triggerPhase` = absolute frequency
  of bin 0 (int64 µHz — the int64/float split is load-bearing, `DESIGN.md:1032`).
- `SpectrumDensity` outputs 6 streams: stream 0 a `DensityFunctionWaveform`
  (`nbins × densityCells`), streams 1-5 analog dBm traces. `IsOutputReady()` is only true on
  a fold boundary, i.e. it does **not** produce output every `Refresh()`.

### 2.4 Update cadence, ownership, threading

**Single-threaded.** `main.cpp:87-99` is `glfwPollEvents(); window.Render();` on the main
thread. `MainWindow::Render()` steps playback (up to `m_stepBudgetMs` of wall clock), records
tone-map compute into its own `m_cmdBuf` **outside the render pass**, then calls
`VulkanWindow::Render()`. There is no worker thread, no queue handoff, no locking anywhere in
the display path.

### 2.5 What is awkward to carry into a GR4 block

| Thing | Problem |
|-------|---------|
| `SigMFSource` (`Oscilloscope` subclass, 1039+388 lines) | A GR4 source replaces it entirely. Dead weight. |
| `PlayerSession` | Its whole job — acquire, schedule K FFTs per row, submit — is what the GR4 scheduler + `processBulk` replace. But `RowHistory` and the group-size scheduling logic must survive somewhere. |
| `Filter::SetInput(StreamDescriptor)` | scopehal filters need an upstream *channel object* holding a `WaveformBase`. A GR4 sink has a `std::span`. Needs a small no-input injector filter. |
| `AnalyzerPane(PlayerSession*)` | Constructor-coupled to the session. Must be re-cut against an engine interface. |
| Vulkan globals | `g_vkComputeDevice`, `g_vkQueueManager`, `VulkanInit(false)`, `ScopehalStaticCleanup()`, `AddDecoderClass()`, `PipelineCacheManager`. All process-global, all order-sensitive (`DESIGN.md:1160-1170` records a real teardown fault). A block constructed by `emplaceBlock` cannot assume any of this has run. |
| `TextureManager`, `VulkanWindow` | App-level singletons owned by `MainWindow`. Blocks must borrow, never own. |
| Shader discovery | `FindDataFile` searches the *binary's* directory + install prefixes; `cmake/CollectShaders.cmake` POST_BUILD-globs every `.spv` next to the executable. Any new executable needs the same step (`DESIGN.md:466-480`). |
| **C++17 vs C++23** | imcufosphor sets `CMAKE_CXX_STANDARD 17` (`CMakeLists.txt:49`). gnuradio4 needs C++23. See §7 and Risks. |

---

## 3. Threading & data handoff — recommendation

### Recommendation

**Drawable blocks. `processBulk` on the scheduler thread writes complex IQ into a lock-free
`gr::CircularBuffer<std::complex<float>>`; `draw()`, called from the application's render
thread, drains it, runs the Vulkan compute, and issues the ImGui calls.**

Concretely, per block:

```
scheduler thread                          render thread (main)
────────────────                          ────────────────────
processBulk(dataIn)                       draw(config)
  scan tags → _pendingMeta (atomic)         if (now - _last < 1000/max_fps) return OK;
  writer.reserve(n) / publish(n)            snapshot _pendingMeta
  consume(n)   [Drop policy: consume         reader.get(k)  →  engine.Push(span)
                even when full]              engine.Dispatch(cmdBuf)  // outside render pass
                                             area.Render(size)        // ImGui
                                             reader.consume(k)
```

### Rationale

1. **Drawable is the GR4-native answer and it already has the plumbing.**
   `BlockModel::uiCategory()`/`draw()` let the host loop discover and render UI blocks
   generically. Settings (`fft_size`, `db_range`, colour map) then live in exactly one place —
   the block's `Annotated<>` members — reachable from a GRC-style editor, from
   `settings().setStaged()`, and from the ImGui panel, with no parallel config object. The
   `DataSink` + `StreamingPoller` route used by `fm_demodulator_imgui.cpp` cannot do this: the
   display would not be a block at all, just app code next to a generic sink.

2. **`gr::CircularBuffer<T>` over a mutex FIFO.** `RtAudioSink` uses `std::mutex` + a hand-rolled
   ring (`RtAudioSink.hpp:73-94`) and `ImChartMonitor` uses `std::mutex` + `HistoryBuffer`.
   Both are fine at audio rates. We are pushing multi-MS/s complex IQ, and a lock held across
   a `memcpy` of a megasample block will show up as scheduler jitter. `gr::CircularBuffer` is
   the lock-free SPSC primitive `StreamingPoller` itself is built on
   (`DataSink.hpp:77-80`), it is already a dependency, and its `ReaderSpanLike`/`reserve`
   API is the same shape as a port's.

3. **Drop, never backpressure.** The GUI must not stall the flowgraph — that is exactly why
   `PollerConfig::overflowPolicy = Drop` exists and is what the incubator's own GUI example
   selects (`fm_demodulator_imgui.cpp:340`). So `processBulk` always consumes its full span and
   silently discards what will not fit, incrementing a `std::atomic<size_t> _dropped` that the
   UI displays. A headless flowgraph (nobody ever calls `draw()`) then runs at full speed with
   the sink as a bit bucket, which is the required behaviour.

4. **Rate limiting in `draw()`, exactly as `ImChartMonitor` does** (`timeout_ms`,
   `ImChartMonitor.hpp:159-165`). Additionally set `in.max_samples` in
   `settingsChanged`/`start()` so the scheduler hands us blocks sized to the FFT batch, the
   same trick at `ImChartMonitor.hpp:84,92`.

5. **Vulkan resources are created lazily on first `draw()`, never in the constructor or
   `start()`.** `emplaceBlock` may run before `VulkanInit`, and `start()` runs on a scheduler
   thread. All `ComputePipeline`/`Texture`/`AcceleratorBuffer` construction must happen on the
   render thread, guarded by a `bool _initialised`. `stop()` must **not** destroy them
   (wrong thread); destruction happens in the render loop's teardown, before
   `ScopehalStaticCleanup()`.

### What is explicitly rejected

- **Double buffering of finished spectra.** Tempting (hand the renderer a completed dB
  spectrum), but it forces the FFT onto the scheduler thread, which means Vulkan submissions
  from the scheduler thread and a second queue + explicit sync with the render queue. The
  existing design has exactly one thread touching Vulkan; keep it.
- **`DataSink` + `StreamingPoller`.** Correct and proven in-tree, but yields app code, not
  blocks. Worth keeping as the *fallback* if Drawable turns out to be under-supported (see
  Risks) — the buffer type is the same, so switching is a small change.

---

## 4. Block designs

Namespace `gr::imcufosphor`. Header-per-block under
`src/gr4blocks/include/gnuradio-4.0/imcufosphor/`.

### 4.1 Common shape

```cpp
template<typename T = float>
requires std::is_floating_point_v<T>
struct AnalyzerSink : gr::Block<AnalyzerSink<T>,
                                gr::Drawable<gr::UICategory::Content, "ImGui">> {
    template<typename U, gr::meta::fixed_string d = "", typename... A_>
    using A = gr::Annotated<U, d, A_...>;

    gr::PortIn<std::complex<T>> in;
    ...
    GR_MAKE_REFLECTABLE(AnalyzerSink, in, /* settings */);

    void start();                                     // sizing only, no GPU
    void stop();                                      // flag shutdown, no GPU
    void settingsChanged(const property_map& old, property_map& now);
    work::Status processBulk(InputSpanLike auto& dataIn) noexcept;
    work::Status draw(const property_map& config = {}) noexcept;
};
```

`T` is the **real scalar**; the port is `std::complex<T>`. Instantiate for `float` only in
phase 1 (`ComplexFFTFilter`/VkFFT are fp32 throughout; a `double` instantiation would silently
downconvert). Add `double` later behind a narrowing conversion in `processBulk` if wanted.

### 4.2 Settings

| Setting | Type | Default | Notes |
|---|---|---|---|
| `fft_size` | `gr::Size_t` | 8192 | `main.cpp:32`. 0 ⇒ one transform per block. |
| `sample_rate` | `float`, `Unit<"Hz">` | 1.f | Fallback; overridden by the `sample_rate` tag unless `ignore_tag_sample_rate`. Matches `DataSink`/`RtAudioSink` naming. |
| `center_frequency` | `double`, `Unit<"Hz">` | 0.0 | Fallback; see §4.4. `double` because `float` cannot hold a GHz carrier to sub-Hz. |
| `window` | `std::string` | `"blackman-harris"` | Maps to `FFTFilter::WindowFunction`. |
| `ignore_tag_sample_rate` | `bool` | false | Same escape hatch as `RtAudioSink.hpp:64`. |
| `max_frame_rate` | `float`, Hz | 30.f | `draw()` rate limit (`timeout_ms` equivalent). |
| `buffer_depth` | `gr::Size_t` | 4 × `fft_size` × batch | Handoff ring capacity in samples. |
| **Spectrum / Analyzer** | | | |
| `db_min`, `db_max` | `float` | -120, 0 | → `SpectrumDensity::SetRange`. |
| `histogram_cells` | `gr::Size_t` | 1024 | → `SetCellCounts` (must be a whole multiple of `density_cells`). |
| `density_cells` | `gr::Size_t` | 128 | gr-fosphor's number. |
| `persist_rise_s`, `persist_decay_s` | `float` | | → `SetPersistence`. |
| `percentile_low/mid/high` | `float` 0-1 | .1/.5/.9 | → `SetPercentiles`. |
| `density_visible`, `trace_mask` | `bool`, `uint32_t` | true, 0x1f | Layer toggles (`SpectrumArea::SetTraceVisible`). |
| `trace_alpha`, `density_floor`, `density_gain` | `float` | | Display tuning, already `SpectrumArea` setters. |
| **Waterfall / Analyzer** | | | |
| `history_rows` | `gr::Size_t` | 0 = pane height | `Waterfall` row count; `WaterfallArea::UpdateSize` normally sets it from the pane. |
| `group_size` | `gr::Size_t` | 1 | Spectra per waterfall row → `SpectrumReducer` (DESIGN §7.3/§8.1). |
| `reducer_mode` | `std::string` | `"max"` | max / average / min-max envelope. |
| `color_map` | `std::string` | `"eLYRE"` (whatever the current default ramp is) | Passed to the area constructors; the ramp is a `TextureManager` PNG. |
| **Analyzer only** | | | |
| `spectrum_fraction` | `float` | 0.4 | `AnalyzerPane::SetSpectrumFraction`. |

Every one of these is already a setter on an existing class, so `settingsChanged` is a
dispatch table, not new logic. The two that require reallocation (`fft_size`,
`histogram_cells`/`density_cells`) must set a `_reconfigure` atomic flag and be applied at the
top of `draw()`, not in `settingsChanged` — `settingsChanged` runs on the scheduler thread and
must not touch Vulkan.

### 4.3 `processBulk`

```
1. Scan dataIn.tags() → sample_rate, center_frequency (§4.4). Store into an
   atomic-swapped small POD (_pendingMeta). If sample_rate changed, note it for the
   renderer to re-derive the frequency axis + persistence constants.
2. n = dataIn.size();
   k = min(n, writer.available());
   copy k complex samples into the ring; publish(k).
   _dropped += (n - k).
3. dataIn.consume(n);            // always the full span — never backpressure
4. return work::Status::OK;
```

`processOne` is deliberately not provided: per-sample locking/publishing is pointless here and
`ImChartMonitor`/`RtAudioSink` both use `processBulk`.

### 4.4 Tag handling — center frequency

There is no gr4 convention (§1.7). Proposal, in precedence order, implemented once in a shared
`detail/MetaFromTags.hpp`:

1. `tag["center_frequency"]` (double or float) — the key we introduce, matching `SoapyRx`'s
   *setting* name so that a future `SoapyRx` tag emission slots straight in.
2. `tag["trigger_meta_info"]["core:frequency"]` — SigMF interop, which is what
   `gr::incubator::sigmf::SigMfSource` actually produces today
   (`SigMfTagSchedule.hpp:48-51`).
3. The block's `center_frequency` setting.

Sample rate: `tag["sample_rate"]` (`gr::tag::SAMPLE_RATE`, float) → setting fallback, with the
`ignore_tag_sample_rate` escape hatch. Use a numeric coercion helper — `RtAudioSink::get_uint_`
(`:218-252`) shows how many types a tag value legitimately arrives as; write the `double`
equivalent.

Also honour `n_dropped_samples` by displaying it, and `end_of_stream` by freezing the display
rather than clearing it.

**Flag: proposing `center_frequency` as a `DefaultTag` upstream is the right long-term fix.**
Until then this is our convention and it needs documenting in the block's `Doc<>`.

### 4.5 The three blocks

| Block | Port | Engine parts | UI |
|---|---|---|---|
| `SpectrumSink<T>` | `PortIn<std::complex<T>>` | injector → `ComplexFFTFilter` → `SpectrumDensity` | `SpectrumArea`, owns its X axis |
| `WaterfallSink<T>` | `PortIn<std::complex<T>>` | injector → `ComplexFFTFilter` → `SpectrumReducer` → `Waterfall` | `WaterfallArea`, owns its X axis |
| `AnalyzerSink<T>` | `PortIn<std::complex<T>>` | all of the above off one FFT | `AnalyzerPane` (both areas, shared X axis) |

All three are `Drawable<UICategory::Content, "ImGui">`.

---

## 5. Analyzer vs. Waterfall vs. Spectrum — recommendation

**Recommendation: three blocks over one shared engine class and the existing area classes. Not
a third independent implementation, and not "Analyzer = two blocks in a container".**

Introduce `imcufosphor::SpectrumEngine` with a parts mask:

```cpp
enum class EnginePart : uint32_t { Density = 1, Waterfall = 2 };

class SpectrumEngine {
public:
    SpectrumEngine(EngineParts parts, const EngineConfig&);
    void Configure(const EngineConfig&);            // idempotent; reallocates as needed
    void Push(std::span<const std::complex<float>>, const StreamMeta&);  // enqueue IQ
    bool Step(vk::raii::CommandBuffer&);            // run graph over queued IQ, one submit
    SpectrumDensity* GetDensity();                  // null unless parts & Density
    Waterfall*       GetWaterfall();                // null unless parts & Waterfall
    const RowHistory& GetRowHistory() const;
    double GetSampleRate() const; double GetCenterFrequency() const;
};
```

This is `PlayerSession` minus `SigMFSource`, minus `StepOneRow`'s file-driven loop, plus the
parts mask. `RowHistory` moves here, which is where DESIGN §16 already says it belongs ("it
lives in `PlayerSession` … because that is where rows are produced").

Rationale:

- `AnalyzerPane` already exists and already solves the shared-axis problem *by construction*
  (`AnalyzerPane.h:19-36`, and DESIGN §16 records the "two owners fitting one shared axis" bug
  that motivated it). Reimplementing that inside a composite block would reintroduce it.
- Three independent blocks would each run their own FFT. At 8192-point transforms over
  megasample blocks that is the dominant cost (DESIGN §13: 1194 MS/s with everything on).
  Analyzer must be one block so the FFT is shared.
- The parts mask is one `if` per allocation site and avoids `SpectrumSink` paying for a
  `Waterfall` ring buffer it never draws.

**The alternative worth naming and rejecting:** a `SpectrumSink` + `WaterfallSink` pair whose
axes are linked through GR4 async ports (`PortOut<property_map, Async>` per
`USER_API_Drawable_UI.md`). Elegant on paper, but pan/zoom linkage over a flowgraph port is a
frame of latency and an update-order question, which is precisely what the shared
`std::shared_ptr<PlotAxis>` was built to eliminate.

---

## 6. FFT inside the block, or pre-FFT'd bins in?

**Recommendation: time-domain complex IQ in, FFT inside the block, reusing `ComplexFFTFilter`
unchanged.** Optionally add a `DataSet<float>`-input variant later; not in phase 1.

Why:

1. **Batching is the throughput story.** `ComplexFFTFilter` performs `floor(N/L)` transforms as
   a single batched VkFFT dispatch and emits them back-to-back in one waveform
   (`ComplexFFTFilter.h:93-110`). DESIGN §13 measures this as 95.8 → 1417 MS/s, a 14.8×
   improvement over per-transform dispatch. A pre-FFT'd input port throws that away: bins
   arrive over a host-side GR4 port, must be uploaded per spectrum, and the batch is gone.
2. **`SpectrumDensity` wants a whole block of spectra at once.** Its input contract is
   literally "one `ComplexFFTFilter` output waveform, holding a whole block of spectra back to
   back" with a `binsPerSpectrum` parameter (`SpectrumDensity.h:100-105`). Feeding it one
   spectrum at a time defeats the two-cadence design (accumulate per block, fold per window)
   that keeps the O(bins × cells) passes at display rate.
3. **The core `gr::blocks::fft::FFT` is the wrong shape.** `fourier/fft.hpp:33` is
   `Block<FFT<T,U,Algo>, Resampling<1024,1>>` emitting `DataSet<U>` — a different data model
   requiring `PortIn<DataSet<float>>`, per-DataSet host copies, and its own window/dB
   conventions.
4. **Calibration.** `ComplexFFTFilter` is dBm into 50 Ω with a documented, verified convention
   (unit-amplitude IQ tone ⇒ +10 dBm) and a known 0.1 dB window-gain quirk inherited from
   `FFTFilter` (DESIGN §7.2). Accepting arbitrary upstream magnitude bins means the display can
   no longer label its Y axis in dBm honestly.
5. **The fftshift + µHz axis + int64 trigger phase** are all produced by
   `ComplexToLogMagnitudeShifted.glsl` and consumed by `Waterfall` and the rasterizer. A
   pre-FFT'd path would have to reproduce all of it host-side.

Costs of this choice, stated plainly:

- The sink is heavyweight: it owns a VkFFT plan, 4-6 compute pipelines and several
  `AcceleratorBuffer`s. Two sinks in one graph = two FFTs. Mitigated by `AnalyzerSink` being
  one block, and by nothing forcing you to instantiate more than one.
- `fft_size` is a block setting rather than a graph-level resampling contract, so it cannot be
  negotiated with upstream blocks the way `Resampling<>` can.
- Users who already have a spectrum (e.g. from a hardware FFT) cannot use these blocks in
  phase 1. **Mitigation, phase 4:** a `SpectrumSink<gr::DataSet<float>>` specialization whose
  `processBulk` copies bins straight into the density accumulator's input buffer, skipping the
  FFT stage. The `SpectrumEngine` parts mask already gives the seam.

---

## 7. Reuse vs. extraction of the existing rendering code

### Reusable essentially as-is

`PlotAxis`, `RowHistory`, `WaterfallArea`, `SpectrumArea` — none of them reference
`PlayerSession`, `SigMFSource`, `MainWindow`, or any global other than the scopehal Vulkan
context. `WaterfallArea` takes a `Waterfall*`; `SpectrumArea` takes a `SpectrumDensity*`. Both
are already "deliberately thin" by design (their own doc comments say so). **No change needed.**

`ComplexFFTFilter`, `SpectrumReducer`, `SpectrumDensity` — plain scopehal `Filter`s. Reusable
unchanged once something supplies their input.

`ngscopeclient-compat` (`VulkanWindow`, `TextureManager`, `PreferenceManager`, the lifted
shaders) — reusable unchanged, but note the `ConfigPathShim.h` preference-path interpose and
the `GetContentScale()` reimplementation are still required.

### Needs extraction / rework

1. **`AnalyzerPane`** — constructor is `AnalyzerPane(PlayerSession*, ...)`. Re-cut to
   `AnalyzerPane(SpectrumEngine*, TextureManager*, colorRamp)`. It uses the session for
   `GetDensity()`, `GetWaterfall()`, row history and sample rate — all of which `SpectrumEngine`
   provides. Small, mechanical.
2. **`PlayerSession` → `SpectrumEngine`** (§5). Keep `PlayerSession` as-is for
   `sigmf-spectrum` and `wfbench`; implement `SpectrumEngine` as the new class and, once it is
   proven, reimplement `PlayerSession` on top of it. Do **not** fork the scheduling logic.
3. **New: `IqInjector`** — a `Filter` with zero inputs and the three output streams
   `ComplexFFTFilter` expects (I, Q, center), fed by `SetSamples(std::span<const std::complex<float>>, double centerHz, double sampleRate)`. This is the piece that replaces
   `SigMFSource` as the graph head. ~150 lines. Model on scopehal's `ImportFilter`s for the
   "filter that produces waveforms from outside the graph" pattern, and on
   `SigMFSource`'s existing `ComplexChannel` stream layout for the stream definitions.
   (`ComplexImportFilter` was rejected in DESIGN D1 for *file-loading* reasons — whole-file
   memory and no `center` stream. Neither objection applies to an injector.)
4. **Library split.** Today `imcufosphor-core` (engine, links `scopehal`+`scopeprotocols`+
   `libsigmf`) and `sigmf-spectrum` (app, owns the areas and `ngscopeclient-compat`). The
   areas need to be in a library, not the executable. Proposed:

   - `imcufosphor-core` — unchanged, minus nothing. Add `IqInjector` and `SpectrumEngine`.
     Drop nothing; `SigMFSource` stays for `sigmf-spectrum`.
   - **new** `imcufosphor-render` — `PlotAxis`, `SpectrumArea`, `WaterfallArea`,
     `AnalyzerPane`. Links `imcufosphor-core` + `ngscopeclient-compat`. `sigmf-spectrum`
     becomes `main.cpp` + `MainWindow.cpp` linking it.
   - **new** `imcufosphor-gr4` — the blocks. See §8/§9.

5. **`libsigmf` dependency leaks.** `imcufosphor-core` links `libsigmf` publicly because of
   `SigMFSource`. Once the GR4 path uses `gr::incubator::sigmf::SigMfSource` instead, that is
   duplicate machinery. Not urgent — `SigMFSource` is `PRIVATE` to `sigmf-spectrum`'s needs in
   practice — but consider splitting `SigMFSource.cpp` out of `imcufosphor-core` into the app
   so the GR4 blocks do not drag flatbuffers in.

### The hard constraint: C++17 vs C++23

`imcufosphor/CMakeLists.txt:49` sets `CMAKE_CXX_STANDARD 17`; gnuradio4 requires C++23 and
gr4-incubator additionally force-includes a `stdfloat` compat header.

Recommended resolution:

- `imcufosphor-core` / `imcufosphor-render` / `ngscopeclient-compat` keep C++17
  (`target_compile_features(... cxx_std_17)`).
- `imcufosphor-gr4` is built with `target_compile_features(... cxx_std_23)` and the same
  `-include` stdfloat shim.
- **The seam must be a header that pulls in no scopehal and no Vulkan.** Introduce
  `include/imcufosphor/RenderHandle.h`: opaque `class SpectrumEngineHandle;` /
  `class AnalyzerViewHandle;` with free functions taking `void*`-free plain types
  (`std::span<const std::complex<float>>`, `double`, `float`, `const char*`, a POD config
  struct). All scopehal/Vulkan/ImGui includes stay behind it in `.cpp` files compiled at
  C++17.
  This also means the block's `draw()` cannot call `ImGui::Begin` directly on ImGui symbols
  from a *different* ImGui build — see Risks.

Alternative, if the seam proves too painful: raise the whole tree to C++20 or C++23 and see
whether scopehal compiles. **Unverified** — worth a 30-minute spike before committing to the
PIMPL seam, because a clean C++23 build would delete a lot of this section.

---

## 8. Where the window and graphics context live

**One application-level context, owned by a host executable. Never per block.**

- New executable `gr4-analyzer` (`src/gr4app/`) does, in order:
  1. `g_log_sinks` setup, `VulkanInit(false)`, `TransportStaticInit/DriverStaticInit/
     ScopeProtocolStaticInit`, `AddDecoderClass(ComplexFFTFilter/SpectrumReducer/
     SpectrumDensity/IqInjector)`.
  2. Create `MainWindow`-equivalent (`VulkanWindow` subclass) + `TextureManager`.
  3. Publish both into a process-global `imcufosphor::RenderHost` (see below).
  4. Build the `gr::Graph`, `emplaceBlock` the sinks, `sched.exchange(std::move(fg))`.
  5. `std::jthread{[&]{ sched.runAndWait(); }}` — exactly the structure of
     `fm_demodulator_imgui.cpp:353-358`.
  6. Render loop on the main thread: `glfwPollEvents()`; for each
     `b : sched.blocks()` with `b->uiCategory() == UICategory::Content`, call
     `b->draw(config)` between `ImGui::NewFrame()` and `ImGui::Render()`; submit the
     tone-map command buffer **before** the render pass, as `MainWindow::Render()` already does.
  7. Teardown: `sched.changeStateTo(REQUESTED_STOP)`, join, destroy blocks' GPU resources,
     destroy window, `ScopehalStaticCleanup()` — in that order. DESIGN §17's teardown fault is
     the precedent: the scheduler must be joined and every Vulkan holder destroyed before
     cleanup.

- **`imcufosphor::RenderHost`** — a singleton accessor
  (`RenderHost& globalRenderHost();`), deliberately modelled on
  `gr::basic::globalDataSinkRegistry()`. Holds the `TextureManager*`, the render
  `QueueHandle`, the DPI scale, and the current frame's `vk::raii::CommandBuffer*`.
  `draw()` fetches it lazily; if it is unset, `draw()` returns `work::Status::OK` without
  rendering. This is global state and I am recommending it with open eyes: scopehal is
  already built on `g_vkComputeDevice`/`g_vkQueueManager`/`g_log_sinks`, so a fourth global
  costs nothing new, and the alternative (threading a context pointer through
  `property_map` settings) is worse.

- **Headless flowgraph.** Nothing calls `draw()`; the block never initialises Vulkan, never
  allocates a texture, and `processBulk` degenerates to "consume and drop". A block should log
  once at `start()` if `globalRenderHost()` is unset, so a headless run is obvious rather than
  mysterious.

- **Multiple sinks in one graph** share the window: each `draw()` opens its own
  `ImGui::Begin(this->name)` window. They do *not* share a `PlotAxis` — axis linkage across
  blocks is out of scope for phase 1.

---

## 9. File layout and CMake

```
imcufosphor/
  CMakeLists.txt                     +option(BUILD_GR4_BLOCKS "" OFF), find gnuradio4 via pkg-config
  src/imcufosphor/
    IqInjector.{cpp,h}               NEW  no-input Filter, I/Q/center streams
    SpectrumEngine.{cpp,h}           NEW  PlayerSession minus the file, plus parts mask
    PlayerSession.{cpp,h}            reimplemented on SpectrumEngine (phase 5)
    AnalyzerPane.{cpp,h}             ctor: SpectrumEngine* instead of PlayerSession*
    CMakeLists.txt                   split out imcufosphor-render
  src/imcufosphor-render/            NEW target (files moved, not copied)
    PlotAxis.{cpp,h} SpectrumArea.{cpp,h} WaterfallArea.{cpp,h} AnalyzerPane.{cpp,h}
  include/imcufosphor/
    RenderHandle.h                   NEW  C++17/23-neutral opaque seam (no scopehal/Vulkan)
    RenderHost.h                     NEW  globalRenderHost()
  src/gr4blocks/                     NEW  C++23
    include/gnuradio-4.0/imcufosphor/
      SpectrumSink.hpp
      WaterfallSink.hpp
      AnalyzerSink.hpp
      detail/IqRing.hpp              gr::CircularBuffer wrapper + drop accounting
      detail/MetaFromTags.hpp        sample_rate / center_frequency extraction (§4.4)
    src/RenderHandle.cpp             C++17 side of the seam — NO, see note
    test/
      qa_SpectrumSink.cpp            Boost.UT, headless (no RenderHost)
      qa_MetaFromTags.cpp
      CMakeLists.txt
  src/gr4app/                        NEW  C++23 host executable
    main.cpp  Gr4MainWindow.{cpp,h}
  notes/gr4-blocks-plan.md           this file
```

Note on `RenderHandle.cpp`: it belongs in `imcufosphor-render` (C++17), not in `src/gr4blocks`.
Only the *declaration* crosses into the C++23 TUs.

CMake:

```cmake
# top level
option(BUILD_GR4_BLOCKS "Build the GNU Radio 4 sink blocks" OFF)
if(BUILD_GR4_BLOCKS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GNURADIO4 REQUIRED IMPORTED_TARGET gnuradio4)
  add_subdirectory(src/gr4blocks)
  add_subdirectory(src/gr4app)
endif()

# src/gr4blocks/CMakeLists.txt
add_library(imcufosphor-gr4 INTERFACE)          # header-only, incubator-style
target_include_directories(imcufosphor-gr4 INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(imcufosphor-gr4 INTERFACE
  PkgConfig::GNURADIO4 imcufosphor-render-iface)   # iface = RenderHandle.h only
target_compile_features(imcufosphor-gr4 INTERFACE cxx_std_23)
target_compile_options(imcufosphor-gr4 INTERFACE
  -include ${CMAKE_SOURCE_DIR}/cmake/gr4_stdfloat_compat.hpp)   # copy from gr4-incubator
```

`gr4-analyzer` needs `imcufosphor_attach_shaders()` and `imcufosphor_attach_icons()`, same as
`sigmf-spectrum` — without the `.spv` collection step nothing resolves at runtime
(DESIGN §7.2).

**Where the blocks live: imcufosphor, not gr4-incubator.** gr4-incubator's stated build policy
is "system dependencies only; no vendored dependency subprojects" and header-only INTERFACE
modules. These blocks need Vulkan, scopehal, scopeprotocols, VkFFT, glslc-compiled SPIR-V and
lifted ngscopeclient sources. They cannot be a `blocks/*` INTERFACE module. If upstreaming is
wanted later, the right shape is a gr4-incubator module that depends on an *installed*
`imcufosphor-render` package — which is another reason to keep the `RenderHandle.h` seam
narrow and stable.

Tests: Boost.UT, same registration style as `blocks/measure/test`. Headless by construction
(no `RenderHost`), so they can verify `processBulk` consumption, drop accounting, tag
extraction, and `settingsChanged` without a GPU. The GPU path is verified by `wfbench`-style
tools, not by unit tests — the machine's Vulkan defect (DESIGN §3, Xid 32) means GPU tests need
`SCOPEHAL_VULKAN_DEVICE_OVERRIDE=1`.

---

## 10. Phased implementation

**Phase 0 — spikes (1-2 days).** Two questions that change the plan if answered badly:
(a) does scopehal compile at C++23? (b) does a `Drawable` block's `draw()` actually get
dispatched through `BlockModel` in the pinned gnuradio4 — write a 40-line no-op drawable block
and confirm `sched.blocks()` + `uiCategory()` + `draw()` works end to end.
*Milestone: both answered, standard/seam decision fixed.*

**Phase 1 — `IqInjector` + `SpectrumEngine`.** No GR4 involvement. Extract
`imcufosphor-render`. Re-cut `AnalyzerPane` against `SpectrumEngine`. Add a `wfbench`-style
tool that pushes synthetic IQ through `SpectrumEngine` and asserts the known-tone result the
existing FFT verification already checks. *Milestone: `sigmf-spectrum` still works, now via
`SpectrumEngine`; a headless tool drives the same engine from a raw IQ span.*

**Phase 2 — `AnalyzerSink<float>` + `gr4-analyzer` host.** Ring buffer, tag extraction, lazy
Vulkan init, `draw()`. Graph: `gr::incubator::sigmf::SigMfSource` → `AnalyzerSink`. This is the
end-to-end proof and it deliberately comes before the other two blocks.
*Milestone: a GR4 flowgraph renders the existing analyzer view from a SigMF file.*

**Phase 3 — `SpectrumSink<float>` and `WaterfallSink<float>`.** Parts mask, per-block settings,
Boost.UT tests. *Milestone: three blocks, tests green, headless run does not stall.*

**Phase 4 — control and polish.** `settings().setStaged()` round trip from the ImGui panel;
drop/overrun counters; `n_dropped_samples` and `end_of_stream` handling; `SoapyRx` →
`AnalyzerSink` live graph. *Milestone: live SDR, tunable from the UI.*

**Phase 5 — optional.** `PlayerSession` reimplemented on `SpectrumEngine` (delete the
duplicate scheduling); `SpectrumSink<DataSet<float>>` pre-FFT'd variant; propose
`center_frequency` as a gr4 `DefaultTag`.

---

## 11. Open questions and risks

1. **Two ImGui builds.** gr4-incubator links a system/`subprojects` ImGui built for the
   **OpenGL3 + GLFW** backend; imcufosphor uses ngscopeclient's ImGui with the **Vulkan**
   backend. One process must contain exactly one ImGui, one `ImGuiContext`, and one backend.
   The plan assumes `gr4-analyzer` uses **imcufosphor's Vulkan ImGui** and that no gr4 header
   pulls in a conflicting one. Verified only to the extent that no gnuradio4 *core* header
   includes ImGui — **not** verified against the incubator's GUI CMake path. This is the
   single biggest integration risk. If ImPlot is wanted alongside, it must be rebuilt against
   the Vulkan ImGui.
2. **C++17/23.** Unverified whether scopehal's headers compile at C++23. Phase 0 spike.
3. **Vulkan device loss on the dev machine.** DESIGN §3/§7.2: every scopehal compute submission
   faults on RTX 5060 Ti / driver 580.173.02 (Xid 32). All GPU work must be developed under
   `SCOPEHAL_VULKAN_DEVICE_OVERRIDE=1` (llvmpipe). This will make the display slow and make
   throughput claims meaningless until the driver issue is resolved.
4. **Which gnuradio4?** Two checkouts, differing `Block.hpp`. Pick one, record the commit in
   this file, and re-verify §1 after any bump. `docs/USER_API_Drawable_UI.md` exists only in
   the newer one, so the older installed headers may lag the documented Drawable contract in
   ways not checked here.
5. **`draw()` dispatch through a running scheduler is unproven in this repo.** `ImChartMonitor`
   with `drawAsynchronously=false` calls `draw()` from `processBulk`; the async path is what we
   need and no in-tree caller exercises it. Phase 0 spike covers this. Fallback: `DataSink` +
   `StreamingPoller` (§3), same ring buffer, app-side rendering.
6. **`Waterfall` row count vs. pane size.** `WaterfallArea::UpdateSize()` resizes the *filter's*
   ring buffer from the pane height. That is a render-thread write to an object the engine also
   touches. In the current single-threaded app it is safe; with a scheduler thread it is not.
   `SpectrumEngine::Step()` must be render-thread-only (it is, in this design — `draw()` calls
   it), so this stays safe *as long as nothing moves `Step()` onto the scheduler thread*.
   Worth a comment at the call site.
7. **Colour ramp assets.** The areas take a ramp name resolved through `TextureManager` from a
   PNG. `gr4-analyzer` needs the same `imcufosphor_attach_icons()` step. Not verified which
   ramps ship or what the default is named — read `MainWindow.cpp` before wiring the
   `color_map` setting.
8. **`Filter` graph execution without `FilterGraphExecutor`.** `SpectrumEngine::Step()` will
   call `Refresh(cmdBuf, queue)` on each filter in order, as `PlayerSession` already does.
   Confirmed that is how the current code works; not confirmed whether any of the filters
   depend on executor-provided state (`GetExecutionCapabilitiesMask` suggests they might).
   Read `PlayerSession.cpp` fully before implementing.
9. **`std::complex<T>` port vs. scopehal's separate I and Q streams.** `IqInjector` must
   de-interleave. At megasample block sizes that is a real host-side copy on the render thread.
   Measure it; if it hurts, do the de-interleave in `processBulk` (scheduler thread) into two
   float rings instead of one complex ring.

---

## 12. As built

Implemented 2026-08-18. This section records where the plan above was wrong or was overtaken,
so the two are not silently in conflict. Everything below is verified, not intended.

### What shipped

| Piece | Where |
|---|---|
| `IqInjector` | `src/imcufosphor/IqInjector.{h,cpp}` |
| `SpectrumEngine`, `EnginePart`, `EngineConfig`, `RegisterImcufosphorFilters()` | `src/imcufosphor/SpectrumEngine.{h,cpp}` |
| `IAnalyzerSource`, `PlaybackStats`, `BlockSpan` | `src/imcufosphor/AnalyzerSource.h` |
| `RenderHost`, `globalRenderHost()` | `src/imcufosphor/RenderHost.{h,cpp}` |
| `imcufosphor-render` library | `src/imcufosphor/CMakeLists.txt` |
| `AnalyzerSink`, `SpectrumSink`, `WaterfallSink` | `gr-imcufosphor/blocks/display/include/gnuradio-4.0/imcufosphor/` |
| `IqRing`, `MetaFromTags`, `StreamStatus` | `.../imcufosphor/detail/` |
| `gr4-analyzer` host | `gr-imcufosphor/apps/gr4-analyzer/` |
| Boost.UT suites | `gr-imcufosphor/blocks/display/test/` (4 suites, all green) |

`gr-imgui/` was deleted. Only its `globalRenderHost()` idea survived.

### Corrections to sections 1-11

1. **§7's C++17/23 seam does not exist.** Every imcufosphor and scopehal TU compiles clean at
   `-std=c++23` with g++-16, and scopehal, ngscopeclient's ImGui and gnuradio4 headers coexist
   in one TU. The whole superbuild moved to C++23 (`CMakeLists.txt`) and the blocks hold
   `SpectrumEngine*` and `AnalyzerPane*` directly. `RenderHandle.h` was never written.
2. **§1.7 is out of date: `gr::tag::FREQUENCY` exists** (`Tag.hpp:205`, key `"frequency"`,
   double, Hz) and is in `kDefaultTags`. We use it. Do not invent `center_frequency` as a tag;
   it remains only a *setting* name. The SigMF fallback via
   `trigger_meta_info["core:frequency"]` is still needed, because the incubator's SigMF source
   publishes the frequency only there.
3. **§7.3's `IqInjector` was over-engineered.** It is not a `Filter`: `ComplexChannel(nullptr, …)`
   already declares exactly the three streams `ComplexFFTFilter` wants, and `Verify.cpp:478-490`
   already drove a real filter that way. No `Refresh()`, no `AddDecoderClass`, ~90 lines.
4. **§11 risk 9 is gone.** Both `std::complex<float>` (PACKED_IQ_FLOAT32) and
   `std::complex<int16_t>` (PACKED_IQ_INT16) are bit-identical to what the shader reads, so a
   push is a `memcpy` with no deinterleave and no conversion. `qa_IqLayout` pins the layout
   assumption down; `wfbench --verify` confirms both decode a tone at +10.000 dBm.
5. **§11 risk 5 is resolved: `draw()` does dispatch** through `BlockModel::draw()` on a running
   multi-threaded scheduler, discovered generically via `uiCategory() == Content`. Proven by a
   throwaway spike before anything depended on it. The `DataSink` fallback was not needed.
6. **`PlayerSession` was not rebased on `SpectrumEngine`** and is untouched, as §7.2's second
   option allowed. It keeps a ~60-line duplicate of the step body. Rebasing needs a "recording
   coordinate provider" seam that is worse than the duplication; revisit only if it drifts.

### Traps found while building, which the plan did not anticipate

1. **`emitErrorMessage()` in `start()` aborts the flowgraph.** Using it for an informational
   "no RenderHost, running headless" notice put the block into the ERROR lifecycle state and
   the graph completed having moved zero samples. Use a log line. This cost the most time of
   anything here and was caught only because `qa_AnalyzerSink` asserts on the sample totals.
2. **A user-declared destructor breaks a GR4 block.** It suppresses the implicit move the graph
   machinery relies on. The "no custom constructors or destructors" rule is real; the teardown
   hook is a named method the host calls (`ReleaseGpuResources()`), not `~Block()`.
3. **`tagValueAsDouble` must try every integer width.** `gr::Size_t` is `uint32_t`, so a tag as
   ordinary as `n_dropped_samples` was silently reported as absent by a coercion that only
   tried 64-bit types.
4. **`imcufosphor_attach_shaders()` used `PROJECT_SOURCE_DIR`**, which resolves to the nearest
   enclosing `project()` - so it broke the moment a subproject called it. Now
   `CMAKE_CURRENT_FUNCTION_LIST_DIR`.
5. **The tone map must be submitted from inside `draw()`,** not batched to the end of the frame.
   `draw()` runs while the window's render pass is being recorded, and the ImGui call that
   samples the texture comes immediately after, so a deferred submit lands after the pass that
   reads it. See `RenderHost::FrameComputeCommandBuffer()`.
6. **A `Drawable` block must issue its ImGui calls on every frame, without exception.**
   The obvious way to honour `timeout_ms` is to return early from `draw()` when the interval
   has not elapsed. That is wrong for an ImGui toolkit: ImGui is immediate mode, so a window
   not submitted on a frame is not drawn on that frame. Against a 16.7 ms vsync a 33 ms limit
   skips every other frame, and the pane blinks at 30 Hz. Rate-limit the *GPU work* - the
   engine step and the tone map - and always fall through to `ImGui::Begin`/`End`. Skipping the
   compute costs nothing visually, because the texture still holds what the last tone map
   wrote. `ImChartMonitor` gets away with the early return only because its toolkit is
   `"console"`, where re-emission means something different.

7. **The default -100/-20 dBm scale is a footgun on quiet recordings.** A signal below `db_min`
   falls off the bottom of the spectrum's amplitude axis and vanishes, while the waterfall
   still shows a flat floor - it clamps out-of-range values to the end of the colour ramp
   rather than dropping them. "Waterfall but no spectrum" is therefore the characteristic look
   of a wrong range rather than of a broken spectrum path, and it cost real debugging time
   before that was understood. `wfbench --survey FILE` prints the peak level; `gr4-analyzer`
   gained `--db-min`/`--db-max` and says as much in `--help`.

8. **Do not set `in.max_samples` from `block_size`.** The ring already decouples the port's
   chunk size from the GPU's block size, and `block_size` defaults to a megasample, far more
   than the port buffer holds. Measured to make no difference; omitted so as not to constrain
   the scheduler for nothing.

### Verification

- `ctest`: 4 suites green, all headless (no `RenderHost`, so `draw()` allocates nothing).
- `wfbench --verify`: all checks pass including the new `SpectrumEngine` section, which drives
  the engine from a raw IQ span in both sample types at both signs of frequency offset, and
  checks the parts mask and the group-size fold.
- Throughput unchanged by the C++23 move: 1854-1870 MS/s on both the old C++17 build and the
  new one, measured back to back.
- `gr4-analyzer` with its built-in generator renders a tone at exactly the requested offset in
  both panes, reporting e.g. `382 rows | 6112 spectra | shown 19.6%`.

### Live SDR

`gr4-analyzer --soapy` was added and verified against a USRP B205mini (UHD 4.6, USB 3):

```
gr4-analyzer --soapy driver=uhd --center 2.44e9 --rate 10e6 --gain 40 --fft 4096 --block 65536
```

renders live 2.4 GHz ISM traffic - LO leakage spike at the centre, bursts streaking across the
waterfall - at 30 rows/s with no overflows. Two things learned:

1. **`SoapyRx` publishes no tags at all.** No `sample_rate`, no `frequency`. The sink's
   fallback settings are therefore the only source of both, and the host has to set them to
   match what it asked the radio for. §1.7's observation still holds for this block.
2. **`device` and `device_args` are not interchangeable.** `device` is a bare driver name that
   `openDevice()` turns into `kwargs["driver"]`; `device_args` is a full key=value string.
   Passing `driver=uhd` as `device` produces `kwargs["driver"]="driver=uhd"` and
   `Device::make()` throws "no match". `--soapy` routes on whether the selector contains '=',
   so both `uhd` and `serial=30AF821` work.

### Throughput: one block per frame was the whole problem

The display first shipped draining exactly one block per frame, which caps it at
`block_size x frame_rate` no matter what the pipeline can do - 2 MS/s at a 64k block and 30 fps,
against a pipeline that benchmarks at ~490 MS/s. 99.9% of a file was being discarded, and the
status line said so (`shown 0.1%`) without anyone reading it as a bug.

`MainWindow::Render()` has always drained to a wall-clock budget instead, and the comment at
`MainWindow.cpp:105-108` says exactly why: *"one row per frame would cap playback at the refresh
rate while the GPU sits idle."* The GR4 sink now does the same, via `step_budget_ms`.

Second fix: `backpressure`. A file source reads far faster than the GPU can transform, so
without flow control some loss is arithmetic rather than a bug - and for a recording that is
wrong, because a recording has no realtime constraint and dropping from it means analysing a
fraction of the capture. `processBulk` can consume only what fits, leaving the rest in the port.
Default on for a file, off for anything live, where dropping a frame beats a receiver overflow.

Measured on a 100 Msample 40 MS/s cf32 recording, 4096-point transforms, 64k blocks:

| | fps | drain | throughput | dropped |
|---|---|---|---|---|
| one block per frame (as shipped) | 30 | 1 block | 2 MS/s | 99.9% |
| step budget, no backpressure | 60 | ~106 blocks / 10 ms | 417 MS/s | 96% |
| step budget + backpressure | 60 | ~105 blocks / 10 ms | 207 MS/s | **0** |

`gr4-bench` was written to get those numbers. It runs the same stages headless - source to a
counting sink, to the sink's ring, and through the whole GPU pipeline - so that "the display is
slow" can be attributed to a stage instead of guessed at.

Two things that cost time and are worth writing down:

1. **A blanked screen throttles presentation to about 1 fps**, which looks exactly like a
   pipeline stall and led to blaming backpressure for a frame rate collapse it had nothing to do
   with. `xset s off -dpms` before measuring anything.
2. **The back-off when the ring is full has to be short.** Returning `OK` having consumed
   nothing makes the scheduler re-invoke immediately and spin, so some wait is needed - but at
   250 us it throttled the whole display to 26 MS/s, because the reader drains in bursts and the
   writer has to refill between them. 20 us, and a ring deep enough to feed a whole burst.

### Not done

- **`SpectrumSink<DataSet<float>>` pre-FFT'd variant (§10 phase 5).** Not started.
- **`center_frequency` upstream proposal (§4.4).** Moot - `gr::tag::FREQUENCY` already exists.
