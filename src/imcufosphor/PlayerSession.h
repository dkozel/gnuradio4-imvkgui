/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of PlayerSession
 */
#ifndef PlayerSession_h
#define PlayerSession_h

#include "AnalyzerSource.h"
#include "SigMFSource.h"
#include "ComplexFFTFilter.h"
#include "SpectrumReducer.h"
#include "SpectrumDensity.h"
#include "GpuTimer.h"
#include "RowHistory.h"

#include "../../lib/scopehal/scopeprotocols/Waterfall.h"

//PlaybackStats and BlockSpan moved to AnalyzerSource.h, which SpectrumEngine shares. Included
//above rather than forward declared, so every existing user of this header still sees them.

/**
	@brief Owns the recording, the filter graph, and playback

	The engine half of the application, kept separate from the window so that the display
	code has no opinion about how data arrives. Analogous to ngscopeclient's Session, minus
	everything to do with live instruments.

	Graph shape (DESIGN.md section 6):

	@verbatim
	SigMFSource -> [I, Q, center] -> ComplexFFTFilter -> SpectrumReducer -> Waterfall
	                                        |
	                                        +-> spectrum trace
	@endverbatim

	The reducer is why this class schedules rather than just calling Refresh() in a line:
	the FFT runs GroupSize() times per waterfall row, so that RBW, line rate and playback
	speed stay independent. See DESIGN.md sections 7.3 and 8.1.
 */
class PlayerSession : public IAnalyzerSource
{
public:
	PlayerSession(SigMFSource* source, std::shared_ptr<QueueHandle> queue);
	virtual ~PlayerSession();

	//not copyable or assignable
	PlayerSession(const PlayerSession&) =delete;
	PlayerSession& operator=(const PlayerSession&) =delete;

	///@brief Outcome of processing one acquisition block
	enum StepResult
	{
		///@brief No data. The source is stopped, at the end, or has a block still in flight.
		STEP_FAILED,

		///@brief A block was processed but the group is not complete, so no row came out
		STEP_BLOCK,

		///@brief A block was processed and the waterfall advanced
		STEP_ROW
	};

	/**
		@brief Acquires one block and runs the whole graph over it in a single submit

		The unit of work. One block carries as many transforms as fit in it, so every fixed
		cost - the read, the command buffer, the fence round trip - is paid once for all of
		them rather than once each.
	 */
	StepResult StepOneBlock();

	/**
		@brief Advances playback until the waterfall gains a row

		May process several blocks: a group spans more than one block whenever the group size
		exceeds the transforms per block, which is the normal case at high sample rates.

		@return True if a row was produced
	 */
	bool StepOneRow();

	///@brief Runs the graph without advancing the waterfall, to refresh the spectrum only
	void RefreshSpectrumOnly();

	SigMFSource* GetSource()
	{ return m_source; }

	ComplexFFTFilter* GetFFT()
	{ return m_fft; }

	SpectrumReducer* GetReducer()
	{ return m_reducer; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// IAnalyzerSource

	virtual SpectrumDensity* GetDensity() override
	{ return m_density; }

	virtual Waterfall* GetWaterfall() override
	{ return m_waterfall; }

	/**
		@brief Which samples went into each waterfall row

		Recorded here rather than in the display because this is where rows are produced.
		Anything drawing a waterfall time axis needs it; see DESIGN.md section 8.2.
	 */
	virtual RowHistory& GetRowHistory() override
	{ return m_rowHistory; }

	/**
		@brief Sample rate of the recording being played

		The recording's rate, not a playback rate: this is what the time axis and the density
		map's persistence constants are derived from, and neither cares how fast the file is
		being read.
	 */
	virtual double GetSampleRate() const override
	{ return m_source->GetRecordingSampleRate(); }

	/**
		@brief Sample range of the most recently processed acquisition block

		The spectrum shows this block, so anything drawn over the spectrum - a timestamp
		readout, an annotation span, a marker - is placed against this range rather than
		against the play cursor, which has already moved on to the next block by the time the
		frame is drawn.
	 */
	virtual const BlockSpan& GetCurrentBlock() const override
	{ return m_currentBlock; }

	///@brief Number of waterfall rows produced since the last reset
	int64_t GetRowsPlayed() const
	{ return m_rowsPlayed; }

	///@brief Rewinds to the start of the recording and clears accumulated state
	void Restart();

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Instrumentation

	const PlaybackStats& GetStats() const
	{ return m_stats; }

	/**
		@brief Zeroes the accumulated timings, here and in the source

		Call after any warmup. The first few passes pay for pipeline creation, shader
		compilation and the FFT plan, none of which recur.
	 */
	void ResetStats();

	///@brief GPU time per stage of the per-block command buffer
	GpuTimer& GetGpuTimer()
	{ return m_spectrumGpuTimer; }

	/**
		@brief Enables GPU timestamp collection

		Off by default. Collect() blocks on the query results, so leaving it on in the
		application would add a stall per submit to measure a stall per submit.
	 */
	void SetGpuTimingEnabled(bool enable)
	{ m_gpuTimingEnabled = enable; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Configuration

	///@brief Transform length, which sets the resolution bandwidth
	int64_t GetFFTLength() const
	{ return m_fftLength; }

	void SetFFTLength(int64_t len);

	/**
		@brief Samples per acquisition

		Independent of the transform length: the block is cut into as many transforms as fit.
		Larger blocks amortize the per-acquisition costs over more transforms and are the
		single reason high sample rates are affordable, at the price of coarser playback
		granularity. Clamped up to the transform length, never below it.
	 */
	int64_t GetBlockSize() const
	{ return m_blockSize; }

	void SetBlockSize(int64_t samples);

	///@brief Transforms per acquisition block at the current settings
	int64_t GetSpectraPerBlock() const
	{ return (m_fftLength > 0) ? (m_blockSize / m_fftLength) : 0; }

	///@brief Spectra combined into each waterfall row
	int64_t GetGroupSize();

	void SetGroupSize(int64_t k);

	/**
		@brief Fraction of the recording actually transformed, 0 to 1

		One block of GetFFTLength() samples is read per FFT and all of it is transformed, so
		this is 1.0 whenever the source block size matches the transform length. It exists
		because that stops being true as soon as playback is paced against a wall clock, and
		a silently low value means the display is missing signals.
	 */
	double GetCoverage() const;

	///@brief dBm at the top of the colour scale
	float GetRangeMax() const
	{ return m_rangeMax; }

	///@brief dBm at the bottom of the colour scale
	float GetRangeMin() const
	{ return m_rangeMin; }

	void SetRange(float minDbm, float maxDbm);

protected:
	///@brief Pushes the transform length and block size down into the source and the filters
	void ApplyLengths();

	SigMFSource* m_source;
	std::shared_ptr<QueueHandle> m_queue;

	ComplexFFTFilter* m_fft;
	SpectrumReducer* m_reducer;
	SpectrumDensity* m_density;
	Waterfall* m_waterfall;

	std::unique_ptr<vk::raii::CommandPool> m_cmdPool;
	std::unique_ptr<vk::raii::CommandBuffer> m_cmdBuf;

	int64_t m_fftLength;
	int64_t m_blockSize;
	int64_t m_rowsPlayed;

	float m_rangeMin;
	float m_rangeMax;

	PlaybackStats m_stats;

	RowHistory m_rowHistory;

	///@brief Where the reducer's current group started, in both coordinates, for the row history
	RowMark m_groupStart;

	///@brief Sample range of the block most recently pushed through the graph
	BlockSpan m_currentBlock;

	bool m_gpuTimingEnabled;
	GpuTimer m_spectrumGpuTimer;
};

#endif
