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

#include "PlayerSession.h"
#include "PlotAxis.h"
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

	Follows ngscopeclient. The container owns the shared X axis and draws exactly one ruler
	for it (WaveformGroup.cpp:285); each area owns its own Y axis, whose meaning differs -
	amplitude for the spectrum, age for the waterfall - and draws it into a right hand gutter
	whose width the container reserves (WaveformGroup.cpp:250-251).

	Everything about the plots themselves stays in the areas. This class knows about layout,
	the shared axis, and the mouse.
 */
class AnalyzerPane
{
public:
	AnalyzerPane(PlayerSession* session, TextureManager* texmgr, const std::string& colorRamp);
	virtual ~AnalyzerPane();

	//not copyable or assignable
	AnalyzerPane(const AnalyzerPane&) =delete;
	AnalyzerPane& operator=(const AnalyzerPane&) =delete;

	/**
		@brief Runs the compute half of both areas

		Must be called outside a render pass, before VulkanWindow::Render().
	 */
	void ToneMap(vk::raii::CommandBuffer& cmdBuf);

	///@brief Lays out and draws both panes plus the shared frequency ruler
	void Render(ImVec2 size);

	SpectrumArea* GetSpectrumArea()
	{ return m_spectrumArea.get(); }

	WaterfallArea* GetWaterfallArea()
	{ return m_waterfallArea.get(); }

	std::shared_ptr<PlotAxis> GetXAxis()
	{ return m_xAxis; }

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

	PlayerSession* m_session;

	///@brief The one frequency axis both areas point at
	std::shared_ptr<PlotAxis> m_xAxis;

	std::unique_ptr<SpectrumArea> m_spectrumArea;
	std::unique_ptr<WaterfallArea> m_waterfallArea;

	float m_spectrumFraction;

	///@brief Set once the axis has been fitted to real data
	bool m_fitted;

};

#endif
