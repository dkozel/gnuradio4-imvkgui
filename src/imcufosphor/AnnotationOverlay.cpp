/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of AnnotationOverlay
 */

#include "AnnotationOverlay.h"

#include <cmath>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing constants
//
// Low enough that a dense set does not bury the waveform under it, high enough that a single
// annotation is not missed. The outline carries the visibility and the fill carries the extent,
// which is why they are so far apart.

///@brief Alpha of the filled interior of a box
static const float g_fillAlpha = 0.12f;

///@brief Alpha of the box outline
static const float g_outlineAlpha = 0.60f;

///@brief Alpha of the label text
static const float g_labelAlpha = 0.90f;

/**
	@brief Decay constants past which a fading span is dropped rather than drawn

	At four time constants the term is under 2%, which on a 12% fill is well below what a
	display can show. Continuing to draw it costs a query and a rectangle for nothing.
 */
static const double g_decayWindows = 4.0;

///@brief Smallest alpha worth submitting to the draw list
static const float g_minAlpha = 0.02f;

///@brief Height of the strip used for annotations that carry no frequency extent, in pixels
static const float g_noFreqStripHeight = 6;

///@brief Gap either side of a label below which a neighbour suppresses it. WaveformArea.cpp:1524.
static const float g_neighborThresholdPixels = 4;

