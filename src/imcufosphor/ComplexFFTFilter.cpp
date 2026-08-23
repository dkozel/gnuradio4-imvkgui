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
	@brief Implementation of ComplexFFTFilter
 */

#include "ComplexFFTFilter.h"

using namespace std;

/**
	@brief Largest magnitude, in microhertz, that any value on the frequency axis may take

	The axis is int64 microhertz, so the hard ceiling is 9.2e18 uHz. Sitting an order of
	magnitude below that leaves room for a bin spacing and a center frequency to be added
	without either the sum or the intermediate products overflowing, and 1e18 uHz is 1 THz -
	roughly thirty times the highest carrier anyone records.

	This is a bound on what the arithmetic can represent, not a statement about what is
	physically interesting. A signal beyond it is a metadata error, and is reported as one.
 */
static const int64_t g_maxAxisMicrohertz = 1000000000000000000LL;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ComplexFFTFilter::ComplexFFTFilter(const string& color)
	: Filter(color, CAT_RF, Unit(Unit::UNIT_HZ))
	, m_cachedNumPoints(0)
	, m_cachedNumBlocks(0)
	, m_cachedNumOuts(0)
	, m_range(70)
	, m_offset(35)
	, m_window(m_parameters["Window"])
	, m_fftLength(m_parameters["FFT length"])
	, m_planBatches(0)
	//scopeprotocols' complex window shaders, resolved through FindDataFile at first use
	, m_blackmanHarrisComputePipeline("shaders/ComplexBlackmanHarrisWindow.spv", 3, sizeof(WindowFunctionArgs))
	, m_rectangularComputePipeline("shaders/ComplexRectangularWindow.spv", 3, sizeof(WindowFunctionArgs))
	, m_cosineSumComputePipeline("shaders/ComplexCosineSumWindow.spv", 3, sizeof(WindowFunctionArgs))
	//ours; fused unpack + convert + window for packed input
	, m_packedWindowComputePipeline(
		"shaders/PackedComplexWindow.spv", 2, sizeof(PackedComplexWindowArgs))
	//ours; see the shader source for why neither upstream postprocess shader fits
	, m_postprocessComputePipeline(
		"shaders/ComplexToLogMagnitudeShifted.spv", 2, sizeof(ComplexToLogMagnitudeShiftedArgs))
{
	//Waterfall constrains its input to microhertz (Waterfall.cpp:71), so anything we want
	//to feed a waterfall has to use that unit.
	m_xAxisUnit = Unit(Unit::UNIT_MICROHZ);
	AddStream(Unit(Unit::UNIT_DBM), "data", Stream::STREAM_TYPE_ANALOG);

	//Input layout follows ComplexSpectrogramFilter.cpp:44-55, which is exactly what
	//ComplexChannel provides. PeakDetectionFilter creates no inputs of its own, so unlike
	//ComplexSpectrogramFilter there is no base class port list to clear first.
	CreateInput<InputConstraintStreamType>("I", Stream::STREAM_TYPE_ANALOG);
	CreateInput<InputConstraintStreamType>("Q", Stream::STREAM_TYPE_ANALOG);
	CreateInput<InputConstraintAND>(
		"center",
		initializer_list<shared_ptr<InputConstraint> >
		{
			make_shared<InputConstraintYUnit>(this, Unit(Unit::UNIT_HZ)),
			make_shared<InputConstraintStreamType>(this, Stream::STREAM_TYPE_ANALOG_SCALAR)
		});

	m_window = FilterParameter(FilterParameter::TYPE_ENUM, Unit(Unit::UNIT_COUNTS));
	m_window.AddEnumValue("Blackman-Harris", WINDOW_BLACKMAN_HARRIS);
	m_window.AddEnumValue("Hamming", WINDOW_HAMMING);
	m_window.AddEnumValue("Hann", WINDOW_HANN);
	m_window.AddEnumValue("Rectangular", WINDOW_RECTANGULAR);
	m_window.SetIntVal(WINDOW_HAMMING);

	//Zero rather than a real default, so that a filter nobody configures behaves exactly as
	//it did before batching: one transform spanning whatever arrives.
	m_fftLength = FilterParameter(FilterParameter::TYPE_INT, Unit(Unit::UNIT_SAMPLEDEPTH));
	m_fftLength.SetIntVal(0);
}

ComplexFFTFilter::~ComplexFFTFilter()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string ComplexFFTFilter::GetProtocolName()
{
	return "Complex FFT";
}

float ComplexFFTFilter::GetOffset(size_t /*stream*/)
{
	return m_offset;
}

