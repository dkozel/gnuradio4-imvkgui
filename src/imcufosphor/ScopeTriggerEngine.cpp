/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of ScopeTriggerEngine
 */

#include "ScopeTriggerEngine.h"

#include <algorithm>
#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ScopeTriggerEngine::ScopeTriggerEngine()
	: m_recordLength(2)
	, m_preSamples(1)
	, m_timescale(0)
	, m_holdoffSamples(0)
	, m_state(CaptureState::Armed)
	, m_scanPos(0)
	, m_armAt(0)
	, m_force(false)
	, m_haveClock(false)
	, m_captureCount(0)
	, m_missedCount(0)
	, m_lastCaptureReal(false)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

void ScopeTriggerEngine::Configure(
	const TriggerConfig& cfg,
	size_t recordLength,
	int64_t timescaleFs,
	uint64_t historyEnd)
{
	//Two samples is what the rasterizer needs to draw a segment, and what ComputePipeline needs
	//to bind a buffer at all - it skips an empty one with only a warning and leaves the previous
	//frame's descriptor in place (ComputePipeline.h:174-178).
	if(recordLength < 2)
		recordLength = 2;

	const bool invalidate = m_cfg.ScanInvalidatedBy(cfg) || (recordLength != m_recordLength);

	//Mode goes through SetMode() so that a change arriving this way makes the same state
	//transition as a change arriving from the toolbar. Two paths into run control that disagree
	//is how a scope ends up stuck in Stopped with the Run button lit.
	const TriggerMode newMode = cfg.mode;
	const TriggerMode oldMode = m_cfg.mode;
	m_cfg = cfg;
	m_cfg.mode = oldMode;

	m_recordLength = recordLength;
	m_timescale = timescaleFs;

	const float pre = clamp(m_cfg.pretrigger, 0.0f, 1.0f);
	m_preSamples = static_cast<size_t>(llround(
		static_cast<double>(pre) * static_cast<double>(recordLength - 1)));
	if(m_preSamples >= recordLength)
		m_preSamples = recordLength - 1;

	//Converted against the sample clock once, here, so there is no holdoff timer running on a
	//different clock that could disagree with the sample indices the search works in.
	m_holdoffSamples = ((timescaleFs > 0) && (m_cfg.holdoffFs > 0))
		? static_cast<uint64_t>((m_cfg.holdoffFs + timescaleFs - 1) / timescaleFs)
		: 0;

	if(invalidate)
		Invalidate(historyEnd);

	SetMode(newMode);
}

void ScopeTriggerEngine::SetMode(TriggerMode mode)
{
	if(mode == m_cfg.mode)
		return;

	m_cfg.mode = mode;

	if(mode == TriggerMode::Stop)
	{
		//Discards anything pending. Stop means stop, and completing a record the user paused
		//partway through would put a sweep on screen after they asked for none.
		m_state = CaptureState::Stopped;
		return;
	}

	//Anything else resumes searching, unless a capture is already in flight
	if((m_state == CaptureState::Stopped) || (m_state == CaptureState::Held))
		m_state = CaptureState::Armed;
}

void ScopeTriggerEngine::Rearm()
{
	if((m_state == CaptureState::Stopped) || (m_state == CaptureState::Held))
		m_state = CaptureState::Armed;
}

