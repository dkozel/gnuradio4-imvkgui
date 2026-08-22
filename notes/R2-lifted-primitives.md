# R2 — lifting ngscopeclient's display primitives

Findings from implementing risk R2 (DESIGN.md §11, design §5): *does decision D4,
"reference, don't copy", actually hold once the lifted primitives have to compile and
link?*

Answered empirically, not on paper. Everything below was verified by building it.

| Repo | Commit |
|------|--------|
| `ngscopeclient/scopehal` | `24dd95fb` (v0.2.1) |
| `ngscopeclient/scopehal-apps` | `455237b7` (v0.2.1) |
| `ngscopeclient/imgui` (a `scopehal-apps` submodule) | `5e6bf29d`, imgui 1.92.8 |

## Verdict

**D4 holds, with two qualifications.** No upstream file needed modification, nothing under
`lib/` was touched, and nothing needs to move to copy-with-sync-script. But:

1. **§5's file list is incomplete.** Two more upstream translation units are non-optional
   (`Preference.cpp`, `PreferenceTree.cpp`), and the inherited *header* graph is larger
   than §5 says: it also drags in `Marker.h` and `FontManager.h`, which §5 does not
   mention.
2. **Two upstream defects have to be worked around on our side**, and both are the kind of
   thing a submodule bump can reintroduce or change:
   - `VulkanWindow::GetContentScale()` is **declared but never defined** anywhere in
     scopehal-apps. Calling it is a link error.
   - `PreferenceManager` hardcodes `~/.config/ngscopeclient` and its singleton destructor
     writes there, so linking it unmodified means our process overwrites the user's real
     ngscopeclient preference file. Fixed with a preprocessor shim
     (`src/ngscopeclient-compat/ConfigPathShim.h`).

Neither is a reason to abandon reference-don't-copy. Both are reasons to re-run
`ngscopeclient-compat-linkcheck` after every submodule bump, which is what that target is for.

## What was built

`src/ngscopeclient-compat/` — a static library `ngscopeclient-compat` plus a `ngscopeclient-compat-linkcheck` executable.
Wired into the superbuild with one line in the top-level `CMakeLists.txt`.

```
src/ngscopeclient-compat/CMakeLists.txt      the whole inherited include graph, declared in one place
src/ngscopeclient-compat/PreferenceSchema.cpp our replacement for upstream's 669-line schema
src/ngscopeclient-compat/ConfigPathShim.h     redirects PreferenceManager's config directory
src/ngscopeclient-compat/LinkCheck.cpp        proves it links; optionally opens a real window
```

Status: **compiles and links clean.** `ngscopeclient-compat-linkcheck` runs and exits 0. With
`--window` it also brings up a real GLFW/Vulkan window, renders one frame through the
lifted `VulkanWindow`, and persists window geometry — that path is opt-in precisely because
a build machine may have no display.

The application target is *not* yet linked against `ngscopeclient-compat`; that is a one-line
`target_link_libraries(imcufosphor ngscopeclient-compat)` in `src/imcufosphor/CMakeLists.txt`, left for
whoever reconciles this with R1.

## Upstream files referenced, and why

All paths relative to `lib/scopehal-apps/src/`. Referenced in place; none copied, none
edited.

### Compiled (5 translation units from `ngscopeclient/`)

| File | Why |
|------|-----|
| `ngscopeclient/VulkanWindow.cpp` | The primitive itself (§5). |
| `ngscopeclient/TextureManager.cpp` | The primitive itself (§5). Needs libpng. |
| `ngscopeclient/PreferenceManager.cpp` | `VulkanWindow.cpp:134,875` use the singleton. |
| `ngscopeclient/Preference.cpp` | **Not in §5.** Defines `Preference` and the builder DSL that `PreferenceManager.h` and our schema use. |
| `ngscopeclient/PreferenceTree.cpp` | **Not in §5.** Defines `PreferenceCategory` / `PreferenceHolder` / `PreferencePath`. `PreferenceManager.h:41` includes `PreferenceTree.h` and `PreferenceManager::m_treeRoot` is a `PreferenceCategory` by value, so this is unavoidable. |

