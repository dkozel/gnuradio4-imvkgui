# Compute shaders owned by this project.
#
# Modeled on lib/scopehal/scopeprotocols/shaders/CMakeLists.txt, minus the install()
# step: imcufosphor is not installed, so the .spv files are consumed from the build tree.
# src/imcufosphor/CMakeLists.txt copies them next to the binary, which is where
# FindDataFile() looks first (scopehal.cpp:821-828).

function(add_compute_shaders target)
	cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")

	set(spvfiles "")

	foreach(source ${arg_SOURCES})
		get_filename_component(base ${source} NAME_WE)

		set(outfile ${CMAKE_CURRENT_BINARY_DIR}/${base}.spv)
		set(spvfiles ${spvfiles} ${outfile})

		add_custom_command(
			OUTPUT ${outfile}
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
			COMMENT "Compile shader ${base}"
			COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.0 -c ${CMAKE_CURRENT_SOURCE_DIR}/${source} -g -o ${outfile})

	endforeach()

	add_custom_target(${target}
		COMMAND ${CMAKE_COMMAND} -E true
		SOURCES ${spvfiles}
	)

endfunction()

add_compute_shaders(
	imcufosphorshaders
	SOURCES
		ComplexToLogMagnitudeShifted.glsl
		PackedComplexWindow.glsl
		SpectrumDensityAccumulate.glsl
		SpectrumDensityFold.glsl
		SpectrumDensityToneMap.glsl
		SpectrumDensityTraces.glsl
		SpectrumReduce.glsl
		SpectrumTraceToneMap.glsl
	)
