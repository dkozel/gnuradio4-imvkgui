/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of IqInjector
 */
#ifndef IqInjector_h
#define IqInjector_h

#include "PackedIQWaveform.h"

#include "../../lib/scopehal/scopehal/ComplexChannel.h"

#include <bit>
#include <complex>
#include <cstdint>
#include <memory>
#include <span>

/**
	@brief Head of the filter graph when the samples come from outside it

	SigMFSource is an Oscilloscope because a recording is something you play. Samples arriving
	from a GNU Radio port, a synthetic generator or a socket are not, and wrapping them in a
	42-virtual Oscilloscope subclass to get them into the graph would be ceremony around a
	memcpy. This is the memcpy.

	@par Why this is not a Filter

	It has no inputs to refresh from and nothing to compute. ComplexFFTFilter's three inputs are
	exactly the three streams ComplexChannel already declares - I, Q and the center frequency
	scalar (ComplexChannel.h:63-66) - and a ComplexChannel with a null Oscilloscope is a
	supported construction that the shipped test suite already drives a real ComplexFFTFilter
	from (Verify.cpp:478-490). Nothing in ComplexFFTFilter::Refresh() calls GetScope(). So this
	class owns a channel and a waveform, and produces data by being told rather than by being
	run. No Filter subclass, no Refresh(), no AddDecoderClass, no refcount.

	@par The packed contract

	Stream 1 ("Q") is wired up by the consumer but never given data. A PackedIQWaveform on
	stream 0 carries both components interleaved, and ComplexFFTFilter clears its needQ flag
	whenever input 0 casts to one (ComplexFFTFilter.cpp:200-212), so a null Q is the contract
	rather than a gap. This is what SigMFSource already publishes on the packed path.

	@par Why the copy is straight

	Both supported sample types are already in the shader's expected layout, so neither push
	deinterleaves and neither converts:

	- std::complex<float> is two adjacent floats, which is what PACKED_IQ_FLOAT32 reads out of
	  din[i*2+0] and din[i*2+1] via uintBitsToFloat (PackedComplexWindow.glsl:179-180). Bias 0
	  and scale 1 make the shader's affine step the identity.
	- std::complex<int16_t> is two adjacent int16s, i.e. one 32-bit word per sample with I in
	  the low half and Q in the high half on a little-endian host, which is exactly what
	  PACKED_IQ_INT16 pulls out with bitfieldExtract (PackedComplexWindow.glsl:148-155).

	The int16 case is the one the standard does not guarantee: array layout is only specified
	for complex<float>, complex<double> and complex<long double>. libstdc++ stores two adjacent
	_Tp with no padding, which the static_asserts below and qa_IqLayout check, but a foreign
	standard library would need a real conversion here.
 */
class IqInjector
{
public:
	IqInjector(const std::string& name = "RX");
	virtual ~IqInjector();

	//not copyable or assignable
	IqInjector(const IqInjector&) =delete;
	IqInjector& operator=(const IqInjector&) =delete;

	/**
		@brief The channel to wire a ComplexFFTFilter's I, Q and center inputs to

		Streams 0, 1 and 2 respectively, matching what SigMFSource exposes, so a consumer
		cannot tell the two apart.
	 */
	ComplexChannel* GetChannel()
	{ return m_chan.get(); }

	/**
		@brief Publishes a block of float IQ

		@param iq			Interleaved complex samples, copied rather than retained
		@param sampleRateHz	Sample rate the block was captured at
		@param centerHz		Center frequency of bin 0's band

		Must be called before the ComplexFFTFilter::Refresh() that consumes it, and on the same
		thread: the waveform is reused between pushes, so a second push while a submit is still
		reading the first would overwrite it underneath the GPU.
	 */
	void SetSamples(std::span<const std::complex<float>> iq, double sampleRateHz, double centerHz);

	/**
		@brief Publishes a block of 16-bit integer IQ

		The native format of most SDRs and of every recording in the demo set, and the one that
		crosses PCIe at 4 bytes per sample instead of 8.
	 */
	void SetSamples(std::span<const std::complex<int16_t>> iq, double sampleRateHz, double centerHz);

	/**
		@brief Raw LSB to volts, for the integer formats

		Folded into the window multiply by the shader rather than applied host-side
		(PackedComplexWindow.glsl:232). The default 1/32767 makes a full scale ci16 tone read
		the same +10 dBm as a unit amplitude float tone, matching what
		SigMFSampleFormat::ToPackedFormat picks for ci16_le.
	 */
	void SetSampleScale(float scale);

	float GetSampleScale() const
	{ return m_scale; }

	/**
		@brief Complex samples pushed since construction

		Monotonic and never reset, so it can serve as the stream coordinate for anything that
		needs to know how far playback has advanced. There is no recording coordinate here:
		samples arriving from a live port have no position in a file.
	 */
	int64_t GetSamplesPushed() const
	{ return m_samplesPushed; }

	///@brief Sample rate of the most recent push
	double GetSampleRate() const
	{ return m_sampleRate; }

	///@brief Center frequency of the most recent push
	double GetCenterFrequency() const
	{ return m_centerHz; }

	///@brief Complex samples in the most recent push
	size_t GetLastBlockSize() const
	{ return m_lastBlockSize; }

protected:
	/**
		@brief The one copy path both overloads reach

		@param data			Source bytes, already in the shader's layout
		@param nsamples		Complex samples, not words and not bytes
		@param format		How the shader should read them back
		@param scale		Raw component to volts
	 */
	void PushPacked(
		const void* data,
		size_t nsamples,
		PackedIQFormat format,
		float scale,
		double sampleRateHz,
		double centerHz);

	std::unique_ptr<ComplexChannel> m_chan;

	/**
		@brief The waveform every push writes into

		Non-owning: handed to the channel once, and InstrumentChannel::SetData early-outs on an
		unchanged pointer (InstrumentChannel.cpp:146-147), so reusing it keeps the allocation
		out of the per-block path. This is the same trick SigMFSource uses, and DESIGN.md
		section 13 measures it as the difference between 21 and 96 MS/s. The channel deletes it.
	 */
	PackedIQWaveform* m_cap;

	///@brief Format m_cap currently holds, so a change of sample type is noticed
	PackedIQFormat m_capFormat;

	double m_sampleRate;
	double m_centerHz;
	float m_scale;
	int64_t m_samplesPushed;
	size_t m_lastBlockSize;
};

//The straight copy in PushPacked() is only a straight copy if the host agrees with the shader
//about how a complex sample is laid out. Both of these are true everywhere imcufosphor builds
//today; failing loudly beats silently transforming noise into a spectrum.
static_assert(sizeof(std::complex<float>) == 8,
	"PACKED_IQ_FLOAT32 push assumes complex<float> is two adjacent floats");
static_assert(sizeof(std::complex<int16_t>) == 4,
	"PACKED_IQ_INT16 push assumes complex<int16_t> is two adjacent int16s");
static_assert(std::endian::native == std::endian::little,
	"PackedComplexWindow.glsl reads native-order words; a big-endian host needs a byte swap");

#endif
