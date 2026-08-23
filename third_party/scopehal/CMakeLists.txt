# scopehal, vendored.
#
# See THIRD_PARTY.md for provenance and for what was changed. The short version: upstream's
# scopehal is 175 translation units and 78,553 lines, of which this project reached 156 - not
# because it used them, but because scopehal.cpp's DriverStaticInit(), TransportStaticInit()
# and InitializePlugins() named every driver class in the library. With those three excised,
# the closure is what is listed below.
#
# The four external dependencies that go with the drivers - yaml-cpp, hidapi, liblxi and
# libtirpc - are gone with them.

# vulkan_raii.hpp is precompiled, and its location comes from the imported target rather than
# from a Vulkan_INCLUDE_DIR variable, which the VulkanHeaders config package does not set.
get_target_property(Vulkan_INCLUDE_DIR Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)

########################################################################
# Sources

set(SCOPEHAL_SOURCES
	# GPU and compute infrastructure
	scopehal/VulkanInit.cpp
	scopehal/ComputePipeline.cpp
	scopehal/PipelineCacheManager.cpp
	scopehal/QueueManager.cpp
	scopehal/QueueHandle.cpp
	scopehal/QueueWrapper.cpp
	scopehal/VulkanFFTPlan.cpp

	# Filter graph
	scopehal/Filter.cpp
	scopehal/FilterGraphExecutor.cpp
	scopehal/FilterParameter.cpp
	scopehal/FlowGraphNode.cpp
	scopehal/InputConstraint.cpp
	scopehal/InstrumentChannel.cpp
	scopehal/OscilloscopeChannel.cpp
	scopehal/Waveform.cpp
	scopehal/DensityFunctionWaveform.cpp

	# Utility
	scopehal/scopehal.cpp
	scopehal/Unit.cpp
	scopehal/FileSystem.cpp
	scopehal/StandardColors.cpp

	# The one filter we use from scopeprotocols, which was a 211-TU shared library
	scopeprotocols/Waterfall.cpp

	# Logging, from what was a separate submodule.
	#
	# STDLogSink is here because ColoredSTDLogSink derives from it, and FILELogSink because
	# log.cpp's ParseLoggerArguments() constructs one for --logfile. Both were assessed as
	# unnecessary from the header graph and turned out to be required at link time; that is
	# what the link check exists to catch.
	log/log.cpp
	log/ColoredSTDLogSink.cpp
	log/STDLogSink.cpp
	log/FILELogSink.cpp

	# The only xptools file in the closure. Socket, UART and HID are not here, and dropping
	# HID.cpp is what removes the hard hidapi dependency upstream declares FATAL_ERROR on.
	xptools/TimeUtil.cpp
	)

########################################################################
# Guard against the file set and the source list drifting apart
#
# "Every file is here for a reason" is the whole point of vendoring, and it decays the moment
# a file can sit in the tree unreferenced. Fail the configure in either direction.

file(GLOB_RECURSE _found_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(SORT _found_sources)

set(_declared ${SCOPEHAL_SOURCES})
list(SORT _declared)

if(NOT "${_found_sources}" STREQUAL "${_declared}")
	set(_extra ${_found_sources})
	list(REMOVE_ITEM _extra ${_declared})
	set(_missing ${_declared})
	list(REMOVE_ITEM _missing ${_found_sources})
	message(FATAL_ERROR
		"third_party/scopehal source list is out of step with the files on disk.\n"
		"  On disk but not built: ${_extra}\n"
		"  Built but not on disk: ${_missing}\n"
		"Add it to SCOPEHAL_SOURCES with a note in THIRD_PARTY.md, or delete it.")
endif()

########################################################################
# The library

add_library(scopehal STATIC ${SCOPEHAL_SOURCES})

# Vendored, not ours. The project's -Wall -Wextra -Wshadow -Wpedantic set is right for code we
# wrote and noise for code we are deliberately keeping close to upstream so it stays diffable.
set_source_files_properties(${SCOPEHAL_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(scopehal
PUBLIC
	# So that both our code and scopehal's own sources can say <scopehal/Filter.h>,
	# <scopeprotocols/Waterfall.h>, <log/log.h>. The directory shape is upstream's, which is
	# what lets every internal relative include - "../scopehal/DensityFunctionWaveform.h" in
	# Waterfall.h, "../log/log.h" in scopehal.h - stay exactly as it was.
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/scopehal
	${CMAKE_CURRENT_SOURCE_DIR}/log
PRIVATE
	# VkFFT does not include glslang_c_interface.h by a standard path
	${glslang_INCLUDE_DIR}/glslang/Include
	)

target_include_directories(scopehal
SYSTEM PUBLIC
	${PROJECT_SOURCE_DIR}/lib/VkFFT/vkFFT
	)

target_link_libraries(scopehal
PUBLIC
	PkgConfig::SIGCXX
	OpenMP::OpenMP_CXX
	Vulkan::Loader
	Vulkan::Headers
	glslang::glslang
	glfw
	)

if(TARGET glslang::SPIRV)
	target_link_libraries(scopehal PUBLIC glslang::SPIRV)
endif()

target_precompile_headers(scopehal
PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/scopehal/scopehal-pch.h
	${Vulkan_INCLUDE_DIR}/vulkan/vulkan_raii.hpp
	${PROJECT_SOURCE_DIR}/lib/VkFFT/vkFFT/vkFFT.h
	)

set_property(TARGET scopehal PROPERTY POSITION_INDEPENDENT_CODE ON)

########################################################################
# Shaders

add_subdirectory(shaders)
