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

#include "SigMFSource.h"
#include "ComplexFFTFilter.h"
#include "SpectrumReducer.h"
#include "SpectrumDensity.h"
#include "GpuTimer.h"
#include "RowHistory.h"

#include "../../lib/scopehal/scopeprotocols/Waterfall.h"

/**
	@brief Where playback time goes, accumulated since the last ResetStats()

	Split this way because the three are fixed by different things and are fixed
	differently: acquisition is host work bounded by the file and the sample converter,
	recording is CPU time building command buffers and scales with the number of dispatches,
	and submit is wall clock waiting on the GPU. Only the last is a GPU cost. Lumping them
	together makes it impossible to tell an overhead-bound pipeline from a compute-bound one,
	which is the entire question at hand.
 */
struct PlaybackStats
{
	///@brief Seconds in AcquireData() and PopPendingWaveform()
	double acquireSec = 0;

	///@brief Seconds recording command buffers, i.e. in Filter::Refresh()
	double recordSec = 0;

	///@brief Seconds blocked in SubmitAndBlock()
	double submitSec = 0;

	///@brief Transforms executed
	int64_t spectra = 0;

	///@brief Calls to SubmitAndBlock()
	int64_t submits = 0;
};

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
class PlayerSession
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

	SpectrumDensity* GetDensity()
	{ return m_density; }

	Waterfall* GetWaterfall()
	{ return m_waterfall; }

	/**
		@brief Which samples went into each waterfall row

		Recorded here rather than in the display because this is where rows are produced.
		Anything drawing a waterfall time axis needs it; see DESIGN.md section 8.2.
	 */
	RowHistory& GetRowHistory()
	{ return m_rowHistory; }

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

	///@brief Play cursor when the reducer's current group started, for the row history
	int64_t m_groupStartSample;

	bool m_gpuTimingEnabled;
	GpuTimer m_spectrumGpuTimer;
};

#endif
