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
	@brief Declaration of ComplexFFTFilter
 */
#ifndef ComplexFFTFilter_h
#define ComplexFFTFilter_h

//scopehal and scopeprotocols export their source directories as PUBLIC include paths,
//so these resolve without relative paths.
#include "scopehal.h"
#include "PeakDetectionFilter.h"
#include "VulkanFFTPlan.h"
#include "FFTFilter.h"

#include "PackedIQWaveform.h"

class QueueHandle;

/**
	@brief Push constants for PackedComplexWindow.glsl

	All members are 4-byte scalars, so the std430 push constant block and this struct have
	the same layout with no padding to reason about.
 */
struct PackedComplexWindowArgs
{
	///@brief Transform length, in complex samples
	uint32_t npoints;

	///@brief Complex samples present in the input buffer
	uint32_t nsamples;

	///@brief A PackedIQFormat
	uint32_t format;

	///@brief Added to each raw component before scaling
	float sampleBias;

	///@brief Multiplied into each biased component to reach volts
	float sampleScale;

	///@brief 2*pi/npoints
	float phaseStep;

	///@brief Cosine-sum window coefficients: w = a0 - a1*cos(x) + a2*cos(2x) - a3*cos(3x)
	float alpha0;
	float alpha1;
	float alpha2;
	float alpha3;
};

/**
	@brief Push constants for ComplexToLogMagnitudeShifted.glsl

	Deliberately not scopeprotocols' ComplexToMagnitudeArgs: that struct describes a
	different shader, and the two only happen to have the same layout today.
 */
struct ComplexToLogMagnitudeShiftedArgs
{
	///@brief Number of FFT bins per transform (equal to the transform length, for a complex FFT)
	uint32_t npoints;

	///@brief Number of consecutive transforms in the buffer
	uint32_t nblocks;

	///@brief Power scale: (volts per unit of bin magnitude)^2 / impedance
	float scale;
};

/**
	@brief A complex (I/Q) FFT producing an absolute-calibrated dBm spectrum trace.

	App-local rather than a scopeprotocols addition; see DESIGN.md decision D3.

	FFTFilter cannot be reused because it creates a single real input
	(FFTFilter.cpp:52). ComplexSpectrogramFilter is complex-aware but emits a whole-file
	STFT image rather than a trace. The input layout here follows
	ComplexSpectrogramFilter.cpp:44-55, which is what ComplexChannel supplies: I, Q, and a
	center-frequency scalar.

	The window functions are scopeprotocols' installed complex window shaders, used
	unmodified. The postprocess is app-local: see ComplexToLogMagnitudeShifted.glsl for
	why neither upstream postprocess shader fits.

	@par Input modes

	The I input accepts either of two things, chosen by what the source hands over:

	  - A PackedIQWaveform, holding interleaved samples in their on-disk binary format. The Q
	    input is then unused and may be left unconnected. One dispatch of
	    PackedComplexWindow.glsl unpacks, converts, windows and interleaves the whole block.
	    This is the fast path and the one SigMFSource uses for every format it can pack.
	  - A pair of UniformAnalogWaveforms carrying planar float I and Q, windowed by
	    scopeprotocols' shaders. This is the fallback for recordings whose format the packed
	    shader does not handle, and what synthesized test signals use.

	Both produce bit-comparable spectra for the same input, modulo the Blackman-Harris
	difference noted in Refresh().

	@par Frequency axis

	The x axis unit is microhertz, because Waterfall constrains its input to UNIT_MICROHZ
	(Waterfall.cpp:71). Output is fftshifted, so bin nouts/2 is the center frequency and
	m_triggerPhase carries the absolute frequency of bin 0.

	@par Amplitude calibration

	dBm into 50 ohms, matching FFTFilter's convention: an IQ tone of unit amplitude
	(I = cos, Q = sin) reads +10 dBm, the same as a real 1 V amplitude cosine.

	@par Resolution bandwidth and batching

	The "FFT length" parameter sets the transform length, and therefore the RBW,
	independently of how many samples arrive per acquisition. An input of N samples with a
	transform length of L produces floor(N/L) consecutive, non-overlapping spectra, laid out
	back to back in one output waveform of floor(N/L)*L bins.

	This is what makes high sample rates affordable. Every fixed per-acquisition cost - the
	file read, the command buffer, the submit and its fence round trip - is paid once per
	*block* rather than once per transform, and the transforms themselves run as a single
	batched VkFFT dispatch. See DESIGN.md section 13 for the measurements.

	Setting the parameter to zero means "one transform over the whole input", which is the
	behaviour this filter had before batching existed, and is also the fallback when the
	input is shorter than the requested transform length.

	@warning The output waveform is a *batch* of spectra, not a single trace, whenever more
	than one transform fits in the input. m_timescale and m_triggerPhase describe the bins
	within one spectrum. A consumer has to know the transform length to index it; that is
	what SpectrumReducer's "Bins per spectrum" parameter is for.
 */
