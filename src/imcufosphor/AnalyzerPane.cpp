/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of AnalyzerPane
 */

#include "AnalyzerPane.h"

#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

AnalyzerPane::AnalyzerPane(IAnalyzerSource* source, TextureManager* texmgr, const string& colorRamp)
	: m_source(source)
	, m_annotations(nullptr)
	, m_clock(nullptr)
	, m_xAxis(make_shared<PlotAxis>(Unit(Unit::UNIT_MICROHZ)))
	, m_spectrumFraction(0.35f)
	, m_fitted(false)
{
	m_spectrumArea = make_unique<SpectrumArea>(source->GetDensity(), texmgr, colorRamp);
	m_waterfallArea = make_unique<WaterfallArea>(source->GetWaterfall(), texmgr, colorRamp);

	//One axis, two areas. From here the two cannot disagree about what frequency is under a
	//given column, because there is only one set of numbers.
	m_spectrumArea->SetXAxis(m_xAxis);
	m_waterfallArea->SetXAxis(m_xAxis);

	//Each area draws its own ruler for the shared axis, under the plot it belongs to. They
	//cannot disagree - there is one axis - and either can be turned off on its own.
	m_spectrumArea->SetShowXAxis(true);
	m_waterfallArea->SetShowXAxis(true);

	//Only the waterfall needs the timebase; the spectrum's vertical axis is amplitude. The
	//clock is null until SetAnnotationSource() supplies one, which WaterfallArea already
	//handles: it drops the wall-clock line from the hover readout and keeps the age axis,
	//which is derived from the row history alone.
	m_waterfallArea->SetTimebase(&source->GetRowHistory(), nullptr, source->GetSampleRate());
}

void AnalyzerPane::SetAnnotationSource(const AnnotationSet* annotations, const RecordingClock* clock)
{
	m_annotations = annotations;
	m_clock = clock;

	//Re-push the timebase now that there is a clock to put in it
	m_waterfallArea->SetTimebase(&m_source->GetRowHistory(), clock, m_source->GetSampleRate());
}

AnalyzerPane::~AnalyzerPane()
{
	//Areas hold pointers into the session's filters, so drop them before anything else runs
	m_spectrumArea.reset();
	m_waterfallArea.reset();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Axis

void AnalyzerPane::FitXAxis()
{
	m_fitted = false;
}

void AnalyzerPane::ClampXAxis(float widthPixels)
{
	double lo = 0;
	double hi = 0;
	if(!m_spectrumArea->GetDataRange(lo, hi))
		return;
	if(widthPixels <= 0)
		return;

	//Never zoom out past the whole span. There is nothing beyond the edges of an FFT to look
	//at, and letting the view go wider means the waterfall's unsigned bin offset has to cope
	//with a negative start (WaterfallToneMap.glsl:29).
	double minPixelsPerUnit = widthPixels / (hi - lo);
	if(m_xAxis->GetPixelsPerUnit() < minPixelsPerUnit)
		m_xAxis->SetPixelsPerUnit(minPixelsPerUnit);

	//Keep the view inside the data
	double span = m_xAxis->PixelsToUnits(widthPixels);
	double offset = m_xAxis->GetOffset();
	if(offset < lo)
		offset = lo;
	if(offset + span > hi)
		offset = hi - span;
	m_xAxis->SetOffset(offset);
}

void AnalyzerPane::HandleMouse(ImVec2 plotPos, ImVec2 plotSize)
{
	//Only when the pointer is actually over a plot, so the controls above keep their wheel
	if(!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		return;

	auto& io = ImGui::GetIO();
	ImVec2 mouse = io.MousePos;
	if( (mouse.x < plotPos.x) || (mouse.x > plotPos.x + plotSize.x) ||
		(mouse.y < plotPos.y) || (mouse.y > plotPos.y + plotSize.y) )
	{
		return;
	}

	//Wheel zooms about the cursor, so whatever is being pointed at stays put
	if(io.MouseWheel != 0)
	{
		double factor = pow(1.2, io.MouseWheel);
		m_xAxis->ZoomAbout(factor, mouse.x - plotPos.x);
	}

	//Left drag pans. Dragging right moves the data right, i.e. the view left.
	if(ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
		if(delta.x != 0)
		{
			m_xAxis->PanPixels(-delta.x);
			ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void AnalyzerPane::ToneMap(vk::raii::CommandBuffer& cmdBuf)
{
	m_waterfallArea->ToneMap(cmdBuf);
	m_spectrumArea->ToneMap(cmdBuf);
}

void AnalyzerPane::Render(ImVec2 size)
{
	if( (size.x <= 0) || (size.y <= 0) )
		return;

	float rulerWidth = GetVerticalRulerWidth();
	float rulerHeight = GetHorizontalRulerHeight();

	//Whichever frequency rulers are switched on, plus both Y gutters, come out of the total
	//before the plots are split. Reserving per ruler rather than once for the pair is what
	//keeps the split fraction meaning the same thing whichever ones are showing.
	float specRuler = m_spectrumArea->GetShowXAxis() ? rulerHeight : 0;
	float fallRuler = m_waterfallArea->GetShowXAxis() ? rulerHeight : 0;

	float plotWidth = size.x - rulerWidth;
	float plotsHeight = size.y - specRuler - fallRuler;
	if( (plotWidth <= 0) || (plotsHeight <= 0) )
	{
		ImGui::Dummy(size);
		return;
	}

	float specHeight = plotsHeight * m_spectrumFraction;
	float fallHeight = plotsHeight - specHeight;

	//Fit on the first frame with data. Doing it every frame would fight the user's zoom.
	if(!m_fitted)
		m_fitted = m_spectrumArea->FitXAxis(plotWidth);

	auto pos = ImGui::GetCursorScreenPos();

	//Mouse first, so a pan takes effect on the frame it happens rather than the next one.
	//The rulers are inside the region: dragging on one pans the axis it labels, which is
	//what a ruler under the cursor looks like it should do.
	HandleMouse(pos, ImVec2(plotWidth, plotsHeight + specRuler + fallRuler));
	ClampXAxis(plotWidth);

	//Spectrum on top. Both areas are handed the full width including their gutter and the
	//height of their own ruler; each carves both out, and since both reserve the same width
	//the plots line up.
	if(specHeight > 0)
		m_spectrumArea->Render(ImVec2(size.x, specHeight + specRuler));

	if(fallHeight > 0)
		m_waterfallArea->Render(ImVec2(size.x, fallHeight + fallRuler));

	//Annotations over both, once each area has reported the rectangle it actually drew into.
	//After rather than before, so a box sits on top of the waveform it describes rather than
	//under it; ImGui's draw list is ordered by submission.
	//Nothing to draw without a recording behind the stream: a live flowgraph has no annotation
	//set, and the overlay's whole job is placing intervals from one against the display.
	if(m_overlay.GetEnabled() && (m_annotations != nullptr))
	{
		auto dl = ImGui::GetWindowDrawList();

		m_overlay.DrawOnSpectrum(
			dl,
			m_spectrumArea->GetPlotRect(),
			*m_xAxis,
			*m_annotations,
			m_source->GetCurrentBlock(),
			m_source->GetSampleRate(),
			m_source->GetDensity()->GetDecaySeconds());

		m_overlay.DrawOnWaterfall(
			dl,
			m_waterfallArea->GetPlotRect(),
			*m_xAxis,
			*m_annotations,
			m_source->GetRowHistory());
	}

	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(size);
}
