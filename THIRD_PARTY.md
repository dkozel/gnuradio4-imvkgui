# Third-party code

Two kinds of external code live in this repository, and the difference is the point:

- **`lib/`** — submodules. Pinned pointers to upstream repositories. We never edit them, and
  upgrading is a pointer bump.
- **`third_party/`** — vendored source. Copied into our history, trimmed to what this project
  uses, and modified where it needed modifying. We own the copy.

Everything in `third_party/` was previously a submodule under `lib/`. It moved because the
arrangement had failed twice: a submodule bump silently removed an API the application called
(`QueueManager::GetRenderQueue`), and the same bump silently orphaned a patch this hardware
needs, leaving every GPU transfer faulting the device for a week without anyone noticing.

---

## `lib/` — submodules

| Path | Upstream | Pinned at | Licence |
|---|---|---|---|
| `lib/libsigmf` | `deepsig/libsigmf` | `b87f8ff5` (v1.0.2-16) | Apache-2.0 |
| `lib/imgui` | `ocornut/imgui` | `b61e56346` (v1.92.8-docking) | MIT — `licenses/imgui.LICENSE` |
| `lib/VkFFT` | `ngscopeclient/VkFFT` | `e0840114` | MIT — `licenses/VkFFT.LICENSE` |

`lib/imgui` points at ocornut rather than ngscopeclient's fork. Diffed against the tag the
fork merges, that fork differs in one file — `imstb_truetype.h`, where three static functions
are given external linkage to silence a warning. Those translation units compile with `-w`
here, so it bought nothing.

`lib/VkFFT` points at the ngscopeclient fork rather than `DTolm/VkFFT`, because unlike the
ImGui fork this one carries two changes that are not upstream: a memory leak fix in
`vkFFT_DeletePlan` and a fix for storing a long-lived pointer to a stack variable in the
memory barrier.

---

## `third_party/scopehal`

Vendored from **`ngscopeclient/scopehal` at `0c6b2f41` (v0.2.2)**. BSD-3-Clause;
`licenses/scopehal.LICENSE`.

Upstream is 175 translation units and 78,553 lines across two shared libraries
(`libscopehal`, `libscopeprotocols` at a further 211). This project's measured link closure
was 156 of the 175 — but 137 of those arrived through a single edge, which is the whole story
of this directory. See "scopehal.cpp" below.

What is here: **26 sources and 40 headers.**

| Directory | Contents |
|---|---|
| `scopehal/` | GPU and compute infrastructure, the filter graph, `Unit`, `scopehal.cpp` |
| `scopeprotocols/` | `Waterfall` only — one filter of 211 |
| `log/` | `log.cpp` and the three sinks |
| `xptools/` | `TimeUtil` only. `Socket`, `UART`, `HID` and `StringUtil` are not here, and dropping `HID.cpp` is what removes the hard `hidapi` dependency upstream declares `FATAL_ERROR` on |
| `shaders/` | 4 `.glsl`, of roughly 105 the two libraries build |

### Local modifications

The pristine copy is commit `7f96dd9`; every file in it is byte-identical to upstream, so
`git diff 7f96dd9 -- third_party` is the complete and current set of changes.

| File | Change |
|---|---|
| `scopehal/AcceleratorBuffer.h` | The NVIDIA Xid 32 workaround. Its own commit (`cc0b087`), so `git format-patch -1 cc0b087` regenerates a patch for upstream. See below. |
| `scopehal/scopehal.cpp` | `TransportStaticInit()`, `InitializePlugins()` and the ~85 `AddDriverClass` calls in `DriverStaticInit()` deleted. Its first three lines become `ScopehalStaticInit()`. **This is the change that matters**: those three functions named every driver class in the library, and were the only reason 137 of the 175 TUs were in the link. |
| `scopehal/scopehal.h` | 62 includes dropped — every SCPI transport, every instrument and instrument channel, the Touchstone and IBIS parsers, five unused filters. `InstrumentChannel.h` added explicitly, having previously arrived via `Instrument.h`. `config.h` dropped: upstream generated it and its entire content was one comment. `yaml-cpp` dropped with the serialization. |
| `scopehal/OscilloscopeChannel.cpp` | 34 `if(m_instrument) return GetScope()->…` pass-throughs collapsed to the branch that ran. Every channel in this project has a null instrument, so those branches were dead — and each was a virtual call making the filter graph depend on `Oscilloscope` being a complete type. |
| `scopehal/OscilloscopeChannel.h`, `scopehal/ComplexChannel.h` | The scope parameter becomes `Instrument*`, which is what the base takes and what every caller passes. `GetScope()` deleted with its last caller. |
| `scopehal/Filter.{h,cpp}`, `scopehal/FlowGraphNode.{h,cpp}` | `SerializeConfiguration()`, `LoadParameters()` and `LoadInputs()` deleted. They wrote the filter graph as YAML for ngscopeclient's session files; nothing here saves or loads a session, and they were the only reason anything linked yaml-cpp. |
| `scopeprotocols/Waterfall.cpp` | An unused `#include` of `FFTFilter.h`. |

