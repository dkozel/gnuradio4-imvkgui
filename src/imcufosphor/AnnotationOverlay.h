/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of AnnotationOverlay
 */
#ifndef AnnotationOverlay_h
#define AnnotationOverlay_h

#include "ImGuiCompat.h"

#include "Annotation.h"
#include "PlayerSession.h"
#include "PlotAxis.h"
#include "RowHistory.h"

/**
	@brief Draws annotation boxes over the spectrum and the waterfall

	Owned by AnalyzerPane and called after each area has rendered, so the areas stay waveform
	renderers with no opinion about metadata and the ruler arithmetic that decides where a plot
	ends lives in one place. The areas contribute a PlotRect and nothing else. See
	notes/annotation-overlay-plan.md §A2.

	Rendering only, for now: no hit testing, no seek, no filter panel. All three are later
	increments against the same rectangles this already computes.
 */
class AnnotationOverlay
{
public:
	AnnotationOverlay();

	bool GetEnabled() const
	{ return m_enabled; }

	void SetEnabled(bool enable)
	{ m_enabled = enable; }

	/**
		@brief Draws spans over the spectrum for the block being shown

		Spans belonging to the current block are drawn at full strength and fade afterwards on
		the density map's own decay constant, so they do not pop in and out as playback crosses
		an annotation edge and there is one persistence control rather than two.

		@param dl			Draw list to render into
		@param rect			Where the spectrum was drawn
		@param xaxis		The shared frequency axis
		@param set			Annotations, in recording coordinates
		@param block		Sample range of the block currently on screen
		@param sampleRate	Recording sample rate, Hz
		@param decaySec		Persistence decay constant, seconds
	 */
	void DrawOnSpectrum(
		ImDrawList* dl,
		const PlotRect& rect,
		const PlotAxis& xaxis,
		const AnnotationSet& set,
		const BlockSpan& block,
		double sampleRate,
		double decaySec);

	/**
		@brief Draws boxes over the waterfall, one per annotation per visible time range

		@param dl		Draw list to render into
		@param rect		Where the waterfall was drawn
		@param xaxis	The shared frequency axis
		@param set		Annotations, in recording coordinates
		@param rows		Row history, indexed by age
	 */
	void DrawOnWaterfall(
		ImDrawList* dl,
		const PlotRect& rect,
		const PlotAxis& xaxis,
		const AnnotationSet& set,
		const RowHistory& rows);

	///@brief Boxes drawn in the last frame, across both panes, after coalescing
	size_t GetLastDrawnCount() const
	{ return m_lastDrawn; }

	/**
		@brief Annotations that matched the visible range last frame, before coalescing

		Larger than GetLastDrawnCount() whenever the set is denser than the display can
		resolve, and larger than the set itself whenever looped playback puts the same
		annotation on screen several times over.
	 */
	size_t GetLastMatchedCount() const
	{ return m_lastMatched; }

protected:
	/**
		@brief A contiguous band of screen rows whose recording sample indices are monotonic

		Playback loops, so recording-relative row starts are only *piecewise* monotonic: they
		run one way down the screen, then jump at each wrap. A binary search over the whole
		column would be invalid. Cutting the column into runs restores the precondition a
		binary search needs, and an annotation spanning a wrap correctly draws as two bands -
		which is what actually happened.
	 */
	struct MonotonicRun
	{
		///@brief First and last age in the run, inclusive
		size_t ageFirst = 0;
		size_t ageLast = 0;

		///@brief Recording sample index at ageFirst (the newest, largest) and ageLast (oldest)
		int64_t sampleNewest = 0;
		int64_t sampleOldest = 0;
	};

	/**
		@brief One box waiting to be coalesced and drawn

		Drawing straight from the query does not work on a real set. dect6 has 2000
		annotations over ten seconds; at a typical line rate one waterfall row covers about
		twenty of them, all in the same frequency band. Twenty translucent rectangles stacked
		in one row of pixels is opaque, and the pane turns into a solid colour that says
		nothing about where the signal actually was. The spectrum is worse, because every
		matching annotation there is drawn at full plot height.

		So boxes are collected, merged where they describe the same band at adjacent times,
		and only then drawn. Coalescing is a rendering concern and deliberately does not touch
		the model: the query still returns every annotation, and GetLastMatchedCount() reports
		how many there were.
	 */
	struct PendingBox
	{
		float x0 = 0;
		float x1 = 0;
		float y0 = 0;
		float y1 = 0;

		///@brief Merge only within a colour, so two emitters sharing a band stay distinct
		uint32_t colorKey = 0;

		float alpha = 1;

		///@brief Index into the set, for the label
		size_t annotation = 0;
	};

	/**
		@brief Merges the collected boxes and draws them

		@param dl			Draw list
		@param set			The set the boxes index into
		@param labelYOffset	Pixels below a box's top edge to put its label

		@return Number of rectangles actually drawn, after merging
	 */
	size_t FlushBoxes(ImDrawList* dl, const AnnotationSet& set, float labelYOffset);

	///@brief Cuts the visible column into monotonic runs. Returns them in top-to-bottom order.
	void FindRuns(const RowHistory& rows, size_t visibleRows, std::vector<MonotonicRun>& out) const;

	/**
		@brief Screen y offset, in pixels from the top of the plot, for a sample within a run

		Interpolates between the two rows bracketing @a sample. The run is monotonic by
		construction, so the binary search inside is valid.
	 */
	double SampleToY(const RowHistory& rows, const MonotonicRun& run, int64_t sample) const;

	///@brief Maps an annotation's frequency edges to x pixels. False if it falls outside the plot.
	bool FrequencyToX(
		const Annotation& a,
		const PlotRect& rect,
		const PlotAxis& xaxis,
		float& x0,
		float& x1) const;

	/**
		@brief Draws the label inside a box if it fits and no neighbour has claimed the spot

		@param used		Positions of labels already placed this pane; appended to

		De-duplication is two dimensional. On the spectrum every label shares a y and it
		reduces to the horizontal test, but on the waterfall two annotations at the same
		frequency and different times are a normal thing to want both labels for, and an
		x-only test would silently drop one of them.
	 */
	void DrawLabel(
		ImDrawList* dl,
		const Annotation& a,
		float x0,
		float x1,
		float y,
		uint32_t color,
		std::vector<ImVec2>& used);

	bool m_enabled;

	///@brief Boxes drawn last frame across both panes, after coalescing
	size_t m_lastDrawn;

	///@brief Annotations that matched last frame, before coalescing
	size_t m_lastMatched;

	//Per pane, so the two do not depend on the order they are called in
	size_t m_drawnSpectrum;
	size_t m_drawnWaterfall;
	size_t m_matchedSpectrum;
	size_t m_matchedWaterfall;

	//Scratch, kept between frames so that a set with thousands of hits does not reallocate
	//every frame. Cleared at the top of each use.
	std::vector<size_t> m_hits;
	std::vector<MonotonicRun> m_runs;
	std::vector<ImVec2> m_labelPos;
	std::vector<PendingBox> m_boxes;
};

#endif
