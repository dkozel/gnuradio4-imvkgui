/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Correctness checks for the signal path

	These live with the benchmark rather than in the application. They were originally run
	from sigmf-spectrum's command line, which meant the shipped binary carried a test suite
	and a set of flags nobody using it would ever type. A build that links is still not
	evidence of a correct FFT, so nothing here was dropped - it moved to the tool whose job
	is to answer that question.
 */
#ifndef Verify_h
#define Verify_h

#include "PlayerSession.h"

#include <string>
#include <vector>

/**
	@brief Checks ComplexFFTFilter against synthesized tones at known offsets

	Needs no recording. Both a positive and a negative frequency offset are checked; a
	positive-only test passes even when I and Q are swapped or the fftshift runs backwards.
 */
bool VerifyComplexFFTFilter();

/**
	@brief Checks the packed I/Q datapath against the planar float one

	Needs no recording: the same tone is synthesized twice, once as ci16 words for the fused
	unpack/window shader and once as the quantized floats those words decode to for the
	planar path, and the two spectra are required to agree. Also exercises a multi-transform
	block, which is the only thing that covers the batched dispatch's per-transform window
	indexing.
 */
bool VerifyPackedIQPath();

/**
	@brief Checks that a recording of a known tone decodes to the right frequency

	@param path			Path to the .sigmf-meta file
	@param expectedHz	Frequency the recording is documented to contain
 */
bool VerifySigMFSource(const std::string& path, double expectedHz);

/**
	@brief Opens each recording and reports what was found

	Not a pass/fail check so much as a way to point the source at a whole dataset and see
	whether any of it fails to open or produces no spectrum.
 */
bool SurveyRecordings(const std::vector<std::string>& paths);

/**
	@brief Checks that batching a block into N transforms matches N separate transforms
 */
bool VerifyBatchedFFT(PlayerSession& session, int64_t fftLength);

/**
	@brief Checks the density histogram, its column totals, and the traces derived from it
 */
bool VerifySpectrumDensity(PlayerSession& session, int64_t fftLength, int64_t blockSize);

/**
	@brief Checks SpectrumEngine end to end from a raw IQ span

	Needs no recording. Synthesizes the same tone in both supported input types and requires
	each to land in the right bin at the right level, which is the only thing that exercises
	IqInjector's claim that a std::complex push is a memcpy: if the layout assumption were
	wrong the tone would decode as noise, and if I and Q were swapped it would decode at the
	mirrored bin, which a positive-only offset would not catch.

	Also checks that the engine's parts mask actually omits what it says it does, and that a
	waterfall row appears after the configured number of spectra rather than immediately.
 */
bool VerifySpectrumEngine();

#endif