`PreferenceSchema.cpp` is deliberately **excluded** — ours replaces it. `FontManager.cpp`
and `GuiLogSink.cpp` are **not** needed: `FontDescription` is only a typedef
(`FontManager.h:43`) and `GuiLogSink` is only declared, never instantiated. Confirmed by a
successful link.

### Compiled (7 translation units from `imgui/`)

`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`,
`backends/imgui_impl_glfw.cpp`, `backends/imgui_impl_vulkan.cpp`,
`misc/cpp/imgui_stdlib.cpp`.

Exactly the set `ngscopeclient.h` includes headers for, minus `imgui_demo.cpp` (upstream's
own demo, not needed). Both Vulkan and GLFW backends are mandatory: `VulkanWindow.cpp`
calls into both directly.

### Headers dragged in (18 from `ngscopeclient/`)

Measured with `g++ -MM` over the five translation units above:

```
BERTState.h          FunctionGeneratorState.h  OscilloscopeState.h  PreferenceTypes.h
Event.h              GuiLogSink.h              PowerSupplyState.h   TextureManager.h
FontManager.h        ImGuiDisabler.h           Preference.h         VulkanWindow.h
LoadState.h          Marker.h                  PreferenceManager.h
MultimeterState.h    ngscopeclient.h           PreferenceTree.h
```

§5 predicted eight of these (`OscilloscopeState.h`, `BERTState.h`, `PowerSupplyState.h`,
`MultimeterState.h`, `LoadState.h`, `FunctionGeneratorState.h`, `GuiLogSink.h`, `Event.h`)
plus `ImGuiDisabler.h`. It missed two:

- **`Marker.h`**, reached via `GuiLogSink.h:38`.
- **`FontManager.h`**, reached via `Preference.h:51` — `FontDescription` is the value type
  behind `Preference::Font`.

**Total inherited from scopehal-apps: 23 `ngscopeclient` files (5 `.cpp` + 18 `.h`), 7
imgui translation units, 1 of 5 scopehal-apps submodules.**

### Not referenced

`Session`, `MainWindow`, `WaveformArea`, `WaveformGroup`, `HistoryManager`, every dialog,
the whole instrument-thread layer. Grepping the five lifted `.cpp` files for `Session`,
`MainWindow`, `RightJustifiedText`, `RectIntersect`, `RectContains`, `InstrumentThread` and
`WaveformThread` returns **nothing**. §5's claim that these primitives are decoupled from
the application is correct.

The shader rows of §5's table (`WaterfallToneMap.glsl`, `waveform-compute.glsl`,
`WaveformToneMap.glsl`) are a `glslc` step, not a C++ compile, and are not part of this
target.

## Submodules of `lib/scopehal-apps` that had to be initialized

**One of five: `src/imgui`.** Left uninitialized and empty:

| Submodule | Why not needed |
|-----------|----------------|
| `src/imgui-node-editor` | Only `FilterGraphEditor.cpp` uses it. |
| `src/ImGuiFileDialog` | Only `IGFDFileBrowser.cpp` uses it. |
| `src/imgui_markdown` | Only the tutorial/markdown dialogs use it. |
| `src/nativefiledialog-extended` | Only `NFDFileBrowser.cpp` uses it; it is also the one that would need `add_subdirectory` on the donor tree, which §3 forbids. |

So the dependency inheritance is genuinely bounded: the lift costs us imgui and libpng and
nothing else new. libpng was already a listed prerequisite (§3).

`src/imgui` is a **fork** (`ngscopeclient/imgui`), not upstream ocornut/imgui. A submodule
bump of scopehal-apps can move it; nothing here depends on fork-specific behaviour, but the
version is worth watching.

## Was `ngscopeclient.h` satisfiable as-is?

