/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SpectrumArea
 */

#include "SpectrumArea.h"

#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SpectrumArea::SpectrumArea(SpectrumDensity* density, TextureManager* texmgr, const string& colorRamp)
	: m_density(density)
	, m_texmgr(texmgr)
	, m_colorRamp(colorRamp)
	, m_width(0)
	, m_height(0)
	, m_xAxis(std::make_shared<PlotAxis>(Unit(Unit::UNIT_MICROHZ)))
	, m_yAxis(Unit(Unit::UNIT_DBM))
	, m_showXAxis(true)
	, m_xAxisFitted(false)
	, m_ownsXAxis(true)
	, m_densityToneMapPipeline("shaders/SpectrumDensityToneMap.spv", 1, sizeof(SpectrumDensityToneMapArgs), 1, 1)
	, m_densityVisible(true)
	, m_densityFloor(0.04f)
	, m_densityGain(1.5f)
	, m_traceToneMapPipeline("shaders/SpectrumTraceToneMap.spv", 5, sizeof(SpectrumTraceToneMapArgs), 1)
	, m_traceMask(0)
	, m_traceAlpha(0.75f)
	, m_persistDecay(0)
	, m_lastXOffset(0)
	, m_lastXPixelsPerUnit(0)
	, m_lastYRange(0)
	, m_lastYOffset(0)
{
	//Only the summary traces are on by default. With all five up the display is unreadable,
	//and median plus the 10th and 95th percentile is the set that actually says what the
	//signal is doing: a middle and an envelope.
	for(size_t i=0; i<NUM_TRACES; i++)
	{
		m_traceVisible[i] = false;
		m_rasterStale[i] = true;
	}
	m_traceVisible[SpectrumDensity::STREAM_MEDIAN - SpectrumDensity::STREAM_MEAN] = true;
	m_traceVisible[SpectrumDensity::STREAM_LOW - SpectrumDensity::STREAM_MEAN] = true;
	m_traceVisible[SpectrumDensity::STREAM_HIGH - SpectrumDensity::STREAM_MEAN] = true;

	//Chosen to stay legible over the density map's colour ramp rather than to be pretty:
	//the ramp runs dark blue to yellow, so the traces are white, grey and warm neutrals.
	m_traceColors[0] = ImVec4(0.60f, 0.85f, 1.00f, 1);	//mean, pale blue
	m_traceColors[1] = ImVec4(1.00f, 1.00f, 1.00f, 1);	//median, white
	m_traceColors[2] = ImVec4(0.55f, 0.55f, 0.60f, 1);	//low, grey
	m_traceColors[3] = ImVec4(1.00f, 0.75f, 0.35f, 1);	//mid, amber
	m_traceColors[4] = ImVec4(1.00f, 0.41f, 0.02f, 1);	//high, Orange

	for(size_t i=0; i<NUM_TRACES; i++)
	{
		m_traceRaster[i].SetCpuAccessHint(AcceleratorBuffer<float>::HINT_NEVER);
		m_traceRaster[i].SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	}
}

SpectrumArea::~SpectrumArea()
{
}

