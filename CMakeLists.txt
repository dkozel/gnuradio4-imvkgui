cmake_minimum_required(VERSION 3.20)
project(imcufosphor VERSION 0.1)
enable_language(CXX)

# Realtime spectrum analyzer / waterfall display for SigMF IQ recordings.
# See DESIGN.md for architecture.
#
# This superbuild is modeled on scopehal-apps/CMakeLists.txt. Both scopehal and
# scopehal-apps are consumed as unmodified submodules; scopehal is built, while
# scopehal-apps is a source donor for lifted display primitives (see DESIGN.md
# section 5). Keep the package detection below in sync with upstream's when
# bumping the submodules.

if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
	message(WARNING "32-bit platforms are unsupported.")
endif()

# Prefer Vulkan SDK packages if the SDK is present
if(DEFINED ENV{VULKAN_SDK})
	message("Detected and using VULKAN_SDK at $ENV{VULKAN_SDK}")
	cmake_path(CONVERT "$ENV{VULKAN_SDK}" TO_CMAKE_PATH_LIST VULKAN_SDK_PATH NORMALIZE)
	list(PREPEND CMAKE_PREFIX_PATH ${VULKAN_SDK_PATH})
endif()

# Needed for vk::PhysicalDevicePortabilitySubsetFeaturesKHR
add_compile_definitions(VK_ENABLE_BETA_EXTENSIONS=1)

find_package(Git)
if(Git_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --always --tags
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE IMCUFOSPHOR_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
endif()
if(NOT IMCUFOSPHOR_VERSION)
	set(IMCUFOSPHOR_VERSION "unknown")
endif()
message("-- imcufosphor version: ${IMCUFOSPHOR_VERSION}")

set(DISABLE_PCH CACHE BOOL "Disable precompiled headers")

# Emit compile_commands.json so clangd and other tooling resolve the submodule
# and pkg-config include paths. Symlink it to the source root:
#   ln -sf build/compile_commands.json compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# PCH must be disabled before any targets are created
if(DISABLE_PCH)
	set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
endif()

# Warning flags mirror scopehal-apps and apply to everything, submodules included.
#
# -Werror deliberately does NOT go here. add_compile_options() applies to every
# subdirectory added below, which would gate the build on warnings in code we do
# not own and cannot fix: gcc 13 emits a false-positive -Wstringop-overflow in
# libsigmf's bundled flatbuffers/src/reflection.cpp, and a submodule bump could
# introduce more at any time. -Werror is applied per-target to our own sources in
# src/imcufosphor/CMakeLists.txt instead.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	add_compile_options(-Wall -Wextra -Wuninitialized)
	add_compile_options(-Wshadow -Wpedantic -Wcast-align -Wwrite-strings)
	add_compile_options(-Wmissing-declarations -Wvla)
endif()

# Warnings promoted to errors for our own code only. Kept in a variable so the
# application target and any future targets stay consistent.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(IMCUFOSPHOR_WERROR_FLAGS
		-Werror
		-Wno-error=deprecated-declarations
		-Wno-error=unused-parameter
		-Wno-error=unused-variable
		-Wno-error=unused-result
		-Wno-error=shadow
		-Wno-error=uninitialized
		-Wno-error=maybe-uninitialized)
else()
	set(IMCUFOSPHOR_WERROR_FLAGS "")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
	add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Woverloaded-virtual=1>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	add_compile_options(-Woverloaded-virtual -Wpointer-sign)
	add_compile_options(-Wno-gnu-zero-variadic-macro-arguments -Wno-unknown-warning-option)
endif()

add_compile_options(-mtune=native)
add_compile_options("$<$<CONFIG:RELEASE>:-O3>")
add_compile_options("$<$<CONFIG:RELWITHDEBINFO>:-O3;-g>")
add_compile_options("$<$<CONFIG:DEBUG>:-Og;-D_DEBUG;-g>")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release")
endif()

if(SANITIZE)
	message(STATUS "SANITIZE set, enabling sanitizers")
	add_compile_options(-fsanitize=address -fsanitize=undefined)
	add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

########################################################################
# Package detection
########################################################################

find_package(PkgConfig MODULE REQUIRED)
find_package(ZLIB REQUIRED)

# sigc++ is used by scopehal's FilterParameter change signals
pkg_check_modules(SIGCXX QUIET IMPORTED_TARGET sigc++-3.0)
if(NOT SIGCXX_FOUND)
	pkg_check_modules(SIGCXX QUIET IMPORTED_TARGET sigc++-2.0)
endif()
if(NOT SIGCXX_FOUND)
	message(FATAL_ERROR "Unable to find any version of sigc++; this is required by scopehal.")
endif()

find_package(Threads MODULE REQUIRED)

find_package(OpenMP MODULE)
if(NOT OpenMP_CXX_FOUND)
	message(FATAL_ERROR "imcufosphor requires OpenMP but your C++ compiler does not appear to support it.")
endif()

# GLFW is required by scopehal's VulkanInit (window system integration probing)
# and by the lifted VulkanWindow.
find_package(glfw3 3.2 REQUIRED)

#WORKAROUND glslang does not look for SPIRV-Tools-opt but needs it on macOS, harmless elsewhere
find_package(SPIRV-Tools-opt REQUIRED)
if(DEFINED VULKAN_SDK_PATH)
	find_package(Vulkan REQUIRED COMPONENTS glslc shaderc_combined SPIRV-Tools)
	find_package(glslang REQUIRED)
	add_library(Vulkan::Loader ALIAS Vulkan::Vulkan)
