/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Throughput benchmark for the GNU Radio 4 datapath into the imcufosphor display

	Answers "where do the samples go", which a display cannot answer about itself: when a sink
	shows 0.1% of what arrives, the interesting question is whether the source, the handoff or
	the GPU is the limit, and only one of those three is visible on screen.

	Three stages, each adding one piece, so the difference between consecutive numbers is the
	cost of the piece that was added:

	  null      source -> NullSink              what the source alone can produce
	  ring      source -> AnalyzerSink headless + the tag scan and the ring copy
	  engine    source -> BenchSink             + the FFT, density, reducer and waterfall on GPU

	Deliberately no window. `engine` runs the whole GPU pipeline without one, so it measures
	the ceiling a display could reach if it were not also presenting frames - which is the
	number worth comparing a slow display against. What it cannot measure is the tone map and
	the present, which are per-frame costs rather than per-sample ones.

	tools/wfbench is the equivalent for the scopehal-side pipeline driven from a file. This one
	exists because the GR4 path has different plumbing - a scheduler, ports, a ring - and a
	regression in any of that is invisible to wfbench.
 */

#include <chrono>
#include <cinttypes>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "../../../lib/scopehal/scopeprotocols/scopeprotocols.h"

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/basic/SignalGenerator.hpp>
#include <gnuradio-4.0/sigmf/SigMfSource.hpp>
#include <gnuradio-4.0/testing/NullSources.hpp>

#include <gnuradio-4.0/imcufosphor/AnalyzerSink.hpp>

#include "SpectrumEngine.h"

using namespace std;

using TSample = complex<float>;

namespace
{

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The benchmark sink

/**
	@brief Runs the real GPU pipeline with no display attached

	AnalyzerSink minus the ring, the rate limit and everything that draws: samples go straight
	from processBulk() into the engine. That is not how the display works - it cannot be, since
	the engine has to be driven from the render thread - but it is the right shape for a
	benchmark, because it removes every source of drops and measures how fast the GPU can
	actually be fed.
 */
template<typename T>
struct BenchSink : gr::Block<BenchSink<T>>
{
	using Description = gr::Doc<"Drives a SpectrumEngine from the scheduler thread, for benchmarking">;

	gr::PortIn<T> in;

	gr::Annotated<gr::Size_t, "fft_size"> fft_size = 8192U;
	gr::Annotated<gr::Size_t, "block_size"> block_size = 1048576U;
	gr::Annotated<float, "sample_rate"> sample_rate = 1.0f;

	GR_MAKE_REFLECTABLE(BenchSink, in, fft_size, block_size, sample_rate);

	unique_ptr<SpectrumEngine> _engine;
	vector<T> _accum;
	std::uint64_t _consumed = 0;
	std::uint64_t _stepped = 0;

	void start()
	{
		auto queue = g_vkQueueManager->GetComputeQueue("gr4-bench");
		_engine = make_unique<SpectrumEngine>(EnginePart::All, queue);

		EngineConfig cfg;
		cfg.fftLength = static_cast<int64_t>(fft_size);
		cfg.blockSize = static_cast<int64_t>(block_size);
		cfg.sampleRate = sample_rate;
		_engine->Configure(cfg);

		_accum.reserve(static_cast<size_t>(block_size));
	}

	void stop()
	{
		//Same thread that created it, because this sink never involves a render thread
		_engine.reset();
	}

