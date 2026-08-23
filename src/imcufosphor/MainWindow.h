/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of MainWindow
 */
#ifndef MainWindow_h
#define MainWindow_h

#include "VulkanWindow.h"
#include "TextureManager.h"

#include "AnalyzerPane.h"
#include "PlayerSession.h"

/**
	@brief Top level application window

	Subclasses the VulkanWindow lifted from ngscopeclient (DESIGN.md section 5) and adds the
	filter graph plus the views over it.

	This is a phase 1 milestone, not the finished application: the two panes are stacked but
	do not yet share a frequency axis, and there is no pan, zoom or cursor. Those arrive with
	AnalyzerPane (DESIGN.md sections 7.5 and 12).
 */
class MainWindow : public VulkanWindow
{
public:
	MainWindow(std::shared_ptr<QueueHandle> queue, SigMFSource* source);

	PlayerSession* GetSession()
	{ return m_session.get(); }
	virtual ~MainWindow();

	//not copyable or assignable
	MainWindow(const MainWindow&) =delete;
	MainWindow& operator=(const MainWindow&) =delete;

	virtual void Render() override;

	/**
		@brief DPI scale, where 1.0 is 96 DPI

		VulkanWindow declares GetContentScale() but no translation unit in scopehal-apps
		defines it - ngscopeclient never calls it, so upstream never notices, and linking
		against it fails for us. Reimplemented here rather than patching the submodule.
	 */
	float GetDpiScale();

	TextureManager* GetTextureManager()
	{ return m_texmgr.get(); }

	AnalyzerPane* GetAnalyzerPane()
	{ return m_pane.get(); }

	SpectrumArea* GetSpectrumArea()
	{ return m_pane->GetSpectrumArea(); }

	///@brief Advances playback by one waterfall row. Returns false if no row was produced.
	bool Step();

protected:
	virtual void RenderUI() override;

	///@brief Draws the transport and display controls
	void RenderControls();

	///@brief Draws the density map and trace visibility controls
	void RenderSpectrumControls();

	std::unique_ptr<TextureManager> m_texmgr;

	///@brief Recording, filter graph and playback
	std::unique_ptr<PlayerSession> m_session;

	///@brief The stacked spectrum and waterfall, sharing one frequency axis
	std::unique_ptr<AnalyzerPane> m_pane;

	///@brief The three configurable percentile traces, as percentages
	float m_percentiles[3];

	///@brief Command pool and buffer for our own compute work, outside the render pass
	std::unique_ptr<vk::raii::CommandPool> m_cmdPool;
	std::unique_ptr<vk::raii::CommandBuffer> m_cmdBuf;

	///@brief True while playback is advancing
	bool m_running;

	/**
		@brief Wall clock milliseconds per frame that may be spent advancing playback

		Presentation is vsync bound at ~15 ms/frame while the filter graph costs under 2 ms,
		so stepping exactly one row per frame caps playback at the refresh rate and leaves
		the GPU idle most of the frame. Stepping until this budget is spent uses that time
		instead. One row is always produced regardless, so the display never freezes.
	 */
	double m_stepBudgetMs;

	///@brief Rows and frames per second over the last interval, for the status line
	double m_measuredRowRate;
	double m_measuredFrameRate;
	double m_lastRateSample;
	int64_t m_lastRateRows;
	int64_t m_lastRateFrames;
	int64_t m_frames;

	///@brief Where the wall clock went last interval, in milliseconds per frame
	double m_msStep;
	double m_msToneMap;
	double m_msPresent;
	double m_accStep;
	double m_accToneMap;
	double m_accPresent;
};

#endif
