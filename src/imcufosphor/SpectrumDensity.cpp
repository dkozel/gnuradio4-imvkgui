/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SpectrumDensity
 */

#include "SpectrumDensity.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SpectrumDensity::SpectrumDensity(const string& color)
	: Filter(color, CAT_RF)
	, m_accumulated(0)
	, m_lastWindowSpectra(0)
	, m_outputReady(false)
	, m_cachedBins(0)
	//512 histogram cells rather than the 1024 the amplitude precision argument suggests.
	//
	//Measured: trace values are identical to within 0.1 dB anywhere from 128 to 1024 cells,
	//because the trace pass interpolates within the cell a percentile lands in, so cell
	//height stops mattering long before it gets small. What does matter is that the hit
	//histogram fits in L2. At 8192 bins, 512 cells is 16 MB and 1024 cells is 32 MB, which
	//is the L2 size on this part - and crossing it costs 5x, not the 2x the extra cells
	//would suggest. See DESIGN.md section 14.
	, m_ncells(512)
	, m_noutcells(256)
	, m_accumulatePipeline("shaders/SpectrumDensityAccumulate.spv", 2, sizeof(SpectrumDensityAccumulateArgs))
	, m_tracePipeline("shaders/SpectrumDensityTraces.spv", 6, sizeof(SpectrumDensityTraceArgs))
	, m_foldPipeline("shaders/SpectrumDensityFold.spv", 2, sizeof(SpectrumDensityFoldArgs))
	, m_binsPerSpectrum(m_parameters["Bins per spectrum"])
	, m_foldInterval(m_parameters["Fold interval"])
	, m_dbMin(-100)
	, m_dbMax(0)
	, m_fracLow(0.10f)
	, m_fracMid(0.60f)
	, m_fracHigh(0.95f)
	, m_riseSec(0.010)
	, m_decaySec(0.150)
	, m_spectrumRate(0)
	, m_range(100)
	, m_offset(50)
{
	//Frequency domain in, frequency domain out. Microhertz because that is what Waterfall
	//and the rest of this pipeline use (Waterfall.cpp:71).
	m_xAxisUnit = Unit(Unit::UNIT_MICROHZ);

	//Stream 0 is the density map. There is no stream type for a frequency-by-amplitude
	//density, and SPECTROGRAM is the closest fit: a DensityFunctionWaveform-backed 2D image
	//with frequency on one axis. Nothing generic consumes it - our own SpectrumArea renders
	//it - so the mislabelled second axis costs nothing today. Worth revisiting if a generic
	//viewer ever gets pointed at this filter.
	AddStream(Unit(Unit::UNIT_DBM), "density", Stream::STREAM_TYPE_SPECTROGRAM);

	AddStream(Unit(Unit::UNIT_DBM), "mean", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit(Unit::UNIT_DBM), "median", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit(Unit::UNIT_DBM), "low", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit(Unit::UNIT_DBM), "mid", Stream::STREAM_TYPE_ANALOG);
	AddStream(Unit(Unit::UNIT_DBM), "high", Stream::STREAM_TYPE_ANALOG);

	CreateInput<InputConstraintStreamType>("din", Stream::STREAM_TYPE_ANALOG);

	m_binsPerSpectrum = FilterParameter(FilterParameter::TYPE_INT, Unit(Unit::UNIT_SAMPLEDEPTH));
	m_binsPerSpectrum.SetIntVal(0);

	//512 spectra is roughly a 60 Hz fold at the rates the LoRa recordings run at. The
	//expensive passes are per cell, not per spectrum, so this is the knob that decides how
	//much they cost.
	m_foldInterval = FilterParameter(FilterParameter::TYPE_INT, Unit(Unit::UNIT_COUNTS));
	m_foldInterval.SetIntVal(512);

	m_hits.SetGpuAccessHint(AcceleratorBuffer<uint32_t>::HINT_LIKELY);
	m_hits.SetCpuAccessHint(AcceleratorBuffer<uint32_t>::HINT_UNLIKELY);
}

SpectrumDensity::~SpectrumDensity()
{
}

