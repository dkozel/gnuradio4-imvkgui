/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks the edge search, the arming rules, the phase arithmetic and the four run modes

	No GPU and no Vulkan device, and that is not incidental. AcceleratorBuffer's constructor
	dereferences g_vkComputeDevice to create two sync events (AcceleratorBuffer.h:561-564), so a
	UniformAnalogWaveform cannot be constructed without a device at all. ScopeTriggerEngine is
	deliberately on the other side of that line: it decides which samples make a record and what
	m_triggerPhase should be, and ScopeCapture is the shell that copies them. Every rule worth
	getting right is therefore a decision, and every decision is checkable here.
 */

#include <boost/ut.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "ScopeHistory.h"
#include "ScopeTrigger.h"
#include "ScopeTriggerEngine.h"

using namespace boost::ut;

namespace {

/**
	@brief ScopeCapture's per-frame loop with the waveforms left out

	The same sequence the real capture runs - drain, materialise the scan window from the scan
	position, Step(), accept - minus the one part that needs a GPU. Driving the engine through
	this rather than calling Step() directly is what makes the scan-position tests mean anything:
	the overlap-by-one lives in the interaction between the two, not in either alone.
 */
struct Rig
{
	ScopeHistory hist;
	ScopeTriggerEngine eng;
	std::vector<float> scan;
	std::vector<CaptureRequest> captures;
	std::vector<std::uint64_t> gaps;
	std::chrono::steady_clock::time_point now{};
	TriggerConfig cfg;

	void Configure(const TriggerConfig& c, std::size_t recordLength, std::int64_t timescale,
		std::size_t comps = 1, std::size_t depth = 1 << 16)
	{
		cfg = c;
		hist.Resize(depth, comps);
		eng.Configure(c, recordLength, timescale, hist.End());
	}

	void FeedReal(std::span<const float> s)
	{ hist.Append(s); }

	void FeedComplex(std::span<const std::complex<float>> s)
	{
		hist.Append(std::span<const float>(
			reinterpret_cast<const float*>(s.data()), s.size() * 2));
	}

	///@brief One display frame
	void Frame()
	{
		scan.clear();
		std::uint64_t base = hist.End();

		if(eng.WantsScan())
		{
			base = std::max(eng.GetScanPos(), hist.Begin());
			if(hist.End() > base)
			{
				scan.resize(static_cast<std::size_t>(hist.End() - base));
				const bool ok = (cfg.op == TriggerOperator::Magnitude)
					? hist.GatherMagSquared(base, scan.size(), scan)
					: hist.Gather(base, scan.size(), cfg.source, scan);
				expect(ok) << "the scan window must be resident by construction";
			}
		}

		auto req = eng.Step(scan, base, hist.Begin(), hist.End(), gaps, now);
		if(req.has_value())
		{
			captures.push_back(*req);
			eng.NoteCaptureComplete(*req, hist.Begin(), now);
		}
	}

	void Frames(int n)
	{
		for(int i = 0; i < n; i++)
			Frame();
	}