else()
	find_package(glslang REQUIRED)
	find_package(VulkanHeaders REQUIRED)
	find_package(VulkanLoader QUIET)
	if(NOT VulkanLoader_FOUND)
		find_library(VulkanLoader_LIB libvulkan.so REQUIRED)
		add_library(Vulkan::Loader SHARED IMPORTED)
		set_property(TARGET Vulkan::Loader PROPERTY IMPORTED_LOCATION ${VulkanLoader_LIB})
	endif()
endif()

# shaderc provides glslc, which compiles the compute shaders
pkg_check_modules(SHADERC shaderc QUIET IMPORTED_TARGET)
if(SHADERC_FOUND)
	cmake_path(GET SHADERC_INCLUDE_DIRS PARENT_PATH SHADERC_PREFIX)
endif()
find_program(Vulkan_GLSLC_EXECUTABLE glslc HINTS SHADERC_PREFIX)
if(NOT Vulkan_GLSLC_EXECUTABLE)
	message(FATAL_ERROR "glslc not found. This is needed to compile shaders. Install shaderc/glslc or load the Vulkan SDK.")
else()
	message("-- Found glslc: ${Vulkan_GLSLC_EXECUTABLE}")
endif()

# VkFFT does not use a standard path for glslang_c_interface.h
get_target_property(glslang_INCLUDE_DIR glslang::glslang INTERFACE_INCLUDE_DIRECTORIES)

if(APPLE)
	find_library(MOLTENVK_LIBRARIES NAMES MoltenVK libMoltenVK)
	if(NOT MOLTENVK_LIBRARIES)
		message(FATAL_ERROR "MoltenVK is required on macOS.")
	endif()
endif()

if(NOT WIN32)
	include(GNUInstallDirs)
endif()

########################################################################
# Vendored upstream code
########################################################################

# scopehal, vendored into third_party/ rather than built from a submodule.
#
# What is there and why is recorded in THIRD_PARTY.md and in that directory's CMakeLists.
# The <scopehal/Filter.h> / <scopeprotocols/Waterfall.h> include style our code uses is
# unchanged - the vendored tree keeps upstream's directory shape precisely so that neither
# our includes nor scopehal's own internal relative ones had to move.
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/scopehal")

# Where the glslc steps drop their .spv output. There is no install() in this project, so
# cmake/RuntimeData.cmake copies them next to each binary instead. Pipe separated rather than
# semicolon separated so the list survives being passed to cmake -P as a single -D argument;
# see cmake/CollectShaders.cmake.
set(IMCUFOSPHOR_SHADER_SOURCE_DIRS
	"${PROJECT_BINARY_DIR}/third_party/scopehal/shaders|${PROJECT_BINARY_DIR}/third_party/ngscopeclient/shaders")

# Where our own glslc step drops its output. Named here rather than in
# src/imcufosphor/CMakeLists.txt because tools/ needs it too and is a sibling, not a child.
set(IMCUFOSPHOR_APP_SPV_DIR "${PROJECT_BINARY_DIR}/src/imcufosphor/shaders-spv")

# imcufosphor_attach_shaders() / imcufosphor_attach_icons(), used by every executable that
# touches a ComputePipeline or a colour ramp.
include("${PROJECT_SOURCE_DIR}/cmake/RuntimeData.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HeaderSelfCheck.cmake")


# libsigmf, header-only. Builds flatc from its bundled flatbuffers on first
# configure unless a system flatbuffers is available.
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/lib/libsigmf" EXCLUDE_FROM_ALL)

# Mark libsigmf's headers (and the flatbuffers/nlohmann headers it re-exports) as
# SYSTEM so their warnings do not reach our -Werror. They are heavy template code
# that emits -Wunused-parameter and -Wmissing-declarations, and they are not ours
# to fix. libsigmf declares them as plain INTERFACE includes, so promote them here
# rather than patching the submodule.
get_target_property(LIBSIGMF_INTERFACE_INCLUDES libsigmf INTERFACE_INCLUDE_DIRECTORIES)
if(LIBSIGMF_INTERFACE_INCLUDES)
	set_target_properties(libsigmf PROPERTIES
		INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LIBSIGMF_INTERFACE_INCLUDES}")
endif()


# Display primitives vendored from ngscopeclient, plus ImGui.
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/ngscopeclient")

add_subdirectory("${PROJECT_SOURCE_DIR}/src/imcufosphor")

# Development tools. Only the ones that link against the engine are built; the raw-Vulkan
# repro cases under tools/vkmintest and tools/vkbisect stay hand-built from the g++ line in
# their header comments, since their whole point is to have no dependency on this project.
add_subdirectory("${PROJECT_SOURCE_DIR}/tools")

########################################################################
# GNU Radio 4 blocks
########################################################################

option(BUILD_GR4_BLOCKS "Build the GNU Radio 4 display blocks" OFF)

if(BUILD_GR4_BLOCKS)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(GNURADIO4 REQUIRED IMPORTED_TARGET gnuradio4)

	add_subdirectory("${PROJECT_SOURCE_DIR}/gr-imcufosphor")
endif()

# scopeprotocols, xptools and log were separate libraries built from the submodule. What we
# use of them - Waterfall.cpp, TimeUtil.cpp, log.cpp and ColoredSTDLogSink.cpp - is four
# files, so they are compiled into the vendored scopehal target rather than kept as three
# more libraries. POSITION_INDEPENDENT_CODE for it is set in third_party/scopehal.
