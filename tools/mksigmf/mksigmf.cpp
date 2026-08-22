/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Writes a synthetic SigMF recording, so the verification suite needs no test data

	wfbench --verify runs six checks and four of them synthesize their own input. The other
	two - VerifySigMFSource and the density/batched checks driven through a PlayerSession -
	need a recording on disk, and until now the only recordings were seven files totalling
	about 7 GB under /data/deepsig/datasets/demo. That made the whole suite unrunnable on any
	other machine, which is why it was never wired into ctest.

	This writes a 4 MiB recording with one known tone in it. It links nothing: the metadata is
	a small JSON document written literally rather than through libsigmf, and the samples are
	a complex exponential. No Vulkan, no scopehal, no libsigmf, so it builds and runs
	anywhere and cannot itself be the reason a test fails.

	@par Why the tone is at +125 kHz

	VerifySigMFSource transforms the recording at 65536 points and requires the peak within
	one bin of where it was told to expect it (Verify.cpp:752-768). A tone that falls between
	bins would still pass - scalloping puts the peak at most half a bin away - but it would
	make the test's margin depend on arithmetic nobody wants to redo. At 1 MS/s, 125 kHz is
	bin 8192 of 65536 and bin 1024 of 8192, exactly, so the tone sits on a bin centre at every
	transform length the suite uses and the expected answer is a round number.
 */

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

///@brief Sample rate of the generated recording
static const double g_sampleRate = 1e6;

///@brief Center frequency reported in the capture segment
static const double g_centerHz = 100e6;

/**
	@brief Tone offset from the center frequency

	Bin exact at every power of two transform length from 8192 up; see the file comment.
 */
static const double g_toneOffsetHz = 125e3;

///@brief Complex samples written. 2^20 at 4 bytes each is 4 MiB.
static const int64_t g_numSamples = 1048576;

/**
	@brief Tone amplitude as a fraction of full scale

	Half scale rather than full: the level is not what any check looks at, and leaving
	headroom means a future amplitude or windowing change cannot turn this into a clipping
	bug that presents as a spectrum artifact.
 */
static const double g_amplitude = 0.5;

/**
	@brief Writes the .sigmf-meta document

	Hand-written rather than built through libsigmf. The metadata this needs is four fields
	and a capture segment, and going through the library would make a test fixture depend on
	the same parsing stack the test exercises.

	@return True on success
 */
static bool WriteMetadata(const string& path)
{
	FILE* fp = fopen(path.c_str(), "w");
	if(!fp)
	{
		fprintf(stderr, "mksigmf: could not open %s for writing: %s\n", path.c_str(), strerror(errno));
		return false;
	}

	//The annotation is not incidental: BuildAnnotationSet() (SigMFSource.cpp:536) is on the
	//open path, so a recording with no annotations would leave it untested.
	fprintf(fp,
		"{\n"
		"    \"global\": {\n"
		"        \"core:datatype\": \"ci16_le\",\n"
		"        \"core:sample_rate\": %.1f,\n"
		"        \"core:num_channels\": 1,\n"
		"        \"core:version\": \"1.1.0\",\n"
		"        \"core:recorder\": \"imcufosphor mksigmf\",\n"
		"        \"core:description\": \"Synthetic %.0f kHz tone, generated for wfbench --verify\"\n"
		"    },\n"
		"    \"captures\": [\n"
		"        {\n"
		"            \"core:sample_start\": 0,\n"
		"            \"core:frequency\": %.1f\n"
		"        }\n"
		"    ],\n"
		"    \"annotations\": [\n"
		"        {\n"
		"            \"core:sample_start\": 0,\n"
		"            \"core:sample_count\": %" PRId64 ",\n"
		"            \"core:freq_lower_edge\": %.1f,\n"
		"            \"core:freq_upper_edge\": %.1f,\n"
		"            \"core:label\": \"tone\"\n"
		"        }\n"
		"    ]\n"
		"}\n",
		g_sampleRate,
		g_toneOffsetHz / 1e3,
		g_centerHz,
		g_numSamples,
		g_centerHz + g_toneOffsetHz - 5e3,
		g_centerHz + g_toneOffsetHz + 5e3);

	if(fclose(fp) != 0)
	{
		fprintf(stderr, "mksigmf: error closing %s: %s\n", path.c_str(), strerror(errno));
		return false;
	}
	return true;
}

/**
	@brief Writes the .sigmf-data file as interleaved little endian int16 IQ

	@return True on success
 */
static bool WriteSamples(const string& path)
{
	FILE* fp = fopen(path.c_str(), "wb");
	if(!fp)
	{
		fprintf(stderr, "mksigmf: could not open %s for writing: %s\n", path.c_str(), strerror(errno));
		return false;
	}

	//Phase is accumulated as an exact rational turn count rather than by adding a step to a
	//double, so the last sample is as precise as the first. Over a million samples a naive
	//accumulator drifts enough to smear the bin this test is checking.
	const double scale = g_amplitude * 32767.0;
	const int64_t chunk = 65536;
	vector<int16_t> buf;
	buf.reserve(chunk * 2);

	for(int64_t base = 0; base < g_numSamples; base += chunk)
	{
		int64_t n = min(chunk, g_numSamples - base);
		buf.clear();

		for(int64_t i = 0; i < n; i++)
		{
			//fmod keeps the argument small, so cos/sin are evaluated where they are accurate
			double turns = fmod((base + i) * g_toneOffsetHz / g_sampleRate, 1.0);
			double phase = 2.0 * M_PI * turns;
			buf.push_back(static_cast<int16_t>(lrint(scale * cos(phase))));
			buf.push_back(static_cast<int16_t>(lrint(scale * sin(phase))));
		}

		if(fwrite(buf.data(), sizeof(int16_t), buf.size(), fp) != buf.size())
		{
			fprintf(stderr, "mksigmf: short write to %s: %s\n", path.c_str(), strerror(errno));
			fclose(fp);
			return false;
		}
	}

	if(fclose(fp) != 0)
	{
		fprintf(stderr, "mksigmf: error closing %s: %s\n", path.c_str(), strerror(errno));
		return false;
	}
	return true;
}

int main(int argc, char* argv[])
{
	if(argc != 2)
	{
		fprintf(stderr,
			"mksigmf: writes a synthetic SigMF recording for the verification suite\n"
			"Usage: mksigmf BASENAME\n"
			"\n"
			"Writes BASENAME.sigmf-meta and BASENAME.sigmf-data: %" PRId64 " complex int16\n"
			"samples at %.0f MS/s, centered at %.0f MHz, with a tone at %+.0f kHz\n"
			"(absolute %.0f Hz).\n",
			g_numSamples,
			g_sampleRate / 1e6,
			g_centerHz / 1e6,
			g_toneOffsetHz / 1e3,
			g_centerHz + g_toneOffsetHz);
		return 1;
	}

	string base(argv[1]);
	if(!WriteMetadata(base + ".sigmf-meta"))
		return 1;
	if(!WriteSamples(base + ".sigmf-data"))
		return 1;

	printf("mksigmf: wrote %s.sigmf-{meta,data}, tone at %.0f Hz\n",
		base.c_str(), g_centerHz + g_toneOffsetHz);
	return 0;
}
