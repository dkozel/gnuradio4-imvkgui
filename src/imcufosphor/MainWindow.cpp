/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of MainWindow
 */

#include "MainWindow.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

MainWindow::MainWindow(shared_ptr<QueueHandle> queue, SigMFSource* source)
	: VulkanWindow("sigmf-spectrum", queue, false, false)
	, m_running(true)
	, m_stepBudgetMs(10)
	, m_measuredRowRate(0)
	, m_measuredFrameRate(0)
	, m_lastRateSample(0)
	, m_lastRateRows(0)
	, m_lastRateFrames(0)
	, m_frames(0)
	, m_msStep(0)
	, m_msToneMap(0)
	, m_msPresent(0)
	, m_accStep(0)
	, m_accToneMap(0)
	, m_accPresent(0)
{
	m_texmgr = make_unique<TextureManager>(queue);

	//Colour ramps live in icons/gradients and are copied next to the binary at build time
	m_texmgr->LoadTexture("eye-gradient-viridis", FindDataFile("icons/gradients/eye-gradient-viridis.png"));

	m_session = make_unique<PlayerSession>(source, queue);

	m_pane = make_unique<AnalyzerPane>(m_session.get(), m_texmgr.get(), "eye-gradient-viridis");

	//Our own command buffer, because the tone map dispatch cannot go inside the render pass
	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		queue->GetQueue()->m_family);
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);
	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuf = make_unique<vk::raii::CommandBuffer>(
		std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	m_percentiles[0] = 10;
	m_percentiles[1] = 60;
	m_percentiles[2] = 95;

	m_lastRateSample = GetTime();
}

MainWindow::~MainWindow()
{
	//Destroy the views before the filters they point at
	m_pane.reset();
	m_session.reset();
	m_texmgr.reset();
}

float MainWindow::GetDpiScale()
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
// Playback

bool MainWindow::Step()
{
	//One acquisition block, not one waterfall row. A row can span many blocks at a large
	//group size, and the caller's per-frame time budget needs a unit of work it can check
	//between.
	return m_session->StepOneBlock() != PlayerSession::STEP_FAILED;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void MainWindow::Render()
{
	double t0 = GetTime();

	//Advance playback before drawing, so this frame shows the newest row.
	//
	//Several rows per frame, not one: the frame is dominated by vsync, and one row per frame
	//would cap playback at the refresh rate while the GPU sits idle. Always steps at least
	//once so a slow graph cannot stall the display.
	if(m_running)
	{
		double deadline = t0 + m_stepBudgetMs / 1000.0;
		do
		{
			if(!Step())
				break;
		}
		while(GetTime() < deadline);
	}

	double t1 = GetTime();

	//Tone map outside the render pass. VulkanWindow calls DoRender() after beginRenderPass
	//(VulkanWindow.cpp:691-697), and compute cannot be dispatched inside a render pass, so
	//this has to happen here rather than in an override of DoRender().
	//Both panes in one command buffer. They read different filters' outputs and write
	//different textures, so no barrier is needed between them.
	if(m_pane->GetWaterfallArea()->GetTexture() != nullptr)
	{
		m_cmdBuf->begin({});
		m_pane->ToneMap(*m_cmdBuf);
		m_cmdBuf->end();
		m_renderQueue->SubmitAndBlock(*m_cmdBuf);
	}

	double t2 = GetTime();

	VulkanWindow::Render();

	double t3 = GetTime();

	//Attribute the frame. Without this it is impossible to tell whether the display is
	//limited by the filter graph, the tone map or presentation, and optimising the wrong
	//one is easy.
	m_frames++;
	m_accStep += (t1 - t0) * 1000;
	m_accToneMap += (t2 - t1) * 1000;
	m_accPresent += (t3 - t2) * 1000;

	double dt = t3 - m_lastRateSample;
	if(dt >= 1.0)
	{
		int64_t nframes = m_frames - m_lastRateFrames;
		m_measuredRowRate = (m_session->GetRowsPlayed() - m_lastRateRows) / dt;
		m_measuredFrameRate = nframes / dt;

		if(nframes > 0)
		{
			m_msStep = m_accStep / nframes;
			m_msToneMap = m_accToneMap / nframes;
			m_msPresent = m_accPresent / nframes;
		}
		m_accStep = 0;
		m_accToneMap = 0;
		m_accPresent = 0;

		m_lastRateSample = t3;
		m_lastRateRows = m_session->GetRowsPlayed();
		m_lastRateFrames = m_frames;
	}
}

void MainWindow::RenderControls()
{
	auto source = m_session->GetSource();
	Unit hz(Unit::UNIT_HZ);

	if(ImGui::Button(m_running ? "Pause" : "Play"))
		m_running = !m_running;

	ImGui::SameLine();
	if(ImGui::Button("Restart"))
		m_session->Restart();

	ImGui::SameLine();
	if(ImGui::Button("Step") && !m_running)
		Step();

	//How much of each frame may go to playback. Raising it trades UI responsiveness for
	//throughput; the display still updates every frame either way.
	ImGui::SameLine();
	ImGui::SetNextItemWidth(130 * GetDpiScale());
	float budget = m_stepBudgetMs;
	if(ImGui::SliderFloat("ms/frame", &budget, 1, 100, "%.0f"))
		m_stepBudgetMs = budget;

	//Transform length. Changing it changes RBW without touching the line rate, which is the
	//whole point of the reducer sitting between the FFT and the waterfall.
	static const int64_t lengths[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110 * GetDpiScale());
	if(ImGui::BeginCombo("FFT", to_string(m_session->GetFFTLength()).c_str()))
	{
		for(size_t i=0; i<sizeof(lengths)/sizeof(lengths[0]); i++)
		{
			bool sel = (lengths[i] == m_session->GetFFTLength());
			if(ImGui::Selectable(to_string(lengths[i]).c_str(), sel))
				m_session->SetFFTLength(lengths[i]);
		}
		ImGui::EndCombo();
	}

	//Acquisition block size. Larger blocks mean more transforms per submit and so more
	//throughput, at the cost of coarser playback granularity. See DESIGN.md section 13.
	static const int64_t blocks[] = {8192, 65536, 262144, 1048576, 4194304};
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120 * GetDpiScale());
	if(ImGui::BeginCombo("Block", to_string(m_session->GetBlockSize()).c_str()))
	{
		for(size_t i=0; i<sizeof(blocks)/sizeof(blocks[0]); i++)
		{
			bool sel = (blocks[i] == m_session->GetBlockSize());
			if(ImGui::Selectable(to_string(blocks[i]).c_str(), sel))
				m_session->SetBlockSize(blocks[i]);
		}
		ImGui::EndCombo();
	}

	//Spectra per waterfall row.
	//
	//Logarithmic, and up to 65536: at 245.76 MS/s a 8192-point transform runs 30 000 times a
	//second, so a readable line rate needs a group in the thousands. The old 1-64 range was
	//sized for a pipeline that could only manage a few thousand transforms per second.
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160 * GetDpiScale());
	int group = m_session->GetGroupSize();
	if(ImGui::SliderInt("Spectra/row", &group, 1, 65536, "%d", ImGuiSliderFlags_Logarithmic))
		m_session->SetGroupSize(group);

	//Colour scale
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200 * GetDpiScale());
	float range[2] = { m_session->GetRangeMin(), m_session->GetRangeMax() };
	if(ImGui::DragFloat2("dBm", range, 1.0f, -160, 60))
	{
		if(range[1] > range[0])
			m_session->SetRange(range[0], range[1]);
	}

	//Status. RBW and row rate are the two numbers that actually describe what is on screen.
	double rbw = (m_session->GetFFTLength() > 0)
		? source->GetRecordingSampleRate() / m_session->GetFFTLength()
		: 0;

	ImGui::Text("%s | %s span at %s | RBW %s | row %" PRId64 " | %.1f rows/s | %.1f fps | coverage %.0f%%",
		source->GetName().c_str(),
		hz.PrettyPrint(source->GetRecordingSampleRate()).c_str(),
		hz.PrettyPrint(source->GetExactCenterFrequency()).c_str(),
		hz.PrettyPrint(rbw).c_str(),
		m_session->GetRowsPlayed(),
		m_measuredRowRate,
		m_measuredFrameRate,
		m_session->GetCoverage() * 100);

	ImGui::SameLine();
	ImGui::Text("| graph %.1f ms  tonemap %.1f ms  present %.1f ms",
		m_msStep, m_msToneMap, m_msPresent);
}

