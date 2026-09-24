/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of ScopeArea
 */
#ifndef ScopeArea_h
#define ScopeArea_h

#include "TextureManager.h"

#include <scopehal/ComputePipeline.h>

#include <memory>
#include <vector>

#include "PlotAxis.h"
#include "ScopeCapture.h"
#include "SpectrumArea.h"

/**
	@brief Push constants for shaders/ScopeTraceToneMap.spv
 */
struct ScopeTraceToneMapArgs
{
	uint32_t width;
	uint32_t height;

	///@brief Bit 0: first trace of the frame. Bit 1: last. See the shader.
	uint32_t flags;

	uint32_t pad;

	float color[4];
};

/**
	@brief Draws a ScopeCapture's records as an overlaid time domain plot

	One plot, every trace on it, each with its own volts per division and vertical position - the
	way a bench scope does it rather than the way a software plot with one subplot per signal
	does. The alternative, stacking an area per port on a shared time axis, is what AnalyzerPane
	does for the spectrum and waterfall; it is better when the signals are unrelated and worse
	when the whole question is how two of them line up, which is what a scope is for.

	@par Reusing the spectrum's rasterizer

	Traces go through the same lifted analog rasterizer SpectrumArea uses, for the same reason
	(DESIGN.md section 5): display persistence lives inside that shader, and the column-wise
	min/max fill it does is exactly the decimation a time domain plot needs when a record is
	wider than the pane. Feeding it raw samples and letting it decide what each column covers is
	the intended design, not a shortcut - a pre-decimated envelope would be the same work done
	worse.

	The X mapping is therefore shared with SpectrumArea down to the push constants; only the unit
	differs, femtoseconds here against microhertz there.

	@par Threading

	Render thread only, like every other area. ToneMap() must be called outside a render pass.
 */
class ScopeArea
{
public:
	ScopeArea(ScopeCapture* capture, TextureManager* texmgr);
	virtual ~ScopeArea();

	//not copyable or assignable
	ScopeArea(const ScopeArea&) =delete;
	ScopeArea& operator=(const ScopeArea&) =delete;

	/**
		@brief Graticule divisions

		Ten by eight, the near universal bench scope graticule, so that "volts per division" and
		"time per division" mean to a user what they mean everywhere else.
	 */
	static const size_t DIVISIONS_X = 10;
	static const size_t DIVISIONS_Y = 8;

	///@brief Hard cap on traces, set by the 32-bit visibility mask
	static const size_t MAX_TRACES = 32;

	bool UpdateSize(ImVec2 size);

	/**
		@brief Rasterizes every visible trace and composites them into one texture

		@param cmdBuf	Command buffer to record into. Caller submits.

		Must be called outside a render pass; compute cannot be dispatched inside one.
	 */
	void ToneMap(vk::raii::CommandBuffer& cmdBuf);

	/**
		@brief Draws the plot, the graticule and both rulers

		@a size is the whole area including the rulers, so a caller can pass the result of
		GetContentRegionAvail() without doing the bookkeeping.
	 */
	void Render(ImVec2 size);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Axes

	PlotAxis& GetXAxis()
	{ return m_xAxis; }

	///@brief Volts, tracking whichever trace is selected. See SetSelectedTrace().
	PlotAxis& GetYAxis()
	{ return m_yAxis; }

	const PlotRect& GetPlotRect() const
	{ return m_plotRect; }

