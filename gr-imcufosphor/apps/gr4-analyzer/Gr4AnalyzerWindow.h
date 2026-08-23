/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of Gr4AnalyzerWindow
 */
#ifndef Gr4AnalyzerWindow_h
#define Gr4AnalyzerWindow_h

#include <memory>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>

#include "VulkanWindowCompat.h"

/**
	@brief The window a GNU Radio flowgraph's display blocks draw into

	Thinner than the standalone application's MainWindow, and deliberately so: it owns the
	window, the texture manager and the frame's pre-render-pass command buffer, and nothing
	else. There is no session, no pane and no filter graph here - those belong to whichever
	blocks the flowgraph happens to contain, which this class does not know about.

	It finds them the generic way, through BlockModel::uiCategory(), so adding a second display
	block to a flowgraph needs no change here.
 */
class Gr4AnalyzerWindow : public VulkanWindow
{
public:
	Gr4AnalyzerWindow(std::shared_ptr<QueueHandle> queue);
	virtual ~Gr4AnalyzerWindow();

	//not copyable or assignable
	Gr4AnalyzerWindow(const Gr4AnalyzerWindow&) =delete;
	Gr4AnalyzerWindow& operator=(const Gr4AnalyzerWindow&) =delete;

	virtual void Render() override;

	/**
		@brief DPI scale, where 1.0 is 96 DPI

		VulkanWindow declares GetContentScale() but no translation unit in scopehal-apps
		defines it - ngscopeclient never calls it, so upstream never notices, and linking
		against it fails for us. Same reimplementation MainWindow carries, for the same reason.
	 */
	float GetDpiScale();

	TextureManager* GetTextureManager()
	{ return m_texmgr.get(); }

	/**
		@brief The drawable blocks to render each frame

		Handed in by main() after the scheduler has taken the graph, because that is the point
		at which the block models exist and are stable. Not owned.
	 */
	void SetDrawableBlocks(std::vector<gr::BlockModel*> blocks)
	{ m_drawables = std::move(blocks); }

	///@brief True once every drawable block has reported it is shutting down
	bool AllBlocksDone() const
	{ return m_allDone; }

	/**
		@brief Wall clock of the last VulkanWindow::Render(), in milliseconds

		Covers the ImGui frame, every block's draw() and the present. Subtracting what the
		blocks report for themselves leaves the presentation cost, which is the only way to
		tell a slow pipeline from a slow swapchain.
	 */
	double GetLastRenderMs() const
	{ return m_lastRenderMs; }

protected:
	virtual void RenderUI() override;

	std::unique_ptr<TextureManager> m_texmgr;

	///@brief Command buffer for work that must precede the render pass, i.e. the tone maps
	std::unique_ptr<vk::raii::CommandPool> m_cmdPool;
	std::unique_ptr<vk::raii::CommandBuffer> m_cmdBuf;

	std::shared_ptr<QueueHandle> m_queue;

	std::vector<gr::BlockModel*> m_drawables;

	bool m_allDone;
	double m_lastRenderMs = 0;
};

#endif
