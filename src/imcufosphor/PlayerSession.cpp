/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of PlayerSession
 */

#include "PlayerSession.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

PlayerSession::PlayerSession(SigMFSource* source, shared_ptr<QueueHandle> queue)
	: m_source(source)
	, m_queue(queue)
	, m_fft(nullptr)
	, m_reducer(nullptr)
	, m_density(nullptr)
	, m_waterfall(nullptr)
	, m_fftLength(8192)
	, m_blockSize(1048576)
	, m_rowsPlayed(0)
	, m_rangeMin(-100)
	, m_rangeMax(-20)
	, m_gpuTimingEnabled(false)
{
	auto chan = m_source->GetChannel();

	m_fft = new ComplexFFTFilter("#ffffff");
	m_fft->AddRef();
	m_fft->SetInput("I", StreamDescriptor(chan, 0));
	m_fft->SetInput("Q", StreamDescriptor(chan, 1));
	m_fft->SetInput("center", StreamDescriptor(chan, 2));
	m_fft->SetWindowFunction(FFTFilter::WINDOW_BLACKMAN_HARRIS);

	m_reducer = new SpectrumReducer("#ffffff");
	m_reducer->AddRef();
	m_reducer->SetInput(0, StreamDescriptor(m_fft, 0));

	//A parallel consumer of the same FFT batch, not a stage after the reducer. The two want
	//different cadences - the reducer folds every block, the density map every window - and
	//produce different output types. DESIGN.md section 10 anticipated widening the reducer
	//itself, which was written before the FFT emitted a batch per block; now that it does, a
	//second consumer is the simpler shape.
	m_density = new SpectrumDensity("#ffffff");
	m_density->AddRef();
	m_density->SetInput(0, StreamDescriptor(m_fft, 0));

	m_waterfall = new Waterfall("#ffffff");
	m_waterfall->AddRef();
	m_waterfall->SetInput(0, StreamDescriptor(m_reducer, 0));

	SetRange(m_rangeMin, m_rangeMax);

	//Transform length and acquisition block size are independent. The FFT filter cuts the
	//block into as many transforms as fit, and the reducer needs to be told how long each
	//one is. Both are set from the same two numbers here, which is the only place they are
	//allowed to be decided.
	ApplyLengths();

	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		m_queue->GetQueue()->m_family);
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuf = make_unique<vk::raii::CommandBuffer>(
		std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	m_source->Start();
}

PlayerSession::~PlayerSession()
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

void PlayerSession::ApplyLengths()
{
	//A block shorter than one transform would make the FFT filter fall back to transforming
	//the whole block, quietly changing the RBW. Clamp instead, so the requested transform
	//length is always what runs.
	if(m_blockSize < m_fftLength)
		m_blockSize = m_fftLength;

	m_source->SetSampleDepth(m_blockSize);
	m_fft->SetFFTLength(m_fftLength);
	m_reducer->SetBinsPerSpectrum(m_fftLength);
	m_density->SetBinsPerSpectrum(m_fftLength);

	//The density map's persistence constants are in seconds, so it needs to know how fast
	//spectra arrive to convert them
	double srate = m_source->GetRecordingSampleRate();
	if( (srate > 0) && (m_fftLength > 0) )
		m_density->SetSpectrumRate(srate / m_fftLength);
}

void PlayerSession::SetFFTLength(int64_t len)
{
	if(len == m_fftLength)
		return;

	m_fftLength = len;
	ApplyLengths();

	//Bin count changes, so anything accumulated against the old length is meaningless
	m_reducer->ClearSweeps();
	m_density->ClearSweeps();
	m_waterfall->ClearSweeps();
}

void PlayerSession::SetBlockSize(int64_t samples)
{
	if(samples == m_blockSize)
		return;

	m_blockSize = samples;
	ApplyLengths();

	//The bin count is unchanged, so the accumulator stays valid; only the number of spectra
	//arriving per Refresh changes.
}

