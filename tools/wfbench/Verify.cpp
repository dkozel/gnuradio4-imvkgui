/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of the correctness checks
 */

//scopehal.h first: ComplexChannel.h names its base class without declaring it
#include "../../lib/scopehal/scopehal/scopehal.h"
#include "../../lib/scopehal/scopehal/ComplexChannel.h"
#include "../../lib/scopehal/scopeprotocols/scopeprotocols.h"

#include "Verify.h"

#include "ComplexFFTFilter.h"
#include "SigMFSource.h"
#include "SpectrumDensity.h"
#include "SpectrumReducer.h"

#include "sigmf_core_generated.h"
#include "sigmf.h"
#include "sigmf_helpers.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>

using namespace std;

/**
	@brief The SigMF record shape we support. Mirrors SigMFSource's.
 */
using SigMFRecord = sigmf::SigMF<
	sigmf::Global<sigmf::core::DescrT>,
	sigmf::Capture<sigmf::core::DescrT>,
	sigmf::Annotation<sigmf::core::DescrT> >;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ComplexFFTFilter verification
//
// A build that links is not evidence of a correct FFT. Synthesize a complex exponential
// at a known offset from center and check that the peak lands in the bin the fftshift
// says it should, for both a positive and a negative offset. The negative case is the
// one that matters: a positive-only test passes even if I and Q are swapped or the shift
// direction is inverted.

///@brief Everything the checks below want to look at from one filter execution
struct ComplexFFTResult
{
	///@brief Index of the largest output bin
	size_t peakBin;

	///@brief Value of the largest output bin, in dBm
	float peakDbm;

	///@brief Absolute frequency of the largest output bin, derived from the output x axis
	double peakHz;

	///@brief Number of output bins
	size_t nouts;
};

/**
	@brief Builds an I/Q waveform pair for a complex exponential and runs one FFT

	@param filt		The filter under test, with its inputs already connected to chan
	@param chan		Channel supplying I, Q and the center frequency scalar
	@param fs		Sample rate, Hz
	@param centerHz	Center frequency reported on the scalar stream, Hz
	@param foffHz	Tone offset from center, Hz. Negative means below center.
	@param npoints	Number of samples to synthesize, which is also the transform length
	@param cmdBuf	Command buffer to record into
	@param queue	Queue to submit on
 */
static ComplexFFTResult RunComplexFFTCase(
	ComplexFFTFilter* filt,
	ComplexChannel* chan,
	double fs,
	double centerHz,
	double foffHz,
	size_t npoints,
	vk::raii::CommandBuffer& cmdBuf,
	shared_ptr<QueueHandle> queue)
{
	ComplexFFTResult ret;
	ret.peakBin = 0;
	ret.peakDbm = -INFINITY;
	ret.peakHz = 0;
	ret.nouts = 0;

	//Unit amplitude complex exponential at foffHz. I leads Q for a positive offset.
	auto wi = new UniformAnalogWaveform;
	auto wq = new UniformAnalogWaveform;
	int64_t fs_per_sample = round(1e15 / fs);
	for(auto w : {wi, wq})
	{
		w->m_timescale = fs_per_sample;
		w->m_triggerPhase = 0;
		w->m_startTimestamp = 0;
		w->m_startFemtoseconds = 0;
		w->Resize(npoints);
		w->PrepareForCpuAccess();
	}
	double dphi = 2 * M_PI * foffHz / fs;
	for(size_t i=0; i<npoints; i++)
	{
		wi->m_samples[i] = cos(dphi * i);
		wq->m_samples[i] = sin(dphi * i);
	}
	wi->MarkModifiedFromCpu();
	wq->MarkModifiedFromCpu();

	chan->SetData(wi, 0);
	chan->SetData(wq, 1);
	chan->UpdateCenterFrequency(centerHz);

	//The filter declares CommandBufferAppend, so it expects an already-open command
	//buffer and leaves submission to the caller, exactly as FilterGraphExecutor does
	//(FilterGraphExecutor.cpp:55-73).
	cmdBuf.begin({});
	filt->Refresh(cmdBuf, queue);
	cmdBuf.end();
	queue->SubmitAndBlock(cmdBuf);

	auto cap = dynamic_cast<UniformAnalogWaveform*>(filt->GetData(0));
	if(!cap)
	{
		LogError("ComplexFFTFilter produced no output\n");
		return ret;
	}
	cap->PrepareForCpuAccess();

	ret.nouts = cap->size();
	for(size_t i=0; i<ret.nouts; i++)
	{
		if(cap->m_samples[i] > ret.peakDbm)
		{
			ret.peakDbm = cap->m_samples[i];
			ret.peakBin = i;
		}
	}

	//x axis is in microhertz, with bin 0 at m_triggerPhase (Waterfall.cpp:71 is why)
	ret.peakHz = (cap->m_triggerPhase + (int64_t)ret.peakBin * cap->m_timescale) / 1e6;

	return ret;
}

