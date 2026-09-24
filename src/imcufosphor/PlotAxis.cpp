/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of PlotAxis and the ruler drawing helpers
 */

#include "PlotAxis.h"

#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PlotAxis

PlotAxis::PlotAxis(Unit unit)
	: m_offset(0)
	, m_pixelsPerUnit(1)
	, m_unit(unit)
{
}

bool PlotAxis::FitRange(double lo, double hi, double lengthPixels)
{
	if( (hi <= lo) || (lengthPixels <= 0) )
		return false;

	m_offset = lo;
	m_pixelsPerUnit = lengthPixels / (hi - lo);
	return true;
}

void PlotAxis::ZoomAbout(double factor, double aboutPixels)
{
	if(factor <= 0)
		return;

	//Value under the fixed point before and after, and shift the offset by the difference.
	//Scaling about the origin instead would slide whatever the user is pointing at out from
	//under the cursor, which makes wheel zoom unusable.
	double before = PositionToUnits(aboutPixels);
	m_pixelsPerUnit *= factor;
	double after = PositionToUnits(aboutPixels);
	m_offset += before - after;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Ticks

/**
	@brief Rounds up to the next 1, 2 or 5 times a power of ten
 */
static double RoundToNiceNumber(double x)
{
	if(x <= 0)
		return 1;

	double decade = pow(10, floor(log10(x)));
	double mantissa = x / decade;

	if(mantissa <= 1)
		return decade;
	if(mantissa <= 2)
		return 2 * decade;
	if(mantissa <= 5)
		return 5 * decade;
	return 10 * decade;
}

void GenerateAxisTicks(
	const PlotAxis& axis,
	double lengthPixels,
	double minLabelSpacing,
	vector<AxisTick>& ticks)
{
	ticks.clear();

	if( (lengthPixels <= 0) || (axis.GetPixelsPerUnit() <= 0) || (minLabelSpacing <= 0) )
		return;

	double major = RoundToNiceNumber(axis.PixelsToUnits(minLabelSpacing));
	if( (major <= 0) || !isfinite(major) )
		return;

	//Five subdivisions unless a 2-decade graduation makes four the natural choice
	int nsub = 5;
	double mantissa = major / pow(10, floor(log10(major)));
	if(fabs(mantissa - 2) < 0.01)
		nsub = 4;
	double minor = major / nsub;

	double lo = axis.GetOffset();
	double hi = axis.GetEnd(lengthPixels);

	//Start one graduation below the visible range so the first minor ticks are not clipped
	double start = floor(lo / major) * major;

	//An axis zoomed absurdly far out would generate millions of ticks and hang the frame.
	//Bail rather than trying to draw them.
	double expected = (hi - lo) / minor;
	if( (expected > 10000) || !isfinite(expected) )
		return;

	for(double v = start; v <= hi + major; v += major)
	{
		for(int i=0; i<nsub; i++)
		{
			double tv = v + i*minor;
			if( (tv < lo) || (tv > hi) )
				continue;

			AxisTick tick;
			tick.value = tv;
			tick.position = axis.UnitsToPosition(tv);
			tick.major = (i == 0);
			ticks.push_back(tick);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

float GetVerticalRulerWidth()
{
	//Same allowance WaveformGroup::GetYAxisWidth() makes for ordinary units
	return 6 * ImGui::GetFontSize();
}

float GetHorizontalRulerHeight()
{
	return 2.5 * ImGui::GetFontSize();
}

float GetHorizontalRulerGap()
{
	return 4;
}

/**
	@brief Spacing between labelled ticks, or zero if fewer than two were generated

	Taken from the ticks rather than recomputed, so a label window derived from it cannot
	disagree with the graduation it is describing.
 */
static double MajorTickStep(const vector<AxisTick>& ticks)
{
	const AxisTick* first = nullptr;
	for(auto& t : ticks)
	{
		if(!t.major)
			continue;
		if(first == nullptr)
			first = &t;
		else
			return fabs(t.value - first->value);
	}
	return 0;
}

/**
	@brief Replaces the micro prefix with an ASCII 'u', in place

	Unit.cpp spells micro as U+03BC, GREEK SMALL LETTER MU (Unit.cpp:320). ImGui's built-in font
	is ProggyClean, which has no glyph for it and draws it as a question mark - so a time ruler
	reads "-200 ?s" rather than "-200 us".

	Substituting here rather than widening the font's glyph range because the range is not the
	problem: the font contains no mu at any code point, so covering more of Unicode would need a
	different font shipped with the application. 'us' is what every oscilloscope that cannot draw
	a mu has always printed.

	This is the only non-ASCII prefix Unit produces. The unit @em suffixes can also be non-ASCII
	(ohms, degrees), but nothing here uses those.
 */
static void AsciifyMicro(string& s)
{
	for(size_t i = 0; (i + 1) < s.size(); )
	{
		if( (static_cast<unsigned char>(s[i]) == 0xce) &&
			(static_cast<unsigned char>(s[i+1]) == 0xbc) )
		{
			s.replace(i, 2, "u");
			i++;
		}
		else
			i++;
	}
}

/**
	@brief Formats one tick, either per value or against the whole ruler's span

	@param axis		Axis being labelled
	@param value	Tick value
	@param sigfigs	Significant figures for the per-value form
	@param lo		Value at the start of the ruler
	@param hi		Value at the end of it
	@param step		Graduation size, or zero if it could not be determined
 */
static string FormatTick(
	const PlotAxis& axis, double value, int sigfigs, double lo, double hi, double step)
{
	string label;

	if(step <= 0)
		label = axis.GetUnit().PrettyPrint(value, sigfigs);
	else
	{
		//A couple of thousandths of a graduation: narrow enough that the trimming keeps every
		//digit that distinguishes this tick from its neighbours, wide enough that it drops the
		//"%.5f" tail. Measured against the awkward cases - a 250 ms graduation on a 1.5 s span,
		//and a 400 ns one straddling zero - rather than guessed.
		const double window = step * 0.005;
		label = axis.GetUnit().PrettyPrintRange(value - window, value + window, lo, hi);
	}

	AsciifyMicro(label);
	return label;
}

void DrawHorizontalRuler(const PlotAxis& axis, ImVec2 pos, ImVec2 size, bool uniformPrefix)
{
	//The gap comes out of the ruler's own height, not out of the plot above it: a baseline
	//drawn exactly on the boundary straddles it and eats the bottom row of the image, and
	//moving the boundary instead would resize the image every time the ruler is toggled.
	float gap = GetHorizontalRulerGap();
	float top = pos.y + gap;
	float height = size.y - gap;
	if( (size.x <= 0) || (height <= 0) )
		return;

	auto list = ImGui::GetWindowDrawList();
	auto color = ImGui::GetColorU32(ImGuiCol_Text);
	float fontSize = ImGui::GetFontSize();

	//Labels need room for a number and a unit suffix
	vector<AxisTick> ticks;
	GenerateAxisTicks(axis, size.x, 6 * fontSize, ticks);

	const double step = uniformPrefix ? MajorTickStep(ticks) : 0;
	const double spanLo = axis.PositionToUnits(0);
	const double spanHi = axis.PositionToUnits(size.x);

	//Baseline along the top, with ticks hanging below it
	list->AddLine(ImVec2(pos.x, top), ImVec2(pos.x + size.x, top), color, 1.5f);

	float majorLen = height * 0.35f;
	float minorLen = height * 0.18f;

	for(auto& t : ticks)
	{
		float x = pos.x + static_cast<float>(t.position);
		if( (x < pos.x) || (x > pos.x + size.x) )
			continue;

		float len = t.major ? majorLen : minorLen;
		list->AddLine(ImVec2(x, top), ImVec2(x, top + len), color, t.major ? 1.5f : 1.0f);

		if(!t.major)
			continue;

		auto label = FormatTick(axis, t.value, 4, spanLo, spanHi, step);
		auto textSize = ImGui::CalcTextSize(label.c_str());

		//Centre the label on its tick, but keep it inside the ruler at both ends rather than
		//letting it run off and get clipped
		float tx = x - textSize.x/2;
		tx = max(tx, pos.x);
		tx = min(tx, pos.x + size.x - textSize.x);

		list->AddText(ImVec2(tx, top + majorLen), color, label.c_str());
	}
}

void DrawVerticalRuler(const PlotAxis& axis, ImVec2 pos, ImVec2 size, bool uniformPrefix)
{
	if( (size.x <= 0) || (size.y <= 0) )
		return;

	auto list = ImGui::GetWindowDrawList();
	auto color = ImGui::GetColorU32(ImGuiCol_Text);
	float fontSize = ImGui::GetFontSize();

	vector<AxisTick> ticks;
	GenerateAxisTicks(axis, size.y, 2.5 * fontSize, ticks);

	const double step = uniformPrefix ? MajorTickStep(ticks) : 0;
	const double spanLo = axis.PositionToUnits(0);
	const double spanHi = axis.PositionToUnits(size.y);

	//Baseline down the left edge, ticks pointing right into the gutter
	list->AddLine(pos, ImVec2(pos.x, pos.y + size.y), color, 1.5f);

	float majorLen = 6;
	float minorLen = 3;

	for(auto& t : ticks)
	{
		//The axis increases upward, ImGui's y increases downward
		float y = pos.y + size.y - static_cast<float>(t.position);
		if( (y < pos.y) || (y > pos.y + size.y) )
			continue;

		float len = t.major ? majorLen : minorLen;
		list->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + len, y), color, t.major ? 1.5f : 1.0f);

		if(!t.major)
			continue;

		auto label = FormatTick(axis, t.value, 3, spanLo, spanHi, step);
		auto textSize = ImGui::CalcTextSize(label.c_str());

		float ty = y - textSize.y/2;
		ty = max(ty, pos.y);
		ty = min(ty, pos.y + size.y - textSize.y);

		list->AddText(ImVec2(pos.x + majorLen + 3, ty), color, label.c_str());
	}
}
