/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of SpectrumReducer
 */
#ifndef SpectrumReducer_h
#define SpectrumReducer_h

#include "../../lib/scopehal/scopehal/scopehal.h"
#include "../../lib/scopehal/scopehal/Filter.h"
#include "../../lib/scopehal/scopehal/ComputePipeline.h"

/**
	@brief Push constants for shaders/SpectrumReduce.spv
 */
struct SpectrumReduceArgs
{
	uint32_t nbins;
	uint32_t nspectra;
	uint32_t op;
	uint32_t mode;
	uint32_t seed;
	float scale;
};

/**
	@brief Combines several spectra into one

	Exists because upstream's Waterfall advances exactly one row per filter graph execution
	(Waterfall.cpp:144) and has no rate control of its own. Without something in between,
	RBW, waterfall line rate and playback speed have only two degrees of freedom: see
	DESIGN.md section 8.1 for why that is a bad trade on file playback.

	Accumulates GroupSize() input spectra and emits one output. Feed this rather than the
	FFT straight into the Waterfall, and drive it from PlayerSession.

	Unlike a normal filter this does not produce a new output on every Refresh(). Check
	IsOutputReady() after each call; it is true only on the execution that completed a
	group, which is when the waterfall should be advanced.

	@par Batched input

	The input is one ComplexFFTFilter output waveform, which holds a whole block of spectra
	back to back. "Bins per spectrum" says how long each one is; zero means the input is a
	single spectrum, which is how this filter behaved before batching.

	A group therefore ends on a block boundary, not on an exact multiple of the group size:
	the reducer folds in every spectrum in a block and emits once the total reaches
	GroupSize(). A row can consequently represent more spectra than GroupSize() asked for -
	GetEffectiveGroupSize() reports how many it actually took. The alternative, stopping
	mid-block, would drop the rest of the block and break gap-free coverage, which is the
	one thing this pipeline exists to provide. At the rates this matters for, a group spans
	several blocks anyway: 245.76 MS/s at 8192 points is 30 000 spectra/s against a display
	that can show a few tens of rows per second.

	@par Phase 3
	The accumulator is deliberately a separate buffer rather than the output waveform, so it
	can widen from one float per frequency bin to a hit count per (frequency bin x amplitude
	cell) without disturbing this interface. That 2D form is a true RTSA density map
	(DESIGN.md section 10).
 */
class SpectrumReducer : public Filter
{
public:
	SpectrumReducer(const std::string& color);
	virtual ~SpectrumReducer();

	///@brief How successive spectra are combined
	enum ReduceMode
	{
		///@brief Strongest value per bin. Never loses a burst, so this is the default.
		MODE_MAX_HOLD,

		///@brief Mean of the dBm values, i.e. video averaging. Smooths noise, hides bursts.
		MODE_AVERAGE
	};

	//Shader opcodes, must match shaders/SpectrumReduce.glsl
	enum Op
	{
		OP_ACCUM = 0,
		OP_FINALIZE = 1
	};

	static std::string GetProtocolName();

	virtual void Refresh(vk::raii::CommandBuffer& cmdBuf, std::shared_ptr<QueueHandle> queue) override;
	virtual void ClearSweeps() override;

	virtual float GetVoltageRange(size_t stream) override;
	virtual void SetVoltageRange(float range, size_t stream) override;
	virtual float GetOffset(size_t stream) override;
	virtual void SetOffset(float offset, size_t stream) override;

	/**
		@brief True if the most recent Refresh() completed a group

		The waterfall should be advanced exactly when this is true.
	 */
	bool IsOutputReady() const
	{ return m_outputReady; }

	///@brief Number of input spectra requested per output row
	int64_t GetGroupSize();

	/**
		@brief Spectra actually combined into the most recently emitted row

		Differs from GetGroupSize() when a block overshoots the group boundary. Zero until
		the first row is emitted.
	 */
	int64_t GetEffectiveGroupSize() const
	{ return m_lastGroupSize; }

	///@brief Bins in one input spectrum, or zero if the input is a single spectrum
	int64_t GetBinsPerSpectrum();

	void SetBinsPerSpectrum(int64_t bins)
	{ m_binsPerSpectrum.SetIntVal(bins); }

	PROTOCOL_DECODER_INITPROC(SpectrumReducer)

protected:
	///@brief Number of spectra accumulated so far in the current group
	int64_t m_accumulated;

	///@brief Spectra that went into the most recently emitted row
	int64_t m_lastGroupSize;

	///@brief True if the last Refresh() emitted an output
	bool m_outputReady;

	///@brief Accumulator, one float per frequency bin
	AcceleratorBuffer<float> m_accumulator;

	///@brief Number of bins the accumulator is sized for
	size_t m_cachedBins;

	ComputePipeline m_computePipeline;

	FilterParameter& m_mode;
	FilterParameter& m_groupSize;

	///@brief Bins in one input spectrum, or zero if the whole input is one spectrum
	FilterParameter& m_binsPerSpectrum;

	float m_range;
	float m_offset;
};

#endif
