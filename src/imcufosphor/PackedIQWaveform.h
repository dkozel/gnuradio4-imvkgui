/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
* Copyright (c) 2026 Derek Kozel and contributors                                                                      *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of PackedIQWaveform
 */
#ifndef PackedIQWaveform_h
#define PackedIQWaveform_h

#include "scopehal.h"

/**
	@brief How the components of a PackedIQWaveform are laid out inside each 32-bit word

	The numeric values are part of the shader ABI: PackedComplexWindow.glsl switches on them
	directly, so they may not be renumbered without changing the shader.
 */
enum PackedIQFormat : uint32_t
{
	PACKED_IQ_INT8		= 0,
	PACKED_IQ_UINT8		= 1,
	PACKED_IQ_INT16		= 2,
	PACKED_IQ_UINT16	= 3,
	PACKED_IQ_INT32		= 4,
	PACKED_IQ_UINT32	= 5,
	PACKED_IQ_FLOAT32	= 6
};

/**
	@brief Interleaved I/Q samples in their on-disk binary format, ready to hand to a shader

	This is the "never deinterleave" datapath. A SigMF recording stores I and Q adjacent to
	each other in one stream, VkFFT wants complex pairs adjacent to each other in one buffer
	(bufferSeparateComplexComponents defaults to 0, and the planar mode is six special cases
	in vkFFT_ReadWrite.h rather than the fast path), and every layout in between is work
	nobody asked for. So the bytes are moved from the file into pinned memory unmodified, the
	same bytes cross PCIe, and a single shader turns them into windowed interleaved floats.

	Contrast the planar path this replaces, which read the file into a staging vector,
	deinterleaved it into two float arrays on the CPU, uploaded both, and had the window
	shader re-interleave them on the GPU. Four layout changes to end up where the input
	started, and 8 bytes per sample over the bus instead of 4 for ci16.

	@par Why a waveform rather than a bare AcceleratorBuffer

	So it can travel through the filter graph like anything else: InstrumentChannel::SetData
	takes ownership, the reuse-the-same-pointer trick in SigMFSource::AcquireData() still
	makes SetData early out, and ComplexFFTFilter can dynamic_cast for it the same way it
	casts for UniformAnalogWaveform. UniformWaveform<S> is already a template and upstream
	already instantiates it on integers (UniformDigitalBusWaveform32 is UniformWaveform
	<uint32_t>), so nothing here is a new capability - only a new sample type.

	@warning size() is the number of 32-bit words, not the number of complex samples. The two
	are equal only for the 16-bit formats. Use m_complexSamples for anything that means
	"how many samples are here", including anything that indexes against m_timescale.
 */
class PackedIQWaveform : public UniformWaveform<uint32_t>
{
public:
	PackedIQWaveform(const std::string& name = "")
		: UniformWaveform<uint32_t>(name)
		, m_format(PACKED_IQ_INT16)
		, m_bias(0)
		, m_scale(1.0f / 32767.0f)
		, m_complexSamples(0)
	{}

	virtual ~PackedIQWaveform()
	{}

	///@brief Bytes one complex sample occupies on disk, counting both components
	static size_t BytesPerComplexSample(PackedIQFormat format)
	{
		switch(format)
		{
			case PACKED_IQ_INT8:
			case PACKED_IQ_UINT8:
				return 2;

			case PACKED_IQ_INT16:
			case PACKED_IQ_UINT16:
				return 4;

			case PACKED_IQ_INT32:
			case PACKED_IQ_UINT32:
			case PACKED_IQ_FLOAT32:
			default:
				return 8;
		}
	}

	/**
		@brief Words needed to hold a given number of complex samples

		Rounded up: an odd sample count in an 8-bit format leaves the top half of the last
		word unused, which is harmless as long as the buffer is big enough to hold it.
	 */
	static size_t WordsForSamples(PackedIQFormat format, size_t nsamples)
	{
		size_t bytes = nsamples * BytesPerComplexSample(format);
		return (bytes + 3) / 4;
	}

	/**
		@brief Resizes to hold a given number of complex samples

		Prefer this to Resize(), which is in words and has no idea what the format is.
	 */
	void ResizeComplexSamples(size_t nsamples)
	{
		m_complexSamples = nsamples;
		Resize(WordsForSamples(m_format, nsamples));
	}

	///@brief Bytes of file data this waveform currently holds
	size_t GetPayloadBytes() const
	{ return m_complexSamples * BytesPerComplexSample(m_format); }

	///@brief Component layout within each word
	PackedIQFormat m_format;

	/**
		@brief Added to each raw component before scaling

		Zero for signed and float formats. For the unsigned formats it is -2^(bits-1), which
		is what makes them offset binary with midscale at zero - the convention
		SigMFSource::ConvertSamples() has always used for them.
	 */
	float m_bias;

	///@brief Multiplied into each biased component to reach volts, normally 1/(2^(bits-1) - 1)
	float m_scale;

	///@brief Complex samples held, as distinct from size(), which is words
	size_t m_complexSamples;
};

#endif