	void Advance(std::int64_t ms)
	{ now += std::chrono::milliseconds(ms); }
};

///@brief A ramp from @a from to @a to over @a n samples
std::vector<float> Ramp(std::size_t n, float from, float to)
{
	std::vector<float> v(n);
	for(std::size_t i = 0; i < n; i++)
		v[i] = from + (to - from) * static_cast<float>(i) / static_cast<float>(n - 1);
	return v;
}

///@brief A square wave of @a cycles periods, @a period samples each, alternating -1 and +1
std::vector<float> Square(std::size_t cycles, std::size_t period)
{
	std::vector<float> v;
	for(std::size_t c = 0; c < cycles; c++)
	{
		for(std::size_t i = 0; i < period; i++)
			v.push_back((i < period/2) ? -1.0f : 1.0f);
	}
	return v;
}

TriggerConfig EdgeCfg(float level = 0.0f, TriggerSlope slope = TriggerSlope::Rising)
{
	TriggerConfig c;
	c.mode = TriggerMode::Normal;
	c.kind = TriggerKind::Edge;
	c.slope = slope;
	c.op = TriggerOperator::Raw;
	c.level = level;
	c.pretrigger = 0;
	return c;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"ScanEdges"> scanEdgeTests = [] {

	"a rising crossing is reported at the left sample of the pair"_test = [] {
		const float w[] = {-1, -1, -1, 1, 1};
		ArmState st;
		auto c = ScanEdges(w, 1000, 0.0f, 0.0f, 0.0f, TriggerSlope::Rising, 0, st);

		expect(c.has_value());
		expect(eq(c->index, std::uint64_t{1002})) << "the crossing lies between w[2] and w[3]";
		expect(c->rising);
	};

	"the level belongs to the above half plane"_test = [] {
		//A sample exactly at the level counts as above. That is what makes rising and falling
		//mutually exclusive, and therefore what stops Any reporting one edge twice.
		ArmState a;
		const float rise[] = {-1, 0, 1};
		auto cr = ScanEdges(rise, 0, 0.0f, 0.0f, 0.0f, TriggerSlope::Rising, 0, a);
		expect(cr.has_value());
		expect(eq(cr->index, std::uint64_t{0})) << "-1 -> 0 is already the crossing";

		ArmState b;
		const float fall[] = {0, -1};
		auto cf = ScanEdges(fall, 0, 0.0f, 0.0f, 0.0f, TriggerSlope::Falling, 0, b);
		expect(cf.has_value());
		expect(eq(cf->index, std::uint64_t{0})) << "0 -> -1 is a fall, since 0 was above";
	};

	"Any finds each crossing exactly once"_test = [] {
		//A scope that reports one edge twice does not look like a bug, it looks like jitter.
		const auto w = Square(8, 16);

		ArmState st;
		std::uint64_t notBefore = 0;
		std::size_t found = 0;
		while(true)
		{
			auto c = ScanEdges(w, 0, 0.0f, 0.0f, 0.0f, TriggerSlope::Any, notBefore, st);
			if(!c.has_value())
				break;
			found++;
			notBefore = c->index + 1;
		}

		//8 cycles of -1 then +1: a rise at each cycle boundary within the buffer and a fall at
		//each half period, minus the leading edge of the first cycle which has no left sample
		std::size_t expected = 0;
		for(std::size_t i = 0; (i + 1) < w.size(); i++)
		{
			const bool rise = (w[i] < 0) && (w[i+1] >= 0);
			const bool fall = (w[i] >= 0) && (w[i+1] < 0);
			if(rise || fall)
				expected++;
		}
		expect(eq(found, expected)) << "every sign change, and nothing else";
	};

	"hysteresis of zero is exactly the plain crossing test"_test = [] {
		//lo == hi == level makes belowArm's condition identical to the rising precondition, so
		//the arm flags cannot change the answer. Pinning this means hysteresis costs nothing to
		//reason about when it is switched off.
		const auto w = Square(4, 10);

		ArmState armed;
		ArmState plain;
		std::uint64_t nb = 0;
		std::vector<std::uint64_t> withArm;
		std::vector<std::uint64_t> withoutArm;

		while(auto c = ScanEdges(w, 0, 0.0f, 0.0f, 0.0f, TriggerSlope::Any, nb, armed))
		{
			withArm.push_back(c->index);
			nb = c->index + 1;
		}

		//The same sweep done by hand, with no arming at all
		for(std::size_t i = 0; (i + 1) < w.size(); i++)
		{
			if(((w[i] < 0) && (w[i+1] >= 0)) || ((w[i] >= 0) && (w[i+1] < 0)))
				withoutArm.push_back(i);
		}
		(void)plain;

		expect(eq(withArm.size(), withoutArm.size()));
		expect(std::ranges::equal(withArm, withoutArm));
	};

	"hysteresis suppresses noise riding on the level"_test = [] {
		//A deterministic dither of +-0.05 straddling the level. Without a band this crosses on
		//most sample pairs; with one it never leaves the band and so never arms.
		std::vector<float> noisy(1000);
		for(std::size_t i = 0; i < noisy.size(); i++)
			noisy[i] = (i % 2) ? 0.05f : -0.05f;

		ArmState bare;
		auto naked = ScanEdges(noisy, 0, 0.0f, 0.0f, 0.0f, TriggerSlope::Rising, 0, bare);
		expect(naked.has_value()) << "with no band, dither crosses immediately";

		ArmState banded;
		auto guarded = ScanEdges(noisy, 0, -0.1f, 0.0f, 0.1f, TriggerSlope::Rising, 0, banded);
		expect(!guarded.has_value()) << "the signal never leaves the band, so it never arms";
	};

	"a band still triggers on the first genuine edge"_test = [] {
		//The cost of starting unarmed, measured: a signal that really does leave the band arms on
		//the first sample that does, not one pair later. If this drifted to "the first edge is
		//always missed", a scope with hysteresis on would look broken rather than selective.
		const float w[] = {-1, -1, 1, 1};
		ArmState st;
		auto c = ScanEdges(w, 0, -0.5f, 0.0f, 0.5f, TriggerSlope::Rising, 0, st);

		expect(c.has_value());
		expect(eq(c->index, std::uint64_t{1})) << "the pair spanning the edge, and no later";
	};

	"notBefore steps over a crossing but still arms on it"_test = [] {
		//Holdoff and re-arming have to agree with each other. A skipped crossing that did not
		//update the arm flags would leave the next one wrongly suppressed.
		const float w[] = {-1, 1, -1, 1, -1, 1};
		ArmState st;
		auto c = ScanEdges(w, 0, -0.5f, 0.0f, 0.5f, TriggerSlope::Rising, 2, st);
		expect(c.has_value());
		expect(eq(c->index, std::uint64_t{2})) << "the crossing at 0 is skipped, the one at 2 taken";
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"trigger phase"> phaseTests = [] {

	"the crossing lands at axis zero"_test = [] {
		//The property that matters, stated as the property rather than as a magic number.
		//GetOffsetScaled() places sample i at i*timescale + triggerPhase, so the trigger sitting
		//at record index pre+frac means that expression is zero there.
		for(std::int64_t ts : {std::int64_t{1000}, std::int64_t{4069010}, std::int64_t{1000000000000}})
		{
			for(std::size_t pre : {std::size_t{0}, std::size_t{1}, std::size_t{500}, std::size_t{1000000}})
			{
				for(float f : {0.0f, 0.25f, 0.5f, 0.999f})
				{
					const std::int64_t phase = TriggerPhaseFs(pre, f, ts);
					const double axis =
						(static_cast<double>(pre) + static_cast<double>(f)) * static_cast<double>(ts)
						+ static_cast<double>(phase);

					//Tolerance scales with the timescale because the fractional part is rounded
					//to a whole femtosecond, and with pre because the double reconstruction here
					//is the lossy one, not the function under test.
					const double tol = std::max(1.0, static_cast<double>(ts) * 1e-6)
						+ static_cast<double>(pre) * static_cast<double>(ts) * 1e-15;
					expect(std::abs(axis) <= tol)
						<< "pre=" << pre << " f=" << f << " ts=" << ts << " axis=" << axis;
				}
			}
		}
	};

	"the phase is negative and is the pre-trigger duration"_test = [] {
		expect(eq(TriggerPhaseFs(2, 0.25f, 1000), std::int64_t{-2250}));
		expect(eq(TriggerPhaseFs(0, 0.0f, 1000), std::int64_t{0}))
			<< "no pre-trigger means the record starts at the trigger";
		expect(lt(TriggerPhaseFs(500, 0.0f, 1000000), std::int64_t{0}))
			<< "sample zero precedes the trigger, so the offset from trigger to clock is negative";
	};

	"the integer part never loses precision"_test = [] {
		//The failure this catches: forming (pre + frac) * timescale as one double. At pre = 1e6
		//and ts = 1e12 the product is 1e18, past double's 9.0e15 exact-integer limit, and the
		//answer comes out wrong by ~100 fs.
		expect(eq(TriggerPhaseFs(1000000, 0.0f, 1000000000000LL),
			-static_cast<std::int64_t>(1000000) * 1000000000000LL));
	};

	"sub-sample interpolation is linear and clamped inside the pair"_test = [] {
		expect(eq(CrossingFraction(-1.0f, 1.0f, 0.0f), 0.5f));
		expect(eq(CrossingFraction(0.0f, 4.0f, 1.0f), 0.25f));
		expect(lt(CrossingFraction(0.0f, 1.0f, 1.0f), 1.0f))
			<< "a fraction of exactly one would put the trigger on the next sample's grid point";
		expect(ge(CrossingFraction(0.0f, 1.0f, -5.0f), 0.0f)) << "and never before this one";
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"scan continuity"> continuityTests = [] {

	"a crossing split across two scan windows fires exactly once"_test = [] {
		Rig rig;
		rig.Configure(EdgeCfg(0.0f), 2, 1000);

		const auto ramp = Ramp(1024, -1.0f, 1.0f);

		rig.FeedReal(std::span(ramp).subspan(0, 512));
		rig.Frame();
		rig.FeedReal(std::span(ramp).subspan(512));
		rig.Frame();

		expect(eq(rig.captures.size(), std::size_t{1}));
	};

	"every split point gives the same answer"_test = [] {
		//The strong version. Any off-by-one in the overlap shows up as a different trigger index
		//or a different phase at some particular split, and only at that split.
		const auto ramp = Ramp(1024, -1.0f, 1.0f);

		std::uint64_t refIndex = 0;
		std::int64_t refPhase = 0;
		bool haveRef = false;
		std::size_t disagreements = 0;

		for(std::size_t split = 1; split < 1024; split++)
		{
			Rig rig;
			rig.Configure(EdgeCfg(0.0f), 2, 1000);

			rig.FeedReal(std::span(ramp).subspan(0, split));
			rig.Frame();
			rig.FeedReal(std::span(ramp).subspan(split));
			rig.Frame();
			rig.Frame();

			if(rig.captures.size() != 1)
			{
				disagreements++;
				continue;
			}

			if(!haveRef)
			{
				refIndex = rig.captures[0].triggerIndex;
				refPhase = rig.captures[0].phaseFs;
				haveRef = true;
			}
			else if((rig.captures[0].triggerIndex != refIndex) ||
				(rig.captures[0].phaseFs != refPhase))
			{
				disagreements++;
			}
		}

		expect(haveRef);
		expect(eq(disagreements, std::size_t{0}))
			<< "split point must not change the trigger index or the phase";
	};

	"the scan never revisits a pair"_test = [] {
		//Feeding one edge in many small pieces must still produce exactly one capture, however
		//the pieces fall.
		Rig rig;
		rig.Configure(EdgeCfg(0.0f), 2, 1000);

		const auto w = Square(1, 64);
		for(std::size_t i = 0; i < w.size(); i += 3)
		{
			rig.FeedReal(std::span(w).subspan(i, std::min<std::size_t>(3, w.size() - i)));
			rig.Frame();
		}
		rig.Frames(2);

		expect(eq(rig.captures.size(), std::size_t{1}))
			<< "one rising edge in the buffer, whatever the chunking";
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"holdoff and arming"> holdoffTests = [] {

	"holdoff is measured in samples derived from the timescale"_test = [] {
		//1 us of holdoff at a 1000 fs sample period is 1000 samples, so a square wave of period
		//100 triggers once every ten cycles rather than once per cycle.
		auto cfg = EdgeCfg(0.0f);
		cfg.holdoffFs = 1000000;

		Rig rig;
		rig.Configure(cfg, 2, 1000);

		const auto w = Square(40, 100);
		//One sample per frame would take 4000 frames; feed in blocks and run a frame each, which
		//is also the realistic shape
		for(std::size_t i = 0; i < w.size(); i += 50)
		{
			rig.FeedReal(std::span(w).subspan(i, std::min<std::size_t>(50, w.size() - i)));
			rig.Frame();
		}
		rig.Frames(2);

		expect(le(rig.captures.size(), std::size_t{5}))
			<< "40 rising edges, 1000-sample holdoff, 4000 samples: at most four intervals";
		expect(ge(rig.captures.size(), std::size_t{1}));

		for(std::size_t i = 1; i < rig.captures.size(); i++)
		{
			expect(ge(rig.captures[i].triggerIndex - rig.captures[i-1].triggerIndex,
				std::uint64_t{1000}));
		}
	};

	"zero holdoff still cannot fire twice on the same edge"_test = [] {
		Rig rig;
		rig.Configure(EdgeCfg(0.0f), 2, 1000);

		const float w[] = {-1, 1, 1, 1, 1, 1, 1, 1};
		rig.FeedReal(w);
		rig.Frames(6);

		expect(eq(rig.captures.size(), std::size_t{1}))
			<< "one edge, however many times the engine is stepped";
	};

	"a trigger is refused until the pre-trigger depth exists"_test = [] {
		//A crossing at sample 4 cannot be the middle of a record that needs 1000 samples before
		//it. Without this the record start underflows.
		auto cfg = EdgeCfg(0.0f);
		cfg.pretrigger = 0.5f;

		Rig rig;
		rig.Configure(cfg, 2001, 1000);
		expect(eq(rig.eng.GetPreSamples(), std::size_t{1000}));

		//An early edge, then a long quiet run, then a second edge well past the pre-trigger depth
		std::vector<float> w;
		w.push_back(-1);
		w.push_back(1);
		w.insert(w.end(), 3000, 1.0f);
		w.push_back(-1);
		w.push_back(1);
		w.insert(w.end(), 3000, 1.0f);

		rig.FeedReal(w);
		rig.Frames(4);

		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(ge(rig.captures[0].triggerIndex, std::uint64_t{1000}))
			<< "the edge at index 0 is unusable, the one at 3002 is not";
		expect(ge(rig.captures[0].start, std::uint64_t{0}));
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"magnitude operator"> magnitudeTests = [] {

	"the squared domain gives the same crossing set as the magnitude domain"_test = [] {
		//This is what licenses skipping a square root per sample.
		std::vector<float> mag(512);
		std::vector<float> mag2(512);
		for(std::size_t i = 0; i < mag.size(); i++)
		{
			const float env = 0.5f + 0.45f * std::sin(static_cast<float>(i) * 0.05f);
			mag[i] = env;
			mag2[i] = env * env;
		}

		const float level = 0.7f;

		std::vector<std::uint64_t> fromMag;
		std::vector<std::uint64_t> fromSquared;

		ArmState a;
		std::uint64_t nb = 0;
		while(auto c = ScanEdges(mag, 0, level, level, level, TriggerSlope::Any, nb, a))
		{
			fromMag.push_back(c->index);
			nb = c->index + 1;
		}

		ArmState b;
		nb = 0;
		const float l2 = level * level;
		while(auto c = ScanEdges(mag2, 0, l2, l2, l2, TriggerSlope::Any, nb, b))
		{
			fromSquared.push_back(c->index);
			nb = c->index + 1;
		}

		expect(eq(fromMag.size(), fromSquared.size()));
		expect(std::ranges::equal(fromMag, fromSquared));
	};

	"a negative magnitude level never triggers"_test = [] {
		//level*level would be positive and would fire on the noise floor, which is the exact bug
		//the guard exists for.
		auto cfg = EdgeCfg(-1.0f);
		cfg.op = TriggerOperator::Magnitude;

		Rig rig;
		rig.Configure(cfg, 2, 1000, 2);

		std::vector<std::complex<float>> w(256);
		for(std::size_t i = 0; i < w.size(); i++)
			w[i] = std::complex<float>(0.3f * std::cos(i * 0.4f), 0.3f * std::sin(i * 0.4f));

		rig.FeedComplex(w);
		rig.Frames(4);

		expect(eq(rig.captures.size(), std::size_t{0}));
	};

	"a burst envelope triggers on its rising edge"_test = [] {
		auto cfg = EdgeCfg(0.5f);
		cfg.op = TriggerOperator::Magnitude;

		Rig rig;
		rig.Configure(cfg, 2, 1000, 2);

		//Quiet, then a burst at amplitude 1
		std::vector<std::complex<float>> w;
		for(std::size_t i = 0; i < 100; i++)
			w.push_back(std::complex<float>(0.01f, 0.0f));
		for(std::size_t i = 0; i < 100; i++)
			w.push_back(std::complex<float>(std::cos(i * 0.3f), std::sin(i * 0.3f)));

		rig.FeedComplex(w);
		rig.Frames(3);

		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(ge(rig.captures[0].triggerIndex, std::uint64_t{99}));
		expect(le(rig.captures[0].triggerIndex, std::uint64_t{101}));
	};

	"the fraction is taken in the magnitude domain"_test = [] {
		//|z|^2 stepping 0 -> 9 against a level of 1 crosses one ninth of the way through in the
		//squared domain and one third of the way through in the real one. The signal was at the
		//second.
		expect(eq(CrossingFraction(0.0f, 3.0f, 1.0f), 1.0f/3.0f))
			<< "magnitudes 0 and 3";
		expect(neq(CrossingFraction(0.0f, 9.0f, 1.0f), 1.0f/3.0f))
			<< "and the squared values would have said something else";
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"run modes"> modeTests = [] {

	"Normal holds its last record when no trigger arrives"_test = [] {
		Rig rig;
		rig.Configure(EdgeCfg(0.0f), 2, 1000);

		const float edge[] = {-1, 1};
		rig.FeedReal(edge);
		rig.Frames(2);
		expect(eq(rig.captures.size(), std::size_t{1}));

		//Flat, well above the level: nothing to cross
		std::vector<float> flat(10000, 1.0f);
		rig.FeedReal(flat);
		rig.Advance(5000);
		rig.Frames(50);

		expect(eq(rig.captures.size(), std::size_t{1}))
			<< "Normal must not manufacture a sweep, however long it waits";
	};

	"Auto forces a sweep after the timeout and keeps searching"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.mode = TriggerMode::Auto;
		cfg.autoTimeoutMs = 100;

		Rig rig;
		rig.Configure(cfg, 4, 1000);

		std::vector<float> flat(1000, 1.0f);
		rig.FeedReal(flat);

		rig.Frame();
		expect(eq(rig.captures.size(), std::size_t{0})) << "not yet";

		rig.Advance(150);
		rig.Frame();
		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(!rig.captures[0].real) << "a timeout sweep is not a trigger, and must say so";

		//Now a real edge
		const float edge[] = {-1, 1, 1, 1, 1};
		rig.FeedReal(edge);
		rig.Frames(3);

		expect(ge(rig.captures.size(), std::size_t{2}));
		expect(rig.captures.back().real) << "and the real edge is still found afterwards";
	};

	"Auto's clock is reset by a real capture, not by a failed search"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.mode = TriggerMode::Auto;
		cfg.autoTimeoutMs = 100;

		Rig rig;
		rig.Configure(cfg, 2, 1000);

		//An edge every frame, with only 10 ms between frames: the timeout must never fire
		for(int i = 0; i < 20; i++)
		{
			const float edge[] = {-1, 1};
			rig.FeedReal(edge);
			rig.Advance(10);
			rig.Frames(2);
		}

		for(const auto& c : rig.captures)
			expect(c.real) << "a fast trigger rate must never be overridden by the timeout";
		expect(ge(rig.captures.size(), std::size_t{5}));
	};

	"Single captures once and then holds"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.mode = TriggerMode::Single;

		Rig rig;
		rig.Configure(cfg, 2, 1000);

		const auto w = Square(8, 16);
		rig.FeedReal(w);
		rig.Frames(10);

		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(rig.eng.GetState() == CaptureState::Held);

		//More edges change nothing until it is re-armed
		rig.FeedReal(w);
		rig.Frames(10);
		expect(eq(rig.captures.size(), std::size_t{1}));

		rig.eng.Rearm();
		expect(rig.eng.GetState() == CaptureState::Armed);
		rig.FeedReal(w);
		rig.Frames(4);
		expect(eq(rig.captures.size(), std::size_t{2}));
	};

	"Stop does not capture, and Force works from Stop"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.mode = TriggerMode::Stop;

		Rig rig;
		rig.Configure(cfg, 4, 1000);

		const auto w = Square(8, 16);
		rig.FeedReal(w);
		rig.Frames(5);
		expect(eq(rig.captures.size(), std::size_t{0}));
		expect(!rig.eng.WantsScan()) << "Stop must not pay for a scan window";

		rig.eng.RequestForce();
		rig.Frames(2);
		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(!rig.captures[0].real) << "forced, so the readout says Auto rather than Trig'd";
		expect(rig.eng.GetState() == CaptureState::Stopped) << "and it stays stopped";
	};

	"free run captures the newest full record every frame"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.kind = TriggerKind::FreeRun;

		Rig rig;
		rig.Configure(cfg, 64, 1000);
		expect(!rig.eng.WantsScan()) << "free run has nothing to search for";

		std::vector<float> w(1000, 0.5f);
		rig.FeedReal(w);
		rig.Frames(3);

		expect(eq(rig.captures.size(), std::size_t{3}));
		expect(eq(rig.captures.back().start, std::uint64_t{1000 - 64}));
		expect(eq(rig.captures.back().phaseFs, TriggerPhaseFs(rig.eng.GetPreSamples(), 0.0f, 1000)));
	};

	"a pending record is not displaced by a later edge"_test = [] {
		//The reason PostFill exists. A trigger whose tail has not arrived must complete as the
		//record it triggered on, not as a later one.
		auto cfg = EdgeCfg(0.0f);
		cfg.pretrigger = 0;

		Rig rig;
		rig.Configure(cfg, 1000, 1000);

		//One edge, then not enough samples to finish the record
		std::vector<float> first;
		first.push_back(-1);
		first.push_back(1);
		first.insert(first.end(), 100, 1.0f);
		rig.FeedReal(first);
		rig.Frame();
		expect(eq(rig.captures.size(), std::size_t{0}));
		expect(rig.eng.GetState() == CaptureState::PostFill);

		//A second edge arrives while we are still filling, plus the tail
		std::vector<float> second;
		second.push_back(-1);
		second.push_back(1);
		second.insert(second.end(), 2000, 1.0f);
		rig.FeedReal(second);
		rig.Frame();

		expect(eq(rig.captures.size(), std::size_t{1}));
		expect(eq(rig.captures[0].triggerIndex, std::uint64_t{0}))
			<< "the record that completes is the one that triggered";
	};

	"a record spanning a discontinuity is abandoned, not drawn"_test = [] {
		auto cfg = EdgeCfg(0.0f);
		cfg.pretrigger = 0;

		Rig rig;
		rig.Configure(cfg, 1000, 1000);

		std::vector<float> first;
		first.push_back(-1);
		first.push_back(1);
		first.insert(first.end(), 100, 1.0f);
		rig.FeedReal(first);
		rig.Frame();
		expect(rig.eng.GetState() == CaptureState::PostFill);

		//Samples were lost partway through the pending record, so it cannot be assembled from
		//samples that are all really adjacent
		rig.gaps.push_back(rig.hist.End());
		std::vector<float> rest(3000, 1.0f);
		rig.FeedReal(rest);
		rig.Frame();

		expect(eq(rig.captures.size(), std::size_t{0}));
		expect(eq(rig.eng.GetMissedCount(), std::uint64_t{1}));
		expect(eq(rig.eng.GetCaptureCount(), std::uint64_t{0}));
	};

	"a gap outside the record does not refuse it"_test = [] {
		//The bug this replaced: the check was "refuse any record starting before the newest gap".
		//On a lossy stream - the normal case, since the display must never stall the flowgraph -
		//there is a gap at the end of every drain, so that refused every record ever built and the
		//scope showed nothing at all. Measured against the real app: 0 sweeps, 44 missed.
		//
		//What matters is whether a gap falls strictly inside the record, not where it sits
		//relative to the start.
		auto cfg = EdgeCfg(0.0f);
		cfg.pretrigger = 0;

		Rig rig;
		rig.Configure(cfg, 64, 1000);

		//One clean run of samples containing an edge, then a gap at the very end of it
		std::vector<float> run;
		run.push_back(-1);
		run.push_back(1);
		run.insert(run.end(), 200, 1.0f);
		rig.FeedReal(run);
		rig.gaps.push_back(rig.hist.End());

		//And then the next run, which is what the gap separates
		std::vector<float> next(200, 1.0f);
		rig.FeedReal(next);

		rig.Frames(3);

		expect(eq(rig.captures.size(), std::size_t{1}))
			<< "the record lies wholly inside the first run, so the gap after it is irrelevant";
		expect(eq(rig.eng.GetMissedCount(), std::uint64_t{0}));
		expect(eq(rig.captures[0].triggerIndex, std::uint64_t{0}));
	};

	"a gap exactly at the record boundary does not refuse it"_test = [] {
		//The record is [start, start+N). A gap at start, or at start+N, separates it from its
		//neighbours and says nothing about the record itself: every sample inside is still
		//adjacent to the next one.
		const std::uint64_t gapsAt[] = {100, 164};
		expect(!ScopeTriggerEngine::Straddles(gapsAt, 100, 64)) << "both boundaries are exclusive";
		expect(ScopeTriggerEngine::Straddles(gapsAt, 99, 64)) << "but 100 is inside [99, 163)";
		expect(ScopeTriggerEngine::Straddles(gapsAt, 101, 64)) << "and 164 is inside [101, 165)";
	};

	"no sample rate means no capture at all"_test = [] {
		//A waveform with a zero timescale is silently dropped by the rasterizer, which leaves a
		//blank pane and no explanation. Refuse earlier, where the reason can be reported.
		auto cfg = EdgeCfg(0.0f);
		cfg.kind = TriggerKind::FreeRun;

		Rig rig;
		rig.Configure(cfg, 4, 0);

		std::vector<float> w(1000, 0.5f);
		rig.FeedReal(w);
		rig.Frames(5);

		expect(eq(rig.captures.size(), std::size_t{0}));
	};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const suite<"reconfiguration"> reconfigTests = [] {

	"changing the level resets the arm state"_test = [] {
		//The flags were latched against the old level and mean nothing under the new one.
		Rig rig;
		auto cfg = EdgeCfg(0.0f);
		cfg.hysteresis = 0.2f;
		rig.Configure(cfg, 2, 1000);

		std::vector<float> w(500, 1.0f);
		rig.FeedReal(w);
		rig.Frames(2);

		const auto before = rig.eng.GetScanPos();
		cfg.level = 0.5f;
		rig.cfg = cfg;
		rig.eng.Configure(cfg, 2, 1000, rig.hist.End());

		expect(eq(rig.eng.GetScanPos(), rig.hist.End()));
		expect(ge(rig.eng.GetScanPos(), before));
	};

	"changing the mode does not reset the scan"_test = [] {
		Rig rig;
		auto cfg = EdgeCfg(0.0f);
		rig.Configure(cfg, 2, 1000);

		std::vector<float> w(500, 1.0f);
		rig.FeedReal(w);
		rig.Frames(2);
		const auto pos = rig.eng.GetScanPos();

		cfg.mode = TriggerMode::Auto;
		rig.eng.Configure(cfg, 2, 1000, rig.hist.End());
		expect(eq(rig.eng.GetScanPos(), pos)) << "run control is not a search parameter";
	};

	"leaving Stop re-arms"_test = [] {
		Rig rig;
		auto cfg = EdgeCfg(0.0f);
		cfg.mode = TriggerMode::Stop;
		rig.Configure(cfg, 2, 1000);
		expect(rig.eng.GetState() == CaptureState::Stopped);

		rig.eng.SetMode(TriggerMode::Normal);
		expect(rig.eng.GetState() == CaptureState::Armed);
	};
};

int main() {}