/**
	@brief Runs the ComplexFFTFilter verification cases

	@return True if everything passed
 */
bool VerifyComplexFFTFilter()
{
	//Test signal parameters. The offsets are exact multiples of the bin size, so the tone
	//lands on a bin with no scalloping loss and the expected amplitude is exact.
	const size_t npoints = 4096;
	const double fs = 10e6;
	const double centerHz = 1e9;
	const double binHz = fs / npoints;
	const double foffHz[] = { +512 * binHz, -512 * binHz, +1 * binHz, -1 * binHz };

	//A unit amplitude tone into 50 ohms is 10 mW. Same convention as FFTFilter: an IQ pair
	//of (cos, sin) at amplitude 1 is the baseband equivalent of a 1 V amplitude carrier.
	const float expectedDbm = 10.0f;

	unique_ptr<ComplexChannel> chan(new ComplexChannel(
		nullptr, "RX", "#4040ff", Unit(Unit::UNIT_FS), Unit(Unit::UNIT_VOLTS), 0));

	auto filt = dynamic_cast<ComplexFFTFilter*>(Filter::CreateFilter("Complex FFT", "#ffffff"));
	if(!filt)
	{
		LogError("Failed to create a Complex FFT filter\n");
		return false;
	}
	filt->AddRef();
	filt->SetInput("I", StreamDescriptor(chan.get(), 0));
	filt->SetInput("Q", StreamDescriptor(chan.get(), 1));
	filt->SetInput("center", StreamDescriptor(chan.get(), 2));

	//Rectangular so the coherent gain correction is unity and the amplitude check is exact
	filt->SetWindowFunction(FFTFilter::WINDOW_RECTANGULAR);

	//Compute queue and command buffer, as FilterGraphExecutor.cpp:547-554 sets up
	shared_ptr<QueueHandle> queue(g_vkQueueManager->GetComputeQueue("ComplexFFTFilterTest"));
	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		queue->GetQueue()->m_family);
	vk::raii::CommandPool pool(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(*pool, vk::CommandBufferLevel::ePrimary, 1);
	vk::raii::CommandBuffer cmdBuf(move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	bool ok = true;
	Unit hz(Unit::UNIT_HZ);
	for(auto foff : foffHz)
	{
		auto res = RunComplexFFTCase(filt, chan.get(), fs, centerHz, foff, npoints, cmdBuf, queue);

		//The whole point of the fftshift: bin nouts/2 is DC, so a tone k bins above center
		//lands k bins right of the middle and a tone below center lands to the left.
		size_t expectedBin = npoints/2 + (ssize_t)llround(foff * npoints / fs);

		bool binOk = (res.nouts == npoints) && (res.peakBin == expectedBin);
		bool ampOk = fabs(res.peakDbm - expectedDbm) < 0.01;
		bool freqOk = fabs(res.peakHz - (centerHz + foff)) < binHz/2;

		LogNotice("f_off %14s: peak bin %5zu (expected %5zu) %s, %7.3f dBm (expected %.3f) %s, %s %s\n",
			hz.PrettyPrint(foff).c_str(),
			res.peakBin, expectedBin, binOk ? "ok" : "FAIL",
			res.peakDbm, expectedDbm, ampOk ? "ok" : "FAIL",
			hz.PrettyPrint(res.peakHz).c_str(), freqOk ? "ok" : "FAIL");

		ok = ok && binOk && ampOk && freqOk;
	}

	//Sanity check that the other windows run and still find the tone in the right bin
	struct { FFTFilter::WindowFunction w; const char* name; } windows[] =
	{
		{ FFTFilter::WINDOW_HAMMING,			"Hamming" },
		{ FFTFilter::WINDOW_HANN,				"Hann" },
		{ FFTFilter::WINDOW_BLACKMAN_HARRIS,	"Blackman-Harris" }
	};
	for(auto& wf : windows)
	{
		filt->SetWindowFunction(wf.w);
		auto res = RunComplexFFTCase(filt, chan.get(), fs, centerHz, -512 * binHz, npoints, cmdBuf, queue);
		size_t expectedBin = npoints/2 - 512;

		//FFTFilter.cpp:222-240's coherent gain corrections are empirical, not exact
		//reciprocals of the window's mean, so they leave up to 0.11 dB of error. Hamming is
		//the worst: 1.862 versus the exact 1/(25/46) = 1.840, which is +0.103 dB. Upstream's
		//real FFT reads the same 10.103 dBm for the same tone, so this is inherited
		//behaviour rather than something wrong with the complex path.
		bool binOk = (res.peakBin == expectedBin);
		bool ampOk = fabs(res.peakDbm - expectedDbm) < 0.15;

		LogNotice("%-16s: peak bin %5zu (expected %5zu) %s, %7.3f dBm %s\n",
			wf.name, res.peakBin, expectedBin, binOk ? "ok" : "FAIL",
			res.peakDbm, ampOk ? "ok" : "FAIL");

		ok = ok && binOk && ampOk;
	}

	filt->Release();
	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SigMFSource verification
//
// Opening a file and reading bytes proves very little. The real questions are whether the
// metadata was interpreted correctly and whether I and Q came out the right way round, and
// both are answered by putting a recording with a documented tone through the filter chain
// and checking where the energy lands.

/**
	@brief Runs a recording through ComplexFFTFilter and reports the strongest bin

	@param src			Recording to play
	@param npoints		Transform length
	@param peakHzOut	Absolute frequency of the strongest bin
	@param peakDbmOut	Level of the strongest bin

	@return True if a spectrum was produced
 */
static bool SpectrumPeakOfRecording(
	SigMFSource& src,
	size_t npoints,
	double& peakHzOut,
	float& peakDbmOut)
{
	peakHzOut = 0;
	peakDbmOut = -INFINITY;

	src.SetSampleDepth(npoints);
	src.Start();
	if(!src.AcquireData())
	{
		LogError("AcquireData() failed\n");
		return false;
	}
	if(!src.PopPendingWaveform())
	{
		LogError("no waveform was queued\n");
		return false;
	}

	auto chan = dynamic_cast<ComplexChannel*>(src.GetChannel(0));
	auto filt = dynamic_cast<ComplexFFTFilter*>(Filter::CreateFilter("Complex FFT", "#ffffff"));
	if(!filt || !chan)
		return false;
	filt->AddRef();
	filt->SetInput("I", StreamDescriptor(chan, 0));
	filt->SetInput("Q", StreamDescriptor(chan, 1));
	filt->SetInput("center", StreamDescriptor(chan, 2));
	filt->SetWindowFunction(FFTFilter::WINDOW_BLACKMAN_HARRIS);

	shared_ptr<QueueHandle> queue(g_vkQueueManager->GetComputeQueue("SigMFSourceTest"));
	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		queue->GetQueue()->m_family);
	vk::raii::CommandPool pool(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(*pool, vk::CommandBufferLevel::ePrimary, 1);
	vk::raii::CommandBuffer cmdBuf(std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	cmdBuf.begin({});
	filt->Refresh(cmdBuf, queue);
	cmdBuf.end();
	queue->SubmitAndBlock(cmdBuf);

	auto cap = dynamic_cast<UniformAnalogWaveform*>(filt->GetData(0));
	bool ok = false;
	if(cap)
	{
		cap->PrepareForCpuAccess();
		size_t peakBin = 0;
		for(size_t i=0; i<cap->size(); i++)
		{
			if(cap->m_samples[i] > peakDbmOut)
			{
				peakDbmOut = cap->m_samples[i];
				peakBin = i;
			}
		}
		//x axis is microhertz, bin 0 at m_triggerPhase
		peakHzOut = (cap->m_triggerPhase + (int64_t)peakBin * cap->m_timescale) / 1e6;
		ok = isfinite(peakDbmOut);
	}

	filt->Release();
	return ok;
}

/**
	@brief Verifies SigMFSource against a recording with a documented tone

	tone_signal.sigmf-meta describes "Complex sinusoidal tone at 1.0 MHz" sampled at 40 MHz
	with a center frequency of zero. The peak must therefore land at +1.0 MHz. The sign is
	the point: if I and Q were swapped, or the ci16 conversion mangled one component, the
	tone would appear at -1.0 MHz and this check would fail.
 */
bool VerifySigMFSource(const string& path, double expectedHz)
{
	SigMFSource src(path);
	if(!src.IsValid())
	{
		LogError("could not open %s: %s\n", path.c_str(), src.GetErrorMessage().c_str());
		return false;
	}

	Unit hz(Unit::UNIT_HZ);
	LogNotice("%s\n", path.c_str());
	LogIndenter li;
	LogNotice("format      %s\n", src.GetSampleFormat().ToString().c_str());
	LogNotice("sample rate %s\n", hz.PrettyPrint(src.GetRecordingSampleRate()).c_str());
	LogNotice("center      %s\n", hz.PrettyPrint(src.GetExactCenterFrequency()).c_str());
	LogNotice("samples     %" PRId64 " (%.3f s)\n",
		src.GetTotalSamples(),
		src.GetRecordingSampleRate() > 0 ? src.GetTotalSamples() / src.GetRecordingSampleRate() : 0.0);
	LogNotice("annotations %zu\n", src.GetRecord().annotations.size());

	const size_t npoints = 65536;
	double peakHz = 0;
	float peakDbm = 0;
	if(!SpectrumPeakOfRecording(src, npoints, peakHz, peakDbm))
		return false;

	double binHz = src.GetRecordingSampleRate() / npoints;
	LogNotice("peak        %s at %.3f dBm (bin size %s)\n",
		hz.PrettyPrint(peakHz).c_str(), peakDbm, hz.PrettyPrint(binHz).c_str());

	//The documented tone is at +1.0 MHz. Allow one bin of scalloping either way; the
	double err = fabs(peakHz - expectedHz);
	bool ok = (err <= binHz);
	LogNotice("expected    %s, error %s -> %s\n",
		hz.PrettyPrint(expectedHz).c_str(),
		hz.PrettyPrint(err).c_str(),
		ok ? "ok" : "WRONG");

	if(!ok && (fabs(peakHz + expectedHz) <= binHz))
		LogError("peak is at the mirror image frequency: I and Q are swapped\n");

	return ok;
}

/**
	@brief Opens every recording in a directory and reports what was found

	A smoke test rather than a correctness check: it confirms the metadata of real-world
	files is interpreted without throwing and that each produces a finite spectrum.
 */
bool SurveyRecordings(const vector<string>& paths)
{
	bool allOk = true;
	Unit hz(Unit::UNIT_HZ);

	for(auto& path : paths)
	{
		SigMFSource src(path);
		if(!src.IsValid())
		{
			//Not necessarily a failure: IQ_800MHz-omnisig.sigmf-meta in the demo dataset is
			//a second annotation set for another recording's data and has no data file of
			//its own, which is exactly what the explicit data path argument is for.
			LogNotice("%-46s SKIP (%s)\n", BaseName(path).c_str(), src.GetErrorMessage().c_str());
			continue;
		}

		double peakHz = 0;
		float peakDbm = 0;
		bool ok = SpectrumPeakOfRecording(src, 65536, peakHz, peakDbm);

		LogNotice("%-46s %-16s %-11s ann=%-5zu peak %s @ %.1f dBm %s\n",
			BaseName(path).c_str(),
			src.GetSampleFormat().ToString().c_str(),
			hz.PrettyPrint(src.GetRecordingSampleRate()).c_str(),
			src.GetRecord().annotations.size(),
			hz.PrettyPrint(peakHz).c_str(),
			peakDbm,
			ok ? "" : "<-- NO SPECTRUM");

		allOk &= ok;
	}

	return allOk;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Verification

/**
	@brief Reads the FFT filter's output back to the host

	@return False if there is no output waveform
 */
static bool CaptureSpectra(PlayerSession& session, vector<float>& out)
{
	auto cap = dynamic_cast<UniformAnalogWaveform*>(session.GetFFT()->GetData(0));
	if(!cap)
		return false;

	cap->PrepareForCpuAccess();

	//Element-wise rather than an iterator range: AcceleratorBufferIterator is not a random
	//access iterator, so it has no operator+
	size_t n = cap->size();
	out.resize(n);
	for(size_t i=0; i<n; i++)
		out[i] = cap->m_samples[i];
	return true;
}

/**
	@brief Checks that batching a block into N transforms gives the same spectra as N separate ones

	The tone tests in the application cover the unbatched path only - they hand the filter
	exactly one transform's worth of samples. Nothing there would notice a wrong batch
	stride, a wrong window offset, or an amplitude scale computed from the block length
	instead of the transform length. Those are the three ways this change can break, and all
	three produce a plausible-looking waterfall.

	The check runs the same samples twice - once as N separate single-transform acquisitions,
	once as one batched block - and requires spectrum b of the batch to equal reference b.
	Comparing per segment rather than against a single reference is what makes it a stride
	test: an offset error would line spectrum b up with the wrong samples, and every segment
	of real recorded data differs from every other.
 */
bool VerifyBatchedFFT(PlayerSession& session, int64_t fftLength)
{
	session.SetFFTLength(fftLength);

	const int64_t nblocks = 8;

	LogNotice("\nVerifying batched output against unbatched, %" PRId64 " transforms of %" PRId64 "\n",
		nblocks, fftLength);
	LogIndenter li;

	//References: N consecutive acquisitions of exactly one transform each
	session.SetBlockSize(fftLength);
	session.Restart();

	vector<vector<float> > refs;
	for(int64_t b=0; b<nblocks; b++)
	{
		if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
		{
			LogError("could not acquire reference block %" PRId64 "\n", b);
			return false;
		}

		vector<float> ref;
		if(!CaptureSpectra(session, ref))
		{
			LogError("no output for reference block %" PRId64 "\n", b);
			return false;
		}
		if(static_cast<int64_t>(ref.size()) != fftLength)
		{
			LogError("reference %" PRId64 " has %zu bins, expected %" PRId64 "\n",
				b, ref.size(), fftLength);
			return false;
		}
		refs.push_back(ref);
	}

	//Same samples again, this time as one batched block
	session.SetBlockSize(fftLength * nblocks);
	session.Restart();
	if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
	{
		LogError("could not acquire the batched block\n");
		return false;
	}

	vector<float> batched;
	if(!CaptureSpectra(session, batched))
	{
		LogError("no batched output\n");
		return false;
	}
	if(static_cast<int64_t>(batched.size()) != fftLength * nblocks)
	{
		LogError("batched output has %zu bins, expected %" PRId64 "\n",
			batched.size(), fftLength * nblocks);
		return false;
	}

	//Compare each spectrum against its own reference.
	//
	//Tolerance in dB, not in ULPs: the two paths run the same shaders, but VkFFT is free to
	//schedule a batched transform differently, and these are logarithms of sums of squares.
	//A stride or offset error lines a segment up with entirely different samples, which on
	//real data shows up as tens of dB, so there is a wide margin between this and anything
	//that matters.
	const float tolerance = 0.01f;
	bool ok = true;
	for(int64_t b=0; b<nblocks; b++)
	{
		auto& ref = refs[b];
		float worst = 0;
		int64_t worstBin = 0;
		for(int64_t i=0; i<fftLength; i++)
		{
			//A bin that is exactly zero in both runs gives -inf in both, and inf minus inf
			//is a NaN that says nothing about correctness
			float a = batched[b*fftLength + i];
			if(!isfinite(a) && !isfinite(ref[i]))
				continue;

			float d = fabs(a - ref[i]);
			if(d > worst)
			{
				worst = d;
				worstBin = i;
			}
		}

		if(!(worst <= tolerance))
		{
			LogError("spectrum %" PRId64 ": worst mismatch %.4f dB at bin %" PRId64 "\n",
				b, worst, worstBin);
			ok = false;
		}
		else
			LogNotice("spectrum %" PRId64 ": max deviation %.6f dB ok\n", b, worst);
	}

	//Where the tone landed, as a sanity check that this is a spectrum at all rather than
	//two identically broken buffers
	int64_t refPeak = 0;
	for(int64_t i=1; i<fftLength; i++)
	{
		if(refs[0][i] > refs[0][refPeak])
			refPeak = i;
	}
	LogNotice("peak bin %" PRId64 " at %.3f dBm\n", refPeak, refs[0][refPeak]);

	if(ok)
		LogNotice("batched FFT verification PASSED\n");
	else
		LogError("batched FFT verification FAILED\n");
	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Density verification

///@brief CPU reimplementation of SpectrumDensityTraces.glsl, for cross-checking
static void CpuPercentiles(
	const vector<uint32_t>& hits,
	size_t bin,
	size_t nbins,
	size_t ncells,
	uint32_t total,
	float dbMin,
	float dbPerCell,
	const vector<float>& fracs,
	vector<float>& out,
	float& mean)
{
	out.assign(fracs.size(), dbMin);
	mean = dbMin;
	if(total == 0)
		return;

	vector<bool> have(fracs.size(), false);
	double moment = 0;
	double cum = 0;

	for(size_t c=0; c<ncells; c++)
	{
		double n = hits[c*nbins + bin];
		if(n == 0)
			continue;

		moment += n * (c + 0.5);
		double next = cum + n;

		for(size_t k=0; k<fracs.size(); k++)
		{
			double target = static_cast<double>(fracs[k]) * total;
			if(!have[k] && (next >= target))
			{
				out[k] = dbMin + (c + (target - cum) / n) * dbPerCell;
				have[k] = true;
			}
		}

		cum = next;
	}

	for(size_t k=0; k<fracs.size(); k++)
	{
		if(!have[k])
			out[k] = dbMin + ncells * dbPerCell;
	}

	mean = dbMin + (moment / total) * dbPerCell;
}

/**
	@brief Checks the density histogram and the traces derived from it

	Two passes over the same samples. The first accumulates without folding and reads the raw
	hit histogram back; the second folds and reads the GPU traces. The source is deterministic
	from sample zero, so both passes see identical data, and the traces can be checked against
	a CPU computation over the histogram the GPU actually built.

	This is the same shape as the batched FFT check: two independent computations of one
	quantity, compared. Checking only that the traces "look reasonable" would pass with a
	transposed histogram, an off-by-one in the CDF, or interpolation applied to the wrong edge.
 */
bool VerifySpectrumDensity(PlayerSession& session, int64_t fftLength, int64_t blockSize)
{
	//Set up the session rather than assuming it. Each check leaves the session configured
	//however it needed, so a later one that trusted the caller's numbers would measure
	//against the wrong block size - which is exactly what happened when these stopped being
	//separate invocations and started running back to back.
	session.SetFFTLength(fftLength);
	session.SetBlockSize(blockSize);

	auto density = session.GetDensity();
	const int64_t nblocks = 4;
	const int64_t spectraPerBlock = blockSize / fftLength;
	const int64_t windowSpectra = nblocks * spectraPerBlock;

	LogNotice("\nVerifying spectrum density, %" PRId64 " blocks of %" PRId64 " spectra\n",
		nblocks, spectraPerBlock);
	LogIndenter li;

	const vector<float> fracs = {0.10f, 0.50f, 0.60f, 0.95f};
	density->SetPercentiles(fracs[0], fracs[2], fracs[3]);

	//Pass 1: accumulate without folding, so the hit histogram survives to be read back
	density->SetFoldInterval(1 << 30);
	session.Restart();
	for(int64_t i=0; i<nblocks; i++)
	{
		if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
		{
			LogError("could not acquire block %" PRId64 "\n", i);
			return false;
		}
	}

	size_t nbins = density->GetCachedBins();
	size_t ncells = density->GetHistogramCells();
	if(nbins == 0)
	{
		LogError("density filter produced no buffers\n");
		return false;
	}

	auto& hitbuf = density->GetHits();
	hitbuf.PrepareForCpuAccess();
	vector<uint32_t> hits(nbins * ncells);
	for(size_t i=0; i<hits.size(); i++)
		hits[i] = hitbuf[i];

	//Every spectrum deposits exactly one hit in every column, because out-of-range values
	//clamp into the end cells rather than being dropped. A column that does not total the
	//spectrum count means the accumulate shader lost or duplicated hits - and the trace pass
	//relies on this identity to avoid a separate pass to total each column.
	bool ok = true;
	size_t badColumns = 0;
	for(size_t b=0; b<nbins; b++)
	{
		uint64_t sum = 0;
		for(size_t c=0; c<ncells; c++)
			sum += hits[c*nbins + b];
		if(sum != static_cast<uint64_t>(windowSpectra))
		{
			if(badColumns < 4)
			{
				LogError("bin %zu has %" PRIu64 " hits, expected %" PRId64 "\n",
					b, sum, windowSpectra);
			}
			badColumns++;
			ok = false;
		}
	}
	if(badColumns == 0)
		LogNotice("column totals: all %zu bins have exactly %" PRId64 " hits ok\n", nbins, windowSpectra);
	else
		LogError("%zu of %zu columns have the wrong hit total\n", badColumns, nbins);

	//Pass 2: same samples, folding at exactly the window we just measured
	density->SetFoldInterval(windowSpectra);
	session.Restart();
	for(int64_t i=0; i<nblocks; i++)
	{
		if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
		{
			LogError("could not acquire block %" PRId64 " on the second pass\n", i);
			return false;
		}
	}

	if(!density->IsOutputReady())
	{
		LogError("density did not fold after %" PRId64 " spectra\n", windowSpectra);
		return false;
	}
	if(density->GetLastWindowSpectra() != windowSpectra)
	{
		LogError("folded %" PRId64 " spectra, expected %" PRId64 "\n",
			density->GetLastWindowSpectra(), windowSpectra);
		return false;
	}

	//Read the GPU traces
	const SpectrumDensity::StreamIndex order[] =
	{
		SpectrumDensity::STREAM_LOW,
		SpectrumDensity::STREAM_MEDIAN,
		SpectrumDensity::STREAM_MID,
		SpectrumDensity::STREAM_HIGH
	};
	vector<vector<float> > gpu(4);
	for(size_t k=0; k<4; k++)
	{
		auto w = dynamic_cast<UniformAnalogWaveform*>(density->GetData(order[k]));
		if(!w || (w->size() != nbins))
		{
			LogError("trace %zu missing or wrong length\n", k);
			return false;
		}
		w->PrepareForCpuAccess();
		gpu[k].resize(nbins);
		for(size_t b=0; b<nbins; b++)
			gpu[k][b] = w->m_samples[b];
	}

	auto meanwave = dynamic_cast<UniformAnalogWaveform*>(density->GetData(SpectrumDensity::STREAM_MEAN));
	if(!meanwave)
	{
		LogError("mean trace missing\n");
		return false;
	}
	meanwave->PrepareForCpuAccess();

	//Compare against the CPU computation over the histogram from pass 1
	float dbMin = density->GetRangeMin();
	float dbPerCell = (density->GetRangeMax() - dbMin) / ncells;

	//Tolerance in dB. Both sides do the same arithmetic, but the GPU accumulates the CDF in
	//float where this uses double, so exact equality is not expected on a 512-hit column.
	const float tolerance = 0.01f;
	float worst = 0;
	float worstMean = 0;
	size_t worstBin = 0;
	vector<float> cpu;
	float cpuMean = 0;
	for(size_t b=0; b<nbins; b++)
	{
		CpuPercentiles(hits, b, nbins, ncells, windowSpectra, dbMin, dbPerCell, fracs, cpu, cpuMean);

		for(size_t k=0; k<4; k++)
		{
			float d = fabs(gpu[k][b] - cpu[k]);
			if(d > worst)
			{
				worst = d;
				worstBin = b;
			}
		}
		worstMean = max(worstMean, fabsf(meanwave->m_samples[b] - cpuMean));
	}

	if(worst > tolerance)
	{
		LogError("percentile traces: worst mismatch %.4f dB at bin %zu\n", worst, worstBin);
		ok = false;
	}
	else
		LogNotice("percentile traces: max deviation %.6f dB ok\n", worst);

	if(worstMean > tolerance)
	{
		LogError("mean trace: worst mismatch %.4f dB\n", worstMean);
		ok = false;
	}
	else
		LogNotice("mean trace: max deviation %.6f dB ok\n", worstMean);

	//Percentiles must be monotonic in the fraction. This catches an unsorted or misassigned
	//target that a per-trace comparison against the same wrong CPU code would not.
	size_t badOrder = 0;
	for(size_t b=0; b<nbins; b++)
	{
		if( (gpu[0][b] > gpu[1][b]) || (gpu[1][b] > gpu[2][b]) || (gpu[2][b] > gpu[3][b]) )
			badOrder++;
	}
	if(badOrder)
	{
		LogError("%zu bins have non-monotonic percentiles\n", badOrder);
		ok = false;
	}
	else
		LogNotice("ordering: p10 <= p50 <= p60 <= p95 in all %zu bins ok\n", nbins);

	//Physical sanity, reported rather than asserted.
	//
	//The bins are magnitudes of a complex FFT, so a noise-only bin holds an exponentially
	//distributed power, and the 10th to 95th percentile spread of that in dB is
	//10*log10(ln(0.05)/ln(0.90)) = 14.5 dB regardless of noise level. A median spread far
	//from that means the histogram is not measuring what we think it is - too narrow would
	//suggest cells are being merged, too wide that the axis mapping is stretched.
	{
		vector<float> spreads(nbins);
		for(size_t b=0; b<nbins; b++)
			spreads[b] = gpu[3][b] - gpu[0][b];
		sort(spreads.begin(), spreads.end());

		size_t peakBin = 0;
		for(size_t b=1; b<nbins; b++)
		{
			if(gpu[1][b] > gpu[1][peakBin])
				peakBin = b;
		}

		LogNotice("median p10-p95 spread %.2f dB across bins (14.5 expected for Gaussian noise)\n",
			spreads[nbins/2]);
		LogNotice("strongest bin %zu: p10 %.1f  p50 %.1f  p60 %.1f  p95 %.1f  mean %.1f dBm\n",
			peakBin, gpu[0][peakBin], gpu[1][peakBin], gpu[2][peakBin], gpu[3][peakBin],
			meanwave->m_samples[peakBin]);
	}

	//The density map must have been written, and its values must be in the range the IIR
	//guarantees. A map that is entirely zero means the fold never ran or wrote elsewhere.
	auto dmap = dynamic_cast<DensityFunctionWaveform*>(
		density->GetData(SpectrumDensity::STREAM_DENSITY));
	if(!dmap)
	{
		LogError("no density map\n");
		return false;
	}
	auto& dbuf = dmap->GetOutData();
	dbuf.PrepareForCpuAccess();

	double dsum = 0;
	float dmax = 0;
	size_t outOfRange = 0;
	for(size_t i=0; i<dbuf.size(); i++)
	{
		float v = dbuf[i];
		if(!(v >= 0.0f) || !(v <= 1.0f))
			outOfRange++;
		dsum += v;
		dmax = max(dmax, v);
	}

	if(outOfRange)
	{
		LogError("%zu density cells outside [0,1]\n", outOfRange);
		ok = false;
	}
	if(dmax <= 0)
	{
		LogError("density map is entirely zero\n");
		ok = false;
	}
	else
	{
		LogNotice("density map %zux%zu, peak %.4f, mean %.6f ok\n",
			dmap->GetWidth(), dmap->GetHeight(), dmax, dsum / dbuf.size());
	}

	//The hits must have been cleared by the fold, or the next window would double count
	hitbuf.PrepareForCpuAccess();
	uint64_t residual = 0;
	for(size_t i=0; i<hitbuf.size(); i++)
		residual += hitbuf[i];
	if(residual)
	{
		LogError("fold left %" PRIu64 " hits uncleared\n", residual);
		ok = false;
	}
	else
		LogNotice("hit histogram cleared by the fold ok\n");

	if(ok)
		LogNotice("spectrum density verification PASSED\n");
	else
		LogError("spectrum density verification FAILED\n");
	return ok;
}

