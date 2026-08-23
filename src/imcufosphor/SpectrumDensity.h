/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of SpectrumDensity
 */
#ifndef SpectrumDensity_h
#define SpectrumDensity_h

#include <scopehal/scopehal.h>
#include <scopehal/Filter.h>
#include <scopehal/ComputePipeline.h>
#include <scopehal/DensityFunctionWaveform.h>

/**
	@brief Push constants for shaders/SpectrumDensityAccumulate.spv
 */
struct SpectrumDensityAccumulateArgs
{
	uint32_t nbins;
	uint32_t nspectra;
	uint32_t ncells;
	float dbMin;
	float cellsPerDb;
};

/**
	@brief Push constants for shaders/SpectrumDensityTraces.spv
 */
struct SpectrumDensityTraceArgs
{
	uint32_t nbins;
	uint32_t ncells;
	uint32_t total;
	float dbMin;
	float dbPerCell;
	float fracLow;
	float fracMid;
	float fracHigh;
};

/**
	@brief Push constants for shaders/SpectrumDensityFold.spv
 */
struct SpectrumDensityFoldArgs
{
	uint32_t nbins;
	uint32_t noutcells;
	uint32_t cellsPerOutput;
	uint32_t nspectra;
	float invRise;
	float invDecay;
};

/**
	@brief The density map itself

	DensityFunctionWaveform leaves FreeGpuMemory() and HasGpuBuffer() pure, so it needs a
	concrete subclass. WaterfallWaveform (Waterfall.h:51-75) is the same three lines plus a
	write row, which we have no use for: a waterfall scrolls, whereas every cell of this map
	is rewritten on every fold.
 */
class SpectrumDensityWaveform : public DensityFunctionWaveform
{
public:
	SpectrumDensityWaveform(size_t width, size_t height)
		: DensityFunctionWaveform(width, height)
	{}

	virtual void FreeGpuMemory() override
	{}

	virtual bool HasGpuBuffer() override
	{ return false; }
};

/**
	@brief A realtime spectrum analyzer density map, and the traces derived from it

	Both of the displays we want are views of one accumulator. A hit histogram over
	(frequency bin x amplitude cell) is the fosphor-style RTSA density when colormapped, and
	a cumulative sum down each column yields percentile and mean traces. Computing them
	separately would cost twice as much and let the two disagree about what the signal did.

	@par Two cadences

	Hits accumulate on every Refresh(), i.e. once per acquisition block. The expensive
	per-cell work - folding into the decaying density map and sweeping the columns for traces
	- happens only when a window's worth of spectra has arrived, which is what
	GetFoldInterval() sets. That keeps the O(bins x cells) passes at display rate rather than
	block rate.

	Traces are therefore exact over a defined window, while the density map decays
	continuously across windows. Both are derived from the same hits, so they cannot
	disagree.

	Like SpectrumReducer, this does not produce new output on every Refresh(). Check
	IsOutputReady().

	@par Input

	One ComplexFFTFilter output waveform, holding a whole block of spectra back to back.
	"Bins per spectrum" says how long each one is, matching SpectrumReducer's parameter of
	the same name; zero means the input is a single spectrum.

	@par Streams

	Stream 0 is the density map, as a DensityFunctionWaveform of nbins x display cells.
	The remainder are analog traces in dBm sharing the input's frequency axis, so they can be
	fed straight to the trace rasterizer.
 */
class SpectrumDensity : public Filter
{
public:
	SpectrumDensity(const std::string& color);
	virtual ~SpectrumDensity();

	///@brief Output stream indices
	enum StreamIndex
	{
		STREAM_DENSITY = 0,
		STREAM_MEAN,
		STREAM_MEDIAN,
		STREAM_LOW,
		STREAM_MID,
		STREAM_HIGH,

		STREAM_COUNT
	};

	static std::string GetProtocolName();

	virtual void Refresh(vk::raii::CommandBuffer& cmdBuf, std::shared_ptr<QueueHandle> queue) override;
	virtual void ClearSweeps() override;

	virtual float GetVoltageRange(size_t stream) override;
	virtual void SetVoltageRange(float range, size_t stream) override;
	virtual float GetOffset(size_t stream) override;
	virtual void SetOffset(float offset, size_t stream) override;

