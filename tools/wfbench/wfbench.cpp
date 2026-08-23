/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Headless throughput benchmark for the waterfall pipeline

	Drives the real PlayerSession - the same source, the same filters, the same command
	buffers the application uses - with no window and no vsync, and reports where the time
	goes. A benchmark that measured a reimplementation of the pipeline would tell us nothing
	about the pipeline.

	The question it exists to answer is whether the pipeline is bound by GPU compute, by host
	sample conversion, or by per-submit overhead. Those have completely different fixes, and
	the totals alone cannot distinguish them. See DESIGN.md section 13.
 */

#include "../../lib/scopehal/scopeprotocols/scopeprotocols.h"

#include "PlayerSession.h"
#include "SigMFSource.h"

#include "Verify.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Configuration

struct BenchConfig
{
	string path;
	int64_t fftLength = 8192;

	///@brief Samples per acquisition. Zero means "same as the transform length".
	int64_t blockSize = 0;

	int64_t spectraPerRow = 1;

	///@brief Spectra per density fold. Zero leaves the filter's default.
	int64_t foldInterval = 0;

	///@brief Amplitude cells in the hit histogram and in the density map. Zero keeps defaults.
	int64_t histogramCells = 0;
	int64_t densityCells = 0;

	///@brief Samples to process in the measured run
	int64_t targetSamples = 0;

	///@brief Seconds to run if no sample target was given
	double duration = 5.0;

	///@brief Samples to process before zeroing the counters
	int64_t warmupSamples = 4 * 1024 * 1024;

	bool warmCache = false;

	///@brief Force the CPU conversion path even where the recording could be packed
	bool forcePlanar = false;
	bool gpuTiming = true;
	bool verify = false;

	///@brief Expected tone frequency for the source check, or zero to skip it
	double expectTone = 0;

	///@brief Recordings to survey instead of benchmarking
	vector<string> surveyPaths;

	string jsonPath;
};

/**
	@brief Parses a sample count, accepting a k/M/G suffix

	Block sizes are powers of two in the millions; typing them out in full invites
	off-by-a-digit errors that look like real performance changes.
 */
