/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of EnginePart, EngineConfig and SpectrumEngine
 */
#ifndef SpectrumEngine_h
#define SpectrumEngine_h

#include "AnalyzerSource.h"
#include "ComplexFFTFilter.h"
#include "GpuTimer.h"
#include "IqInjector.h"
#include "SpectrumReducer.h"

/**
	@brief Which halves of the analysis graph to build

	A spectrum-only display pays for a Waterfall's row ring buffer it never draws, and a
	waterfall-only display pays for a hit histogram of bins by amplitude cells, which at 8192
	bins and 1024 cells is the largest allocation in the pipeline. One flag each so neither
	has to.
 */
enum class EnginePart : uint32_t
{
	Density		= 1,
	Waterfall	= 2,
	All			= 3
};

inline EnginePart operator|(EnginePart a, EnginePart b)
{ return static_cast<EnginePart>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }

inline bool HasPart(EnginePart parts, EnginePart which)
{ return (static_cast<uint32_t>(parts) & static_cast<uint32_t>(which)) != 0; }

/**
	@brief Everything about the analysis that a caller is allowed to decide

	One struct rather than a dozen setters because Configure() has to be idempotent: it is
	called on every settings change from a UI that has no idea which fields actually moved, and
	comparing a whole config against the previous one is how it works out what to reallocate.
 */
struct EngineConfig
{
	///@brief Transform length, which sets the resolution bandwidth
	int64_t fftLength = 8192;

	/**
		@brief Samples per block pushed through the graph

		Independent of the transform length: a block is cut into as many transforms as fit.
		Larger blocks amortize the fixed costs - the command buffer, the fence round trip -
		over more transforms, and are the single reason high sample rates are affordable.
		Clamped up to the transform length, never below it.
	 */
	int64_t blockSize = 1048576;

	///@brief Spectra combined into each waterfall row
	int64_t groupSize = 1;

	FFTFilter::WindowFunction window = FFTFilter::WINDOW_BLACKMAN_HARRIS;

	///@brief dBm at the bottom and top of the colour scale
	float rangeMin = -100;
	float rangeMax = -20;

	///@brief Raw LSB to volts for integer input, ignored for float input
	float sampleScale = 1.0f / 32767.0f;

	///@brief Hz. Zero means not yet known, in which case rate-derived settings are skipped.
	double sampleRate = 0;

	///@brief Hz, centre of the displayed span
	double centerFrequency = 0;
};

/**
	@brief Runs the analysis graph over IQ handed to it from outside

	PlayerSession without the recording. Same filters, same wiring, same one-command-buffer
	one-submit-per-block scheduling; the difference is only where the samples come from, which
	here is a span rather than a file.

	@verbatim
	IqInjector -> [I(packed), Q, center] -> ComplexFFTFilter -+-> SpectrumReducer -> Waterfall
	                                                          +-> SpectrumDensity
	@endverbatim

	@par Threading

	Every public method is render-thread-only, because Step() records and submits Vulkan work
	and Configure() reallocates filter buffers. A caller driving this from a GNU Radio sink
	must therefore call it from draw(), never from processBulk() or settingsChanged(), and must
	do its own handoff between the two. This is not enforceable here and it is the single thing
	most likely to be got wrong, so it is stated at every entry point.
 */
class SpectrumEngine : public IAnalyzerSource
{
public:
	SpectrumEngine(EnginePart parts, std::shared_ptr<QueueHandle> queue);
	virtual ~SpectrumEngine();

	//not copyable or assignable
	SpectrumEngine(const SpectrumEngine&) =delete;
	SpectrumEngine& operator=(const SpectrumEngine&) =delete;

	///@brief Outcome of processing one block
	enum StepResult
	{
		///@brief Nothing was processed
		STEP_FAILED,

		///@brief A block was processed but the group is not complete, so no row came out
		STEP_BLOCK,

		///@brief A block was processed and the waterfall advanced
		STEP_ROW
	};

