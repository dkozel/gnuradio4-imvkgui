/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of ScopeCapture
 */

#include "ScopeCapture.h"

#include <algorithm>
#include <cmath>
#include <ranges>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ScopeCapture::ScopeCapture()
	: m_timescale(0)
	, m_components(1)
	, m_haveRecord(false)
	, m_resyncCount(0)
{
}

ScopeCapture::~ScopeCapture()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

void ScopeCapture::RebuildTraces(size_t ports, size_t comps)
{
	m_traces.clear();
	for(size_t p = 0; p < ports; p++)
	{
		for(size_t c = 0; c < comps; c++)
		{
			ScopeTrace t;
			t.port = p;
			t.component = c;

			if(comps > 1)
				t.name = "ch" + to_string(p) + ((c == 0) ? " I" : " Q");
			else
				t.name = "ch" + to_string(p);

			m_traces.push_back(t);
		}
	}
}

void ScopeCapture::Configure(size_t ports, size_t componentsPerSample, const ScopeCaptureConfig& cfg)
{
	if(ports < 1)
		ports = 1;
	if(componentsPerSample < 1)
		componentsPerSample = 1;

	ScopeCaptureConfig c = cfg;

	//Two samples is the minimum the rasterizer can draw a segment from, and the minimum
	//ComputePipeline will bind at all - it skips an empty buffer with only a warning and leaves
	//the previous frame's descriptor in place (ComputePipeline.h:174-178).
	if(c.recordLength < 2)
		c.recordLength = 2;

	//One division, recomputed from the rate rather than accumulated. At 245.76 MS/s the true
	//period is 4069010.4167 fs against a stored 4069010; that 1.0e-7 relative error accumulates
	//to about a tenth of a sample over a megasample record, which is the same error every
	//scopehal waveform carries and is worth knowing about before someone measures it. See
	//notes/time-domain-plan.md, "Never accumulate femtoseconds".
	int64_t timescale = (c.sampleRate > 0)
		? static_cast<int64_t>(llround(FS_PER_SECOND / c.sampleRate))
		: 0;

	//int64 femtoseconds saturate at 9.22e18, so a record cannot span more than about 9200
	//seconds. Only reachable at absurdly low sample rates, but the failure if it is reached is a
	//wrapped trigger phase and a trace drawn in the wrong place, which nothing else would catch.
	if((timescale > 0) && ((static_cast<double>(c.recordLength) * static_cast<double>(timescale)) > 8e18))
	{
		const size_t maxLen = static_cast<size_t>(8e18 / static_cast<double>(timescale));
		LogWarning("ScopeCapture: record of %zu samples at %" PRId64 " fs/sample overflows the "
			"femtosecond axis, clamping to %zu\n", c.recordLength, timescale, maxLen);
		c.recordLength = max<size_t>(2, maxLen);
	}

	//Enough to hold a record plus room for the trigger to be found somewhere inside the history
	//rather than always at its very edge. Four records is arbitrary but generous; the cost is
	//linear and the benefit is that a trigger near the start of a drain still has its pre-trigger.
	size_t depth = c.historyDepth;
	if(depth == 0)
		depth = c.recordLength * 4;
	depth = max(depth, c.recordLength + 2);
	c.historyDepth = depth;

	const bool portsChanged = (ports != m_histories.size()) || (componentsPerSample != m_components);
	const bool depthChanged = (depth != m_cfg.historyDepth);
	const bool lengthChanged = (c.recordLength != m_cfg.recordLength);

	if(portsChanged)
	{
		//ScopeHistory holds a vector, so it is movable, but the waveforms below are not - and
		//keeping both in the same shape means one loop rather than two conventions.
		m_histories.clear();
		for(size_t i = 0; i < ports; i++)
			m_histories.push_back(make_unique<ScopeHistory>());

		m_components = componentsPerSample;
		RebuildTraces(ports, componentsPerSample);

		//Waveforms are allocated here, which is why Configure() is render thread only:
		//AcceleratorBuffer's constructor needs g_vkComputeDevice.
		m_waveforms.clear();
		for(size_t i = 0; i < m_traces.size(); i++)
		{
			auto w = make_unique<UniformAnalogWaveform>("ScopeCapture." + m_traces[i].name);
			m_waveforms.push_back(std::move(w));
		}
		m_haveRecord = false;
	}

	if(portsChanged || depthChanged)
	{
		for(auto& h : m_histories)
			h->Resize(depth, m_components);

		//The histories were just emptied, so nothing before now is resident and nothing recorded
		//against the old buffer can be assembled from the new one.
		if(!m_histories.empty())
			NoteDiscontinuity(m_histories[0]->End());
	}

	if(portsChanged || lengthChanged)
	{
		for(auto& w : m_waveforms)
		{
			//Exact rather than amortised: AcceleratorBuffer::resize() doubles capacity on growth,
			//and a record length nudged upward by a UI slider would otherwise sit at twice the
			//memory it needs until something called shrink_to_fit.
			w->m_samples.resize(c.recordLength, true);
		}
		m_haveRecord = false;
	}

	m_cfg = c;
	m_timescale = timescale;

	const uint64_t end = m_histories.empty() ? 0 : m_histories[0]->End();
	m_trigger.Configure(c.trigger, c.recordLength, timescale, end);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Ingest

void ScopeCapture::Append(size_t port, span<const float> interleaved)
{
	if(port >= m_histories.size())
		return;

	m_histories[port]->Append(interleaved);
}

void ScopeCapture::NoteDiscontinuity(uint64_t streamIndex)
{
	//Ascending and deduplicated. The sink republishes its newest marker every frame, so without
	//the equality check this would grow by one entry per frame forever.
	if(m_gaps.empty() || (streamIndex > m_gaps.back()))
		m_gaps.push_back(streamIndex);
}

void ScopeCapture::Resync()
{
	//Not a repair. Discarding samples to bring the ports back into step would shift one channel
	//in time against another, and two channels a few samples apart look entirely plausible on
	//screen - which is exactly why that is never done. Throw everything away instead, mark the
	//gap, and lose one acquisition.
	uint64_t newest = 0;
	for(auto& h : m_histories)
		newest = max(newest, h->End());

	for(auto& h : m_histories)
		h->Clear();

	NoteDiscontinuity(newest);
	m_trigger.Invalidate(newest);
	m_resyncCount++;

	LogWarning("ScopeCapture: input histories out of step, discarding and resynchronising\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Acquisition

bool ScopeCapture::CopyRecord(const CaptureRequest& req)
{
	const size_t n = m_cfg.recordLength;

	for(size_t i = 0; i < m_traces.size(); i++)
	{
		auto& t = m_traces[i];
		auto& w = *m_waveforms[i];

		if(t.port >= m_histories.size())
			return false;

		w.PrepareForCpuAccess();
		if(w.m_samples.size() < n)
			return false;

		if(!m_histories[t.port]->Gather(req.start, n, t.component,
			span<float>(w.m_samples.GetCpuPointer(), n)))
		{
			return false;
		}

		w.m_timescale = m_timescale;
		w.m_triggerPhase = req.phaseFs;

		//Left at zero deliberately. notes/time-domain-plan.md section 3 is the long version: a
		//fabricated timestamp reaching a display took a long time to find the first time. If a
		//RecordingClock is ever wired in here, fill these from the stream index; until then the
		//readout is relative only, and says so.
		w.m_startTimestamp = 0;
		w.m_startFemtoseconds = 0;

		w.m_revision++;
		w.MarkModifiedFromCpu();
	}

	return true;
}

bool ScopeCapture::Update(chrono::steady_clock::time_point now)
{
	if(m_histories.empty())
		return false;

	//The invariant the sink's joint admission exists to provide. Checked rather than assumed
	//because the recovery is expensive and silent corruption is not.
	const uint64_t end = m_histories[0]->End();
	for(size_t p = 1; p < m_histories.size(); p++)
	{
		if(m_histories[p]->End() != end)
		{
			Resync();
			return false;
		}
	}

	const uint64_t begin = m_histories[0]->Begin();

	//Materialise the scan window only when the engine will actually look at it. In Stop mode, and
	//in free run, this is the expensive half of the frame and none of it would be read.
	m_scan.clear();
	uint64_t scanBase = end;

	if(m_trigger.WantsScan() && !m_traces.empty())
	{
		const auto& cfg = m_trigger.GetConfig();
		const size_t srcTrace = min(cfg.source, m_traces.size() - 1);
		const auto& src = m_traces[srcTrace];

		scanBase = max(m_trigger.GetScanPos(), begin);
		if(end > scanBase)
		{
			m_scan.resize(static_cast<size_t>(end - scanBase));

			const bool ok = (cfg.op == TriggerOperator::Magnitude) && (m_components >= 2)
				? m_histories[src.port]->GatherMagSquared(scanBase, m_scan.size(), m_scan)
				: m_histories[src.port]->Gather(scanBase, m_scan.size(), src.component, m_scan);

			//Resident by construction, since the window starts at Begin() at the earliest. If it
			//ever is not, an empty window is the safe answer: the engine simply finds nothing.
			if(!ok)
				m_scan.clear();
		}
	}

	//Anything older than the oldest resident sample cannot fall inside a record we could still
	//assemble, so it can go. That is what keeps this bounded without a fixed cap.
	if(!m_gaps.empty())
	{
		const auto stale = ranges::upper_bound(m_gaps, begin);
		m_gaps.erase(m_gaps.begin(), stale);
	}

	auto req = m_trigger.Step(m_scan, scanBase, begin, end, m_gaps, now);
	if(!req.has_value())
		return false;

	if(!CopyRecord(*req))
	{
		m_trigger.NoteAbandoned();
		return false;
	}

	m_trigger.NoteCaptureComplete(*req, begin, now);
	m_haveRecord = true;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Output

UniformAnalogWaveform* ScopeCapture::GetWaveform(size_t i)
{
	if(!m_haveRecord || (i >= m_waveforms.size()))
		return nullptr;

	return m_waveforms[i].get();
}

bool ScopeCapture::GetRecordRange(double& lo, double& hi) const
{
	if((m_timescale <= 0) || m_waveforms.empty() || !m_haveRecord)
		return false;

	const auto& w = *m_waveforms[0];
	const size_t n = w.size();
	if(n < 2)
		return false;

	lo = static_cast<double>(w.m_triggerPhase);
	hi = static_cast<double>(static_cast<int64_t>(n - 1) * m_timescale + w.m_triggerPhase);
	return true;
}