	/**
		@brief True if the most recent Refresh() completed a window

		The density map and the traces are only meaningful after this has been true once.
	 */
	bool IsOutputReady() const
	{ return m_outputReady; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Configuration

	///@brief Bins in one input spectrum, or zero if the input is a single spectrum
	int64_t GetBinsPerSpectrum();

	void SetBinsPerSpectrum(int64_t bins)
	{ m_binsPerSpectrum.SetIntVal(bins); }

	///@brief Spectra per fold, i.e. the trace window
	int64_t GetFoldInterval();

	void SetFoldInterval(int64_t spectra)
	{ m_foldInterval.SetIntVal(spectra); }

	/**
		@brief Sets the amplitude axis, in dBm

		Values outside the range clamp into the end cells rather than being dropped, so every
		column's total stays equal to the spectrum count. That is what lets the trace pass
		find percentiles without a separate pass to total each column, at the cost of biasing
		the end cells when a signal is off scale.
	 */
	void SetRange(float dbMin, float dbMax);

	float GetRangeMin() const
	{ return m_dbMin; }

	float GetRangeMax() const
	{ return m_dbMax; }

	/**
		@brief Sets the persistence rise and decay time constants, in seconds

		Converted to per-spectrum constants against the spectrum rate, so the look holds as
		the sample rate changes. gr-fosphor specifies these in spectra (cl.c:714-715) and its
		persistence therefore varies with sample rate.
	 */
	void SetPersistence(double riseSec, double decaySec);

	/**
		@brief The persistence decay time constant, in seconds

		Exposed so that anything drawn over the density map can fade on the same constant
		rather than introducing a second one. One knob, one look.
	 */
	double GetDecaySeconds() const
	{ return m_decaySec; }

	///@brief Tells the filter how fast spectra arrive, for the persistence conversion
	void SetSpectrumRate(double spectraPerSec)
	{ m_spectrumRate = spectraPerSec; }

	///@brief Percentile fractions, 0 to 1, for the three configurable traces
	void SetPercentiles(float low, float mid, float high);

	/**
		@brief Sets the amplitude resolution of the histogram and of the density map

		These are the knob that decides what the per-fold passes cost: both walk every cell,
		so the cost is linear in histogramCells. Percentile traces interpolate within the
		cell they land in, so trace accuracy degrades far more slowly than cell count
		suggests.

		The density map is separately and usually more coarsely resolved, because a map finer
		than the pane it is drawn into only aliases. histogramCells must be a whole multiple
		of densityCells.
	 */
	void SetCellCounts(size_t histogramCells, size_t densityCells);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Introspection, for the display and for tests

	///@brief Amplitude cells in the hit histogram
	size_t GetHistogramCells() const
	{ return m_ncells; }

	///@brief Amplitude cells in the density map
	size_t GetDensityCells() const
	{ return m_noutcells; }

	///@brief Frequency bins the buffers are sized for
	size_t GetCachedBins() const
	{ return m_cachedBins; }

	///@brief Spectra folded into the most recently completed window
	int64_t GetLastWindowSpectra() const
	{ return m_lastWindowSpectra; }

	///@brief The raw hit histogram, cell major. Exposed for verification.
	AcceleratorBuffer<uint32_t>& GetHits()
	{ return m_hits; }

	PROTOCOL_DECODER_INITPROC(SpectrumDensity)

protected:
	void ReallocateBuffers(size_t nbins);

	///@brief Spectra accumulated since the last fold
	int64_t m_accumulated;

	///@brief Spectra in the most recently completed window
	int64_t m_lastWindowSpectra;

	///@brief True if the last Refresh() completed a window
	bool m_outputReady;

	///@brief Bins the buffers are sized for
	size_t m_cachedBins;

	///@brief Amplitude cells in the hit histogram
	size_t m_ncells;

	///@brief Amplitude cells in the density map
	size_t m_noutcells;

	/**
		@brief Hit counts, cell major: m_hits[cell*nbins + bin]

		Cell major so that adjacent threads write adjacent addresses during the scatter, and
		so the density map that comes out of it is already in the row major order a texture
		wants.
	 */
	AcceleratorBuffer<uint32_t> m_hits;

	ComputePipeline m_accumulatePipeline;
	ComputePipeline m_tracePipeline;
	ComputePipeline m_foldPipeline;

	FilterParameter& m_binsPerSpectrum;
	FilterParameter& m_foldInterval;

	float m_dbMin;
	float m_dbMax;

	float m_fracLow;
	float m_fracMid;
	float m_fracHigh;

	double m_riseSec;
	double m_decaySec;
	double m_spectrumRate;

	float m_range;
	float m_offset;
};

#endif
