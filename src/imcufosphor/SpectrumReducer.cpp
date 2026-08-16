/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SpectrumReducer
 */

#include "SpectrumReducer.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SpectrumReducer::SpectrumReducer(const string& color)
	: Filter(color, CAT_RF)
	, m_accumulated(0)
	, m_lastGroupSize(0)
	, m_outputReady(false)
	, m_cachedBins(0)
	, m_computePipeline("shaders/SpectrumReduce.spv", 3, sizeof(SpectrumReduceArgs))
	, m_mode(m_parameters["Mode"])
	, m_groupSize(m_parameters["Group size"])
	, m_binsPerSpectrum(m_parameters["Bins per spectrum"])
	, m_range(80)
	, m_offset(40)
{
	//Frequency domain in, frequency domain out. Microhertz because that is what Waterfall
	//constrains its input to (Waterfall.cpp:71).
	m_xAxisUnit = Unit(Unit::UNIT_MICROHZ);
	AddStream(Unit(Unit::UNIT_DBM), "data", Stream::STREAM_TYPE_ANALOG);

	CreateInput<InputConstraintStreamType>("din", Stream::STREAM_TYPE_ANALOG);

	m_mode = FilterParameter(FilterParameter::TYPE_ENUM, Unit(Unit::UNIT_COUNTS));
	m_mode.AddEnumValue("Max hold", MODE_MAX_HOLD);
	m_mode.AddEnumValue("Average", MODE_AVERAGE);
	m_mode.SetIntVal(MODE_MAX_HOLD);

	m_groupSize = FilterParameter(FilterParameter::TYPE_INT, Unit(Unit::UNIT_COUNTS));
	m_groupSize.SetIntVal(1);

	//Zero means the input is a single spectrum, which is how this filter behaved before
	//ComplexFFTFilter started emitting a batch per block.
	m_binsPerSpectrum = FilterParameter(FilterParameter::TYPE_INT, Unit(Unit::UNIT_SAMPLEDEPTH));
	m_binsPerSpectrum.SetIntVal(0);

	m_accumulator.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	m_accumulator.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_UNLIKELY);
}

SpectrumReducer::~SpectrumReducer()
{
}

string SpectrumReducer::GetProtocolName()
{
	return "Spectrum Reducer";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

int64_t SpectrumReducer::GetGroupSize()
{
	auto k = m_groupSize.GetIntVal();
	return (k < 1) ? 1 : k;
}

int64_t SpectrumReducer::GetBinsPerSpectrum()
{
	auto n = m_binsPerSpectrum.GetIntVal();
	return (n < 1) ? 0 : n;
}

float SpectrumReducer::GetVoltageRange(size_t /*stream*/)
{
	return m_range;
}

void SpectrumReducer::SetVoltageRange(float range, size_t /*stream*/)
{
	m_range = range;
}

float SpectrumReducer::GetOffset(size_t /*stream*/)
{
	return m_offset;
}

void SpectrumReducer::SetOffset(float offset, size_t /*stream*/)
{
	m_offset = offset;
}

void SpectrumReducer::ClearSweeps()
{
	m_accumulated = 0;
	m_outputReady = false;
	SetData(nullptr, 0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void SpectrumReducer::Refresh(vk::raii::CommandBuffer& cmdBuf, shared_ptr<QueueHandle> queue)
{
	ClearMessages();
	m_outputReady = false;

	auto din = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(0));
	if(!din)
	{
		AddErrorMessage("Missing input", "Input is unconnected or not a uniform waveform");
		SetData(nullptr, 0);
		return;
	}

	size_t inlen = din->size();
	if(inlen == 0)
		return;

	//How the input block is divided into spectra
	size_t nbins = static_cast<size_t>(GetBinsPerSpectrum());
	if( (nbins == 0) || (nbins > inlen) )
		nbins = inlen;
	size_t nspectra = inlen / nbins;

	//A ragged input means "Bins per spectrum" disagrees with whatever produced the block.
	//Reducing it anyway would silently mix bins from adjacent spectra, which looks like a
	//smeared waterfall rather than like a bug, so refuse.
	if(nspectra * nbins != inlen)
	{
		AddErrorMessage("Invalid input",
			"Input of " + to_string(inlen) + " samples is not a whole number of " +
			to_string(nbins) + "-bin spectra");
		return;
	}

	//Carry the input's frequency axis through unchanged. We combine spectra in time, never
	//in frequency, so bin positions are identical to the input's.
	m_xAxisUnit = m_inputs[0]->GetXAxisUnits();

	//A change in transform length invalidates any partial group
	if(m_cachedBins != nbins)
	{
		m_accumulator.resize(nbins);
		m_cachedBins = nbins;
		m_accumulated = 0;
	}

	auto k = GetGroupSize();
	auto mode = m_mode.GetEnumVal<ReduceMode>();

	//Fold every spectrum in the block into the accumulator in one dispatch
	SpectrumReduceArgs args;
	args.nbins = nbins;
	args.nspectra = nspectra;
	args.op = OP_ACCUM;
	args.mode = mode;
	args.seed = (m_accumulated == 0) ? 1 : 0;
	args.scale = 1;

	//The output buffer is only read by OP_FINALIZE, but the descriptor set wants all three
	//bindings populated on every dispatch, so make sure one exists up front
	auto cap = dynamic_cast<UniformAnalogWaveform*>(GetData(0));
	if( (cap == nullptr) || (cap->size() != nbins) )
	{
		cap = SetupEmptyUniformAnalogOutputWaveform(din, 0);
		cap->Resize(nbins);
		SetData(cap, 0);
	}

	m_computePipeline.BindBufferNonblocking(0, din->m_samples, cmdBuf);
	m_computePipeline.BindBufferNonblocking(1, m_accumulator, cmdBuf, true);
	m_computePipeline.BindBufferNonblocking(2, cap->m_samples, cmdBuf, true);
	m_computePipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(nbins, 64));

	m_accumulated += nspectra;

	//Not a complete group yet, so leave the previous output in place
	if(m_accumulated < k)
		return;

	//Emit. Scale by what actually went in, not by what was asked for: a block that
	//overshoots the group boundary contributes all of its spectra.
	m_computePipeline.AddComputeMemoryBarrier(cmdBuf);

	args.op = OP_FINALIZE;
	args.scale = (mode == MODE_AVERAGE) ? (1.0f / m_accumulated) : 1.0f;
	m_computePipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(nbins, 64));

	m_lastGroupSize = m_accumulated;

	//Timestamps come from the last spectrum in the group, so the row is labelled with the
	//most recent data it contains rather than the oldest
	cap->m_timescale = din->m_timescale;
	cap->m_triggerPhase = din->m_triggerPhase;
	cap->m_startTimestamp = din->m_startTimestamp;
	cap->m_startFemtoseconds = din->m_startFemtoseconds;
	cap->m_revision++;
	cap->MarkModifiedFromGpu();

	m_accumulated = 0;
	m_outputReady = true;
}
