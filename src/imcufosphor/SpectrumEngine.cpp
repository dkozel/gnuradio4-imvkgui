/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SpectrumEngine
 */

#include "SpectrumEngine.h"

#include <algorithm>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SpectrumEngine::SpectrumEngine(EnginePart parts, shared_ptr<QueueHandle> queue)
	: m_parts(parts)
	, m_queue(queue)
	, m_fft(nullptr)
	, m_reducer(nullptr)
	, m_density(nullptr)
	, m_waterfall(nullptr)
	, m_configured(false)
	, m_rowsPlayed(0)
	, m_gpuTimingEnabled(false)
{
	m_injector = make_unique<IqInjector>("RX");
	auto chan = m_injector->GetChannel();

	m_fft = new ComplexFFTFilter("#ffffff");
	m_fft->AddRef();
	m_fft->SetInput("I", StreamDescriptor(chan, 0));

	//Wired even though the injector never fills it. GetInputWaveform(1) indexes m_inputs[1]
	//unconditionally, so the input has to exist; a packed waveform on stream 0 is what tells
	//the filter not to read it.
	m_fft->SetInput("Q", StreamDescriptor(chan, 1));
	m_fft->SetInput("center", StreamDescriptor(chan, 2));
	m_fft->SetWindowFunction(m_config.window);

	if(HasPart(m_parts, EnginePart::Waterfall))
	{
		m_reducer = new SpectrumReducer("#ffffff");
		m_reducer->AddRef();
		m_reducer->SetInput(0, StreamDescriptor(m_fft, 0));

		m_waterfall = new Waterfall("#ffffff");
		m_waterfall->AddRef();
		m_waterfall->SetInput(0, StreamDescriptor(m_reducer, 0));
	}

	if(HasPart(m_parts, EnginePart::Density))
	{
		//A parallel consumer of the same FFT batch, not a stage after the reducer. The two want
		//different cadences - the reducer folds every block, the density map every window - and
		//produce different output types.
		m_density = new SpectrumDensity("#ffffff");
		m_density->AddRef();
		m_density->SetInput(0, StreamDescriptor(m_fft, 0));
	}

	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		m_queue->GetQueue()->m_family);
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuf = make_unique<vk::raii::CommandBuffer>(
		std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	//Push the defaults down now, so the engine is usable without an explicit Configure()
	Configure(m_config);
}

