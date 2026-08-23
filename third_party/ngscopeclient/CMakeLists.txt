# The display primitives, vendored from ngscopeclient.
#
# Two source files. Getting them to compile in place used to take twelve: these two, plus
# ngscopeclient's 1604-line preference system, plus a 70-line schema and a 68-line
# preprocessor shim of our own to stop that system rewriting the real ngscopeclient user's
# config file. See WindowGeometry.h for what replaced it.
#
# See THIRD_PARTY.md for provenance and the full list of local modifications.

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/lib/imgui")

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
	message(FATAL_ERROR
		"lib/imgui is empty:\n"
		"    git submodule update --init lib/imgui")
endif()

# TextureManager decodes the colour ramp PNG directly.
find_package(PNG REQUIRED)

########################################################################
# ImGui
#
# Its own target: it is a third-party library we do not modify, unlike the two files below.

set(IMGUI_SOURCES
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
	${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
	${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
	)

add_library(imgui STATIC ${IMGUI_SOURCES})
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES
	COMPILE_OPTIONS "-w"
	SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(imgui SYSTEM PUBLIC
	${IMGUI_DIR}
	${IMGUI_DIR}/misc/cpp
	${IMGUI_DIR}/backends
	)

# IMGUI_DEFINE_MATH_OPERATORS decides whether ImVec2 has arithmetic operators, so it changes
# the type's interface. It must be identical in every translation unit that sees ImVec2,
# ImGui's own included, or objects disagree about the type and the linker cannot tell you.
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

target_link_libraries(imgui PUBLIC glfw Vulkan::Loader Vulkan::Headers)
set_property(TARGET imgui PROPERTY POSITION_INDEPENDENT_CODE ON)

########################################################################
# The window layer

set(NGSCOPECLIENT_SOURCES
	VulkanWindow.cpp
	TextureManager.cpp
	)

add_library(imcufosphor-window STATIC ${NGSCOPECLIENT_SOURCES})

# Vendored, and kept close to upstream so it stays diffable.
set_source_files_properties(${NGSCOPECLIENT_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(imcufosphor-window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(imcufosphor-window
PUBLIC
	scopehal
	imgui
	glfw
	PNG::PNG
	ZLIB::ZLIB
	)

set_property(TARGET imcufosphor-window PROPERTY POSITION_INDEPENDENT_CODE ON)

# Same drift guard as third_party/scopehal. LinkCheck.cpp is ours, not vendored, and is
# built as its own executable below.
file(GLOB _found RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM _found LinkCheck.cpp)
list(SORT _found)
set(_declared ${NGSCOPECLIENT_SOURCES})
list(SORT _declared)
if(NOT "${_found}" STREQUAL "${_declared}")
	message(FATAL_ERROR
		"third_party/ngscopeclient: on disk ${_found}, declared ${_declared}")
endif()

########################################################################
# Shaders

add_subdirectory(shaders)

########################################################################
# Link check
#
# A static library that compiles proves nothing about linking. This forces the linker to
# resolve everything the window layer references. It does no windowing without --window, so
# it is safe on a machine with no display.

add_executable(window-linkcheck LinkCheck.cpp)
target_compile_options(window-linkcheck PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})
target_link_libraries(window-linkcheck imcufosphor-window)

add_test(NAME window-linkcheck COMMAND window-linkcheck)