string SpectrumDensity::GetProtocolName()
{
	return "Spectrum Density";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

int64_t SpectrumDensity::GetBinsPerSpectrum()
{
	auto n = m_binsPerSpectrum.GetIntVal();
	return (n < 1) ? 0 : n;
}

int64_t SpectrumDensity::GetFoldInterval()
{
	auto n = m_foldInterval.GetIntVal();
	return (n < 1) ? 1 : n;
}

float SpectrumDensity::GetVoltageRange(size_t /*stream*/)
{
	return m_range;
}

void SpectrumDensity::SetVoltageRange(float range, size_t /*stream*/)
{
	m_range = range;
}

float SpectrumDensity::GetOffset(size_t /*stream*/)
{
	return m_offset;
}

void SpectrumDensity::SetOffset(float offset, size_t /*stream*/)
{
	m_offset = offset;
}

void SpectrumDensity::SetRange(float dbMin, float dbMax)
{
	//A zero or inverted span would divide by zero when mapping to cells
	if(dbMax <= dbMin)
	{
		LogWarning("SpectrumDensity: ignoring range [%f, %f], max must exceed min\n", dbMin, dbMax);
		return;
	}

	if( (dbMin == m_dbMin) && (dbMax == m_dbMax) )
		return;

	m_dbMin = dbMin;
	m_dbMax = dbMax;

	m_range = dbMax - dbMin;
	m_offset = -(dbMax + dbMin) / 2;

	//Every cell index in both buffers refers to the old axis. gr-fosphor does not do this
	//(cl.c:940-941 just writes the new scale factors), so its display stays visibly wrong
	//for a decay time after any range change.
	ClearSweeps();
}

void SpectrumDensity::SetPersistence(double riseSec, double decaySec)
{
	m_riseSec = riseSec;
	m_decaySec = decaySec;
}

void SpectrumDensity::SetCellCounts(size_t histogramCells, size_t densityCells)
{
	if( (histogramCells == 0) || (densityCells == 0) )
	{
		LogWarning("SpectrumDensity: cell counts must be nonzero\n");
		return;
	}

	//The fold box reduces a whole number of histogram cells into each density cell. A
	//non-integer ratio would silently drop or double count the remainder.
	if(histogramCells % densityCells)
	{
		LogWarning("SpectrumDensity: %zu histogram cells is not a multiple of %zu density cells\n",
			histogramCells, densityCells);
		return;
	}

	if( (histogramCells == m_ncells) && (densityCells == m_noutcells) )
		return;

	m_ncells = histogramCells;
	m_noutcells = densityCells;

	//Force reallocation on the next Refresh; every cell index in both buffers refers to the
	//old resolution
	m_cachedBins = 0;
	ClearSweeps();
}

void SpectrumDensity::SetPercentiles(float low, float mid, float high)
{
	m_fracLow = min(1.0f, max(0.0f, low));
	m_fracMid = min(1.0f, max(0.0f, mid));
	m_fracHigh = min(1.0f, max(0.0f, high));
}

void SpectrumDensity::ClearSweeps()
{
	m_accumulated = 0;
	m_lastWindowSpectra = 0;
	m_outputReady = false;

	//Zero both accumulators on the host. This runs on a range change or a restart, never in
	//the hot path.
	if(!m_hits.empty())
	{
		m_hits.PrepareForCpuAccess();
		for(size_t i=0; i<m_hits.size(); i++)
			m_hits[i] = 0;
		m_hits.MarkModifiedFromCpu();
	}

	auto cap = dynamic_cast<DensityFunctionWaveform*>(GetData(STREAM_DENSITY));
	if(cap)
	{
		auto& buf = cap->GetOutData();
		buf.PrepareForCpuAccess();
		for(size_t i=0; i<buf.size(); i++)
			buf[i] = 0;
		buf.MarkModifiedFromCpu();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Buffer management

void SpectrumDensity::ReallocateBuffers(size_t nbins)
{
	m_cachedBins = nbins;
	m_hits.resize(nbins * m_ncells);

	//The density map is the filter's stream 0 output, so it has to be a waveform rather than
	//a bare buffer. Width is frequency, height is amplitude, which is the row major order
	//the fold shader writes and a texture upload wants.
	auto cap = new SpectrumDensityWaveform(nbins, m_noutcells);
	SetData(cap, STREAM_DENSITY);

	//Trace outputs, one dBm value per bin
	for(size_t i=STREAM_MEAN; i<STREAM_COUNT; i++)
	{
		auto trace = new UniformAnalogWaveform;
		trace->Resize(nbins);
		SetData(trace, i);
	}

	ClearSweeps();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual filter logic

void SpectrumDensity::Refresh(vk::raii::CommandBuffer& cmdBuf, [[maybe_unused]] shared_ptr<QueueHandle> queue)
{
	ClearMessages();
	m_outputReady = false;

	auto din = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(0));
	if(!din)
	{
		AddErrorMessage("Missing input", "Input is unconnected or not a uniform waveform");
		return;
	}

	size_t inlen = din->size();
	if(inlen == 0)
		return;

	//How the input block divides into spectra, same convention as SpectrumReducer
	size_t nbins = static_cast<size_t>(GetBinsPerSpectrum());
	if( (nbins == 0) || (nbins > inlen) )
		nbins = inlen;
	size_t nspectra = inlen / nbins;

	if(nspectra * nbins != inlen)
	{
		AddErrorMessage("Invalid input",
			"Input of " + to_string(inlen) + " samples is not a whole number of " +
			to_string(nbins) + "-bin spectra");
		return;
	}

	m_xAxisUnit = m_inputs[0]->GetXAxisUnits();

	if(m_cachedBins != nbins)
		ReallocateBuffers(nbins);

	auto cap = dynamic_cast<DensityFunctionWaveform*>(GetData(STREAM_DENSITY));
	if(!cap)
		return;

	const float dbSpan = m_dbMax - m_dbMin;

	//Deposit this block's hits
	{
		NamedDebugRange debugRange(cmdBuf, "SpectrumDensity accumulate");

		SpectrumDensityAccumulateArgs args;
		args.nbins = nbins;
		args.nspectra = nspectra;
		args.ncells = m_ncells;
		args.dbMin = m_dbMin;
		args.cellsPerDb = m_ncells / dbSpan;

		m_accumulatePipeline.BindBufferNonblocking(0, din->m_samples, cmdBuf);
		m_accumulatePipeline.BindBufferNonblocking(1, m_hits, cmdBuf);
		m_accumulatePipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(nbins, 64));

		m_hits.MarkModifiedFromGpu();
	}

	m_accumulated += nspectra;

	//Window not complete, so leave the density map and traces as they are
	if(m_accumulated < GetFoldInterval())
		return;

	//Traces first: the fold clears the hit histogram they read
	{
		NamedDebugRange debugRange(cmdBuf, "SpectrumDensity traces");
		ComputePipeline::AddComputeMemoryBarrier(cmdBuf);

		SpectrumDensityTraceArgs args;
		args.nbins = nbins;
		args.ncells = m_ncells;
		args.total = m_accumulated;
		args.dbMin = m_dbMin;
		args.dbPerCell = dbSpan / m_ncells;
		args.fracLow = m_fracLow;
		args.fracMid = m_fracMid;
		args.fracHigh = m_fracHigh;

		m_tracePipeline.BindBufferNonblocking(0, m_hits, cmdBuf);
		for(size_t i=STREAM_MEAN; i<STREAM_COUNT; i++)
		{
			auto trace = dynamic_cast<UniformAnalogWaveform*>(GetData(i));
			if(!trace)
				return;

			//Carry the frequency axis through unchanged. We combine spectra in time, never
			//in frequency, so bin positions are identical to the input's.
			trace->m_timescale = din->m_timescale;
			trace->m_triggerPhase = din->m_triggerPhase;
			trace->m_startTimestamp = din->m_startTimestamp;
			trace->m_startFemtoseconds = din->m_startFemtoseconds;
			trace->m_revision++;

			m_tracePipeline.BindBufferNonblocking(i - STREAM_MEAN + 1, trace->m_samples, cmdBuf, true);
			trace->MarkModifiedFromGpu();
		}

		m_tracePipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(nbins, 64));
	}

	//Fold into the decaying density map, and clear the hits
	{
		NamedDebugRange debugRange(cmdBuf, "SpectrumDensity fold");
		ComputePipeline::AddComputeMemoryBarrier(cmdBuf);

		SpectrumDensityFoldArgs args;
		args.nbins = nbins;
		args.noutcells = m_noutcells;
		args.cellsPerOutput = m_ncells / m_noutcells;
		args.nspectra = m_accumulated;

		//Time constants in seconds, converted against the spectrum rate. Without a rate we
		//cannot convert, so fall back to interpreting them as windows - wrong, but bounded,
		//and the display still decays.
		double risespectra = (m_spectrumRate > 0) ? (m_riseSec * m_spectrumRate) : m_accumulated;
		double decayspectra = (m_spectrumRate > 0) ? (m_decaySec * m_spectrumRate) : (m_accumulated * 16);

		//A rise constant below one spectrum makes (1 - c) negative and pow() of it a NaN
		risespectra = max(1.0, risespectra);
		decayspectra = max(1.0, decayspectra);

		args.invRise = 1.0 / risespectra;
		args.invDecay = 1.0 / decayspectra;

		auto& outbuf = cap->GetOutData();
		m_foldPipeline.BindBufferNonblocking(0, m_hits, cmdBuf);
		m_foldPipeline.BindBufferNonblocking(1, outbuf, cmdBuf);
		m_foldPipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(nbins, 64), m_noutcells);

		outbuf.MarkModifiedFromGpu();
	}

	cap->m_timescale = din->m_timescale;
	cap->m_triggerPhase = din->m_triggerPhase;
	cap->m_startTimestamp = din->m_startTimestamp;
	cap->m_startFemtoseconds = din->m_startFemtoseconds;
	cap->m_revision++;

	m_lastWindowSpectra = m_accumulated;
	m_accumulated = 0;
	m_outputReady = true;
}