const char* SpectrumArea::GetTraceName(size_t i)
{
	static const char* names[NUM_TRACES] = { "Mean", "Median", "Low %", "Mid %", "High %" };
	return (i < NUM_TRACES) ? names[i] : "";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Axes

bool SpectrumArea::GetDataRange(double& lo, double& hi)
{
	auto data = dynamic_cast<UniformAnalogWaveform*>(
		m_density->GetData(SpectrumDensity::STREAM_MEAN));
	if( (data == nullptr) || (data->size() < 2) || (data->m_timescale <= 0) )
		return false;

	lo = data->m_triggerPhase;
	hi = lo + static_cast<double>(data->size()) * data->m_timescale;
	return true;
}

bool SpectrumArea::FitXAxis(float widthPixels)
{
	double lo = 0;
	double hi = 0;
	if(!GetDataRange(lo, hi))
		return false;

	return m_xAxis->FitRange(lo, hi, widthPixels);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sizing

bool SpectrumArea::AllocateTexture(shared_ptr<Texture>& tex, uint32_t w, uint32_t h, const string& name)
{
	if( (w == 0) || (h == 0) )
		return false;

	vk::ImageCreateInfo imageInfo(
		{},
		vk::ImageType::e2D,
		vk::Format::eR32G32B32A32Sfloat,
		vk::Extent3D(w, h, 1),
		1,
		1,
		vk::SampleCountFlagBits::e1,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::SharingMode::eExclusive,
		{},
		vk::ImageLayout::eUndefined);

	tex = make_shared<Texture>(*g_vkComputeDevice, imageInfo, m_texmgr, name);

	//The shaders write the image as eGeneral, so move it out of eUndefined before first use
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
			tex->GetImage(),
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

bool SpectrumArea::UpdateSize(ImVec2 size)
{
	uint32_t x = static_cast<uint32_t>(size.x);
	uint32_t y = static_cast<uint32_t>(size.y);

	if( (x == 0) || (y == 0) )
		return false;
	if( (x == m_width) && (y == m_height) )
		return false;

	//The rasterizer keeps a whole pane column in shared memory, so it silently draws nothing
	//above its limit (waveform-compute.glsl:46,213). Clamp and say so rather than presenting
	//an empty pane.
	const uint32_t maxHeight = 2048;
	if(y > maxHeight)
	{
		LogWarning("SpectrumArea: pane is %u px tall, clamping traces to the rasterizer's %u limit\n",
			y, maxHeight);
		y = maxHeight;
	}

	m_width = x;
	m_height = y;

	LogTrace("SpectrumArea resized to %u x %u\n", x, y);

	//The density texture is as tall as the map has cells, not as tall as the pane
	if(!AllocateTexture(m_densityTexture, m_width,
		static_cast<uint32_t>(m_density->GetDensityCells()), "SpectrumArea.m_densityTexture"))
	{
		return false;
	}

	if(!AllocateTexture(m_traceTexture, m_width, m_height, "SpectrumArea.m_traceTexture"))
		return false;

	for(size_t i=0; i<NUM_TRACES; i++)
		m_traceRaster[i].resize(m_width * m_height);

	//Fresh buffers hold whatever was in that memory. Blending against it would accumulate
	//garbage rather than a previous trace.
	ClearPersistence();

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

shared_ptr<ComputePipeline> SpectrumArea::GetTracePipeline()
{
	//Created on first use, not in the constructor: the variant depends on g_hasShaderInt64,
	//which VulkanInit sets. Same selection WaveformArea.h:246-260 makes.
	if(m_tracePipeline == nullptr)
	{
		string path = "shaders/waveform-compute.analog";
		if(g_hasShaderInt64)
			path += ".int64";
		path += ".dense.spv";

		m_tracePipeline = make_shared<ComputePipeline>(path, 2, sizeof(ConfigPushConstants));
	}

	return m_tracePipeline;
}

void SpectrumArea::RasterizeTrace(vk::raii::CommandBuffer& cmdBuf, size_t i, UniformAnalogWaveform* data)
{
	auto pipe = GetTracePipeline();

	pipe->BindBufferNonblocking(0, m_traceRaster[i], cmdBuf, true);
	pipe->BindBufferNonblocking(1, data->m_samples, cmdBuf);

	size_t nbins = data->size();
	if(data->m_timescale <= 0)
		return;

	//Pan and zoom come from the axis, which may be shared with the waterfall
	int64_t xAxisOffset = llround(m_xAxis->GetOffset());
	double pixelsPerX = m_xAxis->GetPixelsPerUnit();
	if(pixelsPerX <= 0)
		return;

	//The x mapping, following WaveformArea.cpp:2421-2551 exactly.
	//
	//FetchX() returns the sample index plus innerXoff (waveform-compute.glsl:163), so
	//innerXoff is the pan position measured from the first sample, not the absolute
	//frequency of that sample. Passing the trigger phase itself puts sample zero tens of
	//thousands of pixels off the right edge and nothing draws at all.
	//
	//The split into an int64 sample count and a float remainder is not optional at these
	//spans: the trigger phase of a 245.76 MHz recording is 2.3e15 microhertz, which a float
	//cannot hold to anywhere near a bin.
	int64_t innerxoff = xAxisOffset / data->m_timescale;
	int64_t fractionalOffset = xAxisOffset % data->m_timescale;
	int64_t offsetSamples = (xAxisOffset - data->m_triggerPhase) / data->m_timescale;
	int64_t triggerPhaseSamples = data->m_triggerPhase / data->m_timescale;
	int64_t fractionalTriggerPhase = data->m_triggerPhase % data->m_timescale;
	innerxoff -= triggerPhaseSamples;

	//Amplitude. The rasterizer measures y from the middle of the pane, so ybase is the
	//centre and yoff shifts the stream's midpoint onto it.
	float range = m_density->GetVoltageRange(SpectrumDensity::STREAM_MEAN);
	if(range <= 0)
		range = 1;

	//Alpha falls with the number of samples per pixel, so a trace does not saturate simply
	//because it is zoomed out. Same curve as WaveformArea.cpp:2540-2543.
	double xscale = data->m_timescale * pixelsPerX;
	double samplesPerPixel = (xscale > 0) ? (1.0 / xscale) : 1;
	float alphaScaled = m_traceAlpha / sqrt(samplesPerPixel);
	alphaScaled = min(1.0f, alphaScaled) * 2;

	ConfigPushConstants config;
	config.innerXoff = -innerxoff;
	config.windowHeight = m_height;
	config.windowWidth = m_width;
	config.memDepth = nbins;
	config.offset_samples = offsetSamples;
	config.alpha = alphaScaled;
	config.xoff = (fractionalTriggerPhase - fractionalOffset) * pixelsPerX;
	config.xscale = xscale;
	config.ybase = m_height * 0.5f;
	config.yscale = m_height / range;
	config.yoff = m_density->GetOffset(SpectrumDensity::STREAM_MEAN);

	//A persistScale of zero overwrites rather than blends, and the shader writes every pixel
	//of its column (waveform-compute.glsl:396-405), so one pass with it off is a complete
	//clear. No separate buffer fill is needed.
	config.persistScale = m_rasterStale[i] ? 0 : m_persistDecay;
	m_rasterStale[i] = false;

	pipe->Dispatch(cmdBuf, config, m_width, 1, 1);
	m_traceRaster[i].MarkModifiedFromGpu();
}

void SpectrumArea::ToneMap(vk::raii::CommandBuffer& cmdBuf)
{
	if( (m_densityTexture == nullptr) || (m_traceTexture == nullptr) )
		return;

	NamedDebugRange debugRange(cmdBuf, "SpectrumArea");

	//Density map
	if(m_densityVisible)
	{
		auto data = dynamic_cast<DensityFunctionWaveform*>(
			m_density->GetData(SpectrumDensity::STREAM_DENSITY));
		if(data != nullptr)
		{
			auto width = data->GetWidth();
			auto height = data->GetHeight();
			if( (width > 0) && (height > 0) )
			{
				m_densityToneMapPipeline.BindBufferNonblocking(0, data->GetOutData(), cmdBuf);
				m_densityToneMapPipeline.BindStorageImage(
					1,
					**m_texmgr->GetSampler(),
					m_densityTexture->GetView(),
					vk::ImageLayout::eGeneral);
				m_densityToneMapPipeline.BindSampledImage(
					2,
					**m_texmgr->GetSampler(),
					m_texmgr->GetView(m_colorRamp),
					vk::ImageLayout::eShaderReadOnlyOptimal);

				SpectrumDensityToneMapArgs args;
				args.width = width;
				args.height = height;
				args.outwidth = m_width;

				//Same frequency axis the traces use, expressed in bins. The shader takes the
				//max over the bins covering each pixel, so a narrow signal survives being
				//zoomed out.
				double timescale = data->m_timescale;
				double pixelsPerX = m_xAxis->GetPixelsPerUnit();
				if( (timescale <= 0) || (pixelsPerX <= 0) )
					return;

				args.binOffset = (m_xAxis->GetOffset() - data->m_triggerPhase) / timescale;
				args.xscale = 1.0 / (pixelsPerX * timescale);
				args.floorLevel = m_densityFloor;
				args.gain = m_densityGain;

				m_densityToneMapPipeline.Dispatch(cmdBuf, args,
					GetComputeBlockCount(m_width, 64), height);
			}
		}
	}

	//Notice an axis change before rasterizing anything.
	//
	//Persistence accumulates in screen space, so a moved axis means the pixels already in the
	//raster describe a different frequency and amplitude than the ones about to be drawn.
	//Blending the two smears the trace sideways as the view moves - the artifact is obvious
	//while dragging and invisible in a static screenshot, which is why the axes are compared
	//rather than trusted.
	float yRange = m_density->GetVoltageRange(SpectrumDensity::STREAM_MEAN);
	float yOffset = m_density->GetOffset(SpectrumDensity::STREAM_MEAN);
	if( (m_xAxis->GetOffset() != m_lastXOffset) ||
		(m_xAxis->GetPixelsPerUnit() != m_lastXPixelsPerUnit) ||
		(yRange != m_lastYRange) ||
		(yOffset != m_lastYOffset) )
	{
		ClearPersistence();
	}
	m_lastXOffset = m_xAxis->GetOffset();
	m_lastXPixelsPerUnit = m_xAxis->GetPixelsPerUnit();
	m_lastYRange = yRange;
	m_lastYOffset = yOffset;

	//Traces. Rasterize each visible one, then composite them all in a single pass.
	uint32_t mask = 0;
	for(size_t i=0; i<NUM_TRACES; i++)
	{
		if(!m_traceVisible[i])
			continue;

		auto data = dynamic_cast<UniformAnalogWaveform*>(
			m_density->GetData(SpectrumDensity::STREAM_MEAN + i));
		if( (data == nullptr) || (data->size() < 2) )
			continue;

		RasterizeTrace(cmdBuf, i, data);
		mask |= (1u << i);
	}

	//Remember what was composited, so Render() knows whether the texture holds anything. It
	//is stale the moment the last visible trace is turned off: the composite below would
	//write transparent pixels, but skipping it entirely is cheaper, and drawing a texture
	//nothing wrote this frame is what left the last-unchecked trace on screen.
	m_traceMask = mask;
	if(mask == 0)
		return;

	ComputePipeline::AddComputeMemoryBarrier(cmdBuf);

	SpectrumTraceToneMapArgs targs;
	targs.width = m_width;
	targs.height = m_height;
	targs.mask = mask;
	targs.pad = 0;
	for(size_t i=0; i<NUM_TRACES; i++)
	{
		targs.colors[i][0] = m_traceColors[i].x;
		targs.colors[i][1] = m_traceColors[i].y;
		targs.colors[i][2] = m_traceColors[i].z;
		targs.colors[i][3] = 1;
	}

	//Every raster is bound whether or not its trace is visible: the descriptor set wants all
	//five populated, and the shader skips the hidden ones by mask.
	for(size_t i=0; i<NUM_TRACES; i++)
		m_traceToneMapPipeline.BindBufferNonblocking(i, m_traceRaster[i], cmdBuf);
	m_traceToneMapPipeline.BindStorageImage(
		5,
		**m_texmgr->GetSampler(),
		m_traceTexture->GetView(),
		vk::ImageLayout::eGeneral);

	m_traceToneMapPipeline.Dispatch(cmdBuf, targs, GetComputeBlockCount(m_width, 64), m_height);
}

void SpectrumArea::Render(ImVec2 size)
{
	//Carve the rulers out of the space we were given, so callers can pass the whole content
	//region without doing this themselves. The amplitude ruler sits in a right hand gutter,
	//which is where WaveformGroup puts its Y axis (WaveformGroup.cpp:250-251).
	float rulerWidth = GetVerticalRulerWidth();
	float rulerHeight = m_showXAxis ? GetHorizontalRulerHeight() : 0;

	ImVec2 plotSize(size.x - rulerWidth, size.y - rulerHeight);
	if( (plotSize.x <= 0) || (plotSize.y <= 0) )
	{
		m_plotRect.valid = false;
		ImGui::Dummy(size);
		return;
	}

	UpdateSize(plotSize);

	//Fit on the first frame that has data, and never again: refitting every frame would
	//undo the user's pan and zoom
	if(m_ownsXAxis && !m_xAxisFitted)
		m_xAxisFitted = FitXAxis(plotSize.x);

	//The amplitude axis mirrors what the rasterizer was given, so the ruler cannot disagree
	//with the trace it labels
	float range = m_density->GetVoltageRange(SpectrumDensity::STREAM_MEAN);
	float offset = m_density->GetOffset(SpectrumDensity::STREAM_MEAN);
	m_yAxis.FitRange(-offset - range/2, -offset + range/2, plotSize.y);

	auto pos = ImGui::GetCursorScreenPos();

	if( (m_densityTexture == nullptr) || (m_traceTexture == nullptr) )
	{
		m_plotRect.valid = false;
		ImGui::Dummy(size);
		return;
	}

	//Report what was actually drawn into, so an overlay never has to redo the ruler arithmetic
	m_plotRect.pos = pos;
	m_plotRect.size = plotSize;
	m_plotRect.valid = true;

	//Both layers are drawn flipped vertically: cell zero and pane row zero are the bottom of
	//the amplitude axis, and ImGui's origin is the top.
	if(m_densityVisible)
		ImGui::Image(m_densityTexture->GetTexture(), plotSize, ImVec2(0, 1), ImVec2(1, 0));
	else
		ImGui::Dummy(plotSize);

	//Traces go over the density map rather than beside it
	if(m_traceMask != 0)
	{
		ImGui::SetCursorScreenPos(pos);
		ImGui::Image(m_traceTexture->GetTexture(), plotSize, ImVec2(0, 1), ImVec2(1, 0));
	}

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