**Yes, with no shim at all.** This is the single most useful finding: §5 flags this as "the
one place where reference-don't-copy can get sticky", and it turned out not to be sticky.

The lifted `.cpp` files all begin with `#include "ngscopeclient.h"`. That is a *quoted*
include, so it resolves relative to the including file's own directory first — meaning a
trimmed shim named `ngscopeclient.h` placed on our include path **could not have shadowed
it anyway**. Satisfying the real header was the only option, and it costs:

- `#include "../scopehal/scopehal.h"` — resolves for free. `scopehal`'s CMake target
  exports its own source directory as a PUBLIC include (`scopehal/CMakeLists.txt:290-296`),
  so `<that dir>/../scopehal/scopehal.h` lands on the right file. Just linking `scopehal`
  is enough; no path fixups.
- GLFW, and the imgui core + two backends — the submodule above.
- The eight state headers, `Event.h`, `GuiLogSink.h`, `ImGuiDisabler.h`, `Marker.h` — all
  sit next to `ngscopeclient.h` and resolve relative to it. They are **pure declarations**:
  none includes anything outside `ngscopeclient/`, none needs a `.cpp` built, none
  references `Session` or `MainWindow`.

`ngscopeclient.h` also *declares* `InstrumentThread()`, `WaveformThread()`,
`RightJustifiedText()`, `RectIntersect()` and `RectContains()`, which we never define.
Declarations without uses cost nothing at link time — verified, not assumed, which is what
`ngscopeclient-compat-linkcheck` exists to keep verifying.

The include graph is heavy but inert. **R2 can be closed as a non-problem in the form it
was written.** The real friction was elsewhere, in the two items below.

## The minimal preference schema

`src/ngscopeclient-compat/PreferenceSchema.cpp`, ~35 lines of schema against upstream's 669.

`PreferenceManager.h` declares two things it does not define — the singleton storage
(`:111`) and `InitializeDefaults()` (`:101`) — and upstream puts both in
`PreferenceSchema.cpp`. So supplying our own is not merely allowed, it is *required*: link
fails without it. That is a nice property, since it means we cannot accidentally inherit
ngscopeclient's UI schema.

The complete set of preferences the lift reads:

| Path | Type | Read at | Written at |
|------|------|---------|------------|
| `Appearance.Windowing.startup_mode` | enum (`StartupMode`) | `VulkanWindow.cpp:146` | — |
| `Appearance.Startup.startup_size_width` | int | `:180` | `:876` |
| `Appearance.Startup.startup_size_heigth` | int | `:181` | `:877` |
| `Appearance.Startup.startup_pos_x` | int | `:182` | `:878` |
| `Appearance.Startup.startup_pos_y` | int | `:183` | `:879` |
| `Appearance.Startup.monitor_name` | string | `:184` | `:893` |
| `Appearance.Startup.monitor_width` | int | `:185` | `:891` |
| `Appearance.Startup.monitor_heigth` | int | `:186` | `:892` |
| `Appearance.Startup.startup_fullscreen` | bool | `:187` | `:880` |
| `Appearance.Startup.startup_maximized` | bool | `:188` | `:882` |

Ten preferences in two categories. `TextureManager` reads none.

Three constraints on anyone editing that file:

- **Identifiers must match upstream byte for byte**, including `heigth` and
  `startup_size_heigth`, which are misspelled in upstream's schema *and* its reader. Fixing
  the spelling on our side breaks the lookup.
- **Lookups are by string and throw.** `PreferenceCategory::GetLeaf()` raises
  `std::runtime_error` on a miss, so a missing preference is a startup crash, not a compile
  error. `ngscopeclient-compat-linkcheck` reads all ten back explicitly for exactly this reason — it is
  the cheapest possible regression test for a submodule bump adding a preference read.
- **`startup_mode` needs its `EnumValue` mappings**, not just a default: they are what the
  YAML serializer round-trips through.

Verified: running `ngscopeclient-compat-linkcheck` produces a well-formed
`~/.config/imcufosphor/preferences.yml` with all ten values, and reads them back.

