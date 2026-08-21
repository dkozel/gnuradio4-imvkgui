/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of Gr4AnalyzerWindow
 */

#include "Gr4AnalyzerWindow.h"

#include "RenderHost.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

Gr4AnalyzerWindow::Gr4AnalyzerWindow(shared_ptr<QueueHandle> queue)
	: VulkanWindow("gr4-analyzer", queue, false, false)
	, m_queue(queue)
	, m_allDone(false)
{
	m_texmgr = make_unique<TextureManager>(queue);

	//Colour ramps live in icons/gradients and are copied next to the binary at build time by
	//imcufosphor_attach_icons(). Same set the standalone application loads.
	m_texmgr->LoadTexture("eye-gradient-viridis", FindDataFile("icons/gradients/eye-gradient-viridis.png"));

	//The tone map dispatch cannot go inside the render pass, so it needs a command buffer of
	//its own rather than the one VulkanWindow records the frame into
	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		queue->GetQueue()->m_family);
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuf = make_unique<vk::raii::CommandBuffer>(
		std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));
}

Gr4AnalyzerWindow::~Gr4AnalyzerWindow()
{
	//The blocks are not ours to destroy; the scheduler owns them. Dropping the pointers is all
	//that is wanted here, and the host has already had them release their GPU resources.
	m_drawables.clear();
}

float Gr4AnalyzerWindow::GetDpiScale()
{
	float xscale = 1;
	float yscale = 1;
	auto monitor = glfwGetPrimaryMonitor();
	if(monitor)
		glfwGetMonitorContentScale(monitor, &xscale, &yscale);
	if(xscale <= 0)
		xscale = 1;
	return xscale;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void Gr4AnalyzerWindow::Render()
{
	//Lend the blocks a command buffer for the frame. They record and submit their own tone
	//maps through it, one at a time, from inside draw(); see RenderHost::FrameComputeCommandBuffer()
	//for why the submit has to happen there rather than being batched up and issued here.
	imcufosphor::globalRenderHost().BeginFrame(m_cmdBuf.get());

	//ImGui frame, which calls RenderUI() and therefore every block's draw()
	const double t0 = GetTime();
	VulkanWindow::Render();
	m_lastRenderMs = (GetTime() - t0) * 1000.0;

	imcufosphor::globalRenderHost().EndFrame();
}

void Gr4AnalyzerWindow::RenderUI()
{
	//One full-viewport window with the drawable blocks inside it. Each block opens its own
	//ImGui window, so two displays in one flowgraph land side by side without either knowing
	//about the other.
	auto viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);

	bool allDone = !m_drawables.empty();
	for(auto* block : m_drawables)
	{
		if(block->draw({}) != gr::work::Status::DONE)
			allDone = false;
	}
	m_allDone = allDone;
}
