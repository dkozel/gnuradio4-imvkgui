# The scopehal compute shaders this project uses.
#
# Four, of the roughly 105 that upstream's scopehal and scopeprotocols build between them.
# Three are the complex window functions ComplexFFTFilter dispatches (its own postprocess is
# app-local, see DESIGN.md 7.2); the fourth is the one Waterfall.cpp names.
#
# Same glslc invocation as upstream, minus the install() step - this project has no install,
# and cmake/RuntimeData.cmake copies the .spv next to each binary instead.

set(SCOPEHAL_SHADERS
	ComplexBlackmanHarrisWindow.glsl
	ComplexCosineSumWindow.glsl
	ComplexRectangularWindow.glsl
	WaterfallFilter.glsl
	)

set(spvfiles "")
foreach(source ${SCOPEHAL_SHADERS})
	get_filename_component(base ${source} NAME_WE)
	set(outfile ${CMAKE_CURRENT_BINARY_DIR}/${base}.spv)
	list(APPEND spvfiles ${outfile})

	add_custom_command(
		OUTPUT ${outfile}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
		COMMENT "Compile shader ${base}"
		COMMAND ${Vulkan_GLSLC_EXECUTABLE}
			--target-env=vulkan1.0 -c ${CMAKE_CURRENT_SOURCE_DIR}/${source} -g -o ${outfile}
		VERBATIM)
endforeach()

add_custom_target(halshaders ALL SOURCES ${spvfiles})

# Same drift guard as the source list one directory up.
file(GLOB _found RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.glsl")
list(SORT _found)
set(_declared ${SCOPEHAL_SHADERS})
list(SORT _declared)
if(NOT "${_found}" STREQUAL "${_declared}")
	message(FATAL_ERROR
		"third_party/scopehal/shaders: on disk ${_found}, declared ${_declared}")
endif()
