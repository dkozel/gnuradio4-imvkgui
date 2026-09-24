/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of CaptureState, CaptureRequest and ScopeTriggerEngine
 */
#ifndef ScopeTriggerEngine_h
#define ScopeTriggerEngine_h

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ScopeTrigger.h"

/**
	@brief What the acquisition is doing right now

	Four states, not three. PostFill is the one a naive implementation leaves out: a trigger found
	at sample k needs the record [k - pre, k - pre + length), but the history only reaches k+1 at
	the instant the crossing is found. Searching has to @b stop while the tail arrives, or a later
	edge replaces the pending one and the record eventually drawn is not the record that
	triggered - which looks like the scope triggering on the wrong thing rather than like a bug.
 */
enum class CaptureState
{
	///@brief Not searching. The stream is still drained; see ScopeCapture.
	Stopped,

	///@brief Searching for a trigger
	Armed,

	///@brief Trigger found, waiting for the post-trigger samples to arrive
	PostFill,

	///@brief A complete record has been handed out and not yet superseded
	Held
};

/**
	@brief A record the engine is asking the caller to copy

	Handed out only once every sample of [start, start + recordLength) is resident, so a caller
	that has honoured the residency contract of ScopeHistory cannot fail to copy it.
 */
struct CaptureRequest
{
	///@brief Stream index of the record's first sample
	std::uint64_t start = 0;

	///@brief Stream index of the sample at or before the trigger instant
	std::uint64_t triggerIndex = 0;

	///@brief WaveformBase::m_triggerPhase for this record. Negative; see TriggerPhaseFs().
	std::int64_t phaseFs = 0;

	/**
		@brief False if this sweep was not caused by a real level crossing

		True for an edge trigger, false for free run, an Auto timeout and Force. The readout uses
		it to say "Trig'd" rather than "Auto", and NoteCaptureComplete() uses it to decide whether
		holdoff was consumed - a forced sweep must not push the next real trigger away.
	 */
	bool real = false;
};

/**
	@brief Decides which samples make a record, and where the trigger sits inside it

	@par Why this owns no waveform

	AcceleratorBuffer's constructor dereferences g_vkComputeDevice to create two sync events
	(AcceleratorBuffer.h:561-564), which is null until VulkanInit(). A UniformAnalogWaveform
	therefore cannot exist without a Vulkan device at all, and anything holding one is untestable
	on a machine with no GPU.

	So the line is drawn at "decides" versus "copies". Every rule worth getting right - the edge
	convention, holdoff, re-arming, the phase arithmetic, the four modes - is a decision, and
	lives here where a headless test can drive it. ScopeCapture is the shell that owns the
	buffers and does the copy, and has almost no logic left to get wrong.

	@par Threading

	Render thread only, with one exception: RequestForce() is atomic, so a toolbar button or a
	message port can latch a sweep from anywhere.
 */
class ScopeTriggerEngine
{
public:
	ScopeTriggerEngine();

	/**
		@brief Applies a configuration, reallocating nothing

		@param cfg			Trigger settings
		@param recordLength	Samples per record, at least 2
		@param timescaleFs	Femtoseconds per sample, or 0 if the rate is not yet known
		@param historyEnd	Current ScopeHistory::End(), for invalidation

		Idempotent: it diffs against the previous config and only invalidates the scan when
		something the arm flags depend on actually moved.
	 */
	void Configure(const TriggerConfig& cfg, std::size_t recordLength, std::int64_t timescaleFs,
		std::uint64_t historyEnd);

	const TriggerConfig& GetConfig() const
	{ return m_cfg; }

	std::size_t GetRecordLength() const
	{ return m_recordLength; }

	///@brief Record index of the sample at or before the trigger
	std::size_t GetPreSamples() const
	{ return m_preSamples; }

	std::int64_t GetTimescale() const
	{ return m_timescale; }

	/**
		@brief Changes run control without touching the rest of the configuration

		Separate from Configure() because Run/Stop/Single are buttons pressed constantly while
		everything else is a settings dialog. Moving out of Stop re-arms.
	 */
	void SetMode(TriggerMode mode);

	TriggerMode GetMode() const
	{ return m_cfg.mode; }

	/**
		@brief Latches a one-shot forced sweep

		Thread safe, and the only member that is. Consumed by the next Step(); never sticky.
	 */
	void RequestForce()
	{ m_force.store(true, std::memory_order_release); }

	///@brief Re-arms after Single has fired
	void Rearm();

