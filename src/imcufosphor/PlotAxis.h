/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of PlotAxis and the ruler drawing helpers
 */
#ifndef PlotAxis_h
#define PlotAxis_h

#include "ngscopeclient.h"

#include "../../lib/scopehal/scopehal/Unit.h"

/**
	@brief The mapping between one axis of a plot and pixels on screen

	Split out from the areas that use it for two reasons. It is the same arithmetic for
	frequency, amplitude and time, so writing it three times invites three different
	off-by-one errors; and making it an object rather than a pair of members is what lets two
	panes share one instance and be linked by construction rather than by remembering to copy
	values between them.

	Modeled on the transform WaveformGroup exposes (WaveformGroup.h:95-118), with the same
	names, so the two are recognisably the same thing.

	@par Precision

	Offsets are doubles, which carry 53 bits of mantissa. The worst case here is the
	frequency axis in microhertz: the top of the 2.4 GHz band is 2.5e15 uHz against an exact
	integer limit of 9.0e15, so every representable frequency in range is exact. Anything
	pushing past that - a terahertz axis in microhertz, say - would need int64 here and a
	rework of the ruler arithmetic.

	Note that this is about the axis, not the shaders. The trace rasterizer still needs the
	trigger phase split into an int64 sample count and a float remainder, because it does the
	arithmetic in float; see SpectrumArea::RasterizeTrace.
 */
class PlotAxis
{
public:
	PlotAxis(Unit unit = Unit(Unit::UNIT_COUNTS));

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Transforms

	///@brief Converts a distance in axis units to a distance in pixels
	double UnitsToPixels(double units) const
	{ return units * m_pixelsPerUnit; }

	///@brief Converts a distance in pixels to a distance in axis units
	double PixelsToUnits(double pixels) const
	{ return (m_pixelsPerUnit != 0) ? (pixels / m_pixelsPerUnit) : 0; }

	/**
		@brief Converts an absolute axis position to an offset in pixels from the plot origin

		The origin is the left edge for a horizontal axis and the bottom edge for a vertical
		one, so both count in the direction the axis increases. Flipping to ImGui's
		top-left-origin screen coordinates is the caller's job, and is done in exactly one
		place per area.
	 */
	double UnitsToPosition(double units) const
	{ return UnitsToPixels(units - m_offset); }

	///@brief Converts an offset in pixels from the plot origin to an absolute axis position
	double PositionToUnits(double pixels) const
	{ return m_offset + PixelsToUnits(pixels); }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// State

	///@brief Axis value at the plot origin
	double GetOffset() const
	{ return m_offset; }

	void SetOffset(double offset)
	{ m_offset = offset; }

	double GetPixelsPerUnit() const
	{ return m_pixelsPerUnit; }

	void SetPixelsPerUnit(double p)
	{ m_pixelsPerUnit = p; }

	const Unit& GetUnit() const
	{ return m_unit; }

	void SetUnit(Unit unit)
	{ m_unit = unit; }

	///@brief Axis value at the far end of a plot @a lengthPixels long
	double GetEnd(double lengthPixels) const
	{ return PositionToUnits(lengthPixels); }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Manipulation

	/**
		@brief Sets offset and scale so that [lo, hi] exactly fills @a lengthPixels

		@return False if the range or the length is degenerate, in which case nothing changes
	 */
	bool FitRange(double lo, double hi, double lengthPixels);

	///@brief Shifts the view by a pixel distance, positive moving the view toward higher values
	void PanPixels(double pixels)
	{ m_offset += PixelsToUnits(pixels); }

	/**
		@brief Scales about a fixed point, so the value under the cursor stays under it

		@param factor		Multiplier on pixels per unit. Above one zooms in.
		@param aboutPixels	Position to hold fixed, in pixels from the plot origin
	 */
	void ZoomAbout(double factor, double aboutPixels);

protected:
	double m_offset;
	double m_pixelsPerUnit;
	Unit m_unit;
};

/**
	@brief One tick on a ruler

	Generated separately from the drawing so that a caller who wants gridlines, a cursor
	readout or a test can have the tick positions without a draw list.
 */
struct AxisTick
{
	///@brief Position in pixels from the plot origin
	double position;

	///@brief Value at this tick, in axis units
	double value;

	///@brief True for a labelled tick, false for the unlabelled ones between
	bool major;
};

/**
	@brief Generates tick positions for an axis spanning @a lengthPixels

	Graduations are rounded to 1, 2 or 5 times a power of ten, which gives labels a person
	can read. WaveformGroup rounds on a base-5 log with a separate divisor for sub-second
	time units (WaveformGroup.cpp:1052-1059); that exists to make nanoseconds and picoseconds
	land on round numbers, and buys nothing for the decade-friendly units here.

	@param axis				Axis to generate ticks for
	@param lengthPixels		Length of the plot along this axis
	@param minLabelSpacing	Smallest gap between labelled ticks, in pixels
	@param ticks			Output, cleared first
 */
void GenerateAxisTicks(
	const PlotAxis& axis,
	double lengthPixels,
	double minLabelSpacing,
	std::vector<AxisTick>& ticks);

/**
	@brief Draws a horizontal ruler with the axis increasing to the right

	@param axis		Axis to draw
	@param pos		Top left corner, in screen coordinates
	@param size		Extent of the ruler. Ticks hang down from the top edge.
 */
void DrawHorizontalRuler(const PlotAxis& axis, ImVec2 pos, ImVec2 size);

/**
	@brief Draws a vertical ruler with the axis increasing upward

	Ticks and labels sit against the left edge, so this is meant for a gutter to the right of
	the plot it describes - the side ngscopeclient puts its Y axis on
	(WaveformGroup.cpp:250-251).

	@param axis		Axis to draw
	@param pos		Top left corner, in screen coordinates
	@param size		Extent of the ruler
 */
void DrawVerticalRuler(const PlotAxis& axis, ImVec2 pos, ImVec2 size);

///@brief Width to reserve for a vertical ruler, matching WaveformGroup::GetYAxisWidth()
float GetVerticalRulerWidth();

///@brief Height to reserve for a horizontal ruler
float GetHorizontalRulerHeight();

#endif
