/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of AnalyzerPane
 */
#ifndef AnalyzerPane_h
#define AnalyzerPane_h

#include "AnalyzerSource.h"
#include "AnnotationOverlay.h"
#include "PlotAxis.h"
#include "RecordingClock.h"
#include "SpectrumArea.h"
#include "WaterfallArea.h"

/**
	@brief Stacks the spectrum over the waterfall on one shared frequency axis

	@par Why the axis is shared by pointer

	Both areas hold a std::shared_ptr<PlotAxis> and create their own if nobody supplies one,
	so either is usable on its own with a full set of rulers. This pane hands them both the
	same instance, which makes them linked by construction: there is no per-frame copying of
	offset and scale between panes, and therefore no order in which that copying can be
	wrong. Adding a third pane is one more SetXAxis call.

	@par Division of labour

	Follows ngscopeclient. The container owns the shared X axis and the layout; each area
	owns its own Y axis, whose meaning differs - amplitude for the spectrum, age for the
	waterfall - and draws it into a right hand gutter whose width the container reserves
	(WaveformGroup.cpp:250-251).

	Each area also draws a ruler for the shared X axis under its own plot, and each can be
	switched off separately (SetShowXAxis). ngscopeclient draws one ruler for a whole group
	(WaveformGroup.cpp:285) because its stacked plots are all the same kind of thing; here
	the spectrum and the waterfall are read separately often enough to be worth labelling
	separately. The container reserves the height of whichever rulers are showing, so an
	area's plot is the same size whether or not it has one under it.

	Everything about the plots themselves stays in the areas. This class knows about layout,
	the shared axis, and the mouse.
 */
class AnalyzerPane
{
public:
	/**
		@brief Builds both areas over a source of spectra

		Takes the narrow IAnalyzerSource rather than a PlayerSession so that a live flowgraph
		can drive the same display as a file being played back. Requires a source with both
		parts: a single-pane display uses SpectrumArea or WaterfallArea directly instead of
		asking this class to draw half of itself.
	 */
	AnalyzerPane(IAnalyzerSource* source, TextureManager* texmgr, const std::string& colorRamp);
	virtual ~AnalyzerPane();

	/**
		@brief Supplies the two things only a recording has

		Optional, and absent on a live flowgraph: a stream arriving from a port has no
		annotations to draw and no wall clock to pin them to. Without this the overlay draws
		nothing and the waterfall's hover readout omits the timestamp, both of which the areas
		already handle by null check rather than by requiring a caller to disable them.

		Neither pointer is owned; both must outlive the pane.
	 */
	void SetAnnotationSource(const AnnotationSet* annotations, const RecordingClock* clock);

	//not copyable or assignable
	AnalyzerPane(const AnalyzerPane&) =delete;
	AnalyzerPane& operator=(const AnalyzerPane&) =delete;

	/**
		@brief Runs the compute half of both areas

		Must be called outside a render pass, before VulkanWindow::Render().
	 */
	void ToneMap(vk::raii::CommandBuffer& cmdBuf);

	///@brief Lays out and draws both panes plus whichever frequency rulers are enabled
	void Render(ImVec2 size);

	SpectrumArea* GetSpectrumArea()
	{ return m_spectrumArea.get(); }

	WaterfallArea* GetWaterfallArea()
	{ return m_waterfallArea.get(); }

	std::shared_ptr<PlotAxis> GetXAxis()
	{ return m_xAxis; }

	/**
		@brief The annotation overlay, drawn over both areas

		Owned here rather than by either area because it draws over both and needs the plot
		rectangles they report, so putting it in one of them would mean the other's overlay
		came from somewhere else. See notes/annotation-overlay-plan.md §A2.
	 */
	AnnotationOverlay& GetOverlay()
	{ return m_overlay; }

	///@brief Fraction of the height given to the spectrum, the rest to the waterfall
	float GetSpectrumFraction() const
	{ return m_spectrumFraction; }

	void SetSpectrumFraction(float f)
	{ m_spectrumFraction = f; }

	///@brief Resets the frequency axis to the full span of the recording
	void FitXAxis();


protected:
	void HandleMouse(ImVec2 plotPos, ImVec2 plotSize);
	void ClampXAxis(float widthPixels);

	IAnalyzerSource* m_source;

	///@brief Annotations to draw over both areas, or null on a live stream
	const AnnotationSet* m_annotations;

	///@brief Recording sample index to wall clock, or null on a live stream
	const RecordingClock* m_clock;

	///@brief The one frequency axis both areas point at
	std::shared_ptr<PlotAxis> m_xAxis;

	std::unique_ptr<SpectrumArea> m_spectrumArea;
	std::unique_ptr<WaterfallArea> m_waterfallArea;

	///@brief Drawn over both areas after each has rendered
	AnnotationOverlay m_overlay;

	float m_spectrumFraction;

	///@brief Set once the axis has been fitted to real data
	bool m_fitted;

};

#endif
