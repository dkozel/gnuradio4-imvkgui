/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Accumulates several spectra into one, so that the waterfall line rate can be chosen
	independently of the FFT rate. See DESIGN.md sections 7.3, 8.1 and 13.

	Inputs and outputs are in dBm, matching ComplexFFTFilter's output.

	One OP_ACCUM dispatch folds every spectrum in an input block into the accumulator, rather
	than one dispatch per spectrum. At 245.76 MS/s a block holds 128 spectra, and a dispatch
	each would put the launch overhead back that batching just removed. Thread i walks the
	block at stride nbins, so neighbouring threads read neighbouring addresses and every read
	coalesces.

	The accumulator buffer is one float per frequency bin. Phase 3 widens it to a 2D hit
	count per (frequency bin x amplitude cell) for a true RTSA density map, which is why
	SpectrumReducer keeps it in its own buffer rather than reusing the output waveform.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_din
{
	float din[];
};

layout(std430, binding=1) restrict buffer buf_acc
{
	float acc[];
};

layout(std430, binding=2) restrict writeonly buffer buf_dout
{
	float dout[];
};

layout(std430, push_constant) uniform constants
{
	//Number of frequency bins in one spectrum
	uint nbins;

	//Number of consecutive spectra in the input buffer
	uint nspectra;

	//OP_ACCUM or OP_FINALIZE, see SpectrumReducer.h
	uint op;

	//MODE_MAX_HOLD or MODE_AVERAGE, see SpectrumReducer.h
	uint mode;

	//Nonzero if the accumulator is empty and must be seeded from the first spectrum rather
	//than read. Seeding beats clearing to an identity value: max hold has no representable
	//identity in dBm short of -inf.
	uint seed;

	//Applied by OP_FINALIZE. Reciprocal of the spectrum count when averaging, 1 otherwise.
	float scale;
};

#define OP_ACCUM		0
#define OP_FINALIZE		1

#define MODE_MAX_HOLD	0
#define MODE_AVERAGE	1

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint i = gl_GlobalInvocationID.x;
	if(i >= nbins)
		return;

	//Emit. For max hold scale is 1; for averaging it is 1/N, which averages the dBm values
	//rather than the underlying power. That is video averaging, the same thing a spectrum
	//analyzer's "average" detector does, and it is deliberately not a true power average -
	//use max hold if you need peak fidelity.
	if(op == OP_FINALIZE)
	{
		dout[i] = acc[i] * scale;
		return;
	}

	uint first = 0;
	float v;
	if(seed != 0)
	{
		v = din[i];
		first = 1;
	}
	else
		v = acc[i];

	if(mode == MODE_MAX_HOLD)
	{
		for(uint s=first; s<nspectra; s++)
			v = max(v, din[s*nbins + i]);
	}
	else
	{
		for(uint s=first; s<nspectra; s++)
			v += din[s*nbins + i];
	}

	acc[i] = v;
}
