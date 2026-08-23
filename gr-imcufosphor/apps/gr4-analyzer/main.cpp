/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Example host running an imcufosphor display block in a GNU Radio 4 flowgraph

	The reference for how the pieces fit together: scopehal's globals are initialised, a window
	is created and published to the RenderHost, a flowgraph is built and handed to a scheduler
	on its own thread, and the main thread runs the render loop calling draw() on every block
	that says it draws content.

	Not a general-purpose application. sigmf-spectrum is that; this exists to show the wiring
	and to be the thing that proves it works end to end.
 */

#include <chrono>
#include <cinttypes>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

//scopehal.h first: Waterfall.h is not self-contained.
#include <scopehal/scopehal.h>
#include <scopeprotocols/Waterfall.h>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/basic/SignalGenerator.hpp>
#include <gnuradio-4.0/sigmf/SigMfSource.hpp>
#ifdef GR4_ANALYZER_HAVE_SOAPYSDR
#include <gnuradio-4.0/soapysdr/SoapyRx.hpp>
#endif

#include <gnuradio-4.0/imcufosphor/AnalyzerSink.hpp>

#include "Gr4AnalyzerWindow.h"
#include "RenderHost.h"

using namespace std;

namespace
{

///@brief Where the samples come from. Exactly one, chosen by which options were given.
enum class SourceKind
{
	///@brief Built-in tone generator. The default, and the one that needs nothing attached.
	Synthetic,

	///@brief A SigMF recording, via the incubator's cf32_le-only source
	SigMF,

	///@brief A live SDR, via SoapySDR
	Soapy
};

struct Options
{
	SourceKind source = SourceKind::Synthetic;

	///@brief Recording to play, when source is SigMF
	string file;

	uint32_t fftSize = 8192;
	uint32_t blockSize = 1048576;
	uint32_t groupSize = 1;
	bool repeat = true;

	///@brief Sample rate. Requested from the radio, or synthesized, or read from the recording.
	double sampleRate = 10e6;

	///@brief Tone offset from centre, synthetic source only
	double toneHz = 1.5e6;

	///@brief Centre frequency. Tuned on the radio, or just labels the axis for the generator.
	double centerHz = 100e6;

	/**
		@brief Amplitude scale, in dBm

		The defaults suit a near-full-scale signal. A quiet recording needs them moved down, or
		its spectrum falls off the bottom of the amplitude axis and disappears entirely while
		the waterfall still shows a flat floor - because the waterfall clamps out-of-range
		values to the end of the colour ramp rather than clipping them away. That asymmetry
		makes "waterfall but no spectrum" the characteristic look of a wrong range.
	 */
	float dbMin = -100.0f;
	float dbMax = -20.0f;

	///@brief -1 means "decide from the source kind"; 0/1 force it off/on
	int backpressure = -1;

