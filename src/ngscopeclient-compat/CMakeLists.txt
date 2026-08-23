# Layer 2 - display primitives lifted from ngscopeclient.
#
# See DESIGN.md section 5 (risk R2) and notes/R2-lifted-primitives.md.
#
# Every upstream source listed below is referenced in place under
# ${SCOPEHAL_APPS_DIR} and never copied (decision D4). Nothing under lib/ is
# modified: where upstream needs something we do not want, it is supplied from
# this directory as a shim, a wrapper or a subclass.
#
# The whole inherited include graph is declared here and nowhere else, so the
# cost of the lift stays visible in one file.

set(NGSCOPECLIENT_DIR "${SCOPEHAL_APPS_DIR}/src/ngscopeclient")
set(NG_IMGUI_DIR "${SCOPEHAL_APPS_DIR}/src/imgui")

if(NOT EXISTS "${NG_IMGUI_DIR}/imgui.cpp")
	message(FATAL_ERROR
		"lib/scopehal-apps/src/imgui is empty. The lifted display primitives need it:\n"
		"    git -C lib/scopehal-apps submodule update --init src/imgui")
endif()

# libpng is needed by TextureManager, which decodes icon/ramp PNGs directly.
find_package(PNG REQUIRED)

########################################################################
# Upstream sources, referenced in place

# ngscopeclient's imgui fork. Only the core, the two backends named by
# ngscopeclient.h, and the std::string helper are needed; the node editor, file
# dialog, markdown and native-file-dialog submodules are not, so they stay
# uninitialized. imgui_demo.cpp is upstream's own demo and is not built.
set(NGSC_COMPAT_IMGUI_SOURCES
	${NG_IMGUI_DIR}/imgui.cpp
	${NG_IMGUI_DIR}/imgui_draw.cpp
	${NG_IMGUI_DIR}/imgui_tables.cpp
	${NG_IMGUI_DIR}/imgui_widgets.cpp
	${NG_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
	${NG_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
	${NG_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
	)

# The primitives themselves, plus the preference machinery they depend on.
#
# Preference.cpp and PreferenceTree.cpp are not named in DESIGN.md section 5 but
# are non-optional: PreferenceManager.h includes PreferenceTree.h, and the
# Preference / PreferenceCategory implementations live in those two files.
# PreferenceSchema.cpp is deliberately absent - ours replaces it, below.
set(NGSC_COMPAT_NGSCOPECLIENT_SOURCES
	${NGSCOPECLIENT_DIR}/VulkanWindow.cpp
	${NGSCOPECLIENT_DIR}/TextureManager.cpp
	${NGSCOPECLIENT_DIR}/PreferenceManager.cpp
	${NGSCOPECLIENT_DIR}/Preference.cpp
	${NGSCOPECLIENT_DIR}/PreferenceTree.cpp
	)

########################################################################
# The library

add_library(ngscopeclient-compat STATIC
	${NGSC_COMPAT_IMGUI_SOURCES}
	${NGSC_COMPAT_NGSCOPECLIENT_SOURCES}

	# Ours, not upstream's.
	PreferenceSchema.cpp
	)

# The top level sets -Wall -Wextra -Wshadow -Wpedantic etc for everything. That
# is right for our code and useless noise for imgui and ngscopeclient, which we
# neither own nor can fix. Silence the referenced upstream sources; our own
# PreferenceSchema.cpp keeps the project warning flags and gets -Werror.
set_source_files_properties(
	${NGSC_COMPAT_IMGUI_SOURCES}
	${NGSC_COMPAT_NGSCOPECLIENT_SOURCES}
	PROPERTIES
	COMPILE_OPTIONS "-w"
	SKIP_PRECOMPILE_HEADERS ON)

# PreferenceManager::DeterminePath() hardcodes ~/.config/ngscopeclient. Left
# alone, our process would load, rewrite and truncate the real ngscopeclient
# user's preference file with our nine-preference schema on exit. We cannot
# patch the submodule, so redirect it with a preprocessor shim applied to that
# one translation unit. See ConfigPathShim.h.
#
# APPEND, not a second set_source_files_properties(): COMPILE_OPTIONS is a
# single list property and a plain set would silently drop the -w above.
set_property(SOURCE ${NGSCOPECLIENT_DIR}/PreferenceManager.cpp APPEND
	PROPERTY COMPILE_OPTIONS "-include" "${CMAKE_CURRENT_SOURCE_DIR}/ConfigPathShim.h")

set_source_files_properties(PreferenceSchema.cpp
	PROPERTIES
	COMPILE_OPTIONS "${IMCUFOSPHOR_WERROR_FLAGS}")

target_include_directories(ngscopeclient-compat
PUBLIC
	# So that consumers can say #include "VulkanWindow.h". The lifted .cpp files
	# do not need this - their own quoted includes resolve relative to their
	# directory - but our subclasses and schema do.
	${NGSCOPECLIENT_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	)

target_include_directories(ngscopeclient-compat
SYSTEM PUBLIC
	${NG_IMGUI_DIR}
	${NG_IMGUI_DIR}/misc/cpp
	${NG_IMGUI_DIR}/backends
	)

# IMGUI_DEFINE_MATH_OPERATORS decides whether ImVec2 has arithmetic operators, so it changes
# the type's interface. It used to be a #define at ngscopeclient.h:37, which meant it was on
# for any translation unit that happened to include that umbrella and off for the rest -
# including imgui's own sources. Objects then disagree about what ImVec2 is, and the linker
# cannot diagnose it. As a PUBLIC definition on this target it is on everywhere or nowhere.
target_compile_definitions(ngscopeclient-compat PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

# scopehal is PUBLIC because ngscopeclient.h includes ../scopehal/scopehal.h,
# which resolves through scopehal's own exported include directory, and because
# VulkanWindow.h exposes QueueHandle and vk::raii types in its interface.
target_link_libraries(ngscopeclient-compat
PUBLIC
	scopehal
	glfw
	PNG::PNG
	ZLIB::ZLIB
	)

if(LINUX)
	target_link_libraries(ngscopeclient-compat PUBLIC X11)
endif()

set_property(TARGET ngscopeclient-compat PROPERTY POSITION_INDEPENDENT_CODE ON)

########################################################################
# Link check
#
# A static library that compiles proves nothing about linking, and R2 is a
# question about the link too. This executable forces the linker to resolve
# every symbol the lifted primitives pull in. It does no windowing by default -
# this machine may have no display - so running it is always safe and fast.

add_executable(ngscopeclient-compat-linkcheck LinkCheck.cpp)
target_compile_options(ngscopeclient-compat-linkcheck PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})
target_link_libraries(ngscopeclient-compat-linkcheck ngscopeclient-compat)

# Registered with ctest rather than left to be run by hand. Without --window it opens
# nothing and exits immediately, so it is safe on a headless build machine, and it is the
# only thing that would notice the preference schema going out of step with what
# VulkanWindow asks for - which is a startup crash, not a compile error.
add_test(NAME window-linkcheck COMMAND ngscopeclient-compat-linkcheck)
