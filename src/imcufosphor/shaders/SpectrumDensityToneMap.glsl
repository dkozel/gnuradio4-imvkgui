/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Colourizes the RTSA density map.

	This is ngscopeclient's WaterfallToneMap.glsl with the ring-buffer row rotation removed
	and a floor and gain added. The rotation is meaningless here - a density map is a plain
	image, not a scrolling history - and the floor is what makes the display legible.

	Why a floor. A noise bin's power is exponentially distributed, so in dB it has a long
	tail toward negative infinity and a sharp upper edge. Every cell that tail ever visits
	picks up a small nonzero value and holds it for a decay time, so a continuous colour ramp
	renders a wide dim haze below the noise floor that swamps the band shape. gr-fosphor hits
	the same problem and solves it in its palette: the bottom sixteenth of its ramp is a dark
	desaturated band with a hard discontinuity at the top of it (gl_cmap_gen.c:150-179), which
	crushes the haze to near-black.

	Doing it here rather than in a palette keeps the ramp swappable and the threshold
	adjustable, which matters because the right threshold depends on the decay constant and
	on how many spectra land in a window.

	Occupancy at or below the floor is written fully transparent, not black, so the pane
	background shows through and the traces drawn over the top stay legible.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_density
{
	float density[];
};

layout(binding=1, rgba32f) uniform image2D outputTex;
layout(binding=2) uniform sampler2D colorRamp;

layout(std430, push_constant) uniform constants
{
	//Density map dimensions: frequency bins by amplitude cells
	uint width;
	uint height;

	//Output texture width. Height always equals the map height, so the amplitude axis is
	//magnified by the display rather than resampled here.
	uint outwidth;

	//Input bin at output pixel zero. Fractional, and may sit outside the buffer when the
	//view is panned past the end of the data.
	float binOffset;

	//Input bins per output pixel
	float xscale;

	//Occupancy at or below this is invisible
	float floorLevel;

	//Applied after the floor is subtracted and the remainder renormalized
	float gain;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint x = gl_GlobalInvocationID.x;
	uint y = gl_GlobalInvocationID.y;
	if( (x >= outwidth) || (y >= height) )
		return;

	//Take the strongest input bin covering this pixel, so a narrow signal survives being
	//zoomed out instead of being averaged away. Same reasoning as WaterfallToneMap.glsl.
	//
	//Signed, because a panned view can start before bin zero. Doing this in uint - as
	//WaterfallToneMap.glsl does - makes a negative start wrap to four billion and the
	//subsequent clamp pins the whole left edge to the last bin.
	float fstart = float(x) * xscale + binOffset;
	int istart = int(floor(fstart));
	int iend = int(floor(fstart + xscale));

	//Nothing to show outside the data
	if( (iend < 0) || (istart >= int(width)) )
	{
		imageStore(outputTex, ivec2(x, y), vec4(0, 0, 0, 0));
		return;
	}

	//Bound the work when zoomed far out
	const int maxbins = 256;
	if( (iend - istart) > maxbins)
		iend = istart + maxbins;
	istart = clamp(istart, 0, int(width) - 1);
	iend = clamp(iend, 0, int(width) - 1);

	float pixval = 0;
	for(int i=istart; i<=iend; i++)
		pixval = max(pixval, density[y*width + uint(i)]);

	//Floor, renormalize, gain
	float v = (pixval - floorLevel) / max(1e-6, 1.0 - floorLevel);
	v = v * gain;

	vec4 colorOut;
	if(v <= 0)
		colorOut = vec4(0, 0, 0, 0);
	else
	{
		//Half a texel in, so the lookup lands in the middle of the first ramp entry rather
		//than on its edge
		float t = min(v, 0.99);
		colorOut = texture(colorRamp, vec2(t + (0.5 / 255.0), 0.5));
	}

	imageStore(outputTex, ivec2(x, y), colorOut);
}
