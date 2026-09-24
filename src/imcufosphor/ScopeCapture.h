/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of ScopeTrace, ScopeCaptureConfig and ScopeCapture
 */
#ifndef ScopeCapture_h
#define ScopeCapture_h

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

//scopehal.h first: Waveform.h is not self-contained, and reaches std::set through the
//precompiled header upstream builds it with. Same order SpectrumDensity.h uses.
#include <scopehal/scopehal.h>
#include <scopehal/Waveform.h>

#include "ScopeHistory.h"
#include "ScopeTriggerEngine.h"

/**
	@brief One drawable line: a component of one port
 */
struct ScopeTrace
{
	///@brief Which input port it came from
	std::size_t port = 0;

	///@brief 0 for I or a real value, 1 for Q
	std::size_t component = 0;

	///@brief Label for the legend, "ch0 I" or "ch2"
	std::string name;
};

/**
	@brief Everything ScopeCapture needs to size itself, in one struct so it can be diffed
 */
struct ScopeCaptureConfig
{
	///@brief Samples per record. At least 2; see ScopeCapture::Configure().
	std::size_t recordLength = 16384;

	/**
		@brief Samples of history kept per port, or zero to derive one from the record length

		This is also the drain cap, because draining more than the history holds is provably
		pointless - the excess would be overwritten before anything could read it.
	 */
	std::size_t historyDepth = 0;

	///@brief Hz. Zero means not yet known, and nothing is captured until it is.
	double sampleRate = 0;

	TriggerConfig trigger;
};

/**
	@brief Turns a stream of samples into triggered records ready for the rasterizer

	@par What this is, and what ScopeTriggerEngine is

	This is the half that owns buffers. Every rule - the edge convention, holdoff, re-arming, the
	trigger phase, the four run modes - lives in ScopeTriggerEngine, which owns no waveform and is
	therefore drivable with no Vulkan device. AcceleratorBuffer's constructor dereferences
	g_vkComputeDevice (AcceleratorBuffer.h:561-564), so that split is not a preference: it is the
	only way any of this logic can be tested on a machine with no GPU, and qa_ScopeTrigger.cpp is
	what it buys.

	So there is almost nothing here to get wrong. Drain into the histories, hand the trigger
	source to the engine, and when it says a record is ready, copy it.

	@par Threading

	Render thread only, for the same reason SpectrumEngine is: it allocates and writes
	AcceleratorBuffers. A GNU Radio sink must therefore drive this from draw() and never from
	processBulk(). See DEVELOPERS.md section 4.

	@par Reusing one waveform per trace

	@warning This reuses a single UniformAnalogWaveform per trace across frames, overwriting it in
	place. That is only sound because the frame's command buffer ends in SubmitAndBlock(), so
	every read of the samples - including the host to device copy that BindBufferNonblocking()
	records - has retired before draw() returns, and the next write happens on the following
	frame on the same thread. If the frame is ever changed to submit without blocking, this class
	needs a second set of waveforms and an alternation on the frame index. So does IqInjector,
	for the same reason and with the same consequence.
 */
class ScopeCapture
{
public:
	ScopeCapture();
	virtual ~ScopeCapture();

	//not copyable or assignable
	ScopeCapture(const ScopeCapture&) =delete;
	ScopeCapture& operator=(const ScopeCapture&) =delete;

	/**
		@brief Sizes the histories and the output waveforms

		@param ports			Number of input ports
		@param componentsPerSample	1 for a real stream, 2 for an interleaved complex one
		@param cfg				Record length, history depth, sample rate and trigger settings

		Render thread only: it allocates AcceleratorBuffers, which needs a Vulkan device.

		Idempotent, like SpectrumEngine::Configure(). It diffs against the previous configuration
		and reallocates only what actually moved, so a UI with no idea which field changed can
		call it every frame.
	 */
	void Configure(std::size_t ports, std::size_t componentsPerSample,
		const ScopeCaptureConfig& cfg);

	const ScopeCaptureConfig& GetConfig() const
	{ return m_cfg; }

	std::size_t GetPortCount() const
	{ return m_histories.size(); }