void MainWindow::RenderSpectrumControls()
{
	//Density map on/off, and the split between the two panes
	bool dens = m_pane->GetSpectrumArea()->GetDensityVisible();
	if(ImGui::Checkbox("Density", &dens))
		m_pane->GetSpectrumArea()->SetDensityVisible(dens);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120 * GetDpiScale());
	float frac = m_pane->GetSpectrumFraction();
	if(ImGui::SliderFloat("split", &frac, 0.1f, 0.9f, "%.2f"))
		m_pane->SetSpectrumFraction(frac);

	//Trace visibility, with each label in its own trace's colour so the legend is the control
	for(size_t i=0; i<SpectrumArea::NUM_TRACES; i++)
	{
		ImGui::SameLine();
		bool vis = m_pane->GetSpectrumArea()->GetTraceVisible(i);
		ImGui::PushStyleColor(ImGuiCol_Text, m_pane->GetSpectrumArea()->GetTraceColor(i));
		if(ImGui::Checkbox(SpectrumArea::GetTraceName(i), &vis))
			m_pane->GetSpectrumArea()->SetTraceVisible(i, vis);
		ImGui::PopStyleColor();
	}

	//The three configurable percentiles. Median is fixed at 50% and the mean is not a
	//percentile at all, so only three of the five traces have a number to set.
	auto density = m_session->GetDensity();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200 * GetDpiScale());
	float pct[3] = { m_percentiles[0], m_percentiles[1], m_percentiles[2] };
	if(ImGui::DragFloat3("pct", pct, 0.5f, 0, 100, "%.0f%%"))
	{
		for(int i=0; i<3; i++)
			m_percentiles[i] = max(0.0f, min(100.0f, pct[i]));
		density->SetPercentiles(
			m_percentiles[0] / 100, m_percentiles[1] / 100, m_percentiles[2] / 100);
	}

	//Trace persistence. Cheap to expose and it is the whole reason the traces go through the
	//lifted rasterizer rather than a line renderer of our own (DESIGN.md section 5).
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110 * GetDpiScale());
	float persist = m_pane->GetSpectrumArea()->GetPersistDecay();
	if(ImGui::SliderFloat("persist", &persist, 0, 0.99f, "%.2f"))
		m_pane->GetSpectrumArea()->SetPersistDecay(persist);
}

void MainWindow::RenderUI()
{
	auto viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);

	ImGui::Begin(
		"sigmf-spectrum",
		nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	RenderControls();
	RenderSpectrumControls();

	//Spectrum over waterfall on one shared frequency axis
	auto avail = ImGui::GetContentRegionAvail();
	if( (avail.x > 0) && (avail.y > 0) )
		m_pane->Render(avail);

	ImGui::End();
}
