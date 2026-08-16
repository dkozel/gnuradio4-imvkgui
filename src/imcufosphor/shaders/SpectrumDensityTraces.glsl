/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/*
	Extracts mean, median and three configurable percentile traces from the hit histogram.

	One thread per frequency bin, walking its column once. Every trace comes out of that one
	sweep, so adding another percentile costs a comparison rather than a pass.

	Percentiles rather than a true min and max is the point rather than an approximation of
	one: a 95th percentile cannot be pinned by a single outlier spectrum the way a true max
	can, so the trace shows what the signal is doing rather than what it did once. True
	extremes are still available as the 0th and 100th percentiles if that is what is wanted.

	Values are interpolated within the cell they land in, so the traces are not quantized to
	the cell height. Without that, a 1024-cell histogram over 100 dB would step in 0.098 dB
	increments, which is visible on a slow-moving noise floor.

	The mean is a mean of dB values, i.e. video averaging - the same convention
	SpectrumReducer's average mode uses, and the same one gr-fosphor uses throughout
	(display.cl:136 works in log10 of magnitude). It reads roughly 2.5 dB low against a true
	power average on Gaussian noise. That is correct for a display and wrong for a
	measurement; a power average cannot be recovered from a dB histogram at all and would
	need its own linear accumulator ahead of the log.
 */

#version 430
#pragma shader_stage(compute)

layout(std430, binding=0) restrict readonly buffer buf_hits
{
	uint hits[];
};

layout(std430, binding=1) restrict writeonly buffer buf_mean
{
	float traceMean[];
};

layout(std430, binding=2) restrict writeonly buffer buf_median
{
	float traceMedian[];
};

layout(std430, binding=3) restrict writeonly buffer buf_low
{
	float traceLow[];
};

layout(std430, binding=4) restrict writeonly buffer buf_mid
{
	float traceMid[];
};

layout(std430, binding=5) restrict writeonly buffer buf_high
{
	float traceHigh[];
};

layout(std430, push_constant) uniform constants
{
	//Frequency bins in one spectrum
	uint nbins;

	//Amplitude cells spanning the dB range
	uint ncells;

	//Spectra accumulated into the histogram since it was last cleared. Every spectrum
	//deposits exactly one hit per column, so this is also each column's total.
	uint total;

	//Bottom of the amplitude axis, dBm
	float dbMin;

	//dB per amplitude cell
	float dbPerCell;

	//Fractions in 0..1 for the three configurable traces
	float fracLow;
	float fracMid;
	float fracHigh;
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

//Converts a fractional cell position to dBm, measured from the bottom edge of cell zero
float CellToDb(float cell)
{
	return dbMin + cell * dbPerCell;
}

void main()
{
	uint bin = gl_GlobalInvocationID.x;
	if(bin >= nbins)
		return;

	//Nothing accumulated yet. Leaving the previous trace in place would be worse: it would
	//look like live data.
	if(total == 0)
	{
		traceMean[bin] = dbMin;
		traceMedian[bin] = dbMin;
		traceLow[bin] = dbMin;
		traceMid[bin] = dbMin;
		traceHigh[bin] = dbMin;
		return;
	}

	float ftotal = float(total);

	//Targets in units of hits. Sorted ascending so one upward sweep can satisfy all of them.
	float tLow = fracLow * ftotal;
	float tMid = fracMid * ftotal;
	float tHigh = fracHigh * ftotal;
	float tMedian = 0.5 * ftotal;

	float outLow = dbMin;
	float outMid = dbMin;
	float outHigh = dbMin;
	float outMedian = dbMin;
	bool haveLow = false;
	bool haveMid = false;
	bool haveHigh = false;
	bool haveMedian = false;

	float moment = 0;
	float cum = 0;

	for(uint c=0; c<ncells; c++)
	{
		float n = float(hits[c*nbins + bin]);
		if(n == 0)
			continue;

		//First moment, against cell centres
		moment += n * (float(c) + 0.5);

		float next = cum + n;

		//Linear interpolation across the cell the target falls in: the target sits a
		//fraction (target - cum) / n of the way through this cell's hits.
		if(!haveLow && (next >= tLow))
		{
			outLow = CellToDb(float(c) + (tLow - cum) / n);
			haveLow = true;
		}
		if(!haveMedian && (next >= tMedian))
		{
			outMedian = CellToDb(float(c) + (tMedian - cum) / n);
			haveMedian = true;
		}
		if(!haveMid && (next >= tMid))
		{
			outMid = CellToDb(float(c) + (tMid - cum) / n);
			haveMid = true;
		}
		if(!haveHigh && (next >= tHigh))
		{
			outHigh = CellToDb(float(c) + (tHigh - cum) / n);
			haveHigh = true;
		}

		cum = next;
	}

	//A target of exactly 1.0 can fall past the last hit through rounding, so pin anything
	//still unresolved to the top of the occupied range rather than leaving it at dbMin
	if(!haveHigh)
		outHigh = CellToDb(float(ncells));
	if(!haveMid)
		outMid = CellToDb(float(ncells));
	if(!haveMedian)
		outMedian = CellToDb(float(ncells));
	if(!haveLow)
		outLow = CellToDb(float(ncells));

	traceMean[bin] = CellToDb(moment / ftotal);
	traceMedian[bin] = outMedian;
	traceLow[bin] = outLow;
	traceMid[bin] = outMid;
	traceHigh[bin] = outHigh;
}
