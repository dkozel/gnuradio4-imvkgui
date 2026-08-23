# The ngscopeclient compute shaders this project uses.
#
# Upstream builds nine tone mappers and sixteen waveform-compute rasterizer variants. We use
# WaterfallToneMap plus two of the sixteen: the analog dense rasterizer, in its int64 and
# non-int64 forms, chosen at runtime from g_hasShaderInt64 (SpectrumArea.cpp:211).
#
# The two rasterizer variants are the same source compiled with different -D flags, which is
# upstream's scheme. Upstream derived those flags by pattern matching on the output filename;
# writing them out is the same information without the guessing, and with two variants there
# is nothing a loop would buy.

set(GLSLC ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.0 -g)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/WaterfallToneMap.spv
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/WaterfallToneMap.glsl
	COMMENT "Compile shader WaterfallToneMap"
	COMMAND ${GLSLC} -c ${CMAKE_CURRENT_SOURCE_DIR}/WaterfallToneMap.glsl
		-o ${CMAKE_CURRENT_BINARY_DIR}/WaterfallToneMap.spv
	VERBATIM)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.dense.spv
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/waveform-compute.glsl
	COMMENT "Compile shader waveform-compute.analog.dense"
	COMMAND ${GLSLC} -DANALOG_PATH -DDENSE_PACK
		-c ${CMAKE_CURRENT_SOURCE_DIR}/waveform-compute.glsl
		-o ${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.dense.spv
	VERBATIM)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.int64.dense.spv
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/waveform-compute.glsl
	COMMENT "Compile shader waveform-compute.analog.int64.dense"
	COMMAND ${GLSLC} -DANALOG_PATH -DHAS_INT64 -DDENSE_PACK
		-c ${CMAKE_CURRENT_SOURCE_DIR}/waveform-compute.glsl
		-o ${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.int64.dense.spv
	VERBATIM)

add_custom_target(ngshaders ALL SOURCES
	${CMAKE_CURRENT_BINARY_DIR}/WaterfallToneMap.spv
	${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.dense.spv
	${CMAKE_CURRENT_BINARY_DIR}/waveform-compute.analog.int64.dense.spv
	)
