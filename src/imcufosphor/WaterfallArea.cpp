/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of WaterfallArea
 */

#include "WaterfallArea.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

WaterfallArea::WaterfallArea(Waterfall* waterfall, TextureManager* texmgr, const string& colorRamp)
	: m_waterfall(waterfall)
	, m_texmgr(texmgr)
	, m_colorRamp(colorRamp)
	//1 SSBO (the density buffer), 1 storage image (our texture), 1 sampled image (the ramp).
	//Same construction ngscopeclient uses at WaveformArea.cpp:101.
	, m_toneMapPipeline("shaders/WaterfallToneMap.spv", 1, sizeof(WaterfallToneMapArgs), 1, 1)
	, m_width(0)
	, m_height(0)
	, m_xAxis(make_shared<PlotAxis>(Unit(Unit::UNIT_MICROHZ)))
	, m_yAxis(Unit(Unit::UNIT_FS))
	, m_showXAxis(true)
	, m_xAxisFitted(false)
	, m_ownsXAxis(true)
	, m_rowHistory(nullptr)
	, m_sampleRate(0)
{
}

WaterfallArea::~WaterfallArea()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Axes

bool WaterfallArea::GetDataRange(double& lo, double& hi)
{
	auto data = dynamic_cast<WaterfallWaveform*>(m_waterfall->GetData(0));
	if( (data == nullptr) || (data->GetWidth() < 2) || (data->m_timescale <= 0) )
		return false;

	//The frequency of the first bin has to come from the input.
	//
	//Waterfall::Refresh copies the timescale and the timestamps to its output but never
	//m_triggerPhase (Waterfall.cpp:127-131), so the waterfall waveform's own trigger phase
	//is zero and says nothing about where in the spectrum it sits. Reading it would put the
	//band at DC.
	//GetInputWaveform() is protected on FlowGraphNode, so go through the public descriptor
	auto din = dynamic_cast<UniformAnalogWaveform*>(m_waterfall->GetInput(0).GetData());
	if(din == nullptr)
		return false;

	lo = din->m_triggerPhase;
	hi = lo + static_cast<double>(data->GetWidth()) * data->m_timescale;
	return true;
}