	/**
		@brief Asks for the time axis to be fitted to the whole record on the next frame with data

		Deferred rather than immediate because the record length, the sample rate and the pane
		width can all be unknown at the moment a caller wants this.
	 */
	void RequestFit()
	{ m_fitted = false; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Per trace display

	size_t GetTraceCount() const;

	const char* GetTraceName(size_t i) const;

	///@brief Colour of a trace, for the swatch and the ground marker
	ImVec4 GetTraceColor(size_t i) const;

	bool GetTraceVisible(size_t i) const
	{ return (i < MAX_TRACES) ? ((m_traceEnabled & (1u << i)) != 0) : false; }

	void SetTraceVisible(size_t i, bool visible);

	uint32_t GetTraceMask() const
	{ return m_traceEnabled; }

	void SetTraceMask(uint32_t mask)
	{ m_traceEnabled = mask; }

	/**
		@brief Full scale of a trace, in volts per graticule division

		Eight divisions of pane height, so the visible range is eight times this.
	 */
	float GetVoltsPerDiv(size_t i) const;
	void SetVoltsPerDiv(size_t i, float v);

	/**
		@brief Sets the vertical scale of every trace, or of the selected one

		@par Why ganged is the default

		This is a single overlaid plot, so "how many volts is a division" is a question about the
		picture rather than about one line in it. Scaling only the selected trace - which is what
		the first version did, with selection hidden behind a right click on the legend - reads as
		the control being broken: the user turns the knob and one of the traces moves.

		Per-trace scaling still matters when two channels have genuinely different amplitudes, so
		it is a mode rather than a removal. See SetGanged().
	 */
	void ApplyVoltsPerDiv(float v);

	///@brief Multiplies the vertical scale, honouring the ganged flag
	void ScaleVoltsPerDiv(float factor);

	///@brief Moves the vertical position, honouring the ganged flag
	void NudgeTraceOffset(float deltaVolts);

	/**
		@brief Whether vertical controls act on every trace at once

		On by default. Off restricts them to GetSelectedTrace().
	 */
	void SetGanged(bool ganged)
	{ m_ganged = ganged; }

	bool GetGanged() const
	{ return m_ganged; }

	///@brief Voltage at the vertical centre of the pane for this trace
	float GetTraceOffset(size_t i) const;
	void SetTraceOffset(size_t i, float v);

	/**
		@brief Which trace the vertical ruler labels and the vertical controls act on

		A single overlaid plot has one gutter and as many vertical scales as it has traces, so one
		of them has to be the one being labelled. Every bench scope with more than one channel
		makes the same choice.
	 */
	size_t GetSelectedTrace() const
	{ return m_selectedTrace; }

	void SetSelectedTrace(size_t i)
	{ m_selectedTrace = i; }

	void SetTraceAlpha(float alpha)
	{ m_traceAlpha = alpha; }

	float GetTraceAlpha() const
	{ return m_traceAlpha; }

	void SetPersistDecay(float decay)
	{ m_persistDecay = decay; }

	float GetPersistDecay() const
	{ return m_persistDecay; }

	///@brief Discards accumulated persistence on the next rasterize
	void ClearPersistence();

	///@brief Whether to draw the graticule behind the traces
	void SetShowGraticule(bool show)
	{ m_showGraticule = show; }

	bool GetShowGraticule() const
	{ return m_showGraticule; }

	/**
		@brief Whether the mouse may pan and zoom this area

		Off lets a host put the plot inside something that handles its own input.
	 */
	void SetInteractive(bool interactive)
	{ m_interactive = interactive; }

protected:
	void HandleMouse(ImVec2 plotPos, ImVec2 plotSize);
	void FitXAxis(float widthPixels);
	void UpdateYAxis(float heightPixels);
	void DrawGraticule(ImVec2 pos, ImVec2 size);
	void DrawGroundMarkers(ImVec2 pos, ImVec2 size);
	void RasterizeTrace(vk::raii::CommandBuffer& cmdBuf, size_t i, UniformAnalogWaveform* data);
	bool EnsureRaster(size_t i);
	std::shared_ptr<ComputePipeline> GetTracePipeline();
	bool AllocateTexture(std::shared_ptr<Texture>& tex, uint32_t w, uint32_t h, const std::string& name);
	void EnsureTraceState();

	ScopeCapture* m_capture;
	TextureManager* m_texmgr;

	///@brief Size of the plot in pixels, excluding the rulers
	uint32_t m_width;
	uint32_t m_height;

	///@brief Time relative to the trigger. Zero is the trigger instant.
	PlotAxis m_xAxis;

	///@brief Volts, for whichever trace is selected. Derived every frame, never edited directly.
	PlotAxis m_yAxis;

	PlotRect m_plotRect;

	///@brief False until the time axis has been fitted to a real record
	bool m_fitted;

	bool m_showGraticule;
	bool m_interactive;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Trace layer

	///@brief Lifted analog rasterizer, created on first use so the int64 probe has run
	std::shared_ptr<ComputePipeline> m_tracePipeline;

	ComputePipeline m_traceToneMapPipeline;

	/**
		@brief fp32 intensity rasters, one per trace, pane sized

		Allocated on first use rather than up front. At sixteen complex ports on a 1600x900 pane
		this is thirty two buffers of 5.8 MB, and a user running two channels should not pay for
		the thirty they did not ask for.

		Held by pointer because AcceleratorBuffer declares a destructor, which suppresses its
		implicit move constructor, so a vector of them cannot grow. SpectrumArea sidesteps this
		with a fixed array; a scope does not know its trace count until the graph is built.
	 */
	std::vector<std::unique_ptr<AcceleratorBuffer<float>>> m_traceRaster;

	///@brief Every trace composited into one texture
	std::shared_ptr<Texture> m_traceTexture;

	///@brief Which traces the composited texture actually holds, as of the last ToneMap()
	uint32_t m_traceMask;

	///@brief Which traces the user wants drawn
	uint32_t m_traceEnabled;

	std::vector<float> m_voltsPerDiv;
	std::vector<float> m_traceOffset;

	///@brief Per trace: true when that raster no longer matches the current mapping
	std::vector<bool> m_rasterStale;

	size_t m_selectedTrace;

	///@brief True when the vertical controls act on every trace. See ApplyVoltsPerDiv().
	bool m_ganged;

	float m_traceAlpha;
	float m_persistDecay;

	/**
		@brief Axis state the raster buffers were last drawn with

		Compared rather than announced, for the reason SpectrumArea.h gives at length: persistence
		accumulates in screen space, and the axis can be moved by the mouse, a fit, a
		reconfiguration or a host, so noticing the change covers all of them and cannot be
		forgotten by the next one added.
	 */
	double m_lastXOffset;
	double m_lastXPixelsPerUnit;
	std::vector<float> m_lastVoltsPerDiv;
	std::vector<float> m_lastOffset;

	///@brief Revision of the waveform each raster was drawn from, so a held record is not redrawn
	std::vector<uint64_t> m_lastRevision;
};

#endif
