/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Folds a window's worth of hits into the decaying density map, and clears the hits.

	The decay is gr-fosphor's closed-form batch IIR (display.cl:241-247), which is the single
	most valuable idea in that codebase. The per-spectrum update it stands for is

		h <- h + (a/t0r)*(1 - h) - h/t0d

	a rise proportional to hit rate and weighted by (1 - h) so a cell saturates smoothly at
	one instead of clipping, against an exponential decay. Applying that n times has an exact
	closed form, so the density map is touched once per fold no matter how many spectra went
	into the window. Cost is O(bins * cells), independent of the spectrum rate - which is
	what makes a 30 000 spectra/second RTSA affordable at all.

	Two deliberate departures from fosphor:

	  - Its rate constants are counts of spectra, hardcoded at init and never exposed
	    (cl.c:714-715, t0d = 1024), so persistence silently changes with sample rate. At the
	    rates here 1024 spectra is 34 ms, against roughly 100 ms at the rates fosphor was
	    written for. Ours arrive as already-converted per-spectrum constants computed from a
	    time in seconds, so the look holds as the sample rate changes.

	  - It has a fast path that skips cells below 0.01 with no new hits (display.cl:236),
	    which leaves them frozen at whatever sub-0.01 value they held - hence the comment on
	    its clamp about the texture never being cleared. We let those cells decay to zero.
	    The cost is touching every cell every fold, which we are doing regardless.

	The hit histogram is finer than the density map, because the two want different things:
	percentile traces want resolution, and a display finer than the pane it is drawn into
	only aliases. Each output cell sums cellsPerOutput input cells.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict buffer buf_hits
{
	uint hits[];
};

layout(std430, binding=1) restrict buffer buf_density
{
	float density[];
};

layout(std430, push_constant) uniform constants
{
	//Frequency bins in one spectrum, the width of both buffers
	uint nbins;

	//Amplitude cells in the density map
	uint noutcells;

	//Hit histogram cells per density cell
	uint cellsPerOutput;

	//Spectra accumulated into this window
	uint nspectra;

	//Reciprocal of the rise time constant, per spectrum
	float invRise;

	//Reciprocal of the decay time constant, per spectrum
	float invDecay;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint bin = gl_GlobalInvocationID.x;
	uint outcell = gl_GlobalInvocationID.y;
	if( (bin >= nbins) || (outcell >= noutcells) )
		return;

	//Box reduce the hit histogram, and clear it as we go. Every input cell is read by
	//exactly one thread, so clearing here needs no separate pass and no synchronization.
	uint hc = 0;
	uint base = outcell * cellsPerOutput;
	for(uint i=0; i<cellsPerOutput; i++)
	{
		uint idx = (base + i)*nbins + bin;
		hc += hits[idx];
		hits[idx] = 0;
	}

	uint outIdx = outcell*nbins + bin;
	float h = density[outIdx];

	//An empty window would divide by zero. Decay in place instead of skipping, so a stalled
	//source fades out rather than freezing.
	if(nspectra == 0)
	{
		density[outIdx] = clamp(h, 0.0, 1.0);
		return;
	}

	//Fraction of this window's spectra that landed in this cell
	float a = float(hc) / float(nspectra);

	float b = a * invRise;
	float c = b + invDecay;
	float d = b / c;
	float e = pow(1.0 - c, float(nspectra));

	h = (h - d) * e + d;

	//Guard the recursion. c > 1 is reachable from a badly chosen rise constant, which makes
	//(1 - c) negative and pow() of it a NaN, and a NaN in a recursive IIR never leaves.
	if(!(h >= 0.0))
		h = 0.0;

	density[outIdx] = min(h, 1.0);
}
