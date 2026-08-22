Implements a spectrum analyzer display utility to render realtime spectrum from IQ data, using SigMF data as the initial data source.

I've started with ngscopeclient and scopehal as the direct code inspiration, and the project pulls a variety of files directly from those codebases unmodified. These are BSD 3 Clause licenced.

The licencing needs to be formalized, but the intent is for all the scopehal style shaders and directly related code to be BSD 3 Clause to allow upstream usage, and for gr-imgui to be LGPL.

# Building

A fairly standard CMake superbuild. `scopehal`, `scopehal-apps` and `libsigmf` are consumed as
git submodules; `scopehal` is built, `scopehal-apps` is a source donor referenced in place. See
DESIGN.md for the architecture behind that split.

## Submodules

```
git clone git@github.com:dkozel/gr-imgui.git imcufosphor
cd imcufosphor
git submodule update --init --recursive
```

`--recursive` is required: `libsigmf` bundles flatbuffers and nlohmann/json as its own
submodules, and `scopehal` bundles VkFFT, canvas_ity, log and xptools.

Recursing over everything also initializes six `scopehal-apps` submodules the build does not
need (`doc`, `lib`, `ImGuiFileDialog`, `imgui-node-editor`, `imgui_markdown`,
`nativefiledialog-extended`). To skip them, initialize selectively instead:

```
git submodule update --init --recursive lib/scopehal lib/libsigmf
git submodule update --init lib/scopehal-apps
git -C lib/scopehal-apps submodule update --init src/imgui
```

## Dependencies

Beyond a Vulkan-capable toolchain, on Ubuntu 24.04:

```
sudo apt install cmake build-essential pkg-config \
    libyaml-cpp-dev libsigc++-3.0-dev libhidapi-dev libpng-dev zlib1g-dev \
    libglfw3-dev libvulkan-dev glslang-dev spirv-tools glslc
```

`glslc` is used to compile compute shaders in `scopehal`, `scopeprotocols` and this project.
`hidapi` is required by `xptools` even though this project talks to no HID instruments.
If the Vulkan SDK is installed and `VULKAN_SDK` is exported, the top-level `CMakeLists.txt`
prefers the SDK's packages over the distro ones.

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ln -sf build/compile_commands.json compile_commands.json   # once, for clangd
```

This produces `build/src/imcufosphor/sigmf-spectrum` (a standalone test application) and
`build/tools/wfbench/wfbench` (headless benchmark and verification harness), each with the
shaders and icons it needs copied next to the binary — there is no `install()` step, so the
binaries run from the build tree.

Useful options:

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` builds `-Og -g`, `RelWithDebInfo` builds `-O3 -g` |
| `DISABLE_PCH` | off | Disable precompiled headers |
| `SANITIZE` | off | ASan + UBSan on everything |
| `BUILD_GR4_BLOCKS` | `OFF` | Build the GNU Radio 4 blocks, see below |

`-Wall -Wextra` apply to the whole build, submodules included, but `-Werror` is applied per-target
to our own sources only as libsigmf fails.

## GNU Radio 4 blocks (optional)

`gr-imcufosphor/` wraps the display engine as GNU Radio 4 blocks. It is off by default and
gated behind `BUILD_GR4_BLOCKS`.

You must build GNU Radio 4 from source and install it to a prefix:

```
git clone https://github.com/gnuradio/gnuradio4.git
cd gnuradio4
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/opt/gnuradio4
cmake --build build -j$(nproc)
cmake --install build
```

GNU Radio 4 needs a very recent C++23 compiler. This project is built with g++-16, though 14 should
work. The system default g++-13 on Ubuntu 24.04 is not sufficient for the gnuradio4 headers.

The gnuradio4 build generates an environment script at `<gnuradio4-build>/activate.sh` which
exports `PKG_CONFIG_PATH`, `CMAKE_PREFIX_PATH`, `LD_LIBRARY_PATH` and
`GNURADIO4_PLUGIN_DIRECTORIES` for the install prefix. Source it before configuring:

```
source /path/to/gnuradio4/build/activate.sh
```

That is what makes `pkg_check_modules(GNURADIO4 REQUIRED IMPORTED_TARGET gnuradio4)` in the
top-level `CMakeLists.txt` succeed. Without it, set `PKG_CONFIG_PATH` to
`<prefix>/lib/pkgconfig` by hand.

Then configure imcufosphor with the same compiler gnuradio4 was built with — the gnuradio4
core is a static library, so a mismatched compiler will fail to link:

```
source /path/to/gnuradio4/build/activate.sh
cmake -B build-gr4 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16 \
    -DBUILD_GR4_BLOCKS=ON \
    -DGR_IMCUFOSPHOR_BOOST_UT_INCLUDE_DIR=/path/to/gnuradio4/build/projects/gnuradio4-core/_deps/ut-src/include
cmake --build build-gr4 -j$(nproc)
```

`GR_IMCUFOSPHOR_BOOST_UT_INCLUDE_DIR` is only needed for the block unit tests
(`GR_IMCUFOSPHOR_ENABLE_TESTING`, on by default). gnuradio4 fetches boost-ut into its own
build tree rather than installing it, so unless boost-ut is installed system-wide the build
has to be pointed at that copy. The exact path depends on your gnuradio4 build directory.

This adds `build-gr4/gr-imcufosphor/apps/gr4-analyzer/gr4-analyzer` and the block test suites.
Run the tests with:

```
ctest --test-dir build-gr4 --output-on-failure
```

`gr-imcufosphor/` is also structured to build standalone against an installed imcufosphor
package, but imcufosphor has no install/export rules yet, so today it only builds as a
subdirectory of this superbuild.