	/**
		@brief Appends one port's interleaved samples to its history

		@param port			Port index
		@param interleaved	Floats: one per sample for a real port, two for a complex one

		Must be called with an identical sample count for every port in a frame. The sink
		guarantees that by taking one admission decision across all its rings; see ScopeSink. The
		invariant is asserted in Update() rather than repaired here, because repairing it by
		discarding would be a silent time shift, which is the worst failure a scope has.
	 */
	void Append(std::size_t port, std::span<const float> interleaved);

	/**
		@brief Records that the samples either side of @a streamIndex are not adjacent in time

		Used for a dropped block, a ring overflow and a sample rate change alike - all three are
		the same fact about the stream, so they get one mechanism rather than three that can
		disagree.

		Several are kept, not just the newest. On a lossy stream there is a gap at the end of every
		drain, so a single newest-wins marker would sit permanently just behind the newest sample
		and refuse every record. What actually matters is whether a gap falls @b inside a record,
		and answering that needs the gaps that are still within the history.
	 */
	void NoteDiscontinuity(std::uint64_t streamIndex);

	/**
		@brief Advances the acquisition by one frame, rewriting the waveforms if a record completed

		@param now	Monotonic clock, for the Auto timeout

		@return True if a new record was published this frame
	 */
	bool Update(std::chrono::steady_clock::time_point now);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Output

	std::size_t GetTraceCount() const
	{ return m_traces.size(); }

	const ScopeTrace& GetTrace(std::size_t i) const
	{ return m_traces[i]; }

	/**
		@brief The samples of one trace, or null before the first complete record

		Null rather than an empty waveform on purpose. ComputePipeline::BindBuffer skips an empty
		buffer with only a warning and leaves the previous frame's descriptor bound
		(ComputePipeline.h:174-178), so a caller that checked size() instead of the pointer would
		dispatch against whatever was there last.
	 */
	UniformAnalogWaveform* GetWaveform(std::size_t i);

	ScopeTriggerEngine& GetTriggerEngine()
	{ return m_trigger; }

	const ScopeTriggerEngine& GetTriggerEngine() const
	{ return m_trigger; }

	///@brief Femtoseconds per sample, or 0 if the rate is not known
	std::int64_t GetTimescale() const
	{ return m_timescale; }

	///@brief Axis span of a record, in femtoseconds relative to the trigger
	bool GetRecordRange(double& lo, double& hi) const;

	/**
		@brief Frames in which the histories were found out of step and everything was discarded

		Should be zero forever. A nonzero value means the sink's joint admission is not holding,
		which is worth seeing in the status line rather than silently correcting.
	 */
	std::uint64_t GetResyncCount() const
	{ return m_resyncCount; }

	///@brief Samples appended to port 0 since construction
	std::uint64_t GetSampleCount() const
	{ return m_histories.empty() ? 0 : m_histories[0]->End(); }

protected:
	void RebuildTraces(std::size_t ports, std::size_t comps);
	bool CopyRecord(const CaptureRequest& req);
	void Resync();

	ScopeCaptureConfig m_cfg;

	///@brief Femtoseconds per sample, derived from m_cfg.sampleRate and never accumulated
	std::int64_t m_timescale;

	std::size_t m_components;

	/**
		@brief One per port, not one per trace

		I and Q of a port share a history at stride 2, so they cannot possibly disagree with each
		other about which sample is which. Only inter-port alignment is at risk.
	 */
	std::vector<std::unique_ptr<ScopeHistory>> m_histories;

	std::vector<ScopeTrace> m_traces;

	///@brief One per trace. Reused across frames; see the class comment.
	std::vector<std::unique_ptr<UniformAnalogWaveform>> m_waveforms;

	///@brief Scan window for the trigger source, reused so a frame allocates nothing
	std::vector<float> m_scan;

	ScopeTriggerEngine m_trigger;

	/**
		@brief Stream indices where contiguity breaks, ascending

		Pruned in Update() to those still inside the history, so this stays short without any
		policy about how many to keep: a gap older than the oldest resident sample cannot be inside
		any record that could still be assembled.
	 */
	std::vector<std::uint64_t> m_gaps;

	///@brief False until a complete record has been copied, which is what GetWaveform() reports
	bool m_haveRecord;

	std::uint64_t m_resyncCount;
};

#endif