int64_t PlayerSession::GetGroupSize()
{
	return m_reducer->GetGroupSize();
}

void PlayerSession::SetGroupSize(int64_t k)
{
	if(k < 1)
		k = 1;
	m_reducer->GetParameter("Group size").SetIntVal(k);
}

double PlayerSession::GetCoverage() const
{
	auto depth = static_cast<int64_t>(m_source->GetSampleDepth());
	if( (depth <= 0) || (m_fftLength <= 0) )
		return 0;

	//The FFT filter transforms as many whole transforms as fit in the block; anything left
	//over at the end is skipped. Coverage is therefore complete whenever the transform
	//length divides the block size, and short by the remainder when it does not.
	int64_t transformed = (depth / m_fftLength) * m_fftLength;
	return min(1.0, static_cast<double>(transformed) / depth);
}

void PlayerSession::SetRange(float minDbm, float maxDbm)
{
	m_rangeMin = minDbm;
	m_rangeMax = maxDbm;

	//The waterfall maps its input against the source stream's range and offset
	//(Waterfall.cpp:159-160), so the colour scale is set on whatever feeds it.
	float range = maxDbm - minDbm;
	float offset = -(maxDbm + minDbm) / 2;

	m_reducer->SetVoltageRange(range, 0);
	m_reducer->SetOffset(offset, 0);
	m_fft->SetVoltageRange(range, 0);
	m_fft->SetOffset(offset, 0);

	//The density map's amplitude axis is the same colour scale. Unlike the traces this is
	//not merely presentation: it decides the histogram's cell mapping, so a change discards
	//the accumulated map.
	m_density->SetRange(minDbm, maxDbm);
}

void PlayerSession::ResetStats()
{
	m_stats = PlaybackStats();
	m_source->ResetIngestStats();
}