	//Soapy only
	string device;
	string deviceArgs;
	string antenna;
	double gainDb = 30.0;
	double bandwidth = 0.0;		//0 lets the driver pick
	uint32_t channel = 0;
};

void PrintUsage()
{
	printf("gr4-analyzer: renders a GNU Radio 4 flowgraph through the imcufosphor display\n");
	printf("Usage: gr4-analyzer [source] [options]\n");
	printf("\n");
	printf("Sources, pick at most one. With none, a synthetic tone is generated, which is the\n");
	printf("quickest way to see the display working and needs nothing attached.\n");
	printf("\n");
	printf("  --file FILE       a SigMF recording (cf32_le only; that is all the upstream\n");
	printf("                    SigMF source reads). Must be longer than one --block.\n");
	printf("  --soapy [SEL]     a live SDR through SoapySDR. SEL is either a bare driver name\n");
	printf("                    ('uhd') or any key=value selector SoapySDRUtil --find prints\n");
	printf("                    ('serial=30AF821'). Omit it to take the first device found.\n");
	printf("\n");
	printf("Display:\n");
	printf("  --fft N           transform length, default 8192\n");
	printf("  --block N         IQ samples per GPU submit, default 1048576\n");
	printf("  --group N         spectra per waterfall row, default 1\n");
	printf("\n");
	printf("Tuning, for --soapy (requested from the radio) and --rate/--center for the\n");
	printf("generator (where they only synthesize and label):\n");
	printf("  --rate HZ         sample rate, default 10e6\n");
	printf("  --center HZ       centre frequency, default 100e6\n");
	printf("  --gain DB         RX gain, default 30\n");
	printf("  --antenna NAME    e.g. RX2. Driver default if unset.\n");
	printf("  --bandwidth HZ    analog filter bandwidth. Driver default if unset.\n");
	printf("  --channel N       RX channel, default 0\n");
	printf("\n");
	printf("Amplitude scale:\n");
	printf("  --db-min DBM      bottom of the scale, default -100\n");
	printf("  --db-max DBM      top of the scale, default -20\n");
	printf("\n");
	printf("                    A signal below --db-min falls off the bottom of the spectrum's\n");
	printf("                    axis while the waterfall still shows a flat floor, so\n");
	printf("                    'waterfall but no spectrum' usually means these need lowering.\n");
	printf("                    Both are also adjustable at runtime in the Settings panel.\n");
	printf("\n");
	printf("Flow control:\n");
	printf("  --backpressure    throttle the source rather than dropping samples. Default on\n");
	printf("  --no-backpressure for a file, off for anything live, where dropping a frame beats\n");
	printf("                    a receiver overflow. A file has no realtime constraint, so\n");
	printf("                    pacing it means the whole capture is analysed, not a sample.\n");
	printf("  --device-args S   extra SoapySDR device arguments\n");
	printf("\n");
	printf("Other:\n");
	printf("  --tone HZ         tone offset from centre, synthetic source only, default 1.5e6\n");
	printf("  --no-repeat       stop at the end of the recording instead of looping\n");
	printf("\n");
	printf("Examples:\n");
	printf("  gr4-analyzer\n");
	printf("  gr4-analyzer --soapy driver=uhd --center 2.4e9 --rate 20e6 --gain 40\n");
	printf("  gr4-analyzer --file capture.sigmf-meta --fft 4096 --block 65536\n");
	printf("  gr4-analyzer --file quiet.sigmf-meta --db-min -140 --db-max -80\n");
}

bool ParseArgs(int argc, char* argv[], Options& opt)
{
	for(int i=1; i<argc; i++)
	{
		string s(argv[i]);

		if((s == "--file") && (i + 1 < argc))
		{
			opt.file = argv[++i];
			opt.source = SourceKind::SigMF;
		}
		else if(s == "--soapy")
		{
			opt.source = SourceKind::Soapy;

			//The device selector is optional, so only swallow the next argument if it is not
			//itself an option. "--soapy --center 2.4e9" must not try to open a device called
			//"--center".
			if((i + 1 < argc) && (argv[i+1][0] != '-'))
			{
				const string sel = argv[++i];

				//SoapyRx keeps these apart: 'device' is a bare driver name that it turns into
				//kwargs["driver"], while 'device_args' is a full key=value argument string.
				//Passing "driver=uhd" as the former yields kwargs["driver"]="driver=uhd" and
				//Device::make() finds nothing, which is an unhelpful way to learn the
				//difference. Route on whether the user wrote a key=value pair, so that both
				//"--soapy uhd" and the "--soapy serial=30AF821" form printed by
				//SoapySDRUtil --find do what they look like they should.
				if(sel.find('=') != string::npos)
					opt.deviceArgs = sel;
				else
					opt.device = sel;
			}
		}
		else if((s == "--gain") && (i + 1 < argc))
			opt.gainDb = stod(argv[++i]);
		else if((s == "--antenna") && (i + 1 < argc))
			opt.antenna = argv[++i];
		else if((s == "--bandwidth") && (i + 1 < argc))
			opt.bandwidth = stod(argv[++i]);
		else if((s == "--channel") && (i + 1 < argc))
			opt.channel = static_cast<uint32_t>(stoul(argv[++i]));
		else if((s == "--device-args") && (i + 1 < argc))
			opt.deviceArgs = argv[++i];
		else if((s == "--fft") && (i + 1 < argc))
			opt.fftSize = static_cast<uint32_t>(stoul(argv[++i]));
		else if((s == "--block") && (i + 1 < argc))
			opt.blockSize = static_cast<uint32_t>(stoul(argv[++i]));
		else if((s == "--group") && (i + 1 < argc))
			opt.groupSize = static_cast<uint32_t>(stoul(argv[++i]));
		else if(s == "--no-repeat")
			opt.repeat = false;
		else if((s == "--rate") && (i + 1 < argc))
			opt.sampleRate = stod(argv[++i]);
		else if((s == "--tone") && (i + 1 < argc))
			opt.toneHz = stod(argv[++i]);
		else if((s == "--center") && (i + 1 < argc))
			opt.centerHz = stod(argv[++i]);
		else if((s == "--db-min") && (i + 1 < argc))
			opt.dbMin = stof(argv[++i]);
		else if((s == "--db-max") && (i + 1 < argc))
			opt.dbMax = stof(argv[++i]);
		else if(s == "--backpressure")
			opt.backpressure = 1;
		else if(s == "--no-backpressure")
			opt.backpressure = 0;
		else if((s == "--help") || (s == "-h"))
			return false;
		else
		{
			printf("Unrecognised argument '%s'\n\n", s.c_str());
			return false;
		}
	}

	return true;
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

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(Severity::NOTICE));