class ComplexFFTFilter : public PeakDetectionFilter
{
public:
	ComplexFFTFilter(const std::string& color);
	virtual ~ComplexFFTFilter();

	virtual void Refresh(vk::raii::CommandBuffer& cmdBuf, std::shared_ptr<QueueHandle> queue) override;
	virtual uint32_t GetExecutionCapabilitiesMask() override;

	static std::string GetProtocolName();

	virtual float GetVoltageRange(size_t stream) override;
	virtual float GetOffset(size_t stream) override;

	virtual void SetVoltageRange(float range, size_t stream) override;
	virtual void SetOffset(float offset, size_t stream) override;

	///@brief Sets the window function, using FFTFilter's enumeration
	void SetWindowFunction(FFTFilter::WindowFunction f)
	{ m_window.SetIntVal(f); }

	/**
		@brief Sets the transform length

		Zero means one transform spanning the whole input, whatever length that is.
	 */
	void SetFFTLength(int64_t len)
	{ m_fftLength.SetIntVal(len); }

	int64_t GetFFTLength()
	{ return m_fftLength.GetIntVal(); }

	///@brief Bins per spectrum in the most recent Refresh()
	size_t GetBinsPerSpectrum() const
	{ return m_cachedNumOuts; }

	///@brief Transforms performed in the most recent Refresh()
	size_t GetSpectraPerBlock() const
	{ return m_cachedNumBlocks; }

	///@brief Number of output bins from the most recent Refresh(), for tests
	size_t test_GetNumOuts()
	{ return m_cachedNumOuts; }

	PROTOCOL_DECODER_INITPROC(ComplexFFTFilter)

protected:

	void ReallocateBuffers(size_t npoints, size_t nouts, size_t nblocks);

	///@brief Transform length the currently allocated buffers and plan are sized for
	size_t m_cachedNumPoints;

	/**
		@brief Batch count the currently allocated buffers and plan are sized for

		Tracked separately from the transform length, and both are compared before reusing
		the plan. Upstream's spectrogram filters compare only VulkanFFTPlan::size(), which is
		the transform length (SpectrogramFilter.cpp:148, ComplexSpectrogramFilter.cpp:93), so
		a changed batch count with an unchanged transform length silently keeps a plan whose
		numberBatches is wrong.
	 */
	size_t m_cachedNumBlocks;

	///@brief Number of output bins per spectrum from the most recent Refresh()
	size_t m_cachedNumOuts;

	///@brief Windowed input, interleaved I/Q
	AcceleratorBuffer<float> m_rdinbuf;

	///@brief Raw FFT output, interleaved real/imaginary
	AcceleratorBuffer<float> m_rdoutbuf;

	///@brief Vertical range of the output trace, in dB
	float m_range;

	///@brief Vertical offset of the output trace, in dBm
	float m_offset;

	///@brief The window function to apply before transforming
	FilterParameter& m_window;

	///@brief Transform length, or zero for "one transform over the whole input"
	FilterParameter& m_fftLength;

	///@brief Complex-to-complex forward transform
	std::unique_ptr<VulkanFFTPlan> m_vkPlan;

	///@brief Batch count m_vkPlan was built for. VulkanFFTPlan does not report it.
	size_t m_planBatches;

	///@brief scopeprotocols' ComplexBlackmanHarrisWindow.spv
	ComputePipeline m_blackmanHarrisComputePipeline;

	///@brief scopeprotocols' ComplexRectangularWindow.spv
	ComputePipeline m_rectangularComputePipeline;

	///@brief scopeprotocols' ComplexCosineSumWindow.spv
	ComputePipeline m_cosineSumComputePipeline;

	/**
		@brief Our fused unpack + convert + window, for packed input

		Replaces all three pipelines above whenever the input arrives as a PackedIQWaveform,
		which is every recording whose on-disk format the shader can unpack. The three above
		remain for float I/Q input, which is what the synthesized test cases and any
		non-packable recording produce.
	 */
	ComputePipeline m_packedWindowComputePipeline;

	///@brief Our fftshift + dBm conversion
	ComputePipeline m_postprocessComputePipeline;
};

#endif
