/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of ScopeArea
 */

#include "ScopeArea.h"

#include <scopehal/NamedDebugRange.h>

#include <algorithm>
#include <cmath>

using namespace std;

namespace
{

/**
	@brief Trace colours, in I/Q pairs

	Port p takes entries 2p and 2p+1, so a complex port's I and Q are neighbouring shades of one
	hue and a real port simply takes the first of its pair. Relating the pair by hue is the whole
	point: on an overlaid plot the first question is always which lines belong together.

	Starts on the yellow/cyan/magenta/green a bench scope uses for channels one to four, so the
	common case looks like the instrument it is imitating.
 */
const ImVec4 g_traceColors[ScopeArea::MAX_TRACES] =
{
	ImVec4(1.00f, 0.90f, 0.20f, 1), ImVec4(1.00f, 0.60f, 0.10f, 1),	//yellow  / amber
	ImVec4(0.30f, 0.85f, 1.00f, 1), ImVec4(0.25f, 0.50f, 1.00f, 1),	//cyan    / blue
	ImVec4(1.00f, 0.40f, 0.85f, 1), ImVec4(0.75f, 0.35f, 1.00f, 1),	//magenta / violet
	ImVec4(0.40f, 0.95f, 0.45f, 1), ImVec4(0.15f, 0.75f, 0.55f, 1),	//green   / teal
	ImVec4(1.00f, 0.45f, 0.40f, 1), ImVec4(0.90f, 0.25f, 0.25f, 1),	//salmon  / red
	ImVec4(0.70f, 0.90f, 0.40f, 1), ImVec4(0.55f, 0.70f, 0.20f, 1),	//lime    / olive
	ImVec4(0.60f, 0.75f, 1.00f, 1), ImVec4(0.45f, 0.55f, 0.85f, 1),	//periwinkle
	ImVec4(1.00f, 0.75f, 0.55f, 1), ImVec4(0.85f, 0.60f, 0.35f, 1),	//peach   / tan
	ImVec4(0.55f, 1.00f, 0.85f, 1), ImVec4(0.35f, 0.80f, 0.70f, 1),
	ImVec4(0.90f, 0.70f, 1.00f, 1), ImVec4(0.70f, 0.50f, 0.90f, 1),
	ImVec4(1.00f, 1.00f, 0.70f, 1), ImVec4(0.85f, 0.85f, 0.45f, 1),
	ImVec4(0.65f, 0.95f, 1.00f, 1), ImVec4(0.40f, 0.75f, 0.85f, 1),
	ImVec4(1.00f, 0.55f, 0.70f, 1), ImVec4(0.85f, 0.35f, 0.55f, 1),
	ImVec4(0.80f, 1.00f, 0.60f, 1), ImVec4(0.60f, 0.85f, 0.40f, 1),
	ImVec4(0.75f, 0.80f, 0.95f, 1), ImVec4(0.55f, 0.60f, 0.80f, 1),
	ImVec4(0.95f, 0.85f, 0.75f, 1), ImVec4(0.80f, 0.70f, 0.60f, 1)
};

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ScopeArea::ScopeArea(ScopeCapture* capture, TextureManager* texmgr)
	: m_capture(capture)
	, m_texmgr(texmgr)
	, m_width(0)
	, m_height(0)
	, m_xAxis(Unit(Unit::UNIT_FS))
	, m_yAxis(Unit(Unit::UNIT_VOLTS))
	, m_fitted(false)
	, m_showGraticule(true)
	, m_interactive(true)
	, m_traceToneMapPipeline("shaders/ScopeTraceToneMap.spv", 1, sizeof(ScopeTraceToneMapArgs), 1)
	, m_traceMask(0)
	, m_traceEnabled(0xffffffff)
	, m_selectedTrace(0)
	, m_ganged(true)
	, m_traceAlpha(0.75f)
	, m_persistDecay(0)
	, m_lastXOffset(0)
	, m_lastXPixelsPerUnit(0)
{
}

ScopeArea::~ScopeArea()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Trace bookkeeping

size_t ScopeArea::GetTraceCount() const
{
	if(m_capture == nullptr)
		return 0;
	return min(m_capture->GetTraceCount(), MAX_TRACES);
}

const char* ScopeArea::GetTraceName(size_t i) const
{
	if((m_capture == nullptr) || (i >= GetTraceCount()))
		return "";
	return m_capture->GetTrace(i).name.c_str();
}

ImVec4 ScopeArea::GetTraceColor(size_t i) const
{
	return (i < MAX_TRACES) ? g_traceColors[i] : ImVec4(1, 1, 1, 1);
}

void ScopeArea::EnsureTraceState()
{
	const size_t n = GetTraceCount();
	if(m_voltsPerDiv.size() == n)
		return;

	//Grown rather than reassigned, so a trace count change does not throw away the scaling the
	//user set on the traces that already existed.
	m_voltsPerDiv.resize(n, 0.25f);
	m_traceOffset.resize(n, 0.0f);
	m_rasterStale.resize(n, true);
	m_lastVoltsPerDiv.resize(n, 0.0f);
	m_lastOffset.resize(n, 0.0f);
	m_lastRevision.resize(n, 0);

	//New entries are null; the rasters themselves are allocated by EnsureRaster on first use
	m_traceRaster.resize(n);

	if(m_selectedTrace >= n)
		m_selectedTrace = (n > 0) ? (n - 1) : 0;
}

void ScopeArea::SetTraceVisible(size_t i, bool visible)
{
	if(i >= MAX_TRACES)
		return;

	if(visible)
		m_traceEnabled |= (1u << i);
	else
		m_traceEnabled &= ~(1u << i);
}

float ScopeArea::GetVoltsPerDiv(size_t i) const
{
	return (i < m_voltsPerDiv.size()) ? m_voltsPerDiv[i] : 0.25f;
}

void ScopeArea::SetVoltsPerDiv(size_t i, float v)
{
	//Sized here rather than trusting a caller to have drawn a frame first. ApplyConfiguration()
	//runs before the first ToneMap(), so without this the block's volts_per_div setting landed on
	//an empty vector and was silently discarded - every trace came up at the 0.25 default however
	//the flowgraph was configured.
	EnsureTraceState();

	if((i < m_voltsPerDiv.size()) && (v > 0))
		m_voltsPerDiv[i] = v;
}

void ScopeArea::ApplyVoltsPerDiv(float v)
{
	EnsureTraceState();

	if(m_ganged)
	{
		for(size_t i = 0; i < m_voltsPerDiv.size(); i++)
			SetVoltsPerDiv(i, v);
	}
	else
		SetVoltsPerDiv(m_selectedTrace, v);
}

void ScopeArea::ScaleVoltsPerDiv(float factor)
{
	EnsureTraceState();

	if(m_ganged)
	{
		for(size_t i = 0; i < m_voltsPerDiv.size(); i++)
			SetVoltsPerDiv(i, m_voltsPerDiv[i] * factor);
	}
	else
		SetVoltsPerDiv(m_selectedTrace, GetVoltsPerDiv(m_selectedTrace) * factor);
}

void ScopeArea::NudgeTraceOffset(float deltaVolts)
{
	EnsureTraceState();

	if(m_ganged)
	{
		for(size_t i = 0; i < m_traceOffset.size(); i++)
			m_traceOffset[i] += deltaVolts;
	}
	else
		SetTraceOffset(m_selectedTrace, GetTraceOffset(m_selectedTrace) + deltaVolts);
}

float ScopeArea::GetTraceOffset(size_t i) const
{
	return (i < m_traceOffset.size()) ? m_traceOffset[i] : 0.0f;
}

void ScopeArea::SetTraceOffset(size_t i, float v)
{
	EnsureTraceState();

	if(i < m_traceOffset.size())
		m_traceOffset[i] = v;
}

void ScopeArea::ClearPersistence()
{
	for(size_t i = 0; i < m_rasterStale.size(); i++)
		m_rasterStale[i] = true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sizing

bool ScopeArea::AllocateTexture(shared_ptr<Texture>& tex, uint32_t w, uint32_t h, const string& name)
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

bool ScopeArea::EnsureRaster(size_t i)
{
	if(i >= m_traceRaster.size())
		return false;

	if(m_traceRaster[i] == nullptr)
	{
		m_traceRaster[i] = make_unique<AcceleratorBuffer<float>>();
		m_rasterStale[i] = true;
	}

	const size_t want = static_cast<size_t>(m_width) * m_height;
	if(m_traceRaster[i]->size() != want)
	{
		m_traceRaster[i]->resize(want);

		//Fresh memory holds whatever was in it. Blending against that accumulates garbage rather
		//than a previous trace.
		m_rasterStale[i] = true;
	}

	return (want > 0);
}

bool ScopeArea::UpdateSize(ImVec2 size)
{
	uint32_t x = static_cast<uint32_t>(size.x);
	uint32_t y = static_cast<uint32_t>(size.y);

	if( (x == 0) || (y == 0) )
		return false;
	if( (x == m_width) && (y == m_height) )
		return false;

	//The rasterizer keeps a whole pane column in shared memory, so it silently draws nothing
	//above its limit (waveform-compute.glsl:46,213). Clamp and say so rather than presenting an
	//empty pane.
	const uint32_t maxHeight = 2048;
	if(y > maxHeight)
	{
		LogWarning("ScopeArea: pane is %u px tall, clamping traces to the rasterizer's %u limit\n",
			y, maxHeight);
		y = maxHeight;
	}

	m_width = x;
	m_height = y;

	LogTrace("ScopeArea resized to %u x %u\n", x, y);

	if(!AllocateTexture(m_traceTexture, m_width, m_height, "ScopeArea.m_traceTexture"))
		return false;

	//Existing rasters are resized lazily by EnsureRaster; the ones never allocated stay that way
	for(size_t i = 0; i < m_traceRaster.size(); i++)
	{
		if(m_traceRaster[i] != nullptr)
			EnsureRaster(i);
	}

	ClearPersistence();
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Axes

void ScopeArea::FitXAxis(float widthPixels)
{
	double lo = 0;
	double hi = 0;
	if((m_capture == nullptr) || !m_capture->GetRecordRange(lo, hi))
		return;
	if((widthPixels <= 0) || (hi <= lo))
		return;

	m_xAxis.FitRange(lo, hi, widthPixels);
	m_fitted = true;
}

void ScopeArea::UpdateYAxis(float heightPixels)
{
	//The vertical ruler describes one trace, because a single overlaid plot has one gutter and as
	//many vertical scales as it has traces. Derived from that trace's own scaling every frame, so
	//the ruler and the raster cannot disagree about where a volt is.
	const float vpd = GetVoltsPerDiv(m_selectedTrace);
	const float offset = GetTraceOffset(m_selectedTrace);

	const float fullScale = vpd * DIVISIONS_Y;
	if((fullScale <= 0) || (heightPixels <= 0))
		return;

	m_yAxis.SetPixelsPerUnit(heightPixels / fullScale);
	m_yAxis.SetOffset(offset - fullScale * 0.5f);
}

void ScopeArea::HandleMouse(ImVec2 plotPos, ImVec2 plotSize)
{
	if(!m_interactive)
		return;
	if(!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		return;

	auto& io = ImGui::GetIO();
	ImVec2 mouse = io.MousePos;
	if( (mouse.x < plotPos.x) || (mouse.x > plotPos.x + plotSize.x) ||
		(mouse.y < plotPos.y) || (mouse.y > plotPos.y + plotSize.y) )
	{
		return;
	}

	if(io.MouseWheel != 0)
	{
		//Ctrl is the vertical modifier throughout: wheel and drag both act on the time axis
		//without it and on the selected trace's amplitude with it. One modifier rather than two
		//different gestures, because the two axes mean different things and mixing them up in a
		//single drag is how a scope loses the trace off the top of the screen.
		if(io.KeyCtrl)
		{
			const float step = (io.MouseWheel > 0) ? (1.0f / 1.2f) : 1.2f;
			ScaleVoltsPerDiv(step);
		}
		else
		{
			double factor = pow(1.2, io.MouseWheel);
			m_xAxis.ZoomAbout(factor, mouse.x - plotPos.x);
		}
	}

	if(ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);

		if(io.KeyCtrl)
		{
			if(delta.y != 0)
			{
				//Dragging down moves the trace down, which means lowering the voltage at the
				//centre of the pane
				const double perPixel = m_yAxis.PixelsToUnits(1);
				NudgeTraceOffset(static_cast<float>(delta.y * perPixel));
				ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
			}
		}
		else if(delta.x != 0)
		{
			m_xAxis.PanPixels(-delta.x);
			ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rasterization

shared_ptr<ComputePipeline> ScopeArea::GetTracePipeline()
{
	//Created on first use, not in the constructor: the variant depends on g_hasShaderInt64, which
	//VulkanInit sets. Same selection SpectrumArea and WaveformArea both make.
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

void ScopeArea::RasterizeTrace(vk::raii::CommandBuffer& cmdBuf, size_t i, UniformAnalogWaveform* data)
{
	auto pipe = GetTracePipeline();

	pipe->BindBufferNonblocking(0, *m_traceRaster[i], cmdBuf, true);
	pipe->BindBufferNonblocking(1, data->m_samples, cmdBuf);

	const size_t nsamples = data->size();
	if(data->m_timescale <= 0)
		return;

	int64_t xAxisOffset = llround(m_xAxis.GetOffset());
	double pixelsPerX = m_xAxis.GetPixelsPerUnit();
	if(pixelsPerX <= 0)
		return;

	//The x mapping, identical to SpectrumArea::RasterizeTrace and therefore to
	//WaveformArea.cpp:2421-2551. The split into an int64 sample count and a float remainder
	//matters more here than there: a record's trigger phase is a large negative number, the whole
	//pre-trigger duration, which a float cannot hold to anywhere near a sample.
	int64_t innerxoff = xAxisOffset / data->m_timescale;
	int64_t fractionalOffset = xAxisOffset % data->m_timescale;
	int64_t offsetSamples = (xAxisOffset - data->m_triggerPhase) / data->m_timescale;
	int64_t triggerPhaseSamples = data->m_triggerPhase / data->m_timescale;
	int64_t fractionalTriggerPhase = data->m_triggerPhase % data->m_timescale;
	innerxoff -= triggerPhaseSamples;

	//Eight divisions of pane height is full scale, so the rasterizer's yscale follows directly
	//from volts per division and its yoff is the trace's vertical position.
	const float fullScale = GetVoltsPerDiv(i) * DIVISIONS_Y;
	if(fullScale <= 0)
		return;

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
	config.memDepth = nsamples;

	//Deliberately allowed to wrap. offsetSamples goes negative whenever the view is panned left
	//of the record, and the shader adds it in 32-bit modular arithmetic: the sum comes out right
	//for every column whose true sample index is non-negative, and enormous (so the column is
	//left blank) for the ones before the record starts. Clamping it to zero would break panning.
	config.offset_samples = static_cast<uint32_t>(offsetSamples);

	config.alpha = alphaScaled;
	config.xoff = (fractionalTriggerPhase - fractionalOffset) * pixelsPerX;
	config.xscale = xscale;
	config.ybase = m_height * 0.5f;
	config.yscale = m_height / fullScale;
	config.yoff = -GetTraceOffset(i);

	//A persistScale of zero overwrites rather than blends, and the shader writes every pixel of
	//its column, so one pass with it off is a complete clear. No separate buffer fill is needed.
	config.persistScale = m_rasterStale[i] ? 0 : m_persistDecay;
	m_rasterStale[i] = false;

	//Normalise the sub-sample x offset to be non-positive.
	//
	//The shader picks the first sample of each column as floor(col/xscale) + offset_samples
	//(waveform-compute.glsl:234), which inverts
	//
	//    screenX(i) = (i + innerXoff)*xscale + xoff
	//
	//while ignoring the xoff term. A positive xoff shifts every sample rightward, so the sample
	//that actually covers a column has a LOWER index than that estimate - and since the
	//workgroup's 128 threads all start there and count upward, no thread ever visits it. The
	//column draws nothing, and the trace comes out as disconnected fragments rather than a line.
	//
	//Invisible on the spectrum, which is why SpectrumArea carries the same construction without
	//trouble: at many samples per pixel a column that picks the neighbouring sample draws almost
	//the same thing. A scope is routinely zoomed in past one sample per pixel, where the missed
	//column draws nothing at all. Measured at 2.32 px/sample with xoff = +2.23 px.
	//
	//Moving a whole sample from xoff into innerXoff leaves screenX(i) exactly unchanged and turns
	//the estimate into an underestimate, which the upward scan does cover. The offset is bounded
	//by two samples either way - both fractional terms it is built from are less than one sample -
	//so this runs at most twice.
	for(int guard = 0; (config.xoff > 0) && (config.xscale > 0) && (guard < 4); guard++)
	{
		config.xoff -= config.xscale;
		config.innerXoff += 1;
		config.offset_samples -= 1;
	}

	pipe->Dispatch(cmdBuf, config, m_width, 1, 1);
	m_traceRaster[i]->MarkModifiedFromGpu();
}

void ScopeArea::ToneMap(vk::raii::CommandBuffer& cmdBuf)
{
	EnsureTraceState();

	if((m_capture == nullptr) || (m_traceTexture == nullptr))
		return;

	const size_t ntraces = GetTraceCount();
	if(ntraces == 0)
	{
		m_traceMask = 0;
		return;
	}

	NamedDebugRange debugRange(cmdBuf, "ScopeArea");

	//Notice an axis change before rasterizing anything. Persistence accumulates in screen space,
	//so a moved axis means the pixels already in the raster describe a different time and voltage
	//than the ones about to be drawn; blending the two smears the trace as the view moves.
	//Compared rather than announced, because the axis can be moved by the mouse, a fit or a host.
	if( (m_xAxis.GetOffset() != m_lastXOffset) ||
		(m_xAxis.GetPixelsPerUnit() != m_lastXPixelsPerUnit) )
	{
		ClearPersistence();
	}
	m_lastXOffset = m_xAxis.GetOffset();
	m_lastXPixelsPerUnit = m_xAxis.GetPixelsPerUnit();

	//Vertical scaling is per trace, so it invalidates one raster rather than all of them
	for(size_t i = 0; i < ntraces; i++)
	{
		if( (m_voltsPerDiv[i] != m_lastVoltsPerDiv[i]) || (m_traceOffset[i] != m_lastOffset[i]) )
			m_rasterStale[i] = true;

		m_lastVoltsPerDiv[i] = m_voltsPerDiv[i];
		m_lastOffset[i] = m_traceOffset[i];
	}

	//Rasterize whatever changed, then composite everything visible
	uint32_t mask = 0;
	for(size_t i = 0; i < ntraces; i++)
	{
		if(!GetTraceVisible(i))
			continue;

		auto data = m_capture->GetWaveform(i);
		if( (data == nullptr) || (data->size() < 2) )
			continue;

		if(!EnsureRaster(i))
			continue;

		//A held record is not redrawn. Skipping is not only cheaper: re-rasterizing the same
		//samples every frame with persistence on would blend the trace into itself and it would
		//slowly saturate, which in Normal mode with no trigger is the entire time.
		if(m_rasterStale[i] || (data->m_revision != m_lastRevision[i]))
		{
			RasterizeTrace(cmdBuf, i, data);
			m_lastRevision[i] = data->m_revision;
		}

		mask |= (1u << i);
	}

	//Remember what was composited, so Render() knows whether the texture holds anything. It is
	//stale the moment the last visible trace is turned off.
	m_traceMask = mask;
	if(mask == 0)
		return;

	ComputePipeline::AddComputeMemoryBarrier(cmdBuf);

	//One dispatch per trace, accumulating in the image. See ScopeTraceToneMap.glsl for why this
	//is not the single-pass composite the spectrum uses.
	uint32_t lastVisible = 0;
	for(size_t i = 0; i < ntraces; i++)
	{
		if(mask & (1u << i))
			lastVisible = static_cast<uint32_t>(i);
	}

	bool first = true;
	for(size_t i = 0; i < ntraces; i++)
	{
		if((mask & (1u << i)) == 0)
			continue;

		ScopeTraceToneMapArgs args;
		args.width = m_width;
		args.height = m_height;
		args.flags = (first ? 1u : 0u) | ((i == lastVisible) ? 2u : 0u);
		args.pad = 0;

		const auto c = GetTraceColor(i);
		args.color[0] = c.x;
		args.color[1] = c.y;
		args.color[2] = c.z;
		args.color[3] = 1;

		m_traceToneMapPipeline.BindBufferNonblocking(0, *m_traceRaster[i], cmdBuf);
		m_traceToneMapPipeline.BindStorageImage(
			1,
			**m_texmgr->GetSampler(),
			m_traceTexture->GetView(),
			vk::ImageLayout::eGeneral);

		m_traceToneMapPipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(m_width, 64), m_height);

		//Each pass reads what the previous one wrote at the same pixel
		if(i != lastVisible)
			ComputePipeline::AddComputeMemoryBarrier(cmdBuf);

		first = false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

void ScopeArea::DrawGraticule(ImVec2 pos, ImVec2 size)
{
	auto list = ImGui::GetWindowDrawList();

	const auto major = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.18f));
	const auto minor = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f));

	//Gridlines at the ruler's own ticks rather than at fixed screen divisions. A fixed graticule
	//is what a bench scope draws, but a bench scope's graticule is the axis; here there is a
	//labelled ruler as well, and a grid that did not line up with it would be two different
	//answers to the same question.
	vector<AxisTick> ticks;
	const float fontSize = ImGui::GetFontSize();

	GenerateAxisTicks(m_xAxis, size.x, 6 * fontSize, ticks);
	for(auto& t : ticks)
	{
		const float x = pos.x + static_cast<float>(t.position);
		if((x < pos.x) || (x > pos.x + size.x))
			continue;
		list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), t.major ? major : minor, 1.0f);
	}

	GenerateAxisTicks(m_yAxis, size.y, 2.5 * fontSize, ticks);
	for(auto& t : ticks)
	{
		//The axis increases upward, ImGui's y increases downward
		const float y = pos.y + size.y - static_cast<float>(t.position);
		if((y < pos.y) || (y > pos.y + size.y))
			continue;
		list->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), t.major ? major : minor, 1.0f);
	}
}

