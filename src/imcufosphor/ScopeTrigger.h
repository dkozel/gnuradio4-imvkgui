/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Trigger configuration and the primitives a trigger search is built from

	Deliberately free functions over spans plus plain enums, with no dependency beyond the
	standard library. Two reasons:

	- scopehal's Trigger / EdgeTrigger hierarchy is not in the vendored tree, and re-vendoring it
	  would drag Oscilloscope and Instrument back in against DESIGN.md section 3. What is
	  reimplemented here is the pragmatic subset: run control, edge detection and sub-sample
	  interpolation. The hardware arming model has no meaning without an instrument.
	- AcceleratorBuffer's constructor dereferences g_vkComputeDevice (AcceleratorBuffer.h:563),
	  so anything holding a waveform cannot exist without a Vulkan device. Keeping the decisions
	  in a header that touches no waveform is what makes them testable with no GPU at all, and
	  every rule worth getting right is a decision rather than a copy.
 */
#ifndef ScopeTrigger_h
#define ScopeTrigger_h

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/**
	@brief Run control, as an oscilloscope means it

	Same four states every bench scope has. Not scopehal's Oscilloscope::TriggerMode, which mixes
	the user's intent (run, stop) with the instrument's reply (triggered, waiting) in one enum
	because it is reporting what a remote instrument said. Here the intent is ours and the reply
	is CaptureState, so they are two types and cannot be confused.
 */
enum class TriggerMode
{
	///@brief Not searching. The stream is still drained; see ScopeCapture for why.
	Stop,

	///@brief Search, but force a sweep if nothing triggers for a while
	Auto,

	///@brief Search, and show the previous record until something does
	Normal,

	///@brief Search once, then stop
	Single
};

/**
	@brief What arms a capture
 */
enum class TriggerKind
{
	///@brief Every frame captures the newest full record. A rolling scope.
	FreeRun,

	///@brief A level crossing on the source trace
	Edge
};

enum class TriggerSlope
{
	Rising,
	Falling,
	Any
};

/**
	@brief What the level is compared against
 */
enum class TriggerOperator
{
	///@brief The trace's own values
	Raw,

	/**
		@brief The magnitude of the source trace's port, |I + jQ|

		Not meaningful for a real port, where it degenerates to |x|. The point of it is burst
		detection on an IQ stream: an envelope threshold is the trigger an SDR actually wants,
		and expressing it as an operator on an edge trigger means it costs one branch rather
		than a second trigger type.
	 */
	Magnitude
};

/**
	@brief Everything the search needs, in one struct so it can be diffed
 */
struct TriggerConfig
{
	TriggerMode mode = TriggerMode::Auto;
	TriggerKind kind = TriggerKind::Edge;
	TriggerSlope slope = TriggerSlope::Rising;
	TriggerOperator op = TriggerOperator::Raw;

	///@brief Which trace the level is tested against
	std::size_t source = 0;

	float level = 0;

	/**
		@brief Half-width of the dead band around the level

		Not optional for a magnitude trigger. A noise floor sitting on the level crosses it on
		roughly every other sample pair, and holdoff does not fix that - it only reduces the
		rate to one false trigger per holdoff. With this the signal has to leave the band before
		it can re-arm.
	 */
	float hysteresis = 0;

	///@brief Minimum time between accepted triggers
	std::int64_t holdoffFs = 0;

	///@brief How long Auto waits for a real trigger before forcing a sweep
	std::int64_t autoTimeoutMs = 100;

	///@brief Fraction of the record before the trigger instant, 0 to 1
	float pretrigger = 0.5f;

	/**
		@brief True if a change between two configs invalidates the scan position

		The hysteresis arm flags are latched against a particular level in a particular domain.
		Carrying them across a change to any of these would apply a decision made about the old
		level to the new one, which shows up as one spuriously suppressed or spuriously accepted
		trigger immediately after every knob turn.
	 */
	bool ScanInvalidatedBy(const TriggerConfig& other) const
	{
		return (level != other.level) ||
			(hysteresis != other.hysteresis) ||
			(slope != other.slope) ||
			(op != other.op) ||
			(source != other.source) ||
			(kind != other.kind);
	}
};

/**
	@brief Hysteresis state carried across scan windows

	Both flags start @b clear, so a crossing is only ever accepted after the signal has been seen
	outside the band on the side it is crossing from.

	Starting them set instead would make the first crossing after every reset fire whatever the
	band says, which defeats the entire point on the one input hysteresis exists for: a noise
	floor sitting on the level would still produce one trigger per reconfiguration. Nothing is
	lost by starting clear, because the arm conditions are evaluated on the left sample of each
	pair before that pair is tested - so any signal that genuinely leaves the band arms on the
	first sample that does, not one pair later.
 */
struct ArmState
{
	bool belowArm = false;
	bool aboveArm = false;
};

/**
	@brief A level crossing, lying between samples @ref index and index+1
 */
struct Crossing
{
	///@brief Stream index of the left sample of the pair
	std::uint64_t index = 0;

	bool rising = false;
};