float ComplexFFTFilter::GetVoltageRange(size_t /*stream*/)
{
	return m_range;
}

void ComplexFFTFilter::SetVoltageRange(float range, size_t /*stream*/)
{
	m_range = range;
}

void ComplexFFTFilter::SetOffset(float offset, size_t /*stream*/)
{
	m_offset = offset;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual filter logic

uint32_t ComplexFFTFilter::GetExecutionCapabilitiesMask()
{
	return
		(uint32_t)ExecutionCapabilities::CommandBufferAppend |
		(uint32_t)ExecutionCapabilities::CommandBufferTailCall |
		(uint32_t)ExecutionCapabilities::VulkanOnly;
}

void ComplexFFTFilter::ReallocateBuffers(size_t npoints, size_t nouts, size_t nblocks)
{
	m_cachedNumPoints = npoints;
	m_cachedNumBlocks = nblocks;

	m_rdinbuf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_NEVER);
	m_rdinbuf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	m_rdoutbuf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_NEVER);
	m_rdoutbuf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);

	//Compare the batch count as well as the transform length before reusing a plan.
	//VulkanFFTPlan::size() reports only the transform length, so the check upstream's
	//spectrogram filters use (SpectrogramFilter.cpp:148) would keep a plan built for a
	//different numberBatches, and VkFFT would then read and write the wrong extents.
	if(m_vkPlan)
	{
		if( (m_vkPlan->size() != npoints) || (m_planBatches != nblocks) )
			m_vkPlan = nullptr;
	}
	if(!m_vkPlan)
	{
		m_vkPlan = make_unique<VulkanFFTPlan>(
			npoints, nouts, VulkanFFTPlan::DIRECTION_FORWARD, nblocks, VulkanFFTPlan::TYPE_COMPLEX);
		m_planBatches = nblocks;
	}

	//Batches are contiguous and unpadded (VulkanFFTPlan.cpp:89-95), and both buffers are
	//interleaved complex pairs, so twice the point count times the batch count.
	m_rdinbuf.resize(2*npoints*nblocks);
	m_rdoutbuf.resize(2*nouts*nblocks);
}

