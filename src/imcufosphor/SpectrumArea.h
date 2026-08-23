/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of SpectrumArea
 */
#ifndef SpectrumArea_h
#define SpectrumArea_h

#include "TextureManager.h"

#include <scopehal/ComputePipeline.h>

#include "PlotAxis.h"
#include "SpectrumDensity.h"
#include "WaterfallArea.h"

/**
	@brief Push constants for the lifted analog rasterizer, shaders/waveform-compute.*.spv

	Field order and types must match the push_constant block in that shader exactly. Same
	layout as ngscopeclient's ConfigPushConstants (WaveformArea.h:135-149); duplicated for the
	same reason WaterfallToneMapArgs is, namely that lifting WaveformArea.h would drag in the
	whole application.
 */
struct ConfigPushConstants
{
	int64_t innerXoff;
	uint32_t windowHeight;
	uint32_t windowWidth;
	uint32_t memDepth;
	uint32_t offset_samples;
	float alpha;
	float xoff;
	float xscale;
	float ybase;
	float yscale;
	float yoff;
	float persistScale;
};

/**
	@brief Push constants for shaders/SpectrumDensityToneMap.spv
 */
struct SpectrumDensityToneMapArgs
{
	uint32_t width;
	uint32_t height;
	uint32_t outwidth;
	float binOffset;
	float xscale;
	float floorLevel;
	float gain;
};

/**
	@brief Push constants for shaders/SpectrumTraceToneMap.spv
 */
struct SpectrumTraceToneMapArgs
{
	uint32_t width;
	uint32_t height;
	uint32_t mask;
	uint32_t pad;
	float colors[5][4];
};

/**
	@brief Renders a SpectrumDensity filter's output

	Two layers drawn on top of each other: the RTSA density map, colourized through a ramp,
	and up to five percentile and mean traces drawn as lines over it. Either layer can be
	turned off, so this covers both the fosphor-style display and a conventional
	multi-detector spectrum plot without being two separate views.

	The traces go through the lifted analog rasterizer rather than any line drawing of our
	own, which is a design commitment rather than a convenience (DESIGN.md section 5):
	display persistence lives inside that shader, so writing a simpler renderer would
	silently forfeit phosphor on the traces and require rebuilding it later.

	Deliberately thin. Axes, cursors, annotations and linked zoom belong in AnalyzerPane
	(DESIGN.md 7.5), not here.
 */
class SpectrumArea
{
public:
	SpectrumArea(SpectrumDensity* density, TextureManager* texmgr, const std::string& colorRamp);
	virtual ~SpectrumArea();

	//not copyable or assignable
	SpectrumArea(const SpectrumArea&) =delete;
	SpectrumArea& operator=(const SpectrumArea&) =delete;

	///@brief Number of trace layers, matching SpectrumDensity's analog output streams
	static const size_t NUM_TRACES = 5;

	/**
		@brief Resizes the textures and raster buffers to match the pane

		@param size		Pane size in pixels

		@return True if anything was reallocated
	 */
	bool UpdateSize(ImVec2 size);

	/**
		@brief Rasterizes the traces and colourizes both layers

		Must be called outside a render pass - compute cannot be dispatched inside one - so
		this goes in MainWindow::Render() before VulkanWindow::Render(), the same place
		WaterfallArea::ToneMap() does.

		@param cmdBuf	Command buffer to record into. Caller submits.
	 */
	void ToneMap(vk::raii::CommandBuffer& cmdBuf);

	/**
		@brief Draws the plot, its amplitude ruler, and its frequency ruler if it owns one

		@a size is the whole area including the rulers, so a caller can hand this the result
		of GetContentRegionAvail() without doing the bookkeeping.
	 */
	void Render(ImVec2 size);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Axes

	/**
		@brief Replaces the frequency axis with a shared one

		The point of the sharing being by pointer rather than by copying values around: two
		panes given the same instance are linked by construction, and there is no update
		order to get wrong. An area constructed without this owns its own axis and is usable
		on its own.
	 */
	void SetXAxis(std::shared_ptr<PlotAxis> axis)
	{
		m_xAxis = axis;

		//Whoever supplied the axis owns fitting it. Two owners auto-fitting one shared axis
		//means the second one silently undoes the first, every frame.
		m_ownsXAxis = false;
	}

	std::shared_ptr<PlotAxis> GetXAxis()
	{ return m_xAxis; }

	///@brief The amplitude axis, which this area always owns
	PlotAxis& GetYAxis()
	{ return m_yAxis; }

	///@brief Where the spectrum was drawn last frame, for overlays. Invalid before the first.
	const PlotRect& GetPlotRect() const
	{ return m_plotRect; }

	/**
		@brief Whether to draw the frequency ruler

		Off when a container draws one shared ruler for several stacked panes, which is what
		WaveformGroup does with its timeline (WaveformGroup.cpp:285).
	 */
	void SetShowXAxis(bool show)
	{ m_showXAxis = show; }

	bool GetShowXAxis() const
	{ return m_showXAxis; }

	/**
		@brief Sets the frequency axis to show the whole spectrum

		@return False if there is no data to fit to yet
	 */
	bool FitXAxis(float widthPixels);

	///@brief Frequency range of the data, in x axis units. False if there is no data.
	bool GetDataRange(double& lo, double& hi);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Display configuration

	bool GetDensityVisible() const
	{ return m_densityVisible; }

	void SetDensityVisible(bool visible)
	{ m_densityVisible = visible; }

	bool GetTraceVisible(size_t i) const
	{ return (i < NUM_TRACES) ? m_traceVisible[i] : false; }

	void SetTraceVisible(size_t i, bool visible)
	{
		if(i < NUM_TRACES)
			m_traceVisible[i] = visible;
	}