### The Xid 32 workaround

On an RTX 5060 Ti with driver 580.173.02, every scopehal GPU transfer faults the device:
`NVRM: Xid 32`, then `VK_ERROR_DEVICE_LOST`. The trigger is `vkCmdSetEvent` with a TRANSFER
stage mask in a command buffer submitted to an async-compute queue family.
`tools/vkmintest/xid32_repro.cpp` reproduces it in 166 lines with no scopehal involved.

The four `setEvent()` calls in `AcceleratorBuffer.h` route through `SetEventStage()`, which
returns `eAllCommands`. A wider stage mask is strictly more conservative, so it is correct on
hardware that does not need it.

**Status:** local. Not yet submitted upstream; the maintainers may prefer the `vkCmdSetEvent2`
route or find a different underlying cause.

**To verify it is live:** `ctest -R wfbench-verify` must pass without
`SCOPEHAL_VULKAN_DEVICE_OVERRIDE` set. If it faults with `VK_ERROR_DEVICE_LOST`, it is not.

---

## `third_party/ngscopeclient`

Vendored from **`ngscopeclient/scopehal-apps` at `455237b7` (v0.2.1)**. BSD-3-Clause;
`licenses/scopehal-apps.LICENSE`.

Two source files, `VulkanWindow` and `TextureManager`, plus two shaders and one colour ramp.

Compiling those two in place previously took twelve files: these two, ngscopeclient's
1604-line preference system (`Preference.cpp`, `PreferenceTree.cpp`, `PreferenceManager.cpp`
and their headers), a 70-line preference schema of ours, a 68-line preprocessor shim to stop
that system rewriting the real ngscopeclient user's `~/.config` file on exit, and three compat
headers papering over headers that were not self-contained.

### Local modifications

| File | Change |
|---|---|
| `VulkanWindow.{h,cpp}` | The preference system replaced by `WindowGeometry.h` — ten values, read at three sites. `SaveWindowPositionAndSize()` takes a `WindowGeometry&`. The header made self-contained: upstream's had **no `#include` directives at all** and named `GLFWwindow`, `QueueHandle` and ten `vk::raii` types, relying on its includer having read `ngscopeclient.h` first. An unused `VulkanFFTPlan.h` include dropped. |
| `TextureManager.{h,cpp}` | Header made self-contained for the same reason — it named `ImTextureID` and `GLFWimage` without declaring either. |

`WindowGeometry.h` and `LinkCheck.cpp` in that directory are ours, not vendored.

**Window geometry is no longer persisted.** The struct carries defaults and nothing writes a
file, so the window opens at its default position every run. That is a deliberate, visible
cost: the right time to add persistence back is when someone misses it, in whatever format the
project then wants, rather than by keeping 1600 lines of YAML machinery against the
possibility.

### Not copied

`ngscopeclient.h` itself, and the 11 headers it dragged in — `BERTState.h`,
`OscilloscopeState.h`, `PowerSupplyState.h`, `MultimeterState.h`, `LoadState.h`,
`FunctionGeneratorState.h`, `Marker.h`, `FontManager.h`, `GuiLogSink.h`, `Event.h`,
`ImGuiDisabler.h`. That is 1081 lines of ngscopeclient's live-instrument session model, which
reached every display header in this project and every GNU Radio block header through them.
Nine lines of that umbrella were load-bearing; they are now in the two headers that need them.

---

## Keeping this honest

Both `third_party` CMakeLists fail the configure if a `.cpp` is present but not built, or
built but not present. "Every file is here for a reason" decays the moment a file can sit in
the tree unreferenced, so it is checked rather than asserted.

`ctest` covers the rest: `include-style` fails if any of our sources reaches into a dependency
by relative path, `window-linkcheck` forces the linker to resolve everything the window layer
references, and `wfbench-verify` runs the six correctness checks in `tools/wfbench/Verify.h`
against a generated recording — including the GPU path, which is what would notice the Xid 32
fault returning.

## Attribution

BSD-3-Clause clause 1 requires the copyright notice to be retained in source
redistributions. Every vendored file keeps its upstream header verbatim, including in files
whose bodies were trimmed. The four upstream licence texts are in `licenses/`.

Clause 3 means this project must not use the names "ngscopeclient", "scopehal" or the
upstream authors to endorse or promote it. Keeping a vendored class named `VulkanWindow` is
not endorsement and is fine.