void ComplexFFTFilter::Refresh(vk::raii::CommandBuffer& cmdBuf, shared_ptr<QueueHandle> queue)
{
	//Make sure we've got valid inputs.
	//
	//Two shapes are accepted on input 0; see the class comment. Packed is checked first
	//because a PackedIQWaveform is not a UniformAnalogWaveform and the casts are disjoint,
	//so the order only matters for which error message a bad input produces.
	ClearMessages();
	auto din_packed = dynamic_cast<PackedIQWaveform*>(GetInputWaveform(0));
	auto din_i = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(0));
	auto din_q = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(1));
	auto din_center = GetInput(2);

	//Whatever carries the timebase and the sample count, which is the packed buffer in
	//packed mode and the I waveform in planar mode
	WaveformBase* din_ref = din_packed
		? static_cast<WaveformBase*>(din_packed)
		: static_cast<WaveformBase*>(din_i);

	//In packed mode the Q input is unused: both components come out of the same buffer
	const bool needQ = !din_packed;

	if(!din_ref || !din_center || (needQ && !din_q))
	{
		if(!GetInput(0))
			AddErrorMessage("Missing inputs", "No I signal input connected");
		else if(!GetInputWaveform(0))
			AddErrorMessage("Missing inputs", "No waveform available at I input");
		else if(!din_ref)
			AddErrorMessage("Invalid inputs", "Expect a uniform analog or packed I/Q I input");

		if(needQ)
		{
			if(!GetInput(1))
				AddErrorMessage("Missing inputs", "No Q signal input connected");
			else if(!GetInputWaveform(1))
				AddErrorMessage("Missing inputs", "No waveform available at Q input");
			else if(!din_q)
				AddErrorMessage("Invalid inputs", "Expect a uniform analog Q input");
		}

		if(!din_center)
			AddErrorMessage("Missing inputs", "No center frequency control input connected");

		SetData(nullptr, 0);
		return;
	}

	//Force unit to uHz here as well as in the constructor, matching FFTFilter.cpp:170:
	//legacy sessions may have persisted a unit of Hz.
	m_xAxisUnit = Unit(Unit::UNIT_MICROHZ);

	//Complex samples available. size() on a packed waveform is words, not samples, which is
	//why it is asked for m_complexSamples instead.
	const size_t inlen = din_packed
		? din_packed->m_complexSamples
		: min(din_i->size(), din_q->size());
	if(inlen < 2)
	{
		AddErrorMessage("Invalid inputs", "Need at least two input samples");
		SetData(nullptr, 0);
		return;
	}

	//Transform length, and how many of them fit in this block.
	//
	//Zero means "one transform over the whole input", and so does a requested length longer
	//than the input: refusing to run would be a worse answer than transforming what we have.
	//Consecutive and non-overlapping, matching SpectrogramFilter.cpp:176-180.
	size_t npoints = static_cast<size_t>(max<int64_t>(0, m_fftLength.GetIntVal()));
	if( (npoints < 2) || (npoints > inlen) )
		npoints = inlen;
	const size_t nblocks = inlen / npoints;

	//A complex transform has as many output bins as input points; there is no conjugate
	//symmetry to exploit.
	//The packed window shader indexes transforms with the z dimension, which Vulkan only
	//guarantees to 65535 workgroups. Reaching that needs a block of 65535 transforms, which
	//at the shortest useful transform length is still hundreds of megasamples in one
	//acquisition - but check rather than silently dropping the tail.
	const size_t maxBatchDispatch = 65535;
	if(din_packed && (nblocks > maxBatchDispatch))
	{
		AddErrorMessage("Invalid inputs",
			"Block holds " + to_string(nblocks) + " transforms, more than the " +
			to_string(maxBatchDispatch) + " one dispatch can cover");
		SetData(nullptr, 0);
		return;
	}

	const size_t nouts = npoints;
	m_cachedNumOuts = nouts;
	m_cachedNumBlocks = nblocks;
	LogTrace("ComplexFFTFilter: %zu input samples, %zu transforms of %zu points\n",
		inlen, nblocks, npoints);

	if( (m_cachedNumPoints != npoints) || (m_planBatches != nblocks) )
		ReallocateBuffers(npoints, nouts, nblocks);

	//Bin spacing is fs/N. Unlike the real FFT in FFTFilter there is no factor of two:
	//the bins span the whole sample rate, not half of it.
	const double fs_per_sample = din_ref->m_timescale;
	if(fs_per_sample <= 0)
	{
		AddErrorMessage("Invalid inputs", "Input timescale must be positive");
		LogError("ComplexFFTFilter: input timescale is %" PRId64 " fs/sample, expected a "
			"positive value\n", din_ref->m_timescale);
		SetData(nullptr, 0);
		return;
	}

	const double sample_ghz = 1e6 / fs_per_sample;
	const double bin_uhz_raw = sample_ghz * 1e15 / nouts;

	//Range check before the cast, not after.
	//
	//Converting an out of range double to int64 is undefined behaviour, and on x86 it yields
	//INT64_MIN - which is nonzero, so the "bin size rounds to zero" check further down lets it
	//through. The limit is what keeps (nouts/2) * bin_uhz inside int64 when the trigger phase
	//is computed below.
	const double max_bin_uhz = static_cast<double>(g_maxAxisMicrohertz) / nouts;
	if(!isfinite(bin_uhz_raw) || (bin_uhz_raw > max_bin_uhz))
	{
		AddErrorMessage("Invalid inputs", "Sample rate is too high for a microhertz axis");
		LogError("ComplexFFTFilter: bin spacing works out to %g uHz, which does not fit the "
			"frequency axis (limit %g uHz); check the input sample rate\n",
			bin_uhz_raw, max_bin_uhz);
		SetData(nullptr, 0);
		return;
	}

	const int64_t bin_uhz = round(bin_uhz_raw);
	const auto window = m_window.GetEnumVal<WindowFunction>();
	LogTrace("bin size: %s\n", Unit(Unit::UNIT_MICROHZ).PrettyPrint(bin_uhz).c_str());

	//Set up output and copy time scales / configuration.
	//
	//One waveform holds every spectrum in the block, back to back. m_timescale and
	//m_triggerPhase below describe the bins within one spectrum, not the whole buffer;
	//see the class comment.
	auto cap = SetupEmptyUniformAnalogOutputWaveform(din_ref, 0);
	cap->m_timescale = bin_uhz;
	cap->Resize(nouts * nblocks);

	//A very long FFT at a very low sample rate can round the bin size to zero
	if(cap->m_timescale == 0)
	{
		AddErrorMessage("Invalid inputs", "Bin size rounds to zero, FFT is too long for this sample rate");
		SetData(nullptr, 0);
		return;
	}

	//The output is fftshifted, so bin 0 sits nouts/2 bins below the center frequency.
	//m_triggerPhase carries that absolute offset, in x-axis units.
	//
	//Same reasoning as the bin spacing check above: this is a double converted to int64, so
	//it has to be range checked rather than assumed. Both terms are held under
	//g_maxAxisMicrohertz, so their difference cannot overflow.
	const double center_uhz = din_center.GetScalarValue() * 1e6;
	if(!isfinite(center_uhz) || (fabs(center_uhz) > static_cast<double>(g_maxAxisMicrohertz)))
	{
		AddErrorMessage("Invalid inputs", "Center frequency is outside the representable range");
		LogError("ComplexFFTFilter: center frequency is %g Hz, which does not fit the frequency "
			"axis (limit %g Hz)\n",
			din_center.GetScalarValue(), static_cast<double>(g_maxAxisMicrohertz) / 1e6);
		SetData(nullptr, 0);
		return;
	}

	cap->m_triggerPhase = round(center_uhz) - (int64_t)(nouts/2) * bin_uhz;

	//Amplitude calibration.
	//
	//scale converts a bin magnitude to volts RMS. A complex tone of amplitude A lands
	//entirely in one bin with magnitude A*N (a real tone of amplitude A splits between
	//conjugate bins, giving A*N/2, which is why FFTFilter uses sqrt(2)/N and we use half
	//of that). Vrms is then A/sqrt(2), so the two filters agree: a unit-amplitude tone
	//reads +10 dBm either way.
	//
	//Note this is 9.03 dB below what ComplexSpectrogramFilter.cpp:146 computes for the
	//same input; upstream's complex spectrogram uses scale = 2/N, which does not match
	//FFTFilter's real-input convention.
	float scale = sqrt(2.0) / (2 * npoints);

	//Correct for the coherent power gain of the window function.
	//Same constants as FFTFilter.cpp:222-240, since these are the same window shapes.
	switch(window)
	{
		case WINDOW_HAMMING:
			scale *= 1.862;
			break;

		case WINDOW_HANN:
			scale *= 2.013;
			break;

		case WINDOW_BLACKMAN_HARRIS:
			scale *= 2.805;
			break;

		//unit
		case WINDOW_RECTANGULAR:
		default:
			break;
	}

	//Configure the window
	WindowFunctionArgs args;
	args.numActualSamples = npoints;
	args.npoints = npoints;
	args.scale = 2 * M_PI / npoints;
	args.offsetIn = 0;
	args.offsetOut = 0;
	switch(window)
	{
		case WINDOW_HANN:
			args.alpha0 = 0.5;
			break;

		case WINDOW_HAMMING:
			args.alpha0 = 25.0f / 46;
			break;

		default:
			args.alpha0 = 0;
			break;
	}
	args.alpha1 = 1 - args.alpha0;

	{
		NamedDebugRange debugRange(cmdBuf, "ComplexFFTFilter");
		const uint32_t window_block_count = GetComputeBlockCount(npoints, 64);

		//Apply the window function, interleaving I and Q into m_rdinbuf as it goes
		if(din_packed)
		{
			NamedDebugRange shaderRange(cmdBuf, "Unpack and window");

			//Every window this filter offers is a cosine sum, so one shader covers all four
			//and the coefficients are just push constants:
			//    w = a0 - a1*cos(x) + a2*cos(2x) - a3*cos(3x)
			//
			//The Blackman-Harris coefficients here are the standard minimum-sidelobe set with
			//the third term at cos(3x). scopeprotocols' ComplexBlackmanHarrisWindow.glsl:94
			//(and BlackmanHarrisWindow.glsl:83) evaluate that term at cos(6x) instead, which
			//leaves the coherent gain untouched - both cosines average to zero, so the 2.805
			//amplitude correction stays correct - but breaks the four-term cancellation the
			//window exists for. Measured over an 8192 point window, the shipped version peaks
			//at -35.6 dB of sidelobe against -92.0 dB for the correct one. The planar path
			//below still calls the upstream shader and so still has the old behaviour.
			PackedComplexWindowArgs pargs;
			pargs.npoints = npoints;
			pargs.nsamples = inlen;
			pargs.format = din_packed->m_format;
			pargs.sampleBias = din_packed->m_bias;
			pargs.sampleScale = din_packed->m_scale;
			pargs.phaseStep = 2 * M_PI / npoints;
			switch(window)
			{
				case WINDOW_HANN:
					pargs.alpha0 = 0.5;
					pargs.alpha1 = 0.5;
					pargs.alpha2 = 0;
					pargs.alpha3 = 0;
					break;

				case WINDOW_HAMMING:
					pargs.alpha0 = 25.0f / 46;
					pargs.alpha1 = 21.0f / 46;
					pargs.alpha2 = 0;
					pargs.alpha3 = 0;
					break;

				case WINDOW_BLACKMAN_HARRIS:
					pargs.alpha0 = 0.35875f;
					pargs.alpha1 = 0.48829f;
					pargs.alpha2 = 0.14128f;
					pargs.alpha3 = 0.01168f;
					break;

				default:
				case WINDOW_RECTANGULAR:
					pargs.alpha0 = 1;
					pargs.alpha1 = 0;
					pargs.alpha2 = 0;
					pargs.alpha3 = 0;
					break;
			}

			m_packedWindowComputePipeline.BindBufferNonblocking(0, din_packed->m_samples, cmdBuf);
			m_packedWindowComputePipeline.BindBufferNonblocking(1, m_rdinbuf, cmdBuf, true);

			//One dispatch for the whole batch, against one per transform below.
			//
			//The shader takes the within-transform index from x/y and the transform index
			//from z, so the window coefficient no longer has to be derived from a flat global
			//thread ID and no per-transform offset push is needed. At 1 Msample and 8192
			//points that is 1 dispatch instead of 128.
			m_packedWindowComputePipeline.Dispatch(cmdBuf, pargs,
				min(window_block_count, 32768u),
				window_block_count / 32768 + 1,
				static_cast<uint32_t>(nblocks));

			m_packedWindowComputePipeline.AddComputeMemoryBarrier(cmdBuf);
			m_rdinbuf.MarkModifiedFromGpu();
		}
		else
		{
			NamedDebugRange shaderRange(cmdBuf, "Window function");

			ComputePipeline* wpipe = nullptr;
			switch(window)
			{
				case WINDOW_BLACKMAN_HARRIS:
					wpipe = &m_blackmanHarrisComputePipeline;
					break;

				case WINDOW_HANN:
				case WINDOW_HAMMING:
					wpipe = &m_cosineSumComputePipeline;
					break;

				default:
				case WINDOW_RECTANGULAR:
					wpipe = &m_rectangularComputePipeline;
					break;
			}

			//Q is bound at 2, not 1, so the complex window shaders keep the same output
			//binding as the real ones (ComplexCosineSumWindow.glsl:43-47).
			wpipe->BindBufferNonblocking(0, din_i->m_samples, cmdBuf);
			wpipe->BindBufferNonblocking(1, m_rdinbuf, cmdBuf, true);
			wpipe->BindBufferNonblocking(2, din_q->m_samples, cmdBuf);

			//One dispatch per transform.
			//
			//The complex window shaders early out at nthread >= npoints and derive the
			//window coefficient from the raw thread index, so a single oversized dispatch
			//cannot cover several segments - only offsetIn/offsetOut can move the window.
			//Same structure as ComplexSpectrogramFilter.cpp:272-282, including
			//DispatchNoRebind after the first: the bindings never change, only the offsets,
			//and skipping the descriptor rebind is what makes this safe without
			//VK_KHR_push_descriptor.
			for(size_t block=0; block<nblocks; block++)
			{
				args.offsetIn = block * npoints;
				args.offsetOut = block * npoints;

				if(block == 0)
				{
					wpipe->Dispatch(cmdBuf, args,
						min(window_block_count, 32768u),
						window_block_count / 32768 + 1);
				}
				else
				{
					wpipe->DispatchNoRebind(cmdBuf, args,
						min(window_block_count, 32768u),
						window_block_count / 32768 + 1);
				}
			}

			wpipe->AddComputeMemoryBarrier(cmdBuf);
			m_rdinbuf.MarkModifiedFromGpu();
		}

		//Do the actual FFT operation. One batched dispatch for every transform in the block.
		{
			NamedDebugRange shaderRange(cmdBuf, "FFT");
			m_vkPlan->AppendForward(m_rdinbuf, m_rdoutbuf, cmdBuf);
		}

		//fftshift and convert to dBm, every spectrum in one dispatch
		{
			NamedDebugRange shaderRange(cmdBuf, "Postprocess");

			const float impedance = 50;
			ComplexToLogMagnitudeShiftedArgs cargs;
			cargs.npoints = nouts;
			cargs.nblocks = nblocks;
			cargs.scale = scale * scale / impedance;

			const uint32_t post_block_count = GetComputeBlockCount(nouts * nblocks, 64);

			m_postprocessComputePipeline.BindBufferNonblocking(0, m_rdoutbuf, cmdBuf);
			m_postprocessComputePipeline.BindBufferNonblocking(1, cap->m_samples, cmdBuf, true);
			m_postprocessComputePipeline.AddComputeMemoryBarrier(cmdBuf);
			m_postprocessComputePipeline.Dispatch(cmdBuf, cargs,
				min(post_block_count, 32768u),
				post_block_count / 32768 + 1);
		}
	}

	cap->MarkModifiedFromGpu();

}