SpectrumEngine::~SpectrumEngine()
{
	if(m_waterfall)
		m_waterfall->Release();
	if(m_density)
		m_density->Release();
	if(m_reducer)
		m_reducer->Release();
	if(m_fft)
		m_fft->Release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

void SpectrumEngine::Configure(const EngineConfig& config)
{
	EngineConfig cfg = config;

	//A block shorter than one transform would make the FFT filter fall back to transforming
	//the whole block, quietly changing the RBW. Clamp instead, so the requested transform
	//length is always what runs.
	if(cfg.blockSize < cfg.fftLength)
		cfg.blockSize = cfg.fftLength;

	if(cfg.groupSize < 1)
		cfg.groupSize = 1;

	//What actually moved. Computed before m_config is overwritten, and against the clamped
	//values so that a caller asking for a block size that gets clamped to the same place twice
	//does not reallocate on the second call.
	const bool first = !m_configured;
	const bool fftChanged = first || (cfg.fftLength != m_config.fftLength);
	const bool blockChanged = first || (cfg.blockSize != m_config.blockSize);
	const bool groupChanged = first || (cfg.groupSize != m_config.groupSize);
	const bool windowChanged = first || (cfg.window != m_config.window);
	const bool rangeChanged = first ||
		(cfg.rangeMin != m_config.rangeMin) || (cfg.rangeMax != m_config.rangeMax);
	const bool rateChanged = first || (cfg.sampleRate != m_config.sampleRate);
	const bool scaleChanged = first || (cfg.sampleScale != m_config.sampleScale);

	m_config = cfg;
	m_configured = true;

	if(scaleChanged)
		m_injector->SetSampleScale(cfg.sampleScale);

	if(windowChanged)
		m_fft->SetWindowFunction(cfg.window);

	if(fftChanged)
	{
		m_fft->SetFFTLength(cfg.fftLength);
		if(m_reducer)
			m_reducer->SetBinsPerSpectrum(cfg.fftLength);
		if(m_density)
			m_density->SetBinsPerSpectrum(cfg.fftLength);
	}

	if(groupChanged && m_reducer)
		m_reducer->GetParameter("Group size").SetIntVal(cfg.groupSize);

	//The density map's persistence constants are in seconds, so it needs to know how fast
	//spectra arrive to convert them. Skipped entirely until a rate is known: a live source
	//does not have one until the first tag arrives, and guessing would set the wrong decay.
	if((fftChanged || rateChanged) && m_density)
	{
		if((cfg.sampleRate > 0) && (cfg.fftLength > 0))
			m_density->SetSpectrumRate(cfg.sampleRate / cfg.fftLength);
	}

	if(rangeChanged)
	{
		//The waterfall maps its input against the source stream's range and offset
		//(Waterfall.cpp:159-160), so the colour scale is set on whatever feeds it.
		float range = cfg.rangeMax - cfg.rangeMin;
		float offset = -(cfg.rangeMax + cfg.rangeMin) / 2;

		m_fft->SetVoltageRange(range, 0);
		m_fft->SetOffset(offset, 0);

		if(m_reducer)
		{
			m_reducer->SetVoltageRange(range, 0);
			m_reducer->SetOffset(offset, 0);
		}

		//For the density map this is not merely presentation: it decides the histogram's cell
		//mapping, so a change discards the accumulated map.
		if(m_density)
			m_density->SetRange(cfg.rangeMin, cfg.rangeMax);
	}

	//Bin count changed, so anything accumulated against the old length is meaningless. Not on
	//the first call, where there is nothing accumulated to discard.
	if(fftChanged && !first)
	{
		if(m_reducer)
			m_reducer->ClearSweeps();
		if(m_density)
			m_density->ClearSweeps();
		if(m_waterfall)
			m_waterfall->ClearSweeps();
	}

	//blockChanged deliberately does nothing: the bin count is unchanged, so the accumulators
	//stay valid and only the number of spectra arriving per Refresh() moves.
	(void)blockChanged;
}

double SpectrumEngine::GetCoverage() const
{
	auto depth = static_cast<int64_t>(m_injector->GetLastBlockSize());
	if((depth <= 0) || (m_config.fftLength <= 0))
		return 0;

	//The FFT filter transforms as many whole transforms as fit in the block; anything left
	//over at the end is skipped. Coverage is therefore complete whenever the transform length
	//divides the block size, and short by the remainder when it does not.
	int64_t transformed = (depth / m_config.fftLength) * m_config.fftLength;
	return min(1.0, static_cast<double>(transformed) / depth);
}

void SpectrumEngine::ResetStats()
{
	m_stats = PlaybackStats();
}

void SpectrumEngine::Reset()
{
	if(m_reducer)
		m_reducer->ClearSweeps();
	if(m_density)
		m_density->ClearSweeps();
	if(m_waterfall)
		m_waterfall->ClearSweeps();

	m_rowHistory.Clear();
	m_rowsPlayed = 0;

	//The stream coordinate is a monotonic count of samples seen and a reset does not un-see
	//them; zeroing it here would make the waterfall's time axis jump backwards. There is no
	//separate recording coordinate on this path, so the two track each other.
	m_groupStart.streamStart = m_injector->GetSamplesPushed();
	m_groupStart.recordingStart = m_groupStart.streamStart;
	m_currentBlock = BlockSpan();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Processing

SpectrumEngine::StepResult SpectrumEngine::Step(span<const complex<float>> iq)
{
	if(iq.empty())
		return STEP_FAILED;

	double t0 = GetTime();
	m_injector->SetSamples(iq, m_config.sampleRate, m_config.centerFrequency);
	double t1 = GetTime();

	return StepAfterPush(iq.size(), t1 - t0);
}

SpectrumEngine::StepResult SpectrumEngine::Step(span<const complex<int16_t>> iq)
{
	if(iq.empty())
		return STEP_FAILED;

	double t0 = GetTime();
	m_injector->SetSamples(iq, m_config.sampleRate, m_config.centerFrequency);
	double t1 = GetTime();

	return StepAfterPush(iq.size(), t1 - t0);
}

SpectrumEngine::StepResult SpectrumEngine::StepAfterPush(size_t nsamples, double acquireSec)
{
	m_stats.acquireSec += acquireSec;
	double t1 = GetTime();

	//Where this block sits in the stream. Read after the push, which has already advanced the
	//counter past it, so the start is derived by subtraction rather than sampled beforehand.
	int64_t end = m_injector->GetSamplesPushed();
	int64_t start = end - static_cast<int64_t>(nsamples);
	m_currentBlock.streamStart = start;
	m_currentBlock.streamEnd = end;

	//A live stream has no position in a file. Reporting the stream coordinate in both slots
	//keeps anything that reads BlockSpan working, and is honest: for this source they are the
	//same number.
	m_currentBlock.recordingStart = start;
	m_currentBlock.recordingEnd = end;

	//The whole block in one command buffer and one submit. At 245.76 MS/s a 1 Msample block is
	//128 transforms, so the fence round trip is paid once instead of 128 times. See DESIGN.md
	//section 13.
	m_cmdBuf->begin({});
	if(m_gpuTimingEnabled)
		m_gpuTimer.Begin(*m_cmdBuf);

	m_fft->Refresh(*m_cmdBuf, m_queue);
	if(m_gpuTimingEnabled)
		m_gpuTimer.Mark(*m_cmdBuf, "fft");

	//Barriers between stages, which separate submits would provide for free. Without these the
	//reducer may read the FFT output before the postprocess dispatch has finished writing it,
	//and the waterfall may read the reducer's output before it is emitted. Same thing
	//FilterGraphExecutor does between dependency levels (FilterGraphExecutor.cpp:65-66).
	ComputePipeline::AddComputeMemoryBarrier(*m_cmdBuf);

	if(m_reducer)
	{
		m_reducer->Refresh(*m_cmdBuf, m_queue);
		if(m_gpuTimingEnabled)
			m_gpuTimer.Mark(*m_cmdBuf, "reduce");
	}

	//The density map reads the same FFT output the reducer does, so it needs no barrier
	//against the reducer - only against the FFT, which the barrier above already provides.
	if(m_density)
	{
		m_density->Refresh(*m_cmdBuf, m_queue);
		if(m_gpuTimingEnabled)
			m_gpuTimer.Mark(*m_cmdBuf, "density");
	}

	//Only advance the waterfall if the reducer completed a group. A block that lands mid-group
	//would otherwise write a row from a partial accumulation.
	bool advanced = m_reducer && m_reducer->IsOutputReady();
	if(advanced)
	{
		ComputePipeline::AddComputeMemoryBarrier(*m_cmdBuf);
		m_waterfall->Refresh(*m_cmdBuf, m_queue);
	}
	if(m_gpuTimingEnabled)
		m_gpuTimer.Mark(*m_cmdBuf, "waterfall");

	m_cmdBuf->end();

	double t2 = GetTime();
	m_queue->SubmitAndBlock(*m_cmdBuf);
	double t3 = GetTime();

	if(m_gpuTimingEnabled)
		m_gpuTimer.Collect();

	m_stats.recordSec += t2 - t1;
	m_stats.submitSec += t3 - t2;
	m_stats.submits++;
	m_stats.spectra += m_fft->GetSpectraPerBlock();

	if(advanced)
	{
		m_rowsPlayed++;

		//Record which samples this row covers. The reducer may have folded several blocks into
		//this row, so the row starts where the group started, not where this block did.
		m_rowHistory.SetDepth(m_waterfall->GetHeight());
		m_rowHistory.Push(m_groupStart.streamStart, m_groupStart.recordingStart);

		//The next group starts where this block ended
		m_groupStart.streamStart = m_currentBlock.streamEnd;
		m_groupStart.recordingStart = m_currentBlock.recordingEnd;
	}

	return advanced ? STEP_ROW : STEP_BLOCK;
}

void SpectrumEngine::RefreshSpectrumOnly()
{
	m_cmdBuf->begin({});
	m_fft->Refresh(*m_cmdBuf, m_queue);
	m_cmdBuf->end();
	m_queue->SubmitAndBlock(*m_cmdBuf);
}
