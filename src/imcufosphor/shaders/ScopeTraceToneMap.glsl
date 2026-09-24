/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Colourizes one rasterized scope trace onto the shared output texture.

	SpectrumTraceToneMap.glsl composites all of its traces in a single pass, because there are
	always exactly five of them. A scope has as many traces as the flowgraph gave it ports - up to
	thirty two - and GLSL cannot index an array of storage buffers without descriptor indexing.
	Requiring that extension to save some dispatches is a poor trade when the alternative is one
	dispatch per trace with a compute barrier between, which at a few dozen traces is still far
	below the cost of rasterizing them.

	So this is the same blend, done incrementally. The accumulator lives in the output image
	between dispatches, held premultiplied because that is the only form in which repeated
	source-over is associative, and is converted back to straight alpha by whichever dispatch is
	told it is the last one. Doing that conversion on every pass and undoing it on the next would
	work too, and would cost a divide and a multiply per pixel per trace to save one bit of state.

	Intensity is shaped with a fourth root, matching WaveformToneMap.glsl, so a trace that only
	clips a pixel still shows rather than vanishing into the background.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_raster { float raster[]; };

layout(binding=1, rgba32f) uniform image2D outputTex;

layout(std430, push_constant) uniform constants
{
	uint width;
	uint height;

	//Bit 0: this is the first trace, so start from a cleared accumulator rather than reading one.
	//Bit 1: this is the last trace, so convert to straight alpha on the way out.
	//Both are set when only one trace is visible.
	uint flags;

	uint pad;

	//rgb used, a ignored. vec4 rather than vec3 because std430 pads vec3 to 16 bytes anyway.
	vec4 color;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint x = gl_GlobalInvocationID.x;
	uint y = gl_GlobalInvocationID.y;
	if( (x >= width) || (y >= height) )
		return;

	uint npix = y*width + x;

	//Clearing on the first trace rather than in a separate pass is what lets a frame that draws
	//one trace cost exactly one dispatch.
	vec4 acc = ((flags & 1u) != 0u)
		? vec4(0, 0, 0, 0)
		: imageLoad(outputTex, ivec2(x, y));

	float a = clamp(pow(raster[npix], 1.0 / 4), 0.0, 1.0);
	acc.rgb = color.rgb * a + acc.rgb * (1.0 - a);
	acc.a = a + acc.a * (1.0 - a);

	//Back to straight alpha for ImGui, once, at the end
	if( ((flags & 2u) != 0u) && (acc.a > 0) )
		acc.rgb /= acc.a;

	imageStore(outputTex, ivec2(x, y), acc);
}
