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
#include <gnuradio-4.0/basic/ClockSource.hpp>
#include <gnuradio-4.0/basic/SignalGenerator.hpp>
#include <gnuradio-4.0/sigmf/SigMfSource.hpp>
#ifdef GR4_ANALYZER_HAVE_SOAPYSDR
#include <gnuradio-4.0/soapysdr/SoapyRx.hpp>
#endif

#ifdef GR4_ANALYZER_HAVE_OMNISIG
#include <gnuradio-4.0/omnisig/OmniSIGClassifier.hpp>
#endif

#include <gnuradio-4.0/imcufosphor/AnalyzerSink.hpp>
#include <gnuradio-4.0/imcufosphor/ScopeSink.hpp>

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

	/**
		@brief Show the time domain scope instead of the spectrum and waterfall

		A different display, not an extra pane: the two answer different questions and the scope
		needs none of the FFT machinery in front of it.
	 */
	bool scope = false;

	/**
		@brief Input ports on the scope

		Only the synthetic source can fill more than one, because it is the only one that can be
		instantiated more than once with something different on each. A recording or a radio is
		one stream, and fanning it out to several ports would draw the same trace twice.
	 */
	uint32_t scopeChannels = 1;

	uint32_t recordLength = 16384;
	float triggerLevel = 0.0f;
	string triggerMode = "auto";
	string triggerSlope = "rising";
	string triggerOperator = "raw";
	float pretrigger = 0.5f;
	float voltsPerDiv = 0.25f;

	/**
		@brief Insert the OmniSIG classifier between the source and the display

		Its annotations arrive at the sink as ordinary stream tags, so the overlay draws them by the
		same path it draws a recording's stored ones - there is no display-side plumbing for this.
	 */
	bool omnisig = false;

	///@brief Custom .ds model for the classifier. Empty selects the built-in one.
	string omnisigModel;

	///@brief Compute device for the classifier: "" (auto), "cpu", "cuda:0"
	string omnisigDevices;

	///@brief Drop detections below this confidence
	float omnisigConfidence = 0.0f;

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
	printf("Time domain scope, instead of the spectrum and waterfall:\n");
	printf("  --scope           show a triggered oscilloscope plot. Each complex port draws two\n");
	printf("                    traces, I and Q, overlaid on one graticule.\n");
	printf("  --scope-channels N  input ports, default 1. Only the synthetic source can fill\n");
	printf("                    more than one; it generates a different tone on each.\n");
	printf("  --record N        samples per acquisition, default 16384\n");
	printf("  --pretrigger F    fraction of the record before the trigger, default 0.5\n");
	printf("  --trigger MODE    stop | auto | normal | single, default auto\n");
	printf("  --trigger-level L threshold in input units, default 0\n");
	printf("  --trigger-slope S rising | falling | any, default rising\n");
	printf("  --trigger-on WHAT raw | magnitude. 'magnitude' triggers on |I+jQ|, which is what\n");
	printf("                    catches the start of a burst rather than a carrier zero crossing.\n");
	printf("  --volts-per-div V full scale is eight divisions, default 0.25\n");
	printf("\n");
	printf("                    In the pane: drag and wheel pan and zoom the time axis;\n");
	printf("                    ctrl+drag and ctrl+wheel move and scale vertically. The channel\n");
	printf("                    selector says what the vertical controls act on - 'all' by\n");
	printf("                    default, or one trace when two channels differ in amplitude.\n");
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
	printf("Classification:\n");
	printf("  --omnisig         run the OmniSIG classifier on the stream. Its detections arrive at\n");
	printf("                    the display as annotation tags and are drawn over the waterfall,\n");
	printf("                    the same way a recording's stored annotations are.\n");
	printf("  --omnisig-model PATH   a custom .ds model; omit for the built-in one\n");
	printf("  --omnisig-device SPEC  '' (auto), 'cpu', 'cuda:0'\n");
	printf("  --omnisig-confidence C drop detections below C, default 0\n");
	printf("\n");
	printf("                    The classifier works a whole frame at a time and the frame scales\n");
	printf("                    with sample rate - 131072 samples at 40 MS/s, 802816 at 245.76 - so\n");
	printf("                    a recording shorter than one frame produces no detections at all.\n");
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
		else if(s == "--omnisig")
			opt.omnisig = true;
		else if((s == "--omnisig-model") && (i + 1 < argc))
		{
			opt.omnisigModel = argv[++i];
			opt.omnisig = true;
		}
		else if((s == "--omnisig-device") && (i + 1 < argc))
		{
			opt.omnisigDevices = argv[++i];
			opt.omnisig = true;
		}
		else if((s == "--omnisig-confidence") && (i + 1 < argc))
		{
			opt.omnisigConfidence = stof(argv[++i]);
			opt.omnisig = true;
		}
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
		else if(s == "--scope")
			opt.scope = true;
		else if((s == "--scope-channels") && (i + 1 < argc))
		{
			opt.scopeChannels = stoul(argv[++i]);
			opt.scope = true;
		}
		else if((s == "--record") && (i + 1 < argc))
		{
			opt.recordLength = stoul(argv[++i]);
			opt.scope = true;
		}
		else if((s == "--pretrigger") && (i + 1 < argc))
		{
			opt.pretrigger = stof(argv[++i]);
			opt.scope = true;
		}
		else if((s == "--trigger") && (i + 1 < argc))
		{
			opt.triggerMode = argv[++i];
			opt.scope = true;
		}
		else if((s == "--trigger-level") && (i + 1 < argc))
		{
			opt.triggerLevel = stof(argv[++i]);
			opt.scope = true;
		}
		else if((s == "--trigger-slope") && (i + 1 < argc))
		{
			opt.triggerSlope = argv[++i];
			opt.scope = true;
		}
		else if((s == "--trigger-on") && (i + 1 < argc))
		{
			opt.triggerOperator = argv[++i];
			opt.scope = true;
		}
		else if((s == "--volts-per-div") && (i + 1 < argc))
		{
			opt.voltsPerDiv = stof(argv[++i]);
			opt.scope = true;
		}
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
		using TScopeSink = gr::imcufosphor::ScopeSink<TSample>;

		gr::Graph fg;

		//Exactly one of these is built. Pointers rather than a variant because everything below
		//only ever asks "which one is it" twice, at connect and at teardown.
		TSink* sink = nullptr;
		TScopeSink* scopeSink = nullptr;

		if(opt.scope)
		{
			scopeSink = &fg.emplaceBlock<TScopeSink>({
				{"name", "scope"},
				{"n_inputs", gr::Size_t{opt.scopeChannels}},
				{"record_length", gr::Size_t{opt.recordLength}},
				{"pretrigger", opt.pretrigger},

				//Only used until the stream tags a rate. A SigMF recording does; the generator
				//and SoapyRx do not, so for those this is the whole time base.
				{"sample_rate", static_cast<float>(opt.sampleRate)},

				{"trigger_mode", opt.triggerMode},
				{"trigger_slope", opt.triggerSlope},
				{"trigger_operator", opt.triggerOperator},
				{"trigger_level", opt.triggerLevel},
				{"volts_per_div", opt.voltsPerDiv},
			});
		}
		else
		{
		sink = &fg.emplaceBlock<TSink>({
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
		}

		if(opt.scope && opt.omnisig)
		{
			LogError("--omnisig annotates a spectrum display; it has nothing to draw on a scope\n");
			return 1;
		}

		//Only the synthetic source can be instantiated once per port with something different on
		//each. Fanning a recording or a radio out to several ports would draw the same trace N
		//times, which is not multi-channel, it is one channel drawn wrong.
		if(opt.scope && (opt.scopeChannels > 1) && (opt.source != SourceKind::Synthetic))
		{
			LogError("--scope-channels above 1 needs the synthetic source; a recording or a radio "
				"is one stream\n");
			return 1;
		}

#ifdef GR4_ANALYZER_HAVE_OMNISIG
		//Built before the source so that whichever branch below runs has something to connect to.
		//emplaceBlock returns a reference into the graph's shared_ptr storage, so the address is
		//stable across the move into the scheduler.
		gr::omnisig::OmniSIGClassifier* classifier = nullptr;
		if(opt.omnisig)
		{
			gr::property_map cfg{
				{"name", std::string("omnisig")},
				{"model_path", opt.omnisigModel},
				{"devices", opt.omnisigDevices},
				{"confidence_threshold", opt.omnisigConfidence},

				//Blocking, deliberately, even for a live radio. Opportunistic mode exists so a
				//classifier cannot stall a receiver, but it needs a paced source and a delay ring
				//deep enough to outlast an inference, and neither is worth guessing at from here.
				{"mode", std::string("blocking")},
			};

			//A SigMF recording tags its own rate and folds its centre frequency into the capture
			//tag, and the classifier adopts both. The generator and SoapyRx tag neither, so for
			//those these are the only way it learns them - and without a rate it builds no engine
			//and classifies nothing.
			if(opt.source != SourceKind::SigMF)
			{
				cfg["sample_rate"] = static_cast<float>(opt.sampleRate);
				cfg["frequency"] = opt.centerHz;
			}

			classifier = &fg.emplaceBlock<gr::omnisig::OmniSIGClassifier>(cfg);
			LogNotice("OmniSIG classifier enabled%s%s\n",
				opt.omnisigModel.empty() ? "" : ", model ",
				opt.omnisigModel.empty() ? "" : opt.omnisigModel.c_str());
		}
#else
		if(opt.omnisig)
		{
			LogError("This build has no gr-omnisig support. Configure with "
				"-DGR_OMNISIG_DIR=<gr-omnisig-install>/lib/cmake/gr_omnisig and rebuild.\n");
			return 1;
		}
#endif

		//The display is the end of the chain either way; only what sits in front of it changes
		auto connectToDisplay = [&](auto& source, size_t port = 0)
		{
			//The scope's ports are a std::vector, and the compile-time connect<> is a hard error
			//on those (Graph.hpp:594-596). The runtime form with a "name#index" spelling is the
			//only way to reach one.
			if(scopeSink != nullptr)
			{
				return fg.connect(source, "out", *scopeSink, "in#" + std::to_string(port))
					.has_value();
			}

#ifdef GR4_ANALYZER_HAVE_OMNISIG
			if(classifier != nullptr)
			{
				return fg.connect<"out", "in">(source, *classifier).has_value()
					&& fg.connect<"out", "in">(*classifier, *sink).has_value();
			}
#endif
			return fg.connect<"out", "in">(source, *sink).has_value();
		};

		bool connected = false;
		switch(opt.source)
		{
			case SourceKind::Synthetic:
			{
				//The quickest demonstration that the whole path works, and the one that needs
				//nothing on disk and nothing attached.
				//
				//One generator per scope port, each an octave above the last, so that a
				//multi-channel scope shows channels that are visibly different and visibly
				//related - which is what makes a misalignment between them obvious rather than
				//plausible. Every other consumer takes exactly one.
				const size_t nports = (scopeSink != nullptr) ? opt.scopeChannels : 1;
				connected = true;

				//Two things here are not cosmetic.
				//
				//FastSin rather than Sin. ToneGenerator computes Sin as sin(omega * _currentTime)
				//where _currentTime is a float accumulated one tick at a time
				//(algorithm/signal/ToneGenerator.hpp:225). Once it reaches about 1.5 s the float
				//epsilon there (1.19e-7) exceeds the tick, the accumulator stops advancing, and
				//every subsequent sample is identical. Measured: the first duplicate lands at
				//sample 15,067,501 at any rate, which at 10 MS/s is a second and a half.
				//
				//A spectrum display survives that - a frozen tone is still a tone. A scope does
				//not: a run of equal samples contains no level crossing at all, so the trigger
				//correctly never fires and the display looks hung. FastSin advances a recursive
				//phasor instead, renormalised every 65536 samples, and never degenerates:
				//zero duplicates in 40 M samples at 40 MS/s.
				//
				//And a ClockSource, which is how gnuradio4 intends a SignalGenerator to be driven
				//(basic/test/qa_sources.cpp:181-186). Unclocked it free-runs as fast as the
				//scheduler will call it - a few million samples a second in three- to five-sample
				//chunks, of which the display then discards 99.5%. Clocked, the stream actually
				//arrives at --rate, which is the only way the time axis means anything.
				const gr::Size_t chunk = 8192;

				for(size_t i = 0; i < nports; i++)
				{
					const double tone = opt.toneHz * static_cast<double>(1u << i);

					auto& clk = fg.emplaceBlock<gr::blocks::basic::ClockSource<std::uint8_t>>({
						{"name", "clock" + std::to_string(i)},
						{"sample_rate", static_cast<float>(opt.sampleRate)},
						{"chunk_size", chunk},

						//Zero is unlimited. The default is 1024, which would stop the graph after
						//a millisecond and look exactly like a crash.
						{"n_samples_max", gr::Size_t{0}},
					});

					auto& src = fg.emplaceBlock<gr::blocks::basic::SignalGenerator<TSample>>({
						{"name", "tone" + std::to_string(i)},
						{"sample_rate", static_cast<float>(opt.sampleRate)},
						{"chunk_size", chunk},
						{"signal_type", "FastSin"},
						{"frequency", static_cast<float>(tone)},
						{"amplitude", 1.0f / static_cast<float>(i + 1)},
					});

					if(!fg.connect<"out", "clk_in">(clk, src).has_value())
					{
						connected = false;
						break;
					}

					LogNotice("Synthetic source %zu: %.6g Hz tone at %.6g S/s (clocked), "
						"axis centred on %.6g Hz\n", i, tone, opt.sampleRate, opt.centerHz);

					if(!connectToDisplay(src, i))
					{
						connected = false;
						break;
					}
				}
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
				connected = connectToDisplay(src);
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
				connected = connectToDisplay(src);
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

					if(scopeSink != nullptr)
					{
						const auto accepted = scopeSink->_accepted.load();
						const auto dropped = scopeSink->_dropped.load();

						if(scopeSink->_capture)
						{
							const auto& trig = scopeSink->_capture->GetTriggerEngine();
							LogNotice("%5.1f fps  %s  sweeps %" PRIu64 "  missed %" PRIu64
								"  frame %.1f ms  accepted %" PRIu64 "  dropped %" PRIu64 "\n",
								fps, trig.GetStateText(), trig.GetCaptureCount(),
								trig.GetMissedCount(), window.GetLastRenderMs(), accepted, dropped);
						}
						else
						{
							LogNotice("no GPU work yet: accepted %" PRIu64 " dropped %" PRIu64 "\n",
								accepted, dropped);
						}
					}
					else
					{
					const auto pushed = sink->_ring.Pushed();
					const auto dropped = sink->_ring.Dropped();

					if(sink->_engine)
					{
						const auto& stats = sink->_engine->GetStats();
						LogNotice("%5.1f fps  rows %" PRId64 "  drain %zu steps/%.1f ms  "
							"tonemap %.1f ms  frame %.1f ms  buffered %" PRIu64
							"  dropped %" PRIu64 "\n",
							fps, sink->_engine->GetRowsPlayed(),
							sink->_lastDrainSteps, sink->_lastDrainMs, sink->_lastToneMapMs,
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
			}

			//Ask the flowgraph to stop, then let the jthread join at the end of this scope.
			//The render loop has exited, so nothing is calling draw() any more.
			std::ignore = sched.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
		}

		//Scheduler joined. The blocks still exist and still hold GPU resources, and this is the
		//render thread, so this is the one place they can be released correctly.
		if(scopeSink != nullptr)
			scopeSink->ReleaseGpuResources();
		else
			sink->ReleaseGpuResources();

		//Before the window dies, so no draw() can run against a half-destroyed window
		imcufosphor::globalRenderHost().Retract();

		rc = 0;
	}

	//Everything Vulkan is gone; safe to tear down the process-wide state
	ScopehalStaticCleanup();
	return rc;
}