## Things that needed a shim

Neither contradicts D4 — both were solved on our side, with the submodule untouched — but
both are upstream defects that a bump could change.

### 1. `PreferenceManager`'s hardcoded config path

`PreferenceManager.cpp:112-119` hardcodes `~/.config/ngscopeclient/preferences.yml` (and
the `%APPDATA%\ngscopeclient` equivalent above it). `PreferenceManager` is a singleton
constructed at static-init and its **destructor calls `SavePreferences()`**. So linking it
unmodified means our process reads the user's real ngscopeclient preference file at startup
and truncates it at exit, replacing several hundred settings with our ten. That is silent
data loss in someone else's application.

D4 forbids editing the submodule, so `src/ngscopeclient-compat/ConfigPathShim.h` interposes on the two
scopehal helpers the path string passes through (`scopehal.h:306-307`), using function-like
macros that resolve to the real functions with a rewritten argument. It is force-included
(`-include`) into `PreferenceManager.cpp` only, and it includes `ngscopeclient.h` itself
first so the macros are not visible when those two functions are *declared*.

Verified: config directory is now `~/.config/imcufosphor`, and the ngscopeclient one is
left alone.

The Windows branch takes a different path through `DeterminePath()` (wide strings,
`PathCombineW`, neither helper called) and `CreateDirectory` is itself a `windows.h` macro,
so the shim is `#ifndef _WIN32`. **Unsolved for Windows** — flagged in the header.

### 2. `VulkanWindow::GetContentScale()` is declared but never defined

`VulkanWindow.h:54` declares it. Grepping all of `scopehal-apps` for a definition returns
nothing; the only hits are imgui's unrelated `ImGui_ImplGlfw_GetContentScaleForMonitor`.
ngscopeclient never calls it, so its own link never fails.

Calling it from a subclass is an immediate undefined reference — this was found the hard
way, by writing a link check that called it. There is no workaround short of implementing
DPI scaling ourselves in the subclass (`ImGui_ImplGlfw_GetContentScaleForWindow()` is
right there and does the job). Worth an upstream issue.

Every other member declared in `VulkanWindow.h` and `TextureManager.h` is defined.

## Unrelated trap found on the way

Anything calling `VulkanInit()` **must** call `ScopehalStaticCleanup()` before returning
from `main()`. scopehal's `PipelineCacheManager` is a static whose destructor calls
`SaveToDisk()`, which logs; by the time the C runtime unwinds statics the log sinks are
gone and it segfaults on exit. ngscopeclient does this at `main.cpp:325`. Nothing to do with
the lift, but every target in this project inherits it.

## Build hygiene notes

- The lifted sources are compiled with `-w`. The top level applies `-Wall -Wextra -Wshadow
  -Wpedantic ...` to everything, and imgui and ngscopeclient produce a large volume of it —
  noise in code we neither own nor can fix. `-Werror` (`IMCUFOSPHOR_WERROR_FLAGS`) is
  applied per-source to our own `PreferenceSchema.cpp` and to `LinkCheck.cpp`, matching the
  policy in §3.
- `COMPILE_OPTIONS` is a single list property. Two `set_source_files_properties()` calls
  naming the same file silently drop the first — which is how the config-path shim came to
  be built but not applied on the first attempt. The second one uses
  `set_property(... APPEND)`.
- `ngscopeclient-compat` exports `scopehal` PUBLIC: `ngscopeclient.h` includes
  `../scopehal/scopehal.h` through scopehal's own exported include directory, and
  `VulkanWindow.h` puts `QueueHandle` and `vk::raii` types in its interface.

## What to re-run after a submodule bump

```
cmake --build <builddir> --target ngscopeclient-compat-linkcheck && <builddir>/src/ngscopeclient-compat/ngscopeclient-compat-linkcheck
```

That covers: the `ngscopeclient.h` include graph still being satisfiable, every referenced
symbol still being defined, and the preference schema still being complete. Add `--window`
on a machine with a display.