void PlayerSession::Restart()
{
	m_source->SeekToSample(0);
	m_reducer->ClearSweeps();
	m_density->ClearSweeps();
	m_waterfall->ClearSweeps();
	m_rowHistory.Clear();
	m_rowsPlayed = 0;

	//Only the recording coordinate rewinds. The stream coordinate is a monotonic count of
	//samples played and a restart does not un-play them; zeroing it here would make the
	//waterfall's time axis jump backwards across the restart.
	m_groupStart.recordingStart = 0;
	m_groupStart.streamStart = m_source->GetSamplesPlayed();
	m_currentBlock = BlockSpan();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Playback

PlayerSession::StepResult PlayerSession::StepOneBlock()
{
	//Acquire first, outside the command buffer. AcquireData() is host work - a read and a
	//sample conversion - and recording it into a command buffer is not a thing that can be
	//done; the GPU stages below consume its output.
	//Where this block starts, captured before AcquireData() advances both counters past it
	int64_t blockRecordingStart = m_source->GetPlayCursor();
	int64_t blockStreamStart = m_source->GetSamplesPlayed();

	double t0 = GetTime();
	bool got = m_source->AcquireData() && m_source->PopPendingWaveform();
	double t1 = GetTime();
	m_stats.acquireSec += t1 - t0;
	if(!got)
		return STEP_FAILED;

	//...and where it ends, read back rather than assumed: a block at the end of the recording
	//is short, and with looping on, the cursor has already wrapped to the start of the file.
	m_currentBlock.recordingStart = blockRecordingStart;
	m_currentBlock.recordingEnd = m_source->GetPlayCursor();
	m_currentBlock.streamStart = blockStreamStart;
	m_currentBlock.streamEnd = m_source->GetSamplesPlayed();

	//The whole block in one command buffer and one submit.
	//
	//This used to be one submit per spectrum, on the grounds that AcquireData() would
	//otherwise replace the channel's waveform while a queued FFT was still going to read it.
	//That hazard is real, but it belongs to acquiring more than once per submit - which no
	//longer happens, because a block now carries every transform that used to need its own
	//acquisition. At 245.76 MS/s a 1 Msample block is 128 transforms, so the fence round
	//trip is paid once instead of 128 times. See DESIGN.md section 13.
	m_cmdBuf->begin({});
	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Begin(*m_cmdBuf);

	m_fft->Refresh(*m_cmdBuf, m_queue);
	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Mark(*m_cmdBuf, "fft");

	//Barriers between stages, which the separate submits used to provide for free. Without
	//these the reducer may read the FFT output before the postprocess dispatch has finished
	//writing it, and the waterfall may read the reducer's output before it is emitted. Same
	//thing FilterGraphExecutor does between dependency levels
	//(FilterGraphExecutor.cpp:65-66).
	ComputePipeline::AddComputeMemoryBarrier(*m_cmdBuf);

	m_reducer->Refresh(*m_cmdBuf, m_queue);
	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Mark(*m_cmdBuf, "reduce");

	//The density map reads the same FFT output the reducer does, so it needs no barrier
	//against the reducer - only against the FFT, which the barrier above already provides.
	m_density->Refresh(*m_cmdBuf, m_queue);
	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Mark(*m_cmdBuf, "density");

	//Only advance the waterfall if the reducer completed a group. A block that lands
	//mid-group would otherwise write a row from a partial accumulation.
	bool advanced = m_reducer->IsOutputReady();
	if(advanced)
	{
		ComputePipeline::AddComputeMemoryBarrier(*m_cmdBuf);
		m_waterfall->Refresh(*m_cmdBuf, m_queue);
	}
	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Mark(*m_cmdBuf, "waterfall");

	m_cmdBuf->end();

	double t2 = GetTime();
	m_queue->SubmitAndBlock(*m_cmdBuf);
	double t3 = GetTime();

	if(m_gpuTimingEnabled)
		m_spectrumGpuTimer.Collect();

	m_stats.recordSec += t2 - t1;
	m_stats.submitSec += t3 - t2;
	m_stats.submits++;
	m_stats.spectra += m_fft->GetSpectraPerBlock();

	if(advanced)
	{
		m_rowsPlayed++;

		//Record which samples this row covers. The reducer may have folded several blocks into
		//this row, so the row starts where the group started, not where this block did.
		//
		//Both coordinates, because they answer different questions and neither can be derived
		//from the other: the stream count gives the row's age, which the time axis needs and
		//which must not wrap; the recording index says which part of the file it shows, which
		//is what a wall clock and a SigMF annotation are pinned to. A modulo of one to get the
		//other would break the moment seeking is added.
		m_rowHistory.SetDepth(m_waterfall->GetHeight());
		m_rowHistory.Push(m_groupStart.streamStart, m_groupStart.recordingStart);

		//The next group starts where this block ended
		m_groupStart.streamStart = m_currentBlock.streamEnd;
		m_groupStart.recordingStart = m_currentBlock.recordingEnd;
	}

	return advanced ? STEP_ROW : STEP_BLOCK;
}

bool PlayerSession::StepOneRow()
{
	//Advance until a row comes out. Usually one block does it, but at high sample rates a
	//group spans several - 245.76 MS/s at 8192 points is 128 spectra per 1 Msample block
	//against a group size in the hundreds - so this loops. A caller that wants bounded work
	//per call should drive StepOneBlock() instead.
	while(true)
	{
		switch(StepOneBlock())
		{
			case STEP_ROW:
				return true;

			case STEP_BLOCK:
				break;

			//No data, and looping around the end of the recording did not help. Returning
			//rather than spinning: whatever stopped the source will not fix itself here.
			case STEP_FAILED:
			default:
				return false;
		}
	}
}

void PlayerSession::RefreshSpectrumOnly()
{
	m_cmdBuf->begin({});
	m_fft->Refresh(*m_cmdBuf, m_queue);
	m_cmdBuf->end();
	m_queue->SubmitAndBlock(*m_cmdBuf);
}