	/**
		@brief Applies a configuration, reallocating only what actually changed

		Safe to call every frame with an unchanged config. Render thread only.
	 */
	void Configure(const EngineConfig& config);

	const EngineConfig& GetConfig() const
	{ return m_config; }

	/**
		@brief Pushes one block of float IQ through the whole graph in a single submit

		Render thread only. See the class doc.
	 */
	StepResult Step(std::span<const std::complex<float>> iq);

	///@brief Pushes one block of 16-bit integer IQ. Render thread only.
	StepResult Step(std::span<const std::complex<int16_t>> iq);

	///@brief Runs the graph without advancing the waterfall, to refresh the spectrum only
	void RefreshSpectrumOnly();

	///@brief Discards accumulated state without touching the configuration
	void Reset();

	IqInjector* GetInjector()
	{ return m_injector.get(); }

	ComplexFFTFilter* GetFFT()
	{ return m_fft; }

	///@brief The reducer, or null unless this engine has the Waterfall part
	SpectrumReducer* GetReducer()
	{ return m_reducer; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// IAnalyzerSource

	///@brief The density map filter, or null unless this engine has the Density part
	virtual SpectrumDensity* GetDensity() override
	{ return m_density; }

	///@brief The waterfall filter, or null unless this engine has the Waterfall part
	virtual Waterfall* GetWaterfall() override
	{ return m_waterfall; }

	virtual RowHistory& GetRowHistory() override
	{ return m_rowHistory; }

	virtual double GetSampleRate() const override
	{ return m_config.sampleRate; }

	virtual const BlockSpan& GetCurrentBlock() const override
	{ return m_currentBlock; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Instrumentation

	///@brief Waterfall rows produced since the last Reset()
	int64_t GetRowsPlayed() const
	{ return m_rowsPlayed; }

	const PlaybackStats& GetStats() const
	{ return m_stats; }

	/**
		@brief Zeroes the accumulated timings

		Call after any warmup. The first few passes pay for pipeline creation, shader
		compilation and the FFT plan, none of which recur.
	 */
	void ResetStats();

	GpuTimer& GetGpuTimer()
	{ return m_gpuTimer; }

	/**
		@brief Enables GPU timestamp collection

		Off by default. Collect() blocks on the query results, so leaving it on in an
		application would add a stall per submit to measure a stall per submit.
	 */
	void SetGpuTimingEnabled(bool enable)
	{ m_gpuTimingEnabled = enable; }

	/**
		@brief Fraction of the most recent block actually transformed, 0 to 1

		Short of 1 by the remainder whenever the transform length does not divide the block
		size. A silently low value means the display is missing signals.
	 */
	double GetCoverage() const;

protected:
	/**
		@brief Everything after the samples are in the injector

		Both Step() overloads differ only in how they hand bytes to the injector, so the
		command buffer, the barriers, the submit and the row bookkeeping live here once.
	 */
	StepResult StepAfterPush(size_t nsamples, double acquireSec);

	EnginePart m_parts;
	std::shared_ptr<QueueHandle> m_queue;

	std::unique_ptr<IqInjector> m_injector;

	ComplexFFTFilter* m_fft;
	SpectrumReducer* m_reducer;
	SpectrumDensity* m_density;
	Waterfall* m_waterfall;

	std::unique_ptr<vk::raii::CommandPool> m_cmdPool;
	std::unique_ptr<vk::raii::CommandBuffer> m_cmdBuf;

	EngineConfig m_config;

	///@brief False until the first Configure(), so the first one applies everything
	bool m_configured;

	int64_t m_rowsPlayed;

	PlaybackStats m_stats;
	RowHistory m_rowHistory;

	///@brief Where the reducer's current group started, for the row history
	RowMark m_groupStart;

	BlockSpan m_currentBlock;

	bool m_gpuTimingEnabled;
	GpuTimer m_gpuTimer;
};

#endif