void ScopeArea::DrawGroundMarkers(ImVec2 pos, ImVec2 size)
{
	auto list = ImGui::GetWindowDrawList();
	const size_t ntraces = GetTraceCount();
	const float h = ImGui::GetFontSize() * 0.4f;

	for(size_t i = 0; i < ntraces; i++)
	{
		if(!GetTraceVisible(i))
			continue;

		//Where this trace's zero volts lands, which is its own scaling and not the ruler's
		const float fullScale = GetVoltsPerDiv(i) * DIVISIONS_Y;
		if(fullScale <= 0)
			continue;

		const float frac = 0.5f - (0.0f - GetTraceOffset(i)) / fullScale;
		const float y = pos.y + size.y * frac;
		if((y < pos.y) || (y > pos.y + size.y))
			continue;

		//A left pointing wedge on the left edge, in the trace's colour. Without these an overlaid
		//plot gives no way to tell which line is which when they cross.
		const auto color = ImGui::GetColorU32(GetTraceColor(i));
		list->AddTriangleFilled(
			ImVec2(pos.x, y),
			ImVec2(pos.x + h * 1.4f, y - h),
			ImVec2(pos.x + h * 1.4f, y + h),
			color);

		if(i == m_selectedTrace)
			list->AddCircle(ImVec2(pos.x + h * 2.2f, y), h * 0.5f, color, 0, 1.5f);
	}
}