	[[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& dataIn) noexcept
	{
		const size_t n = dataIn.size();
		const size_t want = static_cast<size_t>(block_size);

		//Accumulate to the GPU's block size. The port hands over whatever it happens to have;
		//the engine wants a fixed batch, because that is what makes the FFT one dispatch.
		for(size_t i=0; i<n; i++)
		{
			_accum.push_back(dataIn[i]);
			if(_accum.size() >= want)
			{
				_engine->Step(span<const T>(_accum));
				_accum.clear();
				_stepped++;
			}
		}

		_consumed += n;
		std::ignore = dataIn.consume(n);
		return gr::work::Status::OK;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Harness

struct Options
{
	string file;

	///@brief Synthetic source kind when no file is given: "constant" (unthrottled) or "tone"
	string source = "constant";

	string mode = "all";
	double durationSec = 5.0;
	uint32_t fftSize = 8192;
	uint32_t blockSize = 1048576;
	double sampleRate = 40e6;
};

void PrintUsage()
{
	printf("gr4-bench: throughput of the GNU Radio 4 datapath into the imcufosphor display\n");
	printf("Usage: gr4-bench [--file FILE.sigmf-meta] [options]\n");
	printf("\n");
	printf("  --file FILE       a SigMF recording (cf32_le). Without one, a signal generator\n");
	printf("                    is used, which measures the pipeline rather than the disk.\n");
	printf("  --source S        constant (unthrottled, measures the ceiling) or tone\n");
	printf("                    (SignalGenerator, self-paced to --rate). Default constant.\n");
	printf("  --mode M          null | ring | engine | all (default all)\n");
	printf("  --duration S      seconds per stage, default 5\n");
	printf("  --fft N           transform length, default 8192\n");
	printf("  --block N         samples per GPU submit, default 1048576\n");
	printf("  --rate HZ         generator sample rate, default 40e6\n");
}

bool ParseArgs(int argc, char* argv[], Options& opt)
{
	for(int i=1; i<argc; i++)
	{
		string s(argv[i]);
		if((s == "--file") && (i + 1 < argc))
			opt.file = argv[++i];
		else if((s == "--source") && (i + 1 < argc))
			opt.source = argv[++i];
		else if((s == "--mode") && (i + 1 < argc))
			opt.mode = argv[++i];
		else if((s == "--duration") && (i + 1 < argc))
			opt.durationSec = stod(argv[++i]);
		else if((s == "--fft") && (i + 1 < argc))
			opt.fftSize = static_cast<uint32_t>(stoul(argv[++i]));
		else if((s == "--block") && (i + 1 < argc))
			opt.blockSize = static_cast<uint32_t>(stoul(argv[++i]));
		else if((s == "--rate") && (i + 1 < argc))
			opt.sampleRate = stod(argv[++i]);
		else
			return false;
	}
	return true;
}

/**
	@brief Adds whichever source was asked for and hands it to the connector

	Three, because they answer different questions:

	- @c constant is unthrottled, so it measures the ceiling of everything downstream.
	- @c tone is a SignalGenerator, which derives from BlockingSync and therefore paces itself
	  at its own sample_rate. That makes it a realistic stand-in for a live radio and a
	  useless measure of a ceiling: every stage reports the requested rate and nothing else.
	- a file reads as fast as the disk and the decode allow, which is what the display is
	  actually up against when playing a recording.
 */
template<typename TGraph>
void AddSource(TGraph& fg, const Options& opt, auto&& connectTo)
{
	if(opt.file.empty() && (opt.source == "constant"))
	{
		auto& src = fg.template emplaceBlock<gr::testing::ConstantSource<TSample>>({
			{"name", "constant"},
		});
		connectTo(src);
	}
	else if(opt.file.empty())
	{
		auto& src = fg.template emplaceBlock<gr::blocks::basic::SignalGenerator<TSample>>({
			{"name", "tone"},
			{"sample_rate", static_cast<float>(opt.sampleRate)},
			{"signal_type", "Sin"},
			{"frequency", static_cast<float>(opt.sampleRate / 8)},
			{"amplitude", 1.0f},
		});
		connectTo(src);
	}
	else
	{
		auto& src = fg.template emplaceBlock<gr::incubator::sigmf::SigMFSource<TSample>>({
			{"file_name", opt.file},
			{"repeat", true},
		});
		connectTo(src);
	}
}

/**
	@brief Runs a graph for a fixed wall time and reports the rate

	Wall time rather than a sample count because the point is what the pipeline sustains, and a
	sample count would let a slow stage take as long as it likes to reach it.
 */
struct Result
{
	double seconds = 0;
	std::uint64_t samples = 0;

	[[nodiscard]] double MegasamplesPerSecond() const
	{ return (seconds > 0) ? (static_cast<double>(samples) / seconds / 1e6) : 0; }
};

void Report(const char* label, const Result& r, const Options& opt)
{
	printf("  %-8s %9.1f MS/s", label, r.MegasamplesPerSecond());
	if(!opt.file.empty())
	{
		//A recording has a real time base, so say how much faster than real time this is.
		//Nothing else in the output tells you whether the display could keep up live.
		printf("   %6.1fx realtime", r.MegasamplesPerSecond() * 1e6 / 40e6);
	}
	printf("   (%" PRIu64 " samples in %.2f s)\n", r.samples, r.seconds);
}

} // namespace

int main(int argc, char* argv[])
{
	Options opt;
	if(!ParseArgs(argc, argv, opt))
	{
		PrintUsage();
		return 1;
	}

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(Severity::WARNING));

	if(!VulkanInit(false))
	{
		fprintf(stderr, "Failed to initialize Vulkan\n");
		return 1;
	}
	TransportStaticInit();
	DriverStaticInit();
	ScopeProtocolStaticInit();
	InitializePlugins();
	RegisterImcufosphorFilters();

	printf("gr4-bench: %s, fft %u, block %u, %.1f s per stage\n",
		opt.file.empty() ? opt.source.c_str() : opt.file.c_str(),
		opt.fftSize, opt.blockSize, opt.durationSec);
	printf("\n");

	const bool all = (opt.mode == "all");

	//Each stage runs its own graph and scheduler. Sharing one would mean the later stages
	//inherited whatever state the earlier ones left behind.
	auto runFor = [&](auto&& build) -> Result
	{
		gr::Graph fg;

		//The builder hands back a closure reading whatever counter that stage uses, because
		//the three stages count in three different places
		function<std::uint64_t()> counter = build(fg);

		gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> sched;
		if(auto r = sched.exchange(std::move(fg)); !r)
		{
			fprintf(stderr, "scheduler rejected the graph\n");
			return {};
		}

		const auto t0 = chrono::steady_clock::now();
		std::jthread th([&sched] { std::ignore = sched.runAndWait(); });

		this_thread::sleep_for(chrono::duration<double>(opt.durationSec));
		std::ignore = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
		th.join();

		const auto t1 = chrono::steady_clock::now();

		Result res;
		res.seconds = chrono::duration<double>(t1 - t0).count();
		res.samples = counter();
		return res;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// null: what the source alone can produce

	if(all || (opt.mode == "null"))
	{
		//A shared counter rather than a block reference: NullSink counts nothing, and the
		//source's own sample count is the honest measure of what it produced.
		auto res = runFor([&](gr::Graph& fg) -> function<std::uint64_t()>
		{
			auto& snk = fg.emplaceBlock<gr::testing::CountingSink<TSample>>({{"name", "null"}});
			AddSource(fg, opt, [&](auto& src)
			{
				std::ignore = fg.connect<"out", "in">(src, snk);
			});
			return [&snk] { return static_cast<std::uint64_t>(snk.count); };
		});
		Report("null", res, opt);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// ring: + the tag scan and the handoff copy

	if(all || (opt.mode == "ring"))
	{
		auto res = runFor([&](gr::Graph& fg) -> function<std::uint64_t()>
		{
			auto& snk = fg.emplaceBlock<gr::imcufosphor::AnalyzerSink<TSample>>({
				{"name", "display"},
				{"fft_size", gr::Size_t{opt.fftSize}},
				{"block_size", gr::Size_t{opt.blockSize}},
			});
			AddSource(fg, opt, [&](auto& src)
			{
				std::ignore = fg.connect<"out", "in">(src, snk);
			});

			//Everything that arrived, whether it fitted in the ring or not. Nobody calls
			//draw() here, so nothing is ever drained and the drop count is the whole story.
			return [&snk] { return snk._ring.Pushed() + snk._ring.Dropped(); };
		});
		Report("ring", res, opt);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// engine: + the whole GPU pipeline

	if(all || (opt.mode == "engine"))
	{
		auto res = runFor([&](gr::Graph& fg) -> function<std::uint64_t()>
		{
			auto& snk = fg.emplaceBlock<BenchSink<TSample>>({
				{"name", "engine"},
				{"fft_size", gr::Size_t{opt.fftSize}},
				{"block_size", gr::Size_t{opt.blockSize}},
				{"sample_rate", static_cast<float>(opt.sampleRate)},
			});
			AddSource(fg, opt, [&](auto& src)
			{
				std::ignore = fg.connect<"out", "in">(src, snk);
			});
			return [&snk] { return snk._consumed; };
		});
		Report("engine", res, opt);
	}

	printf("\n");
	ScopehalStaticCleanup();
	return 0;
}
