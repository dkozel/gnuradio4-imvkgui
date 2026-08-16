/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Colourizes up to five rasterized traces into one RGBA texture.

	ngscopeclient gives every channel its own full-pane rgba32f texture and lets ImGui stack
	them (WaveformArea.cpp:2588). That is the right shape when channels come and go and each
	owns its own state, but here the five traces are always the same five, always the same
	size, and always drawn together. Compositing them in one pass costs one texture instead
	of five - 23 MB against 115 MB at 1600x900 - and one dispatch instead of five.

	Intensity is shaped the same way WaveformToneMap.glsl does it, with a fourth root, so a
	trace that only clips a pixel still shows rather than disappearing into the background.

	Traces are composited source-over in index order, so the later ones win where they
	overlap. Accumulation is premultiplied because that is the only form in which repeated
	source-over is associative; the result is un-premultiplied at the end because ImGui
	blends with straight alpha.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_r0 { float r0[]; };
layout(std430, binding=1) restrict readonly buffer buf_r1 { float r1[]; };
layout(std430, binding=2) restrict readonly buffer buf_r2 { float r2[]; };
layout(std430, binding=3) restrict readonly buffer buf_r3 { float r3[]; };
layout(std430, binding=4) restrict readonly buffer buf_r4 { float r4[]; };

layout(binding=5, rgba32f) uniform image2D outputTex;

layout(std430, push_constant) uniform constants
{
	uint width;
	uint height;

	//Bit per trace: set means composite it, clear means skip. Skipping rather than clearing
	//the raster buffer means hiding a trace costs nothing.
	uint mask;

	uint pad;

	//rgb used, a ignored. vec4 rather than vec3 because std430 pads vec3 to 16 bytes anyway.
	vec4 colors[5];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

//Source-over one trace onto the premultiplied accumulator
void Blend(inout vec4 acc, float pixval, vec3 color, bool visible)
{
	if(!visible)
		return;

	float a = clamp(pow(pixval, 1.0 / 4), 0.0, 1.0);
	acc.rgb = color * a + acc.rgb * (1.0 - a);
	acc.a = a + acc.a * (1.0 - a);
}

void main()
{
	uint x = gl_GlobalInvocationID.x;
	uint y = gl_GlobalInvocationID.y;
	if( (x >= width) || (y >= height) )
		return;

	uint npix = y*width + x;

	//Unrolled: GLSL cannot index an array of storage buffers without descriptor indexing,
	//and five bindings is not worth requiring the extension for.
	vec4 acc = vec4(0, 0, 0, 0);
	Blend(acc, r0[npix], colors[0].rgb, (mask & 1u) != 0u);
	Blend(acc, r1[npix], colors[1].rgb, (mask & 2u) != 0u);
	Blend(acc, r2[npix], colors[2].rgb, (mask & 4u) != 0u);
	Blend(acc, r3[npix], colors[3].rgb, (mask & 8u) != 0u);
	Blend(acc, r4[npix], colors[4].rgb, (mask & 16u) != 0u);

	//Back to straight alpha for ImGui
	if(acc.a > 0)
		acc.rgb /= acc.a;

	imageStore(outputTex, ivec2(x, y), acc);
}