static uint32_t PackColor(const AnnotationColor& c, float alpha)
{
	if(alpha > 1)
		alpha = 1;
	if(alpha < 0)
		alpha = 0;

	return IM_COL32(
		static_cast<int>(c.r * 255.0f),
		static_cast<int>(c.g * 255.0f),
		static_cast<int>(c.b * 255.0f),
		static_cast<int>(alpha * 255.0f));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction

AnnotationOverlay::AnnotationOverlay()
	: m_enabled(false)
	, m_lastDrawn(0)
	, m_lastMatched(0)
	, m_drawnSpectrum(0)
	, m_drawnWaterfall(0)
	, m_matchedSpectrum(0)
	, m_matchedWaterfall(0)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Coalescing

size_t AnnotationOverlay::FlushBoxes(ImDrawList* dl, const AnnotationSet& set, float labelYOffset)
{
	if(m_boxes.empty())
		return 0;

	//Group by colour and horizontal extent, then by vertical position, so that the merge below
	//is a single forward sweep over runs of compatible boxes
	sort(m_boxes.begin(), m_boxes.end(),
		[](const PendingBox& a, const PendingBox& b)
		{
			if(a.colorKey != b.colorKey)
				return a.colorKey < b.colorKey;
			if(a.x0 != b.x0)
				return a.x0 < b.x0;
			if(a.x1 != b.x1)
				return a.x1 < b.x1;
			return a.y0 < b.y0;
		});

	m_labelPos.clear();

	size_t drawn = 0;
	size_t i = 0;
	while(i < m_boxes.size())
	{
		PendingBox cur = m_boxes[i];

		size_t j = i + 1;
		while(j < m_boxes.size())
		{
			const PendingBox& n = m_boxes[j];

			//Different emitter, or a different part of the spectrum: not the same box
			if(n.colorKey != cur.colorKey)
				break;
			if(fabsf(n.x0 - cur.x0) > 1)
				break;
			if(fabsf(n.x1 - cur.x1) > 1)
				break;

			//A real gap in time is information - it is the difference between one long
			//transmission and a burst pattern - so only touching or overlapping boxes merge
			if(n.y0 > cur.y1 + 1)
				break;

			if(n.y1 > cur.y1)
				cur.y1 = n.y1;

			//The merged box takes the strongest alpha of its parts, so a still-active
			//annotation is not dimmed by an older one it happens to touch
			if(n.alpha > cur.alpha)
			{
				cur.alpha = n.alpha;
				cur.annotation = n.annotation;
			}

			j++;
		}

		auto rgb = ColorForKey(cur.colorKey);
		dl->AddRectFilled(
			ImVec2(cur.x0, cur.y0), ImVec2(cur.x1, cur.y1), PackColor(rgb, cur.alpha * g_fillAlpha));
		dl->AddRect(
			ImVec2(cur.x0, cur.y0), ImVec2(cur.x1, cur.y1), PackColor(rgb, cur.alpha * g_outlineAlpha));

		//Labels only where there is vertical room, so a one pixel band does not get a line of
		//text three times its height
		if(cur.y1 - cur.y0 >= ImGui::GetTextLineHeight())
		{
			DrawLabel(
				dl, set[cur.annotation], cur.x0, cur.x1, cur.y0 + labelYOffset,
				PackColor(rgb, cur.alpha * g_labelAlpha), m_labelPos);
		}

		drawn++;
		i = j;
	}

	return drawn;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Geometry

bool AnnotationOverlay::FrequencyToX(
	const Annotation& a,
	const PlotRect& rect,
	const PlotAxis& xaxis,
	float& x0,
	float& x1) const
{
	float left = rect.pos.x;
	float right = rect.pos.x + rect.size.x;

	//No frequency extent means the annotation says something about a time range and nothing
	//about frequency, so it spans the whole width rather than guessing at one
	if(!a.hasFreq)
	{
		x0 = left;
		x1 = right;
		return true;
	}

	//The axis is in microhertz (AnalyzerPane.cpp:23). Doubles are exact at this magnitude:
	//2.4 GHz is 2.4e15 uHz against a 9.0e15 exact-integer limit, per PlotAxis.h:30-40.
	x0 = left + static_cast<float>(xaxis.UnitsToPosition(a.freqLoHz * 1e6));
	x1 = left + static_cast<float>(xaxis.UnitsToPosition(a.freqHiHz * 1e6));

	//Cull before anything reaches the draw list. The set is queried by time, so a zoomed-in
	//view can match thousands of annotations that are nowhere near the visible frequencies.
	if( (x1 < left) || (x0 > right) )
		return false;

	//A narrow emitter seen zoomed out is still worth one pixel rather than nothing
	if(x1 - x0 < 1)
		x1 = x0 + 1;

	return true;
}

void AnnotationOverlay::FindRuns(
	const RowHistory& rows, size_t visibleRows, vector<MonotonicRun>& out) const
{
	out.clear();
	if(visibleRows == 0)
		return;

	RowMark first;
	if(!rows.GetRow(0, first))
		return;

	MonotonicRun run;
	run.ageFirst = 0;
	run.ageLast = 0;
	run.sampleNewest = first.recordingStart;
	run.sampleOldest = first.recordingStart;

	for(size_t age=1; age<visibleRows; age++)
	{
		RowMark m;
		if(!rows.GetRow(age, m))
			break;

		//Going down the screen is going back in time, so the recording index should fall.
		//When it rises instead, playback looped between these two rows and a new run starts.
		if(m.recordingStart <= run.sampleOldest)
		{
			run.ageLast = age;
			run.sampleOldest = m.recordingStart;
		}
		else
		{
			out.push_back(run);
			run.ageFirst = age;
			run.ageLast = age;
			run.sampleNewest = m.recordingStart;
			run.sampleOldest = m.recordingStart;
		}
	}

	out.push_back(run);
}

double AnnotationOverlay::SampleToY(
	const RowHistory& rows, const MonotonicRun& run, int64_t sample) const
{
	//Clamp rather than extrapolate past the ends of the run
	if(sample >= run.sampleNewest)
		return static_cast<double>(run.ageFirst);
	if(sample <= run.sampleOldest)
		return static_cast<double>(run.ageLast);

	//Largest age whose recording index is still at or above the one we want. The run is
	//monotonic by construction, which is the entire reason it was cut out in the first place.
	size_t lo = run.ageFirst;
	size_t hi = run.ageLast;
	while(lo < hi)
	{
		//Biased upward so that lo can advance; hi - lo >= 1 here, so mid >= lo + 1 >= 1 and
		//the decrement below cannot underflow
		size_t mid = lo + (hi - lo + 1) / 2;

		RowMark m;
		if(!rows.GetRow(mid, m))
		{
			hi = mid - 1;
			continue;
		}

		if(m.recordingStart >= sample)
			lo = mid;
		else
			hi = mid - 1;
	}

	RowMark a;
	if(!rows.GetRow(lo, a))
		return static_cast<double>(lo);
	if(lo >= run.ageLast)
		return static_cast<double>(lo);

	RowMark b;
	if(!rows.GetRow(lo + 1, b))
		return static_cast<double>(lo);

	//Between age lo and lo+1 the index falls from a to b, so interpolate within that row
	double span = static_cast<double>(a.recordingStart - b.recordingStart);
	if(span <= 0)
		return static_cast<double>(lo);

	double frac = static_cast<double>(a.recordingStart - sample) / span;
	return static_cast<double>(lo) + frac;
}

void AnnotationOverlay::DrawLabel(
	ImDrawList* dl,
	const Annotation& a,
	float x0,
	float x1,
	float y,
	uint32_t color,
	vector<ImVec2>& used)
{
	if(a.label.empty())
		return;

	ImVec2 sz = ImGui::CalcTextSize(a.label.c_str());

	//Only when the box is actually wide enough for the text. Clipping a label to a box narrower
	//than one word produces a smear that reads as corruption.
	if(x1 - x0 < sz.x + 4)
		return;

	//De-duplicate against labels already placed. Without it a dense set - dect6 has 2000
	//annotations, omnisig 9182 - turns into a solid bar of overlapping text.
	//
	//Both axes, not just x: on the waterfall two annotations at the same frequency and
	//different times are exactly what the display is for, and suppressing one of those would
	//hide real data rather than tidy up a collision.
	float lineHeight = ImGui::GetTextLineHeight();
	for(size_t i=0; i<used.size(); i++)
	{
		if( (fabsf(used[i].x - x0) < sz.x + g_neighborThresholdPixels) &&
			(fabsf(used[i].y - y) < lineHeight) )
		{
			return;
		}
	}
	used.push_back(ImVec2(x0, y));

	dl->AddText(ImVec2(x0 + 2, y), color, a.label.c_str());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Spectrum

void AnnotationOverlay::DrawOnSpectrum(
	ImDrawList* dl,
	const PlotRect& rect,
	const PlotAxis& xaxis,
	const AnnotationSet& set,
	const BlockSpan& block,
	double sampleRate,
	double decaySec)
{
	m_drawnSpectrum = 0;
	m_matchedSpectrum = 0;
	m_lastDrawn = m_drawnSpectrum + m_drawnWaterfall;
	m_lastMatched = m_matchedSpectrum + m_matchedWaterfall;

	if(!m_enabled || !rect.valid || set.empty() || (sampleRate <= 0))
		return;

	int64_t blockStart = block.recordingStart;
	int64_t blockEnd = block.recordingEnd;

	//A block that straddled the end of the file during looped playback has already wrapped its
	//recording coordinate, so the end reads below the start. The stream coordinates did not
	//wrap, so they give the block's true length and an end that keeps the arithmetic below
	//monotonic. Nothing matches past the end of the recording, so overshooting is harmless.
	if(blockEnd < blockStart)
		blockEnd = blockStart + (block.streamEnd - block.streamStart);

	//How far back a span can still be fading. With no decay configured, only the current block
	//is shown rather than dividing by zero below.
	int64_t decayWindow = 0;
	if(decaySec > 0)
		decayWindow = static_cast<int64_t>(g_decayWindows * decaySec * sampleRate);

	m_hits.clear();
	set.QueryOverlapping(blockStart - decayWindow, blockEnd, m_hits);
	if(m_hits.empty())
		return;

	float top = rect.pos.y;
	float bottom = rect.pos.y + rect.size.y;

	dl->PushClipRect(rect.pos, ImVec2(rect.pos.x + rect.size.x, bottom), true);

	m_boxes.clear();
	for(size_t i=0; i<m_hits.size(); i++)
	{
		const Annotation& a = set[m_hits[i]];

		//Fade is a pure function of the annotation and the current block, so there is no
		//per-annotation state to reset on seek, restart or resize, and it is independent of
		//frame rate. Still-active spans give a non-positive age and so full alpha.
		double alpha = 1.0;
		if(decaySec > 0)
		{
			double ageSec = static_cast<double>(blockEnd - a.sampleEnd) / sampleRate;
			if(ageSec > 0)
				alpha = exp(-ageSec / decaySec);
		}
		if(alpha * g_outlineAlpha < g_minAlpha)
			continue;

		float x0 = 0;
		float x1 = 0;
		if(!FrequencyToX(a, rect, xaxis, x0, x1))
			continue;

		PendingBox box;
		box.x0 = x0;
		box.x1 = x1;
		box.y0 = top;

		//An annotation with no frequency extent gets a thin strip at the top rather than a
		//full-width wash, which over the whole plot would swamp the trace it sits on
		box.y1 = a.hasFreq ? bottom : (top + g_noFreqStripHeight);

		box.colorKey = a.colorKey;
		box.alpha = static_cast<float>(alpha);
		box.annotation = m_hits[i];
		m_boxes.push_back(box);

		m_matchedSpectrum++;
	}

	//Every box here spans the full plot height, so coalescing collapses the identical bands
	//that a dense set produces down to one rectangle per distinct emitter and frequency
	m_drawnSpectrum = FlushBoxes(dl, set, g_noFreqStripHeight + 1);

	dl->PopClipRect();

	m_lastDrawn = m_drawnSpectrum + m_drawnWaterfall;
	m_lastMatched = m_matchedSpectrum + m_matchedWaterfall;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Waterfall

void AnnotationOverlay::DrawOnWaterfall(
	ImDrawList* dl,
	const PlotRect& rect,
	const PlotAxis& xaxis,
	const AnnotationSet& set,
	const RowHistory& rows)
{
	m_drawnWaterfall = 0;
	m_matchedWaterfall = 0;
	m_lastDrawn = m_drawnSpectrum + m_drawnWaterfall;
	m_lastMatched = m_matchedSpectrum + m_matchedWaterfall;

	if(!m_enabled || !rect.valid || set.empty())
		return;

	//Screen y from the top of the plot is exactly the row's age: the tone map resolves the ring
	//so the last output row is the newest (WaterfallToneMap.glsl:56-63) and the blit is one
	//texel per pixel with v flipped. So the overlay never touches writerow. Rows older than the
	//plot height exist in the ring but are off screen.
	size_t visibleRows = static_cast<size_t>(rect.size.y);
	if(visibleRows > rows.GetCount())
		visibleRows = rows.GetCount();
	if(visibleRows < 2)
		return;

	FindRuns(rows, visibleRows, m_runs);

	dl->PushClipRect(
		rect.pos, ImVec2(rect.pos.x + rect.size.x, rect.pos.y + rect.size.y), true);

	//Collected across every run, not per run, so that an annotation split by a loop wrap can
	//still merge with its other half where the two abut on screen
	m_boxes.clear();

	for(size_t r=0; r<m_runs.size(); r++)
	{
		const MonotonicRun& run = m_runs[r];

		//One query per run per frame, not one per annotation
		m_hits.clear();
		set.QueryOverlapping(run.sampleOldest, run.sampleNewest + 1, m_hits);

		float runTop = rect.pos.y + static_cast<float>(run.ageFirst);
		float runBottom = rect.pos.y + static_cast<float>(run.ageLast) + 1;

		for(size_t i=0; i<m_hits.size(); i++)
		{
			const Annotation& a = set[m_hits[i]];

			float x0 = 0;
			float x1 = 0;
			if(!FrequencyToX(a, rect, xaxis, x0, x1))
				continue;

			//The newer edge is higher on screen. A zero length annotation is widened to one
			//sample so it draws as a line rather than a rectangle of no height.
			int64_t newerEdge = AnnotationSet::EffectiveEnd(a);
			float yA = rect.pos.y + static_cast<float>(SampleToY(rows, run, newerEdge));
			float yB = rect.pos.y + static_cast<float>(SampleToY(rows, run, a.sampleStart));

			//Clamp into the run's own band, so an annotation running past a wrap is cut at the
			//boundary and drawn again by the next run rather than smeared across both
			if(yA < runTop)
				yA = runTop;
			if(yB > runBottom)
				yB = runBottom;
			if(yB < yA)
				continue;
			if(yB - yA < 1)
				yB = yA + 1;

			PendingBox box;
			box.x0 = x0;
			box.x1 = x1;
			box.y0 = yA;
			box.y1 = yB;
			box.colorKey = a.colorKey;
			box.alpha = 1;
			box.annotation = m_hits[i];
			m_boxes.push_back(box);

			m_matchedWaterfall++;
		}
	}

	//At a typical line rate one row of pixels covers about twenty dect6 annotations, all in
	//the same band. Merging them into one box per band per contiguous time range is what makes
	//the pane readable instead of a solid wash, and it preserves the gaps between bursts.
	m_drawnWaterfall = FlushBoxes(dl, set, 1);

	dl->PopClipRect();

	m_lastDrawn = m_drawnSpectrum + m_drawnWaterfall;
	m_lastMatched = m_matchedSpectrum + m_matchedWaterfall;
}