	//false: do not skip GLFW. We need a WSI-capable device, and one device serves both the
	//compute pipeline and the window.
	if(!VulkanInit(false))
	{
		LogError("Failed to initialize Vulkan\n");
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

	int rc = 1;

	//Scoped so that everything holding a Vulkan object is destroyed before
	//ScopehalStaticCleanup(). DESIGN.md section 17 records the teardown fault this prevents.
	{
		auto queue = g_vkQueueManager->GetQueueFromPool(
			QueueManager::QUEUE_POOL_RENDER, "gr4-analyzer.render");
		Gr4AnalyzerWindow window(queue);

		//Published before the graph is built, so that a block's first draw() finds it. Nothing
		//reads it until then - construction happens on whatever thread emplaceBlock runs on,
		//and touches no GPU state.
		imcufosphor::globalRenderHost().Publish(
			window.GetTextureManager(), queue, window.GetDpiScale());

		using TSample = complex<float>;
		using TSink = gr::imcufosphor::AnalyzerSink<TSample>;

		gr::Graph fg;

		auto& sink = fg.emplaceBlock<TSink>({
			{"name", "analyzer"},
			{"fft_size", gr::Size_t{opt.fftSize}},
			{"block_size", gr::Size_t{opt.blockSize}},
			{"group_size", gr::Size_t{opt.groupSize}},

			//Only used until the stream tags a rate and a centre frequency. A SigMF recording
			//tags both; the signal generator and SoapyRx tag neither, so for those these are
			//the whole story.
			{"sample_rate", static_cast<float>(opt.sampleRate)},
			{"center_frequency", opt.centerHz},

			{"db_min", opt.dbMin},
			{"db_max", opt.dbMax},

			//On for a recording, off for anything live. A file has no realtime constraint, so
			//pacing it by the display costs nothing and means the whole capture is analysed
			//rather than whatever fraction the display happened to catch. A radio is the
			//opposite: throttling it turns dropped frames into receiver overflows.
			{"backpressure", (opt.backpressure >= 0)
				? (opt.backpressure != 0)
				: (opt.source == SourceKind::SigMF)},
		});

		bool connected = false;
		switch(opt.source)
		{
			case SourceKind::Synthetic:
			{
				//The quickest demonstration that the whole path works, and the one that needs
				//nothing on disk and nothing attached.
				auto& src = fg.emplaceBlock<gr::blocks::basic::SignalGenerator<TSample>>({
					{"name", "tone"},
					{"sample_rate", static_cast<float>(opt.sampleRate)},
					{"signal_type", "Sin"},
					{"frequency", static_cast<float>(opt.toneHz)},
					{"amplitude", 1.0f},
				});

				LogNotice("Synthetic source: %.6g Hz tone at %.6g S/s, axis centred on %.6g Hz\n",
					opt.toneHz, opt.sampleRate, opt.centerHz);
				connected = fg.connect<"out", "in">(src, sink).has_value();
				break;
			}

			case SourceKind::SigMF:
			{
				//cf32_le only, which is all the incubator's SigMF source reads. A ci16 recording
				//needs either a converting source or something else feeding the sink's
				//complex<int16_t> instantiation.
				auto& src = fg.emplaceBlock<gr::incubator::sigmf::SigMFSource<TSample>>({
					{"file_name", opt.file},
					{"repeat", opt.repeat},
				});

				LogNotice("SigMF source: %s\n", opt.file.c_str());
				connected = fg.connect<"out", "in">(src, sink).has_value();
				break;
			}

			case SourceKind::Soapy:
			{
#ifdef GR4_ANALYZER_HAVE_SOAPYSDR
				gr::property_map cfg{
					{"name", std::string("radio")},
					{"device", opt.device},
					{"sample_rate", static_cast<float>(opt.sampleRate)},
					{"center_frequency", opt.centerHz},
					{"gain", opt.gainDb},
					{"channel", gr::Size_t{opt.channel}},

					//The display drains block_size at a time; handing it larger chunks costs
					//nothing and cuts the number of round trips through the port.
					{"max_chunk_size", std::uint32_t{65536}},
				};

				//Left unset rather than passed as empty or zero, so the driver picks its own
				//default instead of being told to use nothing
				if(!opt.deviceArgs.empty())
					cfg["device_args"] = opt.deviceArgs;
				if(!opt.antenna.empty())
					cfg["antenna"] = opt.antenna;
				if(opt.bandwidth > 0)
					cfg["bandwidth"] = opt.bandwidth;

				auto& src = fg.emplaceBlock<gr::incubator::soapysdr::SoapyRx<TSample>>(cfg);

				//Report what was actually asked for. The radio may round the rate and the
				//frequency to what its clocking can produce, and SoapyRx reads the achieved
				//values back into its own settings, so this line is the request rather than
				//the result.
				LogNotice("SoapySDR source: %s%s at %.6g Hz, %.6g S/s, %.1f dB gain\n",
					opt.device.empty() && opt.deviceArgs.empty() ? "(first device found)" : "",
					opt.device.empty() ? opt.deviceArgs.c_str() : opt.device.c_str(),
					opt.centerHz, opt.sampleRate, opt.gainDb);
				connected = fg.connect<"out", "in">(src, sink).has_value();
#else
				LogError("This build has no SoapySDR support. Install libsoapysdr-dev and "
					"reconfigure.\n");
				return 1;
#endif
				break;
			}
		}

		if(!connected)
		{
			LogError("Failed to connect the flowgraph\n");
			return 1;
		}

		gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> sched;
		if(auto r = sched.exchange(std::move(fg)); !r)
		{
			LogError("Scheduler rejected the flowgraph\n");
			return 1;
		}

		//Discover the drawable blocks generically, by category rather than by type, so that a
		//flowgraph with two displays in it needs no change here
		vector<gr::BlockModel*> drawables;
		for(const auto& b : sched.blocks())
		{
			if(b && (b->uiCategory() == gr::UICategory::Content))
				drawables.push_back(b.get());
		}
		LogNotice("Rendering %zu drawable block(s)\n", drawables.size());
		window.SetDrawableBlocks(drawables);

		{
			//Scheduler on its own thread; the main thread owns the window, the Vulkan queue and
			//ImGui for the whole run
			jthread schedThread([&sched]
			{
				if(auto r = sched.runAndWait(); !r)
					LogError("Scheduler stopped with an error\n");
			});

			//Once a second, say what is actually getting through. Without this the only
			//evidence the pipeline is running is the picture, which is no help over a
			//remote session and no help at all when the answer is "nothing is arriving".
			double lastReport = GetTime();
			int64_t frames = 0;

			while(!glfwWindowShouldClose(window.GetWindow()))
			{
				glfwPollEvents();
				window.Render();
				frames++;

				const double now = GetTime();
				if(now - lastReport >= 1.0)
				{
					const double fps = frames / (now - lastReport);
					frames = 0;
					lastReport = now;

					const auto pushed = sink._ring.Pushed();
					const auto dropped = sink._ring.Dropped();

					if(sink._engine)
					{
						const auto& stats = sink._engine->GetStats();
						LogNotice("%5.1f fps  rows %" PRId64 "  drain %zu steps/%.1f ms  "
							"tonemap %.1f ms  frame %.1f ms  buffered %" PRIu64
							"  dropped %" PRIu64 "\n",
							fps, sink._engine->GetRowsPlayed(),
							sink._lastDrainSteps, sink._lastDrainMs, sink._lastToneMapMs,
							window.GetLastRenderMs(), pushed, dropped);
						std::ignore = stats;
					}
					else
					{
						LogNotice("no GPU work yet: buffered %" PRIu64 " dropped %" PRIu64 "\n",
							pushed, dropped);
					}
				}
			}

			//Ask the flowgraph to stop, then let the jthread join at the end of this scope.
			//The render loop has exited, so nothing is calling draw() any more.
			std::ignore = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
		}

		//Scheduler joined. The blocks still exist and still hold GPU resources, and this is the
		//render thread, so this is the one place they can be released correctly.
		sink.ReleaseGpuResources();

		//Before the window dies, so no draw() can run against a half-destroyed window
		imcufosphor::globalRenderHost().Retract();

		rc = 0;
	}

	//Everything Vulkan is gone; safe to tear down the process-wide state
	ScopehalStaticCleanup();
	return rc;
}
