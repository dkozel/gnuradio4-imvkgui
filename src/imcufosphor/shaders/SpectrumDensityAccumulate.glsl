/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Deposits one hit per (frequency bin, amplitude cell) for every spectrum in a block.

	This is the accumulator both display modes are built on: colormapping it gives the
	fosphor-style RTSA density, and a cumulative sum down a column gives percentile and mean
	traces. See DESIGN.md section 10.

	One thread owns one frequency column for the whole block, so nothing is shared between
	threads and there are no atomics. gr-fosphor needs them (display.cl:170-177, three
	build-time variants including a hand-rolled spin loop for hardware without local atomics)
	only because it buckets sixteen frequency bins into one workgroup's shared memory. Owning
	a column outright is both simpler and faster, and it costs nothing here because a block
	already holds far more spectra than the GPU has threads to spare.

	Layout is cell-major - hits[cell*nbins + bin] - for two reasons. Adjacent threads then
	write adjacent addresses, so the scatter coalesces, and it is already the row-major order
	a texture upload wants, with a row per amplitude cell.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_din
{
	float din[];
};

layout(std430, binding=1) restrict buffer buf_hits
{
	uint hits[];
};

layout(std430, push_constant) uniform constants
{
	//Frequency bins in one spectrum
	uint nbins;

	//Consecutive spectra in the input block
	uint nspectra;

	//Amplitude cells spanning the dB range
	uint ncells;

	//Bottom of the amplitude axis, dBm
	float dbMin;

	//Cells per dB, i.e. ncells / (dbMax - dbMin)
	float cellsPerDb;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint bin = gl_GlobalInvocationID.x;
	if(bin >= nbins)
		return;

	for(uint s=0; s<nspectra; s++)
	{
		float v = din[s*nbins + bin];

		//Map to an amplitude cell.
		//
		//Written with negated comparisons so that a NaN takes the first branch instead of
		//falling through to the int conversion. Both are reachable: the postprocess shader
		//emits log10 of a magnitude, so an exactly-zero bin gives -inf, and -inf or NaN
		//converted to int is undefined behaviour that in practice lands somewhere arbitrary
		//in the buffer. gr-fosphor guards the same hazard on its trace IIRs
		//(display.cl:206-207), where a single NaN is unrecoverable.
		//
		//Out-of-range values clamp into the end cells rather than being dropped, so every
		//spectrum deposits exactly one hit in every column. That keeps each column's total
		//equal to the spectrum count, which is what lets the trace pass find percentiles in
		//a single sweep with no separate pass to total the column.
		uint cell;
		if(!(v > dbMin))
			cell = 0;
		else
		{
			float t = (v - dbMin) * cellsPerDb;
			if(!(t < float(ncells)))
				cell = ncells - 1;
			else
				cell = uint(t);
		}

		//No atomic: this thread is the only writer of this column
		hits[cell*nbins + bin] += 1;
	}
}
