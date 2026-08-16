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

AnalyzerPane::AnalyzerPane(PlayerSession* session, TextureManager* texmgr, const string& colorRamp)
	: m_session(session)
	, m_xAxis(make_shared<PlotAxis>(Unit(Unit::UNIT_MICROHZ)))
	, m_spectrumFraction(0.35f)
	, m_fitted(false)
{
	m_spectrumArea = make_unique<SpectrumArea>(session->GetDensity(), texmgr, colorRamp);
	m_waterfallArea = make_unique<WaterfallArea>(session->GetWaterfall(), texmgr, colorRamp);

	//One axis, two areas. From here the two cannot disagree about what frequency is under a
	//given column, because there is only one set of numbers.
	m_spectrumArea->SetXAxis(m_xAxis);
	m_waterfallArea->SetXAxis(m_xAxis);

	//We draw the shared ruler once, between the two plots
	m_spectrumArea->SetShowXAxis(false);
	m_waterfallArea->SetShowXAxis(false);

	//Only the waterfall needs the timebase; the spectrum's vertical axis is amplitude
	m_waterfallArea->SetTimebase(
		&session->GetRowHistory(), session->GetSource()->GetRecordingSampleRate());
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

	//The shared ruler and both Y gutters come out of the total before the plots are split
	float plotWidth = size.x - rulerWidth;
	float plotsHeight = size.y - rulerHeight;
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

	//Mouse first, so a pan takes effect on the frame it happens rather than the next one
	HandleMouse(pos, ImVec2(plotWidth, plotsHeight));
	ClampXAxis(plotWidth);

	//Spectrum on top. Both areas are handed the full width including their gutter; each
	//carves its own out, and since both reserve the same width the plots line up.
	if(specHeight > 0)
		m_spectrumArea->Render(ImVec2(size.x, specHeight));

	if(fallHeight > 0)
		m_waterfallArea->Render(ImVec2(size.x, fallHeight));

	//One frequency ruler for both, at the bottom
	DrawHorizontalRuler(
		*m_xAxis,
		ImVec2(pos.x, pos.y + plotsHeight),
		ImVec2(plotWidth, rulerHeight));

	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(size);
}