	/**
		@brief Advances the acquisition by one frame

		@param scan			Trigger source values at stream indices [scanBase, scanBase+size).
							Empty when the caller had nothing to search or the engine did not
							want a search; see WantsScan().
		@param scanBase		Stream index of scan[0]
		@param historyBegin	Oldest sample still resident
		@param historyEnd	One past the newest sample resident
		@param gaps			Stream indices where the samples stop being contiguous, ascending. A
							record is refused if one falls strictly inside it.
		@param now			Monotonic clock, for the Auto timeout

		@return A record ready to be copied, or nothing

		The returned request is @b not yet accounted for. The caller copies the samples and then
		calls NoteCaptureComplete(), or NoteAbandoned() if it could not. Until one of those the
		engine stays in PostFill and will re-offer the same record, so a copy that fails for a
		reason the engine cannot see does not silently become a capture that never happened.
	 */
	std::optional<CaptureRequest> Step(
		std::span<const float> scan,
		std::uint64_t scanBase,
		std::uint64_t historyBegin,
		std::uint64_t historyEnd,
		std::span<const std::uint64_t> gaps,
		std::chrono::steady_clock::time_point now);

	/**
		@brief True if Step() will use a scan window this frame

		Lets the caller skip materialising one - which is the expensive half of a frame in Stop
		mode, and pure waste in free run.
	 */
	bool WantsScan() const
	{ return (m_state == CaptureState::Armed) && (m_cfg.kind == TriggerKind::Edge); }

	///@brief Stream index of the left sample of the next pair to evaluate
	std::uint64_t GetScanPos() const
	{ return m_scanPos; }

	/**
		@brief Accepts the record Step() handed out

		@param req			The request, unmodified
		@param historyBegin	Oldest resident sample, for the pre-trigger constraint
		@param now			Monotonic clock
	 */
	void NoteCaptureComplete(const CaptureRequest& req, std::uint64_t historyBegin,
		std::chrono::steady_clock::time_point now);

	/**
		@brief Rejects the record Step() handed out, counting it as missed

		A record that could not be copied whole is dropped rather than patched. A partial record
		drawn as if it were whole is the worst outcome available: it is wrong and it looks right.
	 */
	void NoteAbandoned();

	/**
		@brief Discards the scan position and the hysteresis state

		Called when the level, slope, operator or source moves, and when the history is cleared.
		The arm flags were latched against the old configuration and mean nothing under the new
		one; carrying them across shows up as exactly one spuriously suppressed or accepted
		trigger after every knob turn.
	 */
	void Invalidate(std::uint64_t historyEnd);

	CaptureState GetState() const
	{ return m_state; }

	///@brief Records handed out and accepted since construction
	std::uint64_t GetCaptureCount() const
	{ return m_captureCount; }

	///@brief Records refused because they spanned a gap or aged out
	std::uint64_t GetMissedCount() const
	{ return m_missedCount; }

	///@brief Whether the last accepted record came from a real level crossing
	bool GetLastCaptureReal() const
	{ return m_lastCaptureReal; }

	/**
		@brief True if any gap falls strictly inside [start, start + recordLength)

		Strictly inside is the whole point. A gap at or before the first sample, or at or after
		the last, separates this record from its neighbours but says nothing about the record
		itself - every sample in it is still adjacent to the next. Refusing those as well is what
		makes a lossy stream display nothing at all, because in steady state there is a gap at the
		end of every drain.
	 */
	static bool Straddles(std::span<const std::uint64_t> gaps, std::uint64_t start, std::size_t n);

	///@brief Human readable state for the status line
	const char* GetStateText() const;

protected:
	std::optional<CaptureRequest> TryComplete(std::uint64_t historyBegin, std::uint64_t historyEnd,
		std::span<const std::uint64_t> gaps);


	bool BeginForcedCapture(std::uint64_t historyBegin, std::uint64_t historyEnd);

	void ReArm(std::uint64_t triggerIndex, std::uint64_t historyBegin);

	TriggerConfig m_cfg;

	std::size_t m_recordLength;

	///@brief Record index of the trigger, derived from m_cfg.pretrigger and m_recordLength
	std::size_t m_preSamples;

	std::int64_t m_timescale;

	///@brief Holdoff converted against the sample clock, so the two cannot disagree
	std::uint64_t m_holdoffSamples;

	CaptureState m_state;

	///@brief The record found but not yet handed out or completed
	CaptureRequest m_pending;

	///@brief Stream index of the left sample of the next pair to evaluate. Monotonic.
	std::uint64_t m_scanPos;

	/**
		@brief Holdoff, expressed as the earliest crossing the search will accept

		A position rather than a countdown, so there is no second clock to drift against the
		sample indices the search works in. The other constraint on an acceptable crossing - that
		the pre-trigger depth is still resident - is applied where the scan is issued, because it
		has to hold before the first capture as well and this only moves on a trigger.
	 */
	std::uint64_t m_armAt;

	ArmState m_arm;

	std::atomic<bool> m_force;

	std::chrono::steady_clock::time_point m_lastCapture;

	///@brief False until the first Step(), so Auto does not force a sweep before any data arrives
	bool m_haveClock;

	std::uint64_t m_captureCount;
	std::uint64_t m_missedCount;
	bool m_lastCaptureReal;
};

#endif