/**
	@brief Finds the first level crossing in a window

	@param w			Scan values. w[0] is the sample at stream index @a base.
	@param base			Stream index of w[0]
	@param levelLo		Below this, a rising trigger re-arms
	@param level		The threshold itself
	@param levelHi		At or above this, a falling trigger re-arms
	@param slope		Which direction to accept
	@param notBefore	Crossings whose left sample is earlier than this are stepped over, but
						still update the arm flags. That is what makes holdoff and re-arming
						agree with each other rather than being two independent gates.
	@param st			Hysteresis state, updated in place

	@return The first accepted crossing, or nothing

	@par The half-open convention

	The level belongs to the "above" half plane: a sample exactly at the level counts as above.
	Every crossing of the level is then exactly one of rising or falling and never both, which is
	what makes TriggerSlope::Any neither miss an edge nor report one twice. A scope that reports
	one edge twice does not look like a bug, it looks like jitter, which is why this is pinned by
	a test rather than left to read correctly.

	@par Why only the first

	One acquisition draws one record. Finding the rest of the crossings in the window is work
	whose result is discarded, and stopping early makes the cost proportional to "samples until
	the trigger" rather than "samples drained".
 */
inline std::optional<Crossing> ScanEdges(
	std::span<const float> w,
	std::uint64_t base,
	float levelLo,
	float level,
	float levelHi,
	TriggerSlope slope,
	std::uint64_t notBefore,
	ArmState& st)
{
	for(std::size_t i = 0; (i + 1) < w.size(); i++)
	{
		const float a = w[i];
		const float b = w[i + 1];

		//Arm on the left sample before the pair is tested, so a signal that dips out of the band
		//and returns within one sample still arms. Testing after would lose that edge entirely.
		//
		//The comparisons are strict below and inclusive above, matching the half-open convention
		//the crossing tests use. That is what makes a zero-width band degenerate exactly: with
		//levelLo == levelHi == level every sample arms precisely one flag, and it is always the
		//one the corresponding crossing test is about to require.
		if(a < levelLo)
			st.belowArm = true;
		if(a >= levelHi)
			st.aboveArm = true;

		if((base + i) < notBefore)
			continue;

		const bool rise = (a < level) && (b >= level);
		const bool fall = (a >= level) && (b < level);

		if(rise && st.belowArm && (slope != TriggerSlope::Falling))
		{
			st.belowArm = false;
			return Crossing{base + i, true};
		}

		if(fall && st.aboveArm && (slope != TriggerSlope::Rising))
		{
			st.aboveArm = false;
			return Crossing{base + i, false};
		}
	}

	return std::nullopt;
}

/**
	@brief Where between two samples the level was crossed, as a fraction of a sample

	@param a		Left sample value
	@param b		Right sample value
	@param level	The threshold

	@return A fraction in [0, 1)

	Linear interpolation, the same approximation scopehal's interpolators make. @a b differs from
	@a a whenever the values came from a crossing ScanEdges actually reported, since both of its
	tests are strict on one side.

	Clamped to just below one so that P + f stays strictly inside the pair. A fraction of exactly
	one would put the trigger on the next sample's grid point, where it is indistinguishable from
	a trigger one sample later - which is precisely the one-sample jitter this function exists to
	remove.
 */
inline float CrossingFraction(float a, float b, float level)
{
	const float d = b - a;
	if(d == 0)
		return 0;

	const float f = (level - a) / d;
	return std::clamp(f, 0.0f, std::nextafter(1.0f, 0.0f));
}

/**
	@brief WaveformBase::m_triggerPhase for a record whose trigger sits at index @a pre + @a frac

	@param pre		Record index of the sample at or before the trigger
	@param frac		Sub-sample position of the trigger past that sample, in [0, 1)
	@param timescale	Femtoseconds per sample

	@return The phase, in femtoseconds. Negative: sample zero precedes the trigger.

	@par The convention

	X = 0 on screen is the trigger instant. GetOffsetScaled() (Waveform.h:841) places sample i at
	<tt>i * m_timescale + m_triggerPhase</tt>, so putting the crossing at zero means the phase is
	minus the pre-trigger duration. The rasterizer reproduces that identity exactly, including for
	negative phases - see ScopeArea::RasterizeTrace.

	Anchoring at the trigger rather than at the start of the record is what lets the record length
	and the pre-trigger fraction change without the view jumping.

	@par Why the integer and fractional parts are added separately

	Forming <tt>(pre + frac) * timescale</tt> as one double loses femtoseconds: at a million
	samples of pre-trigger and a millisecond sample period the product is 1e18, well past double's
	9.0e15 exact-integer limit. Same rule as notes/time-domain-plan.md, "Never accumulate
	femtoseconds" - and the same failure, a phase error that only shows up on long records.
 */
inline std::int64_t TriggerPhaseFs(std::size_t pre, float frac, std::int64_t timescale)
{
	const std::int64_t whole = static_cast<std::int64_t>(pre) * timescale;
	const std::int64_t part = std::llround(static_cast<double>(frac) * static_cast<double>(timescale));
	return -(whole + part);
}

#endif