void ScopeArea::Render(ImVec2 size)
{
	EnsureTraceState();

	//Carve the rulers out of the space we were given, so callers can pass the whole content
	//region without doing this themselves. Same layout contract SpectrumArea uses.
	float rulerWidth = GetVerticalRulerWidth();
	float rulerHeight = GetHorizontalRulerHeight();

	ImVec2 plotSize(size.x - rulerWidth, size.y - rulerHeight);
	if( (plotSize.x <= 0) || (plotSize.y <= 0) )
	{
		m_plotRect.valid = false;
		ImGui::Dummy(size);
		return;
	}

	//A reallocated texture holds whatever was in that memory, and ToneMap() already ran this
	//frame against the old size - so there is nothing valid to draw until the next one. Dropping
	//the mask skips one frame rather than showing a pane of garbage during a window resize.
	if(UpdateSize(plotSize))
		m_traceMask = 0;

	//Fit on the first frame with a record. Doing it every frame would fight the user's zoom.
	if(!m_fitted)
		FitXAxis(plotSize.x);

	UpdateYAxis(plotSize.y);

	auto pos = ImGui::GetCursorScreenPos();

	//Mouse first, so a pan takes effect on the frame it happens rather than the next one
	HandleMouse(pos, plotSize);

	m_plotRect.pos = pos;
	m_plotRect.size = plotSize;
	m_plotRect.valid = true;

	if(m_showGraticule)
		DrawGraticule(pos, plotSize);

	//The traces, flipped: raster row 0 is the bottom of the Y axis, ImGui's origin is top left
	if((m_traceTexture != nullptr) && (m_traceMask != 0))
	{
		ImGui::SetCursorScreenPos(pos);
		ImGui::Image(m_traceTexture->GetTexture(), plotSize, ImVec2(0, 1), ImVec2(1, 0));
	}
	else
	{
		ImGui::SetCursorScreenPos(pos);
		ImGui::Dummy(plotSize);
	}

	DrawGroundMarkers(pos, plotSize);

	//Amplitude in the right hand gutter, time underneath. Both ask for a uniform SI prefix: the
	//time axis crosses zero by construction, and a per-value prefix would label the trigger
	//instant "0.000 fs" in the middle of a ruler counting microseconds.
	DrawVerticalRuler(m_yAxis, ImVec2(pos.x + plotSize.x, pos.y),
		ImVec2(rulerWidth, plotSize.y), true);
	DrawHorizontalRuler(m_xAxis, ImVec2(pos.x, pos.y + plotSize.y),
		ImVec2(plotSize.x, rulerHeight), true);

	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(size);
}
