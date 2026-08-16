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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ComplexFFTFilter::ComplexFFTFilter(const string& color)
	: PeakDetectionFilter(color, CAT_RF)
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
	m_window.AddEnumValue("Blackman-Harris", FFTFilter::WINDOW_BLACKMAN_HARRIS);
	m_window.AddEnumValue("Hamming", FFTFilter::WINDOW_HAMMING);
	m_window.AddEnumValue("Hann", FFTFilter::WINDOW_HANN);
	m_window.AddEnumValue("Rectangular", FFTFilter::WINDOW_RECTANGULAR);
	m_window.SetIntVal(FFTFilter::WINDOW_HAMMING);

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
	//Peak detection has to read back the output on the CPU, so it cannot tail call.
	if(m_numpeaks.GetIntVal() > 0)
	{
		return
			(uint32_t)ExecutionCapabilities::CommandBufferAppend |
			(uint32_t)ExecutionCapabilities::VulkanOnly;
	}
	else
	{
		return
			(uint32_t)ExecutionCapabilities::CommandBufferAppend |
			(uint32_t)ExecutionCapabilities::CommandBufferTailCall |
			(uint32_t)ExecutionCapabilities::VulkanOnly;
	}
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
	//Make sure we've got valid inputs
	ClearMessages();
	auto din_i = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(0));
	auto din_q = dynamic_cast<UniformAnalogWaveform*>(GetInputWaveform(1));
	auto din_center = GetInput(2);
	if(!din_i || !din_q || !din_center)
	{
		if(!GetInput(0))
			AddErrorMessage("Missing inputs", "No I signal input connected");
		else if(!GetInputWaveform(0))
			AddErrorMessage("Missing inputs", "No waveform available at I input");
		else if(!din_i)
			AddErrorMessage("Invalid inputs", "Expect a uniform analog I input");

		if(!GetInput(1))
			AddErrorMessage("Missing inputs", "No Q signal input connected");
		else if(!GetInputWaveform(1))
			AddErrorMessage("Missing inputs", "No waveform available at Q input");
		else if(!din_q)
			AddErrorMessage("Invalid inputs", "Expect a uniform analog Q input");

		if(!din_center)
			AddErrorMessage("Missing inputs", "No center frequency control input connected");

		SetData(nullptr, 0);
		return;
	}

	//Force unit to uHz here as well as in the constructor, matching FFTFilter.cpp:170:
	//legacy sessions may have persisted a unit of Hz.
	m_xAxisUnit = Unit(Unit::UNIT_MICROHZ);

	const size_t inlen = min(din_i->size(), din_q->size());
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
	const size_t nouts = npoints;
	m_cachedNumOuts = nouts;
	m_cachedNumBlocks = nblocks;
	LogTrace("ComplexFFTFilter: %zu input samples, %zu transforms of %zu points\n",
		inlen, nblocks, npoints);

	if( (m_cachedNumPoints != npoints) || (m_planBatches != nblocks) )
		ReallocateBuffers(npoints, nouts, nblocks);

	//Bin spacing is fs/N. Unlike the real FFT in FFTFilter there is no factor of two:
	//the bins span the whole sample rate, not half of it.
	const double fs_per_sample = din_i->m_timescale;
	const double sample_ghz = 1e6 / fs_per_sample;
	const double bin_uhz_raw = sample_ghz * 1e15 / nouts;
	const int64_t bin_uhz = round(bin_uhz_raw);
	const auto window = m_window.GetEnumVal<FFTFilter::WindowFunction>();
	LogTrace("bin size: %s\n", Unit(Unit::UNIT_MICROHZ).PrettyPrint(bin_uhz).c_str());

	//Set up output and copy time scales / configuration.
	//
	//One waveform holds every spectrum in the block, back to back. m_timescale and
	//m_triggerPhase below describe the bins within one spectrum, not the whole buffer;
	//see the class comment.
	auto cap = SetupEmptyUniformAnalogOutputWaveform(din_i, 0);
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
	const double center_uhz = din_center.GetScalarValue() * 1e6;
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
		case FFTFilter::WINDOW_HAMMING:
			scale *= 1.862;
			break;

		case FFTFilter::WINDOW_HANN:
			scale *= 2.013;
			break;

		case FFTFilter::WINDOW_BLACKMAN_HARRIS:
			scale *= 2.805;
			break;

		//unit
		case FFTFilter::WINDOW_RECTANGULAR:
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
		case FFTFilter::WINDOW_HANN:
			args.alpha0 = 0.5;
			break;

		case FFTFilter::WINDOW_HAMMING:
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
		{
			NamedDebugRange shaderRange(cmdBuf, "Window function");

			ComputePipeline* wpipe = nullptr;
			switch(window)
			{
				case FFTFilter::WINDOW_BLACKMAN_HARRIS:
					wpipe = &m_blackmanHarrisComputePipeline;
					break;

				case FFTFilter::WINDOW_HANN:
				case FFTFilter::WINDOW_HAMMING:
					wpipe = &m_cosineSumComputePipeline;
					break;

				default:
				case FFTFilter::WINDOW_RECTANGULAR:
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

	//If doing peak detection, block now.
	//
	//Only meaningful for a single spectrum: FindPeaks() treats the output as one trace, so
	//on a batch it would report peaks at positions that mean nothing. Say so rather than
	//returning confident nonsense.
	if( (m_numpeaks.GetIntVal() > 0) && (nblocks > 1) )
	{
		AddErrorMessage("Unsupported",
			"Peak detection needs a single spectrum; this block holds " + to_string(nblocks));
	}
	else if(m_numpeaks.GetIntVal() > 0)
	{
		cmdBuf.end();
		queue->SubmitAndBlock(cmdBuf);

		//Peak search (for now this runs on the CPU)
		FindPeaks(cap, cmdBuf, queue);
	}
}
