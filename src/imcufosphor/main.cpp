/**
	@file
	@brief Program entry point for sigmf-spectrum

	A realtime spectrum analyzer and waterfall display for SigMF IQ recordings. Takes one
	recording and opens a window on it; everything else is adjusted in the UI.

	There is deliberately no command line surface beyond that. The flags this once had were
	either developer conveniences for reproducing a display state in a screenshot, or a test
	suite bolted onto the shipped binary. The tests moved to tools/wfbench, which is where a
	question about correctness belongs. See DESIGN.md for the architecture.
 */

#include "../../lib/scopehal/scopehal/scopehal.h"
#include "../../lib/scopehal/scopeprotocols/scopeprotocols.h"

#include "ComplexFFTFilter.h"
#include "MainWindow.h"
#include "SigMFSource.h"
#include "SpectrumDensity.h"
#include "SpectrumReducer.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Defaults
//
// Every one of these is adjustable in the UI, so they are starting points rather than
// policy. The block size is the one that matters for throughput: it decides how many
// transforms are batched into each submit (DESIGN.md section 13).

///@brief Transform length, which sets the resolution bandwidth
static const uint64_t g_defaultFFTLength = 8192;

///@brief Samples per acquisition
static const uint64_t g_defaultBlockSize = 1048576;

///@brief Spectra combined into each waterfall row
static const int g_defaultGroupSize = 1;

///@brief Rows to play before the first frame, so the waterfall opens with history
static const int g_defaultPrefill = 400;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GUI

/**
	@brief Opens a recording and runs the application window until it is closed

	@param path		Path to the .sigmf-meta file

	@return True on clean exit
 */
static bool RunGui(const string& path)
{
	SigMFSource source(path);
	if(!source.IsValid())
	{
		LogError("could not open %s: %s\n", path.c_str(), source.GetErrorMessage().c_str());
		return false;
	}

	Unit hz(Unit::UNIT_HZ);
	LogNotice("Playing %s: %s at %s, %" PRId64 " samples\n",
		source.GetName().c_str(),
		hz.PrettyPrint(source.GetRecordingSampleRate()).c_str(),
		hz.PrettyPrint(source.GetExactCenterFrequency()).c_str(),
		source.GetTotalSamples());

	shared_ptr<QueueHandle> queue(g_vkQueueManager->GetRenderQueue("MainWindow.render"));
	MainWindow window(queue, &source);
	window.GetSession()->SetFFTLength(g_defaultFFTLength);
	window.GetSession()->SetBlockSize(g_defaultBlockSize);
	window.GetSession()->SetGroupSize(g_defaultGroupSize);

	//Render one frame before prefilling.
	//
	//Waterfall's ring buffer defaults to a single row (Waterfall.cpp:56) and only gets its
	//real height when WaterfallArea::UpdateSize() runs, which happens inside RenderUI().
	//Prefilling first would push every row into a one-row buffer and then lose the lot when
	//the pane resized.
	window.Render();

	//Fill the waterfall so it opens with history rather than a blank pane
	for(int i=0; i<g_defaultPrefill; i++)
		window.Step();


	int64_t frames = 0;
	double t0 = GetTime();
	while(!glfwWindowShouldClose(window.GetWindow()))
	{
		glfwPollEvents();
		window.Render();
		frames++;
		//Frame rate report, so a stall or a slow path is obvious without attaching a profiler
		if(frames % 600 == 0)
			LogDebug("%" PRId64 " frames in %.1f s (%.1f fps)\n", frames, GetTime() - t0, frames / (GetTime() - t0));
	}

	return true;
}

int main(int argc, char* argv[])
{
	//One positional argument, no options.
	if(argc != 2)
	{
		fprintf(stderr,
			"sigmf-spectrum: realtime spectrum analyzer and waterfall for SigMF IQ recordings\n"
			"Usage: sigmf-spectrum FILE.sigmf-meta\n");
		return 1;
	}
	string path(argv[1]);

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(Severity::NOTICE));

	//Bring up Vulkan. Do not skip GLFW: we need a window system integration capable device
	//so the compute and render paths share one device.
	if(!VulkanInit(false))
	{
		LogError("Vulkan initialization failed\n");
		return 1;
	}

	//Register transports, drivers and filters
	TransportStaticInit();
	DriverStaticInit();
	ScopeProtocolStaticInit();
	InitializePlugins();

	//Our own filters. scopeprotocols is consumed unmodified (DESIGN.md D3), so anything
	//app-local registers here rather than in ScopeProtocolStaticInit().
	AddDecoderClass(ComplexFFTFilter);
	AddDecoderClass(SpectrumReducer);
	AddDecoderClass(SpectrumDensity);

	bool ok = RunGui(path);

	//Tear down the Vulkan globals explicitly. Required, not optional: PipelineCacheManager's
	//destructor writes the shader cache and logs while doing it, and if it runs as a static
	//destructor it can outlive g_log_sinks and segfault on the way out.
	ScopehalStaticCleanup();

	return ok ? 0 : 1;
}
