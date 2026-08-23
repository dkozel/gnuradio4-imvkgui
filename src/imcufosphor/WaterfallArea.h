/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of WaterfallArea
 */
#ifndef WaterfallArea_h
#define WaterfallArea_h

#include "ngscopeclient.h"
#include "TextureManager.h"

#include <scopehal/ComputePipeline.h>
#include <scopeprotocols/Waterfall.h>

#include "PlotAxis.h"
#include "RecordingClock.h"
#include "RowHistory.h"

/**
	@brief Push constants for shaders/WaterfallToneMap.spv

	Field order and types must match the push_constant block in that shader exactly. Same
	layout as ngscopeclient's WaterfallToneMapArgs (WaveformArea.h:87); duplicated rather
	than lifted because it is a handful of POD fields and lifting WaveformArea.h would drag
	in the whole application.
 */
class WaterfallToneMapArgs
{
public:
	WaterfallToneMapArgs(
		uint32_t w, uint32_t h, uint32_t outwidth, uint32_t outheight, uint32_t o, uint32_t p, float x)
	: m_width(w)
	, m_height(h)
	, m_outwidth(outwidth)
	, m_outheight(outheight)
	, m_offsetSamples(o)
	, m_writeRow(p)
	, m_xscale(x)
	{}

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_outwidth;
	uint32_t m_outheight;
	uint32_t m_offsetSamples;
	uint32_t m_writeRow;
	float m_xscale;
};

/**
	@brief Renders a Waterfall filter's output

	The fp32 density buffer never leaves the GPU: WaterfallToneMap.spv colourizes it into a
	texture and ImGui samples that texture directly.

	Deliberately thin. Everything about axes, cursors, annotations and linked zoom belongs
	in AnalyzerPane (DESIGN.md 7.5), not here.
 */
class WaterfallArea
{
public:
	WaterfallArea(Waterfall* waterfall, TextureManager* texmgr, const std::string& colorRamp);
	virtual ~WaterfallArea();

	//not copyable or assignable
	WaterfallArea(const WaterfallArea&) =delete;
	WaterfallArea& operator=(const WaterfallArea&) =delete;

	/**
		@brief Resizes the output texture and the waterfall's row count to match the pane

		@param size		Pane size in pixels

		@return True if anything was reallocated
	 */
	bool UpdateSize(ImVec2 size);

	/**
		@brief Tone maps the density buffer into our texture

		Must be called outside a render pass - compute cannot be dispatched inside one - so
		this goes in MainWindow::Render() before VulkanWindow::Render(), not in DoRender().

		@param cmdBuf	Command buffer to record into. Caller submits.
	 */
	void ToneMap(vk::raii::CommandBuffer& cmdBuf);

	/**
		@brief Draws the waterfall, its time ruler, and its frequency ruler if it owns one

		@a size is the whole area including the rulers.
	 */
	void Render(ImVec2 size);

	std::shared_ptr<Texture> GetTexture()
	{ return m_texture; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Axes

	///@brief Replaces the frequency axis with a shared one. See SpectrumArea::SetXAxis.
	void SetXAxis(std::shared_ptr<PlotAxis> axis)
	{
		m_xAxis = axis;

		//Whoever supplied the axis owns fitting it. Two owners auto-fitting one shared axis
		//means the second one silently undoes the first, every frame.
		m_ownsXAxis = false;
	}

	std::shared_ptr<PlotAxis> GetXAxis()
	{ return m_xAxis; }

	///@brief The time axis, which this area always owns
	PlotAxis& GetYAxis()
	{ return m_yAxis; }

	///@brief Where the waterfall was drawn last frame, for overlays. Invalid before the first.
	const PlotRect& GetPlotRect() const
	{ return m_plotRect; }

	void SetShowXAxis(bool show)
	{ m_showXAxis = show; }

	bool GetShowXAxis() const
	{ return m_showXAxis; }

	/**
		@brief Supplies what is needed to label the time axis and time readouts

		The waterfall itself cannot know any of this: upstream records no per-row timestamps
		(DESIGN.md section 8.2), so the age of a row can only come from whoever produced it.

		Note that this area does not count samples for itself. Sample counters are maintained
		where samples and rows are produced - SigMFSource and PlayerSession - and handed here;
		a display that kept its own would be a second counter to disagree with the first.

		@param history		Row history from PlayerSession, or null to label in rows
		@param clock		Recording clock from the source, or null for no time readout
		@param sampleRate	Recording sample rate, Hz
	 */
	void SetTimebase(const RowHistory* history, const RecordingClock* clock, double sampleRate)
	{
		m_rowHistory = history;
		m_clock = clock;
		m_sampleRate = sampleRate;
	}

	///@brief Sets the frequency axis to show the whole spectrum. False if there is no data.
	bool FitXAxis(float widthPixels);

	///@brief Frequency range of the data, in x axis units. False if there is no data.
	bool GetDataRange(double& lo, double& hi);

protected:
	Waterfall* m_waterfall;
	TextureManager* m_texmgr;
	std::string m_colorRamp;

	ComputePipeline m_toneMapPipeline;
	std::shared_ptr<Texture> m_texture;

	///@brief Size of the output texture in pixels
	uint32_t m_width;
	uint32_t m_height;

	///@brief Frequency. Shared with the spectrum when both live in an AnalyzerPane.
	std::shared_ptr<PlotAxis> m_xAxis;

	///@brief Age of each row, in femtoseconds, increasing downward from zero at the top
	PlotAxis m_yAxis;

	bool m_showXAxis;

	///@brief True once the frequency axis has been fitted to real data
	bool m_xAxisFitted;

	///@brief False once a container has supplied a shared axis, which it then fits itself
	bool m_ownsXAxis;

	///@brief Where the waterfall itself was drawn last frame, excluding the rulers
	PlotRect m_plotRect;

	///@brief Not owned; supplied by whoever drives playback
	const RowHistory* m_rowHistory;

	///@brief Not owned; supplied by whoever drives playback. Null means no time readout.
	const RecordingClock* m_clock;

	double m_sampleRate;
};

#endif