static bool ParseCount(const char* str, int64_t& out)
{
	char* end = nullptr;
	double v = strtod(str, &end);
	if(end == str)
		return false;

	switch(*end)
	{
		case 'k':
		case 'K':
			v *= 1024;
			end++;
			break;

		case 'm':
		case 'M':
			v *= 1024 * 1024;
			end++;
			break;

		case 'g':
		case 'G':
			v *= 1024 * 1024 * 1024;
			end++;
			break;

		default:
			break;
	}

	if(*end != '\0')
		return false;

	out = static_cast<int64_t>(v);
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Page cache

/**
	@brief Reads the whole data file so the measured run hits the page cache

	Cold and warm numbers differ by more than any optimization we are likely to make, so the
	state of the cache has to be a deliberate choice rather than a function of what ran
	before. The demo recordings are at most 2.9 GB against 93 GB of RAM, so they fit.
 */
static void WarmCache(const string& dataPath)
{
	int fd = open(dataPath.c_str(), O_RDONLY);
	if(fd < 0)
	{
		LogWarning("could not open %s to warm the page cache\n", dataPath.c_str());
		return;
	}

	double t0 = GetTime();
	vector<uint8_t> buf(8 * 1024 * 1024);
	int64_t total = 0;
	while(true)
	{
		ssize_t r = read(fd, buf.data(), buf.size());
		if(r <= 0)
			break;
		total += r;
	}
	close(fd);

	LogNotice("Warmed page cache: %.1f GB in %.1f s\n", total / 1e9, GetTime() - t0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reporting

static void ReportGpuTimer(const char* what, GpuTimer& timer)
{
	if(!timer.IsSupported())
	{
		LogNotice("    %-10s (device cannot timestamp)\n", what);
		return;
	}
	if(timer.GetSampleCount() == 0)
	{
		LogNotice("    %-10s (no samples)\n", what);
		return;
	}

	string line;
	auto& labels = timer.GetLabels();
	for(size_t i=0; i<labels.size(); i++)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "%s %.3f ms  ", labels[i].c_str(), timer.GetAverageMs(i));
		line += buf;
	}

	LogNotice("    %-10s %s(total %.3f ms over %" PRId64 " passes)\n",
		what, line.c_str(), timer.GetAverageTotalMs(), timer.GetSampleCount());
}

static bool WriteJson(const string& path, const BenchConfig& cfg, PlayerSession& session, double wallSec)
{
	FILE* fp = fopen(path.c_str(), "w");
	if(!fp)
	{
		LogError("could not open %s for writing\n", path.c_str());
		return false;
	}

	auto source = session.GetSource();
	auto& stats = session.GetStats();
	int64_t samples = source->GetSamplesDelivered();

	fprintf(fp, "{\n");
	fprintf(fp, "  \"file\": \"%s\",\n", cfg.path.c_str());
	fprintf(fp, "  \"fft_length\": %" PRId64 ",\n", cfg.fftLength);
	fprintf(fp, "  \"block_size\": %" PRIu64 ",\n", source->GetSampleDepth());
	fprintf(fp, "  \"spectra_per_row\": %" PRId64 ",\n", session.GetGroupSize());
	fprintf(fp, "  \"warm_cache\": %s,\n", cfg.warmCache ? "true" : "false");
	fprintf(fp, "  \"packed_ingest\": %s,\n", source->IsUsingPackedPath() ? "true" : "false");
	fprintf(fp, "  \"wall_sec\": %.6f,\n", wallSec);
	fprintf(fp, "  \"samples\": %" PRId64 ",\n", samples);
	fprintf(fp, "  \"megasamples_per_sec\": %.3f,\n", samples / wallSec / 1e6);
	fprintf(fp, "  \"spectra\": %" PRId64 ",\n", stats.spectra);
	fprintf(fp, "  \"rows\": %" PRId64 ",\n", session.GetRowsPlayed());
	fprintf(fp, "  \"submits\": %" PRId64 ",\n", stats.submits);
	fprintf(fp, "  \"cpu_read_sec\": %.6f,\n", source->GetReadSeconds());
	fprintf(fp, "  \"cpu_convert_sec\": %.6f,\n", source->GetConvertSeconds());
	fprintf(fp, "  \"cpu_acquire_sec\": %.6f,\n", stats.acquireSec);
	fprintf(fp, "  \"cpu_record_sec\": %.6f,\n", stats.recordSec);
	fprintf(fp, "  \"submit_wait_sec\": %.6f\n", stats.submitSec);
	fprintf(fp, "}\n");

	fclose(fp);
	LogNotice("Wrote %s\n", path.c_str());
	return true;
}

static void Report(const BenchConfig& cfg, PlayerSession& session, double wallSec)
{
	auto source = session.GetSource();
	auto& stats = session.GetStats();
	Unit hz(Unit::UNIT_HZ);

	int64_t samples = source->GetSamplesDelivered();
	double rate = samples / wallSec;
	double srate = source->GetRecordingSampleRate();

	LogNotice("\n");
	LogNotice("Results\n");
	{
		LogIndenter li;

		LogNotice("%-22s %.1f MS/s\n", "throughput", rate / 1e6);
		if(srate > 0)
			LogNotice("%-22s %.3fx\n", "realtime ratio", rate / srate);
		LogNotice("%-22s %.0f /s\n", "transforms", stats.spectra / wallSec);
		LogNotice("%-22s %.1f /s\n", "waterfall rows", session.GetRowsPlayed() / wallSec);
		LogNotice("%-22s %.0f /s\n", "submits", stats.submits / wallSec);
		LogNotice("%-22s %" PRId64 " in %.2f s\n", "samples processed", samples, wallSec);

		//Per-submit cost is the number that says whether this is overhead bound. If the
		//submit count is high and each one is short, the pipeline is paying fixed costs.
		if(stats.submits > 0)
		{
			LogNotice("%-22s %.1f us\n", "wall clock per submit",
				wallSec / stats.submits * 1e6);
		}

		LogNotice("\n");
		LogNotice("Host time (of %.2f s wall clock)\n", wallSec);
		{
			LogIndenter li2;
			double read = source->GetReadSeconds();
			double convert = source->GetConvertSeconds();

			//Acquire covers read and convert plus waveform allocation, so the remainder is
			//the allocation and bookkeeping cost that neither of the other two counts.
			double other = stats.acquireSec - read - convert;

			LogNotice("%-22s %7.1f ms  %5.1f%%\n", "file read",
				read * 1000, 100 * read / wallSec);
			LogNotice("%-22s %7.1f ms  %5.1f%%\n", "sample convert",
				convert * 1000, 100 * convert / wallSec);
			LogNotice("%-22s %7.1f ms  %5.1f%%\n", "waveform alloc/other",
				other * 1000, 100 * other / wallSec);
			LogNotice("%-22s %7.1f ms  %5.1f%%\n", "command recording",
				stats.recordSec * 1000, 100 * stats.recordSec / wallSec);
			LogNotice("%-22s %7.1f ms  %5.1f%%\n", "blocked on submit",
				stats.submitSec * 1000, 100 * stats.submitSec / wallSec);
		}

		if(cfg.gpuTiming)
		{
			LogNotice("\n");
			LogNotice("GPU time per block\n");
			ReportGpuTimer("stages", session.GetGpuTimer());
		}

		LogNotice("\n");
		if(srate > 0)
		{
			LogNotice("RBW %s at %s span\n",
				hz.PrettyPrint(srate / cfg.fftLength).c_str(),
				hz.PrettyPrint(srate).c_str());
		}
		LogNotice("coverage %.1f%%\n", session.GetCoverage() * 100);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Entry point

static void Usage()
{
	printf("wfbench: headless throughput benchmark for the imcufosphor waterfall pipeline\n");
	printf("Usage: wfbench --file FILE.sigmf-meta [options] [logger options]\n");
	printf("  --file FILE            recording to play (required)\n");
	printf("  --fft N                transform length, default 8192\n");
	printf("  --block N              samples per acquisition, default same as --fft\n");
	printf("  --spectra-per-row N    spectra combined into each waterfall row, default 1\n");
	printf("  --fold-interval N      spectra per density fold, default 512\n");
	printf("  --histogram-cells N    amplitude cells in the hit histogram, default 1024\n");
	printf("  --density-cells N      amplitude cells in the density map, default 256\n");
	printf("  --samples N            samples to process in the measured run\n");
	printf("  --duration S           seconds to run if --samples is not given, default 5\n");
	printf("  --warmup N             samples to process before zeroing counters, default 4M\n");
	printf("  --warm-cache           read the whole recording first, so the run hits page cache\n");
	printf("  --planar               force the CPU convert path, for A/B against the packed one\n");
	printf("  --no-gpu-timing        skip timestamp queries (they add a stall per submit)\n");
	printf("  --verify               run every correctness check on this recording, then exit\n");
	printf("  --expect-tone HZ       additionally check the source decodes a tone at HZ\n");
	printf("  --survey FILE...       report what each recording contains, then exit\n");
	printf("  --json FILE            also write the results as JSON\n");
	printf("Counts accept k, M and G suffixes.\n");
}

/**
	@brief Parses the command line

	@return -1 to carry on, otherwise the process exit code
 */
static int ParseArguments(int argc, char* argv[], BenchConfig& cfg)
{
	Severity console_verbosity = Severity::NOTICE;

	for(int i=1; i<argc; i++)
	{
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;

		string s(argv[i]);
		auto next = [&](const char* what) -> const char*
		{
			if(i+1 >= argc)
			{
				fprintf(stderr, "%s requires an argument\n", what);
				exit(1);
			}
			return argv[++i];
		};

		if(s == "--help")
		{
			Usage();
			return 0;
		}
		else if(s == "--file")
			cfg.path = next("--file");
		else if(s == "--fft")
		{
			if(!ParseCount(next("--fft"), cfg.fftLength))
			{
				fprintf(stderr, "--fft needs a count\n");
				return 1;
			}
		}
		else if(s == "--block")
		{
			if(!ParseCount(next("--block"), cfg.blockSize))
			{
				fprintf(stderr, "--block needs a count\n");
				return 1;
			}
		}
		else if(s == "--spectra-per-row")
		{
			if(!ParseCount(next("--spectra-per-row"), cfg.spectraPerRow))
			{
				fprintf(stderr, "--spectra-per-row needs a count\n");
				return 1;
			}
		}
		else if(s == "--samples")
		{
			if(!ParseCount(next("--samples"), cfg.targetSamples))
			{
				fprintf(stderr, "--samples needs a count\n");
				return 1;
			}
		}
		else if(s == "--warmup")
		{
			if(!ParseCount(next("--warmup"), cfg.warmupSamples))
			{
				fprintf(stderr, "--warmup needs a count\n");
				return 1;
			}
		}
		else if(s == "--duration")
			cfg.duration = atof(next("--duration"));
		else if(s == "--warm-cache")
			cfg.warmCache = true;
		else if(s == "--planar")
			cfg.forcePlanar = true;
		else if(s == "--no-gpu-timing")
			cfg.gpuTiming = false;
		else if(s == "--fold-interval")
		{
			if(!ParseCount(next("--fold-interval"), cfg.foldInterval))
			{
				fprintf(stderr, "--fold-interval needs a count\n");
				return 1;
			}
		}
		else if(s == "--histogram-cells")
		{
			if(!ParseCount(next("--histogram-cells"), cfg.histogramCells))
			{
				fprintf(stderr, "--histogram-cells needs a count\n");
				return 1;
			}
		}
		else if(s == "--density-cells")
		{
			if(!ParseCount(next("--density-cells"), cfg.densityCells))
			{
				fprintf(stderr, "--density-cells needs a count\n");
				return 1;
			}
		}
		else if(s == "--verify")
			cfg.verify = true;
		else if(s == "--expect-tone")
			cfg.expectTone = atof(next("--expect-tone"));
		else if(s == "--survey")
		{
			while(i+1 < argc)
				cfg.surveyPaths.push_back(argv[++i]);
		}
		else if(s == "--json")
			cfg.jsonPath = next("--json");
		else
		{
			fprintf(stderr, "Unrecognized argument \"%s\", use --help\n", s.c_str());
			return 1;
		}
	}

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	if(cfg.path.empty() && cfg.surveyPaths.empty())
	{
		Usage();
		return 1;
	}

	//Match the application's Vulkan bring-up exactly, GLFW and all. Passing true here would
	//skip window system integration, which can select a different device or a different
	//queue family layout - and on this workstation the queue family layout is what decides
	//whether the Xid-32 defect fires (DESIGN.md section 3, R7). A benchmark on a different
	//device configuration than the application is not measuring the application.
	if(!VulkanInit(false))
	{
		LogError("Vulkan initialization failed\n");
		return 1;
	}

	//DriverStaticInit() is three lines of initialization we genuinely depend on followed by
	//about a hundred AddDriverClass calls for instruments this application will never open, so
	//call the three directly. InitializeSearchPaths() is what makes FindDataFile() resolve
	//shaders/*.spv next to the binary; DetectCPUFeatures() sets the g_has* flags that inline
	//code in scopehal's headers reads; Unit::InitializeLocales() is required before any
	//PrettyPrint(). TransportStaticInit() registers twelve SCPI transports and
	//InitializePlugins() dlopens whatever it finds in /usr/lib/scopehal/plugins, neither of
	//which has anything to offer a file player.
	InitializeSearchPaths();
	DetectCPUFeatures();
	Unit::InitializeLocales();
	ScopeProtocolStaticInit();

	//Our app-local filters, same as main.cpp
	AddDecoderClass(ComplexFFTFilter);
	AddDecoderClass(SpectrumReducer);
	AddDecoderClass(SpectrumDensity);

	//Survey needs no session, and takes a list rather than a single recording
	if(!cfg.surveyPaths.empty())
	{
		bool sok = SurveyRecordings(cfg.surveyPaths);
		ScopehalStaticCleanup();
		return sok ? 0 : 1;
	}

	return -1;
}

/**
	@brief Everything that owns Vulkan resources

	Separate from main() so the session and the source are destroyed before
	ScopehalStaticCleanup() tears the device down. Calling cleanup with a live PlayerSession
	on the stack destroys the device first and then runs the session's destructor against it,
	which faults inside the driver.
 */
static int Run(BenchConfig& cfg)
{
	SigMFSource source(cfg.path);
	if(!source.IsValid())
	{
		LogError("could not open %s: %s\n", cfg.path.c_str(), source.GetErrorMessage().c_str());
		return 1;
	}

	if(cfg.blockSize == 0)
		cfg.blockSize = cfg.fftLength;

	//Applied before anything reports on it, and before the first AcquireData(), since the two
	//ingest paths publish different waveforms
	if(cfg.forcePlanar)
		source.SetPackedPathAllowed(false);

	Unit hz(Unit::UNIT_HZ);
	LogNotice("wfbench\n");
	{
		LogIndenter li;

		LogNotice("device            %s\n",
			g_vkComputePhysicalDevice->getProperties().deviceName.data());
		LogNotice("push descriptors  %s\n", g_hasPushDescriptor ? "yes" : "no");
		LogNotice("unified memory    %s\n", g_vulkanDeviceHasUnifiedMemory ? "yes" : "no");
		LogNotice("recording         %s\n", source.GetName().c_str());
		LogNotice("format            %s\n", source.GetSampleFormat().ToString().c_str());
		LogNotice("ingest path       %s\n",
			source.IsUsingPackedPath()
				? "packed (file bytes to GPU unconverted)"
				: (source.IsPackedPathSupported()
					? "planar float (packed path available, forced off)"
					: "planar float (format cannot be packed)"));
		LogNotice("sample rate       %s\n", hz.PrettyPrint(source.GetRecordingSampleRate()).c_str());
		LogNotice("total samples     %" PRId64 "\n", source.GetTotalSamples());
		LogNotice("transform length  %" PRId64 "\n", cfg.fftLength);
		LogNotice("block size        %" PRId64 " (%" PRId64 " transforms/block)\n",
			cfg.blockSize, cfg.blockSize / cfg.fftLength);
		LogNotice("spectra per row   %" PRId64 "\n", cfg.spectraPerRow);
	}

	if(cfg.warmCache)
		WarmCache(source.GetDataPath());

	shared_ptr<QueueHandle> queue(
		g_vkQueueManager->GetQueueFromPool(QueueManager::QUEUE_POOL_FILTER, "wfbench.compute"));
	PlayerSession session(&source, queue);
	session.SetFFTLength(cfg.fftLength);
	session.SetBlockSize(cfg.blockSize);
	session.SetGroupSize(cfg.spectraPerRow);
	session.SetGpuTimingEnabled(cfg.gpuTiming);
	if(cfg.foldInterval > 0)
		session.GetDensity()->SetFoldInterval(cfg.foldInterval);
	if( (cfg.histogramCells > 0) || (cfg.densityCells > 0) )
	{
		auto d = session.GetDensity();
		size_t hc = (cfg.histogramCells > 0) ? cfg.histogramCells : d->GetHistogramCells();
		size_t dc = (cfg.densityCells > 0) ? cfg.densityCells : d->GetDensityCells();
		d->SetCellCounts(hc, dc);
	}

	if(cfg.verify)
	{
		//Everything, in one flag. Splitting these across several was a way to lose one of
		//them from a gate without noticing.
		LogNotice("\nComplexFFTFilter verification\n");
		bool fftOk;
		{
			LogIndenter li;
			fftOk = VerifyComplexFFTFilter();
		}
		LogNotice("ComplexFFTFilter verification %s\n", fftOk ? "PASSED" : "FAILED");

		LogNotice("\nPacked I/Q datapath verification\n");
		bool packedOk;
		{
			LogIndenter li;
			packedOk = VerifyPackedIQPath();
		}
		LogNotice("Packed I/Q datapath verification %s\n", packedOk ? "PASSED" : "FAILED");

		bool vok = fftOk && packedOk;
		vok = VerifyBatchedFFT(session, cfg.fftLength) && vok;
		vok = VerifySpectrumDensity(session, cfg.fftLength, cfg.blockSize) && vok;

		//Drives the same filters as everything above, but from a raw IQ span rather than from
		//a recording, which is the path the GNU Radio blocks take
		vok = VerifySpectrumEngine() && vok;

		//Only meaningful on a recording documented to contain one tone
		if(cfg.expectTone > 0)
			vok = VerifySigMFSource(cfg.path, cfg.expectTone) && vok;

		LogNotice("\n%s\n", vok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
		return vok ? 0 : 1;
	}

	//Warm up before measuring. The first passes pay for pipeline creation, shader module
	//loading and the VkFFT plan build, none of which recur, and the FFT plan in particular
	//costs tens of milliseconds.
	LogNotice("\nWarming up (%" PRId64 " samples)...\n", cfg.warmupSamples);
	double tWarm = GetTime();
	while(source.GetSamplesDelivered() < cfg.warmupSamples)
	{
		//StepOneBlock, not StepOneRow: with a large group size a row can take hundreds of
		//blocks, and the loop condition needs to be checked per block, not per row.
		if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
		{
			LogError("source stopped during warmup; the recording may be too short for "
				"these settings\n");
			return 1;
		}
	}
	LogNotice("Warmup took %.2f s\n", GetTime() - tWarm);

	//Measure
	session.ResetStats();
	LogNotice("\nMeasuring...\n");

	double t0 = GetTime();
	double deadline = t0 + cfg.duration;
	while(true)
	{
		if(cfg.targetSamples > 0)
		{
			if(source.GetSamplesDelivered() >= cfg.targetSamples)
				break;
		}
		else if(GetTime() >= deadline)
			break;

		if(session.StepOneBlock() == PlayerSession::STEP_FAILED)
			break;
	}
	double wallSec = GetTime() - t0;

	Report(cfg, session, wallSec);

	bool ok = true;
	if(!cfg.jsonPath.empty())
		ok = WriteJson(cfg.jsonPath, cfg, session, wallSec);

	//Tear down the Vulkan globals explicitly, the same way the application does.
	//
	//PipelineCacheManager's destructor writes the shader cache and logs while doing it. Left
	//to static destruction it outlives g_log_sinks and dereferences freed sinks
	//(log.cpp:342-344). This was previously worked around here by emptying g_log_sinks;
	//scopehal supplies the actual fix.

	return ok ? 0 : 1;
}

int main(int argc, char* argv[])
{
	BenchConfig cfg;
	int parseResult = ParseArguments(argc, argv, cfg);
	if(parseResult >= 0)
		return parseResult;

	int ret = Run(cfg);

	//Tear down the Vulkan globals explicitly. Required, not optional: PipelineCacheManager's
	//destructor writes the shader cache and logs while doing it, and if it runs as a static
	//destructor it can outlive g_log_sinks and segfault on the way out.
	ScopehalStaticCleanup();
	return ret;
}