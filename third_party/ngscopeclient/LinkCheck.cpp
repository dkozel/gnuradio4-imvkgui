/**
	@file
	@brief Proves the lifted display primitives link, not just compile.

	Risk R2 (DESIGN.md section 11) is an empirical question about ngscopeclient.h's include
	graph, and half of that question is the link: ngscopeclient.h declares InstrumentThread,
	WaveformThread, RightJustifiedText, RectIntersect and RectContains, and pulls in
	GuiLogSink, none of which we build. Declarations alone cost nothing, but only a linked
	executable demonstrates that.

	So this target exists to make the linker resolve everything the lifted primitives
	reference, and to instantiate the subclass shape DESIGN.md section 5 assumes
	(VulkanWindow with overridden DoRender / RenderUI).

	Running it does no windowing unless asked. The build machine may have no display, and a
	link check that hangs waiting on a compositor is worse than no link check at all:

	    ngscopeclient-compat-linkcheck            static checks only, always safe, exits immediately
	    ngscopeclient-compat-linkcheck --window   brings up Vulkan and a real window, renders one frame

	See notes/R2-lifted-primitives.md.
 */

#include "VulkanWindow.h"
#include "TextureManager.h"
#include "WindowGeometry.h"

using namespace std;

/**
	@brief The subclass shape the application will use.

	DESIGN.md section 5 calls VulkanWindow "already a subclassable base". This is that claim
	compiled: a derived class overriding both virtuals, with no MainWindow or Session in
	sight.
 */
class LinkCheckWindow : public VulkanWindow
{
public:
	LinkCheckWindow(shared_ptr<QueueHandle> queue)
	: VulkanWindow("ngscopeclient-compat link check", queue, false, false)
	{}

protected:
	virtual void DoRender(vk::raii::CommandBuffer& cmdBuf) override
	{ VulkanWindow::DoRender(cmdBuf); }

	virtual void RenderUI() override
	{ ImGui::ShowMetricsWindow(); }
};

/**
	@brief Actually opens a window. Only reached with --window.
 */
static bool RunWindowSmokeTest()
{
	if(!VulkanInit(false))
	{
		LogError("VulkanInit failed\n");
		return false;
	}

	auto queue = g_vkQueueManager->GetQueueFromPool(
		QueueManager::QUEUE_POOL_RENDER, "ngscopeclient-compat-linkcheck.render");

	TextureManager textures(queue);
	LinkCheckWindow window(queue);
	window.Render();
	WindowGeometry geometry;
	window.SaveWindowPositionAndSize(geometry);

	//Deliberately not calling window.GetContentScale(). VulkanWindow.h:54 declares it but no
	//translation unit in scopehal-apps defines it - ngscopeclient never calls it, so upstream
	//never notices. Calling it from here is an undefined reference at link time. Anything we
	//need it for has to be reimplemented in the subclass.
	LogNotice("Rendered one frame, window is %s\n", window.IsFullscreen() ? "fullscreen" : "windowed");
	return true;
}

int main(int argc, char* argv[])
{
	Severity console_verbosity = Severity::NOTICE;
	bool doWindow = false;

	for(int i=1; i<argc; i++)
	{
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;

		string s(argv[i]);
		if(s == "--window")
			doWindow = true;
		else if(s == "--help")
		{
			printf("ngscopeclient-compat-linkcheck: link check for the lifted ngscopeclient display primitives\n");
			printf("Usage: ngscopeclient-compat-linkcheck [--window] [logger options]\n");
			return 0;
		}
		else
		{
			fprintf(stderr, "Unrecognized command-line argument \"%s\", use --help\n", s.c_str());
			return 1;
		}
	}

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	//Force the linker to resolve the primitives even when we never construct them. Taking
	//addresses of the virtuals is enough and, unlike calling them, needs no Vulkan device.
	void (VulkanWindow::*pRender)() = &VulkanWindow::Render;
	void (TextureManager::*pLoad)(const string&, const string&) = &TextureManager::LoadTexture;
	LogNotice("VulkanWindow::Render and TextureManager::LoadTexture resolved (%d, %d)\n",
		pRender != nullptr, pLoad != nullptr);

		return 1;

	if(doWindow)
	{
		bool ok = RunWindowSmokeTest();

		//Not optional. scopehal's PipelineCacheManager is a static whose destructor calls
		//SaveToDisk(), which logs; by the time the C runtime unwinds statics the log sinks
		//are gone and it segfaults on the way out. ngscopeclient calls this at the end of
		//main.cpp:325 for the same reason. Nothing to do with the lift, but anything that
		//calls VulkanInit() inherits it.
		ScopehalStaticCleanup();

		if(!ok)
			return 1;
	}
	else
		LogNotice("Skipping window smoke test; pass --window to run it\n");

	return 0;
}
