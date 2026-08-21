/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of IqInjector
 */

#include "IqInjector.h"

#include <cmath>
#include <cstring>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

IqInjector::IqInjector(const string& name)
	: m_cap(nullptr)
	, m_capFormat(PACKED_IQ_INT16)
	, m_sampleRate(0)
	, m_centerHz(0)
	, m_scale(1.0f / 32767.0f)
	, m_samplesPushed(0)
	, m_lastBlockSize(0)
{
	//Null scope: there is no instrument behind this, and nothing downstream asks for one.
	//Units match what SigMFSource gives its channel so a consumer sees the same thing either
	//way - femtoseconds on the X axis, volts on I and Q.
	m_chan = make_unique<ComplexChannel>(
		nullptr,
		name,
		"#4040ff",
		Unit(Unit::UNIT_FS),
		Unit(Unit::UNIT_VOLTS),
		0);
}

IqInjector::~IqInjector()
{
	//m_cap is deliberately not deleted here: the channel owns it, and destroying the channel
	//takes it with it. Deleting it here as well would be a double free.
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

void IqInjector::SetSampleScale(float scale)
{
	m_scale = scale;
	if(m_cap)
		m_cap->m_scale = scale;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Publishing

void IqInjector::SetSamples(span<const complex<float>> iq, double sampleRateHz, double centerHz)
{
	//Bias 0 and scale 1: the shader's affine step is the identity for floats, because a float
	//IQ sample is already in volts by the time anyone hands it to us.
	PushPacked(iq.data(), iq.size(), PACKED_IQ_FLOAT32, 1.0f, sampleRateHz, centerHz);
}

void IqInjector::SetSamples(span<const complex<int16_t>> iq, double sampleRateHz, double centerHz)
{
	PushPacked(iq.data(), iq.size(), PACKED_IQ_INT16, m_scale, sampleRateHz, centerHz);
}

void IqInjector::PushPacked(
	const void* data,
	size_t nsamples,
	PackedIQFormat format,
	float scale,
	double sampleRateHz,
	double centerHz)
{
	m_sampleRate = sampleRateHz;
	m_centerHz = centerHz;
	m_lastBlockSize = nsamples;

	//A change of sample type changes how the shader must read the buffer, and the old contents
	//mean nothing under the new interpretation. Cheaper to start a fresh waveform than to
	//explain a half-converted one; this happens once, if ever, not per block.
	if(m_cap && (m_capFormat != format))
	{
		m_chan->SetData(nullptr, 0);
		m_cap = nullptr;
	}

	if(!m_cap)
	{
		m_cap = new PackedIQWaveform(m_chan->GetHwname() + ".iq");
		m_capFormat = format;
		m_cap->m_format = format;

		//Zero for both supported formats: signed integers and floats are already centered on
		//zero. The bias exists for the offset-binary unsigned formats, which nothing pushes
		//through here yet.
		m_cap->m_bias = 0;

		m_chan->SetData(m_cap, 0);

		//Stream 1 stays empty for the life of the channel. See the class doc.
		m_chan->SetData(nullptr, 1);
	}

	m_cap->m_scale = scale;

	//Femtoseconds per sample, the timebase everything downstream measures against.
	//ComplexFFTFilter divides into it to recover the sample rate, so a zero here would be a
	//divide by zero rather than a merely wrong axis.
	if(sampleRateHz > 0)
		m_cap->m_timescale = llround(1e15 / sampleRateHz);

	//No trigger phase and no wall clock: a stream from a live port has no origin to be offset
	//from. Anything needing absolute time gets it from tags, not from here.
	m_cap->m_triggerPhase = 0;
	m_cap->m_startTimestamp = 0;
	m_cap->m_startFemtoseconds = 0;

	//Bumped so downstream caches see the contents as new even though the pointer did not move
	m_cap->m_revision++;

	m_cap->ResizeComplexSamples(nsamples);

	//The whole buffer is about to be overwritten, so there is nothing on the GPU worth
	//downloading first. Using the plain PrepareForCpuAccess() here would cost a readback per
	//block to fetch data that is discarded on the next line.
	m_cap->m_samples.PrepareForCpuAccessIgnoringGpuData();

	if(nsamples > 0)
	{
		memcpy(
			m_cap->m_samples.GetCpuPointer(),
			data,
			nsamples * PackedIQWaveform::BytesPerComplexSample(format));
	}

	m_cap->MarkSamplesModifiedFromCpu();

	//float, not double: the stream's scalar value is a float, whose ULP at 2.4 GHz is about
	//256 Hz. Consumers that need the exact value read it back from here rather than from the
	//stream, which is why GetCenterFrequency() exists.
	m_chan->UpdateCenterFrequency(static_cast<float>(centerHz));

	m_samplesPushed += static_cast<int64_t>(nsamples);
}