	///@brief Human readable name of a trace layer, for the UI
	static const char* GetTraceName(size_t i);

	///@brief Colour of a trace layer, for the UI swatch
	ImVec4 GetTraceColor(size_t i) const
	{ return (i < NUM_TRACES) ? m_traceColors[i] : ImVec4(1, 1, 1, 1); }

	/**
		@brief Brightness of the rasterized traces

		Passed to the rasterizer as its alpha. Not the same thing as the density map's
		intensity, which comes from the accumulator's occupancy.
	 */
	void SetTraceAlpha(float alpha)
	{ m_traceAlpha = alpha; }

	float GetTraceAlpha() const
	{ return m_traceAlpha; }

	/**
		@brief Display persistence for the traces, 0 to 1

		Fed straight to the rasterizer's persistScale, which accumulates the previous frame's
		raster scaled by this. Zero overwrites, which is the default.
	 */
	void SetPersistDecay(float decay)
	{ m_persistDecay = decay; }

	float GetPersistDecay() const
	{ return m_persistDecay; }

	/**
		@brief Discards accumulated persistence on the next rasterize

		Persistence accumulates in screen space, so anything that changes where a given
		frequency or amplitude lands on screen invalidates what is already in the raster. The
		old trace would otherwise be blended into the new one at the old mapping, which reads
		as the trace smearing sideways.

		Called automatically whenever the axes or the pane size change; exposed for callers
		with a reason of their own, and named to match WaveformArea::ClearPersistence().
	 */
	void ClearPersistence()
	{
		for(size_t i=0; i<NUM_TRACES; i++)
			m_rasterStale[i] = true;
	}

	/**
		@brief Occupancy below which the density map is drawn as nothing

		A noise bin's dB distribution has a long tail downward, so without a floor the map
		shows a wide dim haze under the noise. See SpectrumDensityToneMap.glsl.
	 */
	void SetDensityFloor(float f)
	{ m_densityFloor = f; }

	float GetDensityFloor() const
	{ return m_densityFloor; }

	///@brief Brightness of the density map, applied after the floor
	void SetDensityGain(float g)
	{ m_densityGain = g; }

	float GetDensityGain() const
	{ return m_densityGain; }

protected:
	void RasterizeTrace(vk::raii::CommandBuffer& cmdBuf, size_t i, UniformAnalogWaveform* data);
	std::shared_ptr<ComputePipeline> GetTracePipeline();
	bool AllocateTexture(std::shared_ptr<Texture>& tex, uint32_t w, uint32_t h, const std::string& name);

	SpectrumDensity* m_density;
	TextureManager* m_texmgr;
	std::string m_colorRamp;

	///@brief Size of the plot in pixels, excluding the rulers
	uint32_t m_width;
	uint32_t m_height;

	///@brief Frequency. Shared with the waterfall when both live in an AnalyzerPane.
	std::shared_ptr<PlotAxis> m_xAxis;

	///@brief Amplitude in dBm. Always ours; nothing else has the same vertical meaning.
	PlotAxis m_yAxis;

	bool m_showXAxis;

	///@brief Where the spectrum itself was drawn last frame, excluding the rulers
	PlotRect m_plotRect;

	///@brief True once the frequency axis has been fitted to real data
	bool m_xAxisFitted;

	///@brief False once a container has supplied a shared axis, which it then fits itself
	bool m_ownsXAxis;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Density layer

	ComputePipeline m_densityToneMapPipeline;

	/**
		@brief Colourized density map

		As tall as the density map has amplitude cells, not as tall as the pane. ImGui
		magnifies it, which is what gr-fosphor does with its 128-cell histogram
		(gl.c:211,436-438) and is why its display looks smooth rather than banded. Rendering
		at cell resolution also means a resize never touches the accumulator.
	 */
	std::shared_ptr<Texture> m_densityTexture;

	bool m_densityVisible;
	float m_densityFloor;
	float m_densityGain;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Trace layer

	///@brief Lifted analog rasterizer, created on first use so the int64 probe has run
	std::shared_ptr<ComputePipeline> m_tracePipeline;

	ComputePipeline m_traceToneMapPipeline;

	///@brief fp32 intensity rasters, one per trace, pane sized
	AcceleratorBuffer<float> m_traceRaster[NUM_TRACES];

	///@brief All five traces composited into one texture
	std::shared_ptr<Texture> m_traceTexture;

	///@brief Which traces the composited texture actually holds, as of the last ToneMap()
	uint32_t m_traceMask;

	bool m_traceVisible[NUM_TRACES];
	ImVec4 m_traceColors[NUM_TRACES];

	float m_traceAlpha;
	float m_persistDecay;

	/**
		@brief Per trace: true when that raster no longer matches the current mapping

		Per trace rather than one flag for the area, because a hidden trace is not redrawn
		and so never gets the clean pass. One flag would be cleared by whichever traces
		happened to be visible, and re-showing a hidden one later would blend it against a
		raster left over from a different zoom.
	 */
	bool m_rasterStale[NUM_TRACES];

	/**
		@brief Axis state the raster buffers were last drawn with

		Compared rather than announced. ngscopeclient calls ClearPersistence() from each of
		its pan and zoom handlers (WaveformGroup.cpp:1385,1389), which works but means every
		future caller that moves the axis has to remember to do it. Our axis is a shared
		object that the pane, the clamp, a startup zoom and eventually a cursor jump can all
		move, so noticing the change covers all of them and cannot be forgotten.
	 */
	double m_lastXOffset;
	double m_lastXPixelsPerUnit;
	float m_lastYRange;
	float m_lastYOffset;
};

#endif