bool WaterfallArea::FitXAxis(float widthPixels)
{
	double lo = 0;
	double hi = 0;
	if(!GetDataRange(lo, hi))
		return false;

	return m_xAxis->FitRange(lo, hi, widthPixels);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sizing

bool WaterfallArea::UpdateSize(ImVec2 size)
{
	uint32_t x = static_cast<uint32_t>(size.x);
	uint32_t y = static_cast<uint32_t>(size.y);

	if( (x == 0) || (y == 0) )
		return false;
	if( (x == m_width) && (y == m_height) )
		return false;

	m_width = x;
	m_height = y;

	//The waterfall keeps one row per acquisition and the shader indexes rows directly, so
	//the ring buffer has to be as tall as the pane. ngscopeclient rounds to a power of two
	//to avoid churn on every pixel of resize (WaveformArea.cpp:227-235); do the same.
	size_t roundedY = pow(2, ceil(log2(y)));
	if(m_waterfall->GetHeight() != roundedY)
	{
		m_waterfall->SetHeight(roundedY);

		//Reallocating the ring buffer discards its contents, so the waterfall has to re-run
		//before anything can be drawn from it
		FilterGraphExecutor ex;
		set<FlowGraphNode*> nodes;
		nodes.emplace(m_waterfall);
		ex.RunBlocking(nodes);
	}

	LogTrace("WaterfallArea resized to %u x %u, reallocating texture\n", x, y);

	vk::ImageCreateInfo imageInfo(
		{},
		vk::ImageType::e2D,
		vk::Format::eR32G32B32A32Sfloat,
		vk::Extent3D(x, y, 1),
		1,
		1,
		vk::SampleCountFlagBits::e1,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::SharingMode::eExclusive,
		{},
		vk::ImageLayout::eUndefined);

	m_texture = make_shared<Texture>(*g_vkComputeDevice, imageInfo, m_texmgr, "WaterfallArea.m_texture");

	//The shader writes the image as eGeneral, so move it out of eUndefined before first use
	{
		lock_guard<mutex> lock(g_vkTransferMutex);
		vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
		vk::ImageMemoryBarrier barrier(
			vk::AccessFlagBits::eNone,
			vk::AccessFlagBits::eShaderWrite,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eGeneral,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			m_texture->GetImage(),
			range);

		g_vkTransferCommandBuffer->begin({});
		g_vkTransferCommandBuffer->pipelineBarrier(
			vk::PipelineStageFlagBits::eTopOfPipe,
			vk::PipelineStageFlagBits::eComputeShader,
			{}, {}, {}, barrier);
		g_vkTransferCommandBuffer->end();
		g_vkTransferQueue->SubmitAndBlock(*g_vkTransferCommandBuffer);
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void WaterfallArea::ToneMap(vk::raii::CommandBuffer& cmdBuf)
{
	if(m_texture == nullptr)
		return;

	auto data = dynamic_cast<WaterfallWaveform*>(m_waterfall->GetData(0));
	if(data == nullptr)
		return;

	auto width = data->GetWidth();
	auto height = data->GetHeight();
	if( (width == 0) || (height == 0) )
		return;

	m_toneMapPipeline.BindBufferNonblocking(0, data->GetOutData(), cmdBuf);
	m_toneMapPipeline.BindStorageImage(
		1,
		**m_texmgr->GetSampler(),
		m_texture->GetView(),
		vk::ImageLayout::eGeneral);
	m_toneMapPipeline.BindSampledImage(
		2,
		**m_texmgr->GetSampler(),
		m_texmgr->GetView(m_colorRamp),
		vk::ImageLayout::eShaderReadOnlyOptimal);

	//Pan and zoom from the frequency axis, which the spectrum may be sharing
	double lo = 0;
	double hi = 0;
	if(!GetDataRange(lo, hi))
		return;

	double binsPerPixel = 1.0 / (m_xAxis->GetPixelsPerUnit() * data->m_timescale);
	double binOffset = (m_xAxis->GetOffset() - lo) / data->m_timescale;
	if( (binsPerPixel <= 0) || !isfinite(binsPerPixel) || !isfinite(binOffset) )
		return;

	//offset_samples is unsigned in the shader (WaterfallToneMap.glsl:29), so a view panned
	//left of the first bin would wrap to four billion and pin the whole row to the last bin.
	//AnalyzerPane clamps the axis to the data so this cannot normally happen; refuse rather
	//than render garbage if something else moves it.
	if(binOffset < 0)
		binOffset = 0;

	WaterfallToneMapArgs args(
		width, height, m_width, m_height,
		static_cast<uint32_t>(binOffset), data->GetWriteRow(), binsPerPixel);
	m_toneMapPipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(m_width, 64), m_height);
}

void WaterfallArea::Render(ImVec2 size)
{
	float rulerWidth = GetVerticalRulerWidth();
	float rulerHeight = m_showXAxis ? GetHorizontalRulerHeight() : 0;

	ImVec2 plotSize(size.x - rulerWidth, size.y - rulerHeight);
	if( (plotSize.x <= 0) || (plotSize.y <= 0) )
	{
		ImGui::Dummy(size);
		return;
	}

	UpdateSize(plotSize);

	if(m_ownsXAxis && !m_xAxisFitted)
		m_xAxisFitted = FitXAxis(plotSize.x);

	//Time axis, from the row history rather than from an assumed line rate.
	//
	//Age increases downward, so the axis runs from zero at the top to the age of the oldest
	//visible row at the bottom, and the ruler is drawn flipped relative to the other panes.
	//The span is measured, not assumed; the interpolation between ticks does assume a steady
	//line rate, which is true whenever the block size and group size are not being changed.
	double spanFs = 0;
	if( (m_rowHistory != nullptr) && (m_sampleRate > 0) )
	{
		int64_t spanSamples = m_rowHistory->GetSampleSpan(static_cast<size_t>(plotSize.y));
		spanFs = spanSamples * (FS_PER_SECOND / m_sampleRate);
	}
	if(spanFs > 0)
	{
		m_yAxis.SetUnit(Unit(Unit::UNIT_FS));
		m_yAxis.FitRange(-spanFs, 0, plotSize.y);
	}
	else
	{
		//No history yet, or no sample rate. Label in rows rather than lying about seconds.
		m_yAxis.SetUnit(Unit(Unit::UNIT_COUNTS));
		m_yAxis.FitRange(-plotSize.y, 0, plotSize.y);
	}

	auto pos = ImGui::GetCursorScreenPos();

	if(m_texture == nullptr)
	{
		ImGui::Dummy(size);
		return;
	}

	//Flipped vertically: the shader writes row 0 as the newest, and ImGui's origin is top
	//left, so sampling v from 1 to 0 puts the newest row at the top
	ImGui::Image(m_texture->GetTexture(), plotSize, ImVec2(0, 1), ImVec2(1, 0));

	DrawVerticalRuler(m_yAxis, ImVec2(pos.x + plotSize.x, pos.y), ImVec2(rulerWidth, plotSize.y));

	if(m_showXAxis)
	{
		DrawHorizontalRuler(
			*m_xAxis, ImVec2(pos.x, pos.y + plotSize.y), ImVec2(plotSize.x, rulerHeight));
	}

	//Claim the whole region so the next widget starts below us
	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(size);
}