void ScopeTriggerEngine::Invalidate(uint64_t historyEnd)
{
	//Not historyEnd-1: that would re-evaluate a pair the previous configuration already saw, and
	//it underflows on an empty history. One pair lost at the moment of a deliberate invalidation
	//is not a signal anyone was watching for.
	m_scanPos = historyEnd;
	m_arm = ArmState();

	if(m_state == CaptureState::PostFill)
		m_state = (m_cfg.mode == TriggerMode::Stop) ? CaptureState::Stopped : CaptureState::Armed;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Acquisition

bool ScopeTriggerEngine::BeginForcedCapture(uint64_t historyBegin, uint64_t historyEnd)
{
	if(historyEnd < m_recordLength)
		return false;

	const uint64_t start = historyEnd - m_recordLength;
	if(start < historyBegin)
		return false;

	m_pending.start = start;
	m_pending.triggerIndex = start + m_preSamples;
	m_pending.phaseFs = TriggerPhaseFs(m_preSamples, 0.0f, m_timescale);
	m_pending.real = false;

	m_state = CaptureState::PostFill;
	return true;
}

void ScopeTriggerEngine::ReArm(uint64_t triggerIndex, uint64_t /*historyBegin*/)
{
	//The +1 is what stops a scan that re-enters at the same index from reporting the same edge
	//twice. Without it a zero holdoff is not "no holdoff", it is "trigger repeatedly on one edge".
	//
	//The other constraint on m_armAt - that the pre-trigger depth must still be resident - is
	//applied at the scan site instead, because it has to hold before the first capture too and
	//nothing has called this yet at that point.
	m_armAt = max(m_armAt, triggerIndex + 1 + m_holdoffSamples);
}

bool ScopeTriggerEngine::Straddles(span<const uint64_t> gaps, uint64_t start, size_t n)
{
	const uint64_t end = start + n;
	for(uint64_t g : gaps)
	{
		if( (g > start) && (g < end) )
			return true;
	}
	return false;
}

optional<CaptureRequest> ScopeTriggerEngine::TryComplete(
	uint64_t historyBegin,
	uint64_t historyEnd,
	span<const uint64_t> gaps)
{
	if(m_state != CaptureState::PostFill)
		return nullopt;

	const uint64_t start = m_pending.start;

	//Straddling a gap, or aged out of the ring. Either way the record cannot be assembled from
	//samples that are all really adjacent, and a record spliced across a gap is wrong in a way
	//that looks entirely plausible on screen.
	//
	//Strictly inside, not "anywhere at or before the end". On a lossy stream - which is the
	//normal case, since a display must never stall a flowgraph - there is a gap at the end of
	//every drain, so refusing every record that merely starts before the newest gap refuses all
	//of them and the scope displays nothing at all. Measured: 0 sweeps, 44 missed.
	if(Straddles(gaps, start, m_recordLength))
	{
		NoteAbandoned();
		return nullopt;
	}
	if(start < historyBegin)
	{
		NoteAbandoned();
		return nullopt;
	}

	//Still filling. Stay in PostFill and keep the pending record; this is the whole reason that
	//state exists.
	if((start + m_recordLength) > historyEnd)
		return nullopt;

	return m_pending;
}

optional<CaptureRequest> ScopeTriggerEngine::Step(
	span<const float> scan,
	uint64_t scanBase,
	uint64_t historyBegin,
	uint64_t historyEnd,
	span<const uint64_t> gaps,
	chrono::steady_clock::time_point now)
{
	if(!m_haveClock)
	{
		m_lastCapture = now;
		m_haveClock = true;
	}

	const bool forced = m_force.exchange(false, memory_order_acq_rel);

	//No sample rate means no time axis. Refuse to capture rather than publish a waveform whose
	//timescale the rasterizer will silently reject (SpectrumArea.cpp:231), which leaves a blank
	//pane and no explanation of why.
	if(m_timescale <= 0)
		return nullopt;

	switch(m_state)
	{
		case CaptureState::PostFill:
			return TryComplete(historyBegin, historyEnd, gaps);

		case CaptureState::Stopped:
		case CaptureState::Held:
			//Force is the only way to get one sweep out of a paused scope, and it is what the
			//button means to whoever pressed it.
			if(forced && BeginForcedCapture(historyBegin, historyEnd))
				return TryComplete(historyBegin, historyEnd, gaps);
			return nullopt;

		case CaptureState::Armed:
		default:
			break;
	}

	if(forced || (m_cfg.kind == TriggerKind::FreeRun))
	{
		if(BeginForcedCapture(historyBegin, historyEnd))
			return TryComplete(historyBegin, historyEnd, gaps);
		return nullopt;
	}

	//The pre-trigger depth has to exist before a crossing can be the middle of a record. Applied
	//here rather than folded into m_armAt because it must hold before the first capture too, and
	//because historyBegin moves every frame while m_armAt only moves on a trigger.
	const uint64_t notBefore = max(m_armAt, historyBegin + m_preSamples);

	if(scan.size() >= 2)
	{
		const bool magnitude = (m_cfg.op == TriggerOperator::Magnitude);
		const float h = fabsf(m_cfg.hysteresis);

		//A negative magnitude level can never be crossed, but level*level is positive and would
		//fire on the noise floor. Squaring an unreachable threshold into a reachable one is
		//exactly the bug this guard exists for.
		if(magnitude && (m_cfg.level < 0))
		{
			m_scanPos = scanBase + scan.size() - 1;
			return nullopt;
		}

		//The band transforms with the domain. Squaring is monotonic on non-negative values, so
		//the crossings of |z| against level are exactly the crossings of |z|^2 against level^2 -
		//not approximately, exactly - which is what licenses skipping a square root per sample.
		float level = m_cfg.level;
		float lo = level - h;
		float hi = level + h;
		if(magnitude)
		{
			lo = (lo > 0) ? (lo * lo) : 0.0f;
			hi = hi * hi;
			level = level * level;
		}

		auto crossing = ScanEdges(scan, scanBase, lo, level, hi, m_cfg.slope, notBefore, m_arm);

		//Advanced whether or not anything was found, and to the last sample rather than one past
		//it, so the pair straddling this boundary is the first one the next window evaluates.
		//Every adjacent pair is then seen by exactly one frame, forever.
		m_scanPos = scanBase + scan.size() - 1;

		if(crossing.has_value())
		{
			const size_t k = static_cast<size_t>(crossing->index - scanBase);

			//Two square roots, of the bracketing samples only, so the fraction is taken in the
			//magnitude domain rather than the squared one. |z|^2 stepping 0 -> 9 against a level
			//of 1 crosses a quarter of the way through in the squared domain and a third of the
			//way through in the real one; the second is where the signal actually was.
			float a = scan[k];
			float b = scan[k + 1];
			if(magnitude)
			{
				a = sqrtf(a);
				b = sqrtf(b);
			}

			const float frac = CrossingFraction(a, b, m_cfg.level);

			m_pending.start = crossing->index - m_preSamples;
			m_pending.triggerIndex = crossing->index;
			m_pending.phaseFs = TriggerPhaseFs(m_preSamples, frac, m_timescale);
			m_pending.real = true;

			m_state = CaptureState::PostFill;
			return TryComplete(historyBegin, historyEnd, gaps);
		}
	}

	//Auto's clock runs from the last completed capture, not from the last failed search. A slow
	//but real trigger rate must not be overridden by the timeout, or a 1 Hz signal shows forced
	//sweeps it never asked for and the real edge never gets drawn.
	if(m_cfg.mode == TriggerMode::Auto)
	{
		const auto elapsed = now - m_lastCapture;
		if(elapsed > chrono::milliseconds(m_cfg.autoTimeoutMs))
		{
			if(BeginForcedCapture(historyBegin, historyEnd))
				return TryComplete(historyBegin, historyEnd, gaps);
		}
	}

	return nullopt;
}

void ScopeTriggerEngine::NoteCaptureComplete(
	const CaptureRequest& req,
	uint64_t historyBegin,
	chrono::steady_clock::time_point now)
{
	m_captureCount++;
	m_lastCapture = now;
	m_haveClock = true;
	m_lastCaptureReal = req.real;

	//A forced sweep does not consume holdoff. Auto has to keep hunting for the real edge at the
	//rate the signal provides it, not be pushed away every time the timeout fires.
	if(req.real)
		ReArm(req.triggerIndex, historyBegin);

	switch(m_cfg.mode)
	{
		//One sweep, then hold it. Held rather than Stopped so the readout can tell "captured and
		//waiting for you" apart from "you pressed Stop".
		case TriggerMode::Single:
			m_state = CaptureState::Held;
			break;

		case TriggerMode::Stop:
			m_state = CaptureState::Stopped;
			break;

		default:
			m_state = CaptureState::Armed;
			break;
	}
}

void ScopeTriggerEngine::NoteAbandoned()
{
	m_missedCount++;
	m_state = (m_cfg.mode == TriggerMode::Stop) ? CaptureState::Stopped : CaptureState::Armed;
}

const char* ScopeTriggerEngine::GetStateText() const
{
	switch(m_state)
	{
		case CaptureState::Stopped:
			return "Stop";

		case CaptureState::Armed:
			return (m_cfg.kind == TriggerKind::FreeRun) ? "Roll" : "Wait";

		case CaptureState::PostFill:
			return "Trig'd";

		case CaptureState::Held:
		default:
			return m_lastCaptureReal ? "Trig'd" : "Auto";
	}
}
