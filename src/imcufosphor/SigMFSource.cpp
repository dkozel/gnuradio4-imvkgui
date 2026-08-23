/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of SigMFSource
 */

#include "SigMFSource.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Metadata value limits
//
// A .sigmf-meta file is untrusted input: it is a JSON document that may have been written by
// anything. Everything below is eventually divided by, or scaled into an int64 microhertz
// axis, so a value that is merely implausible here becomes undefined behaviour three filters
// downstream rather than a bad-looking display. Check once, at load, and say so on the
// console; the rest of the pipeline may then assume the numbers are usable.

///@brief Slowest sample rate we will accept, Hz. Below this the femtoseconds per sample overflows int64.
static const double g_minSampleRate = 1e-3;

///@brief Fastest sample rate we will accept, Hz. Above this the microhertz bin spacing overflows int64.
static const double g_maxSampleRate = 1e12;

/**
	@brief Sample rate substituted when the metadata does not supply a usable one

	core:sample_rate is optional in the SigMF spec, so a recording without one is legal and
	must still open. Every consumer divides by the rate, though, so it cannot be left at zero:
	that is what made the frequency axis read -4.13 THz. One megahertz is an arbitrary but
	bounded stand-in, and the warning says the axis is not to scale.
 */
static const double g_defaultSampleRate = 1e6;

///@brief Largest center frequency magnitude we will accept, Hz. Above this, Hz*1e6 overflows int64.
static const double g_maxCenterFrequency = 1e12;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SigMFSampleFormat

bool SigMFSampleFormat::Parse(const string& str, string& errorOut)
{
	//Grammar is (c|r)(f|i|u)(8|16|32|64)(_le|_be)?
	if(str.size() < 3)
	{
		errorOut = "datatype \"" + str + "\" is too short to be a SigMF datatype";
		return false;
	}

	size_t pos = 0;

	switch(str[pos])
	{
		case 'c':
			m_complex = true;
			break;

		case 'r':
			m_complex = false;
			break;

		default:
			errorOut = "datatype \"" + str + "\" must begin with 'c' or 'r'";
			return false;
	}
	pos++;

	switch(str[pos])
	{
		case 'f':
			m_kind = KIND_FLOAT;
			break;

		case 'i':
			m_kind = KIND_SIGNED;
			break;

		case 'u':
			m_kind = KIND_UNSIGNED;
			break;

		default:
			errorOut = "datatype \"" + str + "\" must specify 'f', 'i' or 'u'";
			return false;
	}
	pos++;

	//Bit width runs to the end or to the endianness suffix
	size_t suffix = str.find('_', pos);
	string bitstr = (suffix == string::npos) ? str.substr(pos) : str.substr(pos, suffix - pos);
	m_bits = atoi(bitstr.c_str());
	switch(m_bits)
	{
		case 8:
		case 16:
		case 32:
		case 64:
			break;

		default:
			errorOut = "datatype \"" + str + "\" has unsupported width " + bitstr;
			return false;
	}

	//Floats are only defined at 32 and 64 bits
	if( (m_kind == KIND_FLOAT) && (m_bits < 32) )
	{
		errorOut = "datatype \"" + str + "\" is a float narrower than 32 bits";
		return false;
	}

	//Endianness. Absent is legal for 8-bit types where it is meaningless.
	m_littleEndian = true;
	if(suffix != string::npos)
	{
		string end = str.substr(suffix + 1);
		if(end == "le")
			m_littleEndian = true;
		else if(end == "be")
			m_littleEndian = false;
		else
		{
			errorOut = "datatype \"" + str + "\" has unrecognized endianness \"" + end + "\"";
			return false;
		}
	}
	else if(m_bits > 8)
	{
		//The spec requires an endianness suffix above 8 bits, but be lenient and assume
		//little endian rather than refusing to open the file
		LogWarning("SigMF datatype \"%s\" has no endianness suffix, assuming little endian\n", str.c_str());
	}

	return true;
}

bool SigMFSampleFormat::ToPackedFormat(PackedIQFormat& formatOut, float& biasOut, float& scaleOut) const
{
	//A real-valued recording has no Q to interleave with, so there is nothing for the complex
	//unpack shader to do with it
	if(!m_complex)
		return false;

	//The shader loads whole 32-bit words with the device's byte order, so the file's has to
	//match the host's. Below 8 bits per component the question does not arise.
	constexpr bool hostLittleEndian = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
	if( (m_bits > 8) && (m_littleEndian != hostLittleEndian) )
		return false;

	switch(m_kind)
	{
		case KIND_FLOAT:
			//fp64 in a shader would need a feature bit for a format nothing in the demo
			//dataset uses, and would halve throughput where it is supported
			if(m_bits != 32)
				return false;
			formatOut = PACKED_IQ_FLOAT32;
			biasOut = 0;
			scaleOut = 1;
			return true;

		//Both integer kinds normalize to +/- 1.0 full scale, and the unsigned ones are offset
		//binary with midscale at zero. Identical to what ConvertSamples() does, deliberately:
		//a recording must read the same however it got to the GPU.
		case KIND_SIGNED:
		case KIND_UNSIGNED:
		{
			const bool isSigned = (m_kind == KIND_SIGNED);
			switch(m_bits)
			{
				case 8:
					formatOut = isSigned ? PACKED_IQ_INT8 : PACKED_IQ_UINT8;
					biasOut = isSigned ? 0.0f : -128.0f;
					scaleOut = 1.0f / 127.0f;
					return true;

				case 16:
					formatOut = isSigned ? PACKED_IQ_INT16 : PACKED_IQ_UINT16;
					biasOut = isSigned ? 0.0f : -32768.0f;
					scaleOut = 1.0f / 32767.0f;
					return true;

				case 32:
					formatOut = isSigned ? PACKED_IQ_INT32 : PACKED_IQ_UINT32;
					biasOut = isSigned ? 0.0f : -2147483648.0f;
					scaleOut = 1.0f / 2147483647.0f;
					return true;

				//64-bit would need shaderInt64 to extract and would lose precision on the
				//way into a float anyway
				default:
					return false;
			}
		}

		default:
			return false;
	}
}

string SigMFSampleFormat::ToString() const
{
	string ret = m_complex ? "complex " : "real ";
	switch(m_kind)
	{
		case KIND_FLOAT:
			ret += "float";
			break;

		case KIND_SIGNED:
			ret += "int";
			break;

		case KIND_UNSIGNED:
			ret += "uint";
			break;
	}
	ret += to_string(m_bits);
	if(m_bits > 8)
		ret += m_littleEndian ? " LE" : " BE";
	return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

SigMFSource::SigMFSource(const string& metaPath, const string& dataPath)
	: m_valid(false)
	, m_fd(-1)
	, m_dataStartByte(0)
	, m_totalSamples(0)
	, m_sampleRate(0)
	, m_playCursor(0)
	, m_samplesPlayed(0)
	, m_blockSize(65536)
	, m_looping(true)
	, m_atEnd(false)
	, m_triggerArmed(false)
	, m_triggerOneShot(false)
	, m_icap(nullptr)
	, m_qcap(nullptr)
	, m_packedCap(nullptr)
	, m_usePackedPath(false)
	, m_packedPathSupported(false)
	, m_packedPathAllowed(true)
	, m_packedFormat(PACKED_IQ_INT16)
	, m_packedBias(0)
	, m_packedScale(1.0f / 32767.0f)
	, m_startTimestamp(0)
	, m_startFemtoseconds(0)
	, m_tRead(0)
	, m_tConvert(0)
	, m_samplesDelivered(0)
{
	m_nickname = "sigmf";
	m_recordingName = "SigMF recording";

	//One complex channel, which gives us I, Q and the center frequency scalar.
	//
	//Null scope pointer, as IqInjector.cpp:34 does: there is no instrument behind a file, and
	//nothing downstream asks for one. OscilloscopeChannel only dereferences it in the
	//per-channel hardware accessors (coupling, attenuation, deskew, voltage range), none of
	//which anything here calls - every GetVoltageRange() in the display is on a filter.
	m_chan = std::make_unique<ComplexChannel>(
		nullptr,
		"RX",
		"#4040ff",
		Unit(Unit::UNIT_FS),
		Unit(Unit::UNIT_VOLTS),
		0);
	m_chan->SetDefaultDisplayName();

	LoadMetadata(metaPath, dataPath);
}

SigMFSource::~SigMFSource()
{
	if(m_fd >= 0)
		close(m_fd);
}

void SigMFSource::LoadMetadata(const string& metaPath, const string& dataPath)
{
	//Parse the metadata.
	//
	//Note we do not use sigmf::metadata_file_to_json(): it is not a template and hardcodes
	//a record type of core+antenna+capture_details+signal, which would dictate ours.
	try
	{
		ifstream fp(metaPath);
		if(!fp)
		{
			m_errorMessage = "could not open metadata file " + metaPath;
			return;
		}
		ostringstream buf;
		buf << fp.rdbuf();
		m_record = nlohmann::json::parse(buf.str());
	}
	catch(const exception& e)
	{
		m_errorMessage = string("could not parse metadata: ") + e.what();
		return;
	}

	auto& global = m_record.global.get<sigmf::core::DescrT>();

	//Sample format
	if(global.datatype.empty())
	{
		m_errorMessage = "metadata has no core:datatype";
		return;
	}
	if(!m_format.Parse(global.datatype, m_errorMessage))
		return;
	if(!m_format.IsComplex())
	{
		m_errorMessage = "datatype \"" + global.datatype + "\" is real; only complex IQ is supported";
		return;
	}

	//Multi-channel recordings interleave differently and we have nowhere to put the extra
	//channels, so refuse rather than silently misinterpreting the data
	if(global.num_channels.has_value() && (global.num_channels.value() > 1))
	{
		m_errorMessage = "core:num_channels is " + to_string(global.num_channels.value()) +
			"; only single channel recordings are supported";
		return;
	}

	//Sample rate is optional in the spec, and a missing one is not fatal - it only means the
	//frequency axis has no scale. It cannot be left at zero, though: AcquireData() would then
	//emit a timescale of one femtosecond per sample, an implied 1 PHz rate, and the bin
	//spacing computed from it overflows int64 in ComplexFFTFilter. Substitute a bounded
	//default instead, and say which.
	if(!global.sample_rate.has_value())
	{
		m_sampleRate = g_defaultSampleRate;
		LogWarning("SigMF recording has no core:sample_rate; assuming %g Hz. "
			"The frequency axis will not be to scale.\n", m_sampleRate);
	}
	else if(!isfinite(global.sample_rate.value()) ||
		(global.sample_rate.value() < g_minSampleRate) ||
		(global.sample_rate.value() > g_maxSampleRate))
	{
		//Note that this also catches a negative rate, which was previously indistinguishable
		//from an absent one and silently became zero
		LogError("SigMF core:sample_rate is %g Hz, outside the supported range %g to %g Hz; "
			"assuming %g Hz instead\n",
			global.sample_rate.value(), g_minSampleRate, g_maxSampleRate, g_defaultSampleRate);
		m_sampleRate = g_defaultSampleRate;
	}
	else
		m_sampleRate = global.sample_rate.value();

	//Center frequencies are scaled into an int64 microhertz axis downstream, so one that
	//cannot fit is a range error rather than an exotic band. Checked here, once per recording,
	//rather than in GetExactCenterFrequency(), which runs once per acquisition and would
	//repeat the message a thousand times a second. That accessor clamps silently.
	for(size_t i=0; i<m_record.captures.size(); i++)
	{
		auto& c = m_record.captures[i].get<sigmf::core::DescrT>();
		if(!c.frequency.has_value())
			continue;

		double f = c.frequency.value();
		if(!isfinite(f) || (fabs(f) > g_maxCenterFrequency))
		{
			LogError("SigMF capture %zu has core:frequency %g Hz, outside the supported range "
				"+/- %g Hz; treating it as 0 Hz\n", i, f, g_maxCenterFrequency);
		}
	}

	//core:metadata_only means there is deliberately no data file
	if(global.metadata_only.has_value() && global.metadata_only.value())
	{
		m_errorMessage = "recording is marked core:metadata_only and has no sample data";
		return;
	}

	//Locate the data file. SigMF pairs by basename, but that is not always right: the demo
	//dataset has IQ_800MHz-omnisig.sigmf-meta, a second annotation set for
	//IQ_800MHz.sigmf-data. Hence the explicit override.
	m_dataPath = dataPath;
	if(m_dataPath.empty())
	{
		const string metaExt = ".sigmf-meta";
		if( (metaPath.size() > metaExt.size()) &&
			(metaPath.compare(metaPath.size() - metaExt.size(), metaExt.size(), metaExt) == 0) )
		{
			m_dataPath = metaPath.substr(0, metaPath.size() - metaExt.size()) + ".sigmf-data";
		}
		else
			m_dataPath = metaPath + ".sigmf-data";
	}

	m_fd = open(m_dataPath.c_str(), O_RDONLY);
	if(m_fd < 0)
	{
		m_errorMessage = "could not open data file " + m_dataPath +
			" (pass an explicit data path if the basename does not match)";
		return;
	}

	struct stat st;
	if(0 != fstat(m_fd, &st))
	{
		m_errorMessage = "could not stat data file " + m_dataPath;
		return;
	}

	//Skip any header the first capture declares.
	//
	//header_bytes is a uint64 in the generated metadata type, so a negative value in the JSON
	//wraps to something near 2^64 and then narrows to a negative int64. One bound catches
	//both that and a header that simply runs past the end of the file: either way the offset
	//is unusable, and left alone it inflates m_totalSamples and makes every pread() fail.
	if(!m_record.captures.empty())
	{
		auto& c0 = m_record.captures[0].get<sigmf::core::DescrT>();
		if(c0.header_bytes.has_value())
		{
			uint64_t hdr = c0.header_bytes.value();
			if(hdr > static_cast<uint64_t>(st.st_size))
			{
				LogError("SigMF core:header_bytes is %" PRIu64 ", which is negative or past the "
					"end of a %" PRId64 " byte data file; assuming 0\n",
					hdr, static_cast<int64_t>(st.st_size));
				hdr = 0;
			}
			m_dataStartByte = static_cast<int64_t>(hdr);
		}
	}

	int64_t usableBytes = st.st_size - m_dataStartByte;
	if(usableBytes <= 0)
	{
		m_errorMessage = "data file " + m_dataPath + " contains no samples";
		return;
	}
	m_totalSamples = usableBytes / m_format.BytesPerSample();
	if(m_totalSamples <= 0)
	{
		m_errorMessage = "data file " + m_dataPath + " is shorter than one sample";
		return;
	}

	//The recording's own idea of when it was taken, one epoch per capture segment.
	//
	//core:datetime is what makes an absolute timestamp possible at all. Without it the most
	//anything downstream can honestly say is how far into the recording a sample sits, and the
	//clock reports that rather than inventing a wall clock.
	m_clock.Clear();
	for(size_t i=0; i<m_record.captures.size(); i++)
	{
		auto& c = m_record.captures[i].get<sigmf::core::DescrT>();
		m_clock.AddCapture(static_cast<int64_t>(c.sample_start.value_or(0)), c.datetime);
	}
	m_clock.Finalize(m_sampleRate);

	//Annotations, converted out of libsigmf's types once here so that nothing downstream has
	//to see them. See BuildAnnotationSet() for what gets dropped and why.
	BuildAnnotationSet(m_record, m_totalSamples, m_annotations);

	//Waveform timestamps, which scopehal wants populated whether or not the real time is
	//known. Seed them from the metadata when it is there and from the data file's mtime when
	//it is not.
	//
	//The mtime is a guess, and it is allowed to reach scopehal's fields and nowhere else.
	//dect6.sigmf-meta was recorded in 2022 and misspells its key as "core::datetime" with two
	//colons, so it parses as absent; labelling a display from the mtime would confidently date
	//that recording to whenever the file was last copied. Anything user-facing asks
	//GetClock() instead, which says when it does not know.
	auto t0 = m_clock.TimeOfSample(0);
	if(t0.absolute)
	{
		m_startTimestamp = t0.sec;
		m_startFemtoseconds = t0.fs;
	}
	else
		GetTimestampOfFile(m_dataPath, m_startTimestamp, m_startFemtoseconds);

	m_recordingName = BaseName(m_dataPath);
	m_valid = true;

	//Decide once, at open, whether this recording's bytes can go to the GPU as they are.
	//AcquireData() branches on the answer for every block, and the two paths allocate
	//different waveforms, so it must not change underneath them.
	m_packedPathSupported = m_format.ToPackedFormat(m_packedFormat, m_packedBias, m_packedScale);
	m_usePackedPath = m_packedPathSupported && m_packedPathAllowed;
	LogTrace("Sample ingest path: %s\n",
		m_usePackedPath
			? "packed (file bytes to GPU unconverted)"
			: "CPU conversion to planar float");

	LogTrace("Opened SigMF recording %s: %s, %" PRId64 " samples at %g Hz, %zu captures, "
		"%zu annotations, %s\n",
		m_dataPath.c_str(),
		m_format.ToString().c_str(),
		m_totalSamples,
		m_sampleRate,
		m_record.captures.size(),
		m_record.annotations.size(),
		t0.absolute
			? ("starting " + RecordingClock::Format(t0)).c_str()
			: "no usable core:datetime, times will be relative");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Annotation conversion

void BuildAnnotationSet(const SigMFRecord& rec, int64_t totalSamples, AnnotationSet& out)
{
	out.Clear();

	//get() is non-const in libsigmf, so reading a const record needs this. The alternative is
	//taking a non-const reference here and propagating that all the way up to the caller,
	//which would mean the display could not hold a const source.
	auto& annotations = const_cast<SigMFRecord&>(rec).annotations;

	size_t noStart = 0;
	size_t swapped = 0;
	size_t clamped = 0;

	for(size_t i=0; i<annotations.size(); i++)
	{
		auto& a = annotations[i].get<sigmf::core::DescrT>();

		//An annotation with no core:sample_start cannot be placed on either axis. Skipping it
		//is also what keeps us away from libsigmf's own helper, which continues without
		//advancing its iterator on exactly this input and hangs (sigmf_helpers.h:123-125).
		if(!a.sample_start.has_value())
		{
			noStart++;
			continue;
		}

		Annotation ann;
		ann.sampleStart = static_cast<int64_t>(a.sample_start.value());
		if(ann.sampleStart < 0)
		{
			noStart++;
			continue;
		}

		//core:sample_count is optional. Absent means a point in time rather than a span, which
		//the model represents as a zero length annotation and the overlay draws as a line.
		if(a.sample_count.has_value())
		{
			int64_t count = static_cast<int64_t>(a.sample_count.value());
			if(count < 0)
				count = 0;
			ann.sampleEnd = ann.sampleStart + count;
		}
		else
			ann.sampleEnd = ann.sampleStart;

		//Clamp to the recording. An end past the last sample would place a box below the oldest
		//row the waterfall can ever show.
		if( (totalSamples > 0) && (ann.sampleEnd > totalSamples) )
		{
			ann.sampleEnd = totalSamples;
			clamped++;
		}

		//The two frequency edges are independently optional in the schema. Both present is the
		//only case that describes a band; one alone is not half a band, it is no band.
		if(a.freq_lower_edge.has_value() && a.freq_upper_edge.has_value())
		{
			double lo = a.freq_lower_edge.value();
			double hi = a.freq_upper_edge.value();

			if(isfinite(lo) && isfinite(hi))
			{
				//Reversed edges are a data error, not a negative width rectangle
				if(lo > hi)
				{
					std::swap(lo, hi);
					swapped++;
				}

				ann.freqLoHz = lo;
				ann.freqHiHz = hi;
				ann.hasFreq = true;
			}
		}

		ann.label = a.label;
		ann.description = a.description;
		ann.comment = a.comment;
		ann.generator = a.generator;
		ann.colorKey = ColorKeyForLabel(ann.label);

		out.Add(ann);
	}

	out.Finalize();

	LogTrace("Built annotation set: %zu usable of %zu (%zu with no sample_start, "
		"%zu with swapped frequency edges, %zu clamped to the end of the recording)\n",
		out.size(), annotations.size(), noStart, swapped, clamped);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Metadata accessors

size_t SigMFSource::CaptureIndexForSample(int64_t sample) const
{
	//Captures are in ascending sample_start order per the spec, but real files are not
	//guaranteed to honour that, so scan rather than binary search. Capture counts are
	//small - every recording in the demo dataset has exactly one.
	size_t best = 0;
	int64_t bestStart = -1;
	for(size_t i=0; i<m_record.captures.size(); i++)
	{
		auto& c = const_cast<SigMFRecord&>(m_record).captures[i].get<sigmf::core::DescrT>();
		int64_t start = c.sample_start.value_or(0);
		if( (start <= sample) && (start > bestStart) )
		{
			bestStart = start;
			best = i;
		}
	}
	return best;
}

double SigMFSource::GetExactCenterFrequency() const
{
	if(m_record.captures.empty())
		return 0;

	auto idx = CaptureIndexForSample(m_playCursor);
	auto& c = const_cast<SigMFRecord&>(m_record).captures[idx].get<sigmf::core::DescrT>();

	//A center frequency of zero is legitimate - tone_signal.sigmf-meta in the demo dataset
	//is a baseband recording - so absent and zero must not be conflated
	double f = c.frequency.value_or(0.0);

	//Clamp silently: LoadMetadata() has already logged this once, and we are called once per
	//acquisition
	if(!isfinite(f) || (fabs(f) > g_maxCenterFrequency))
		return 0;

	return f;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Playback control

void SigMFSource::SeekToSample(int64_t sample)
{
	if(sample < 0)
		sample = 0;
	if(sample > m_totalSamples)
		sample = m_totalSamples;
	m_playCursor = sample;
	m_atEnd = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sample conversion

/**
	@brief Byteswaps in place if the file endianness does not match ours

	Only called for multi-byte formats whose endianness differs from the host.
 */
static void SwapBytes(uint8_t* p, size_t count, int width)
{
	for(size_t i=0; i<count; i++)
	{
		uint8_t* base = p + i*width;
		for(int j=0; j<width/2; j++)
			std::swap(base[j], base[width - 1 - j]);
	}
}

void SigMFSource::ConvertSamples(
	const uint8_t* raw,
	size_t nsamples,
	UniformAnalogWaveform* idata,
	UniformAnalogWaveform* qdata)
{
	//Components, not samples: one complex sample is two of these
	size_t ncomponents = nsamples * 2;

	//Byteswap first if needed. m_readBuffer is ours, so this is safe to do in place.
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		bool needSwap = !m_format.m_littleEndian && (m_format.m_bits > 8);
	#else
		bool needSwap = m_format.m_littleEndian && (m_format.m_bits > 8);
	#endif
	if(needSwap)
		SwapBytes(const_cast<uint8_t*>(raw), ncomponents, m_format.m_bits / 8);

	switch(m_format.m_kind)
	{
		case SigMFSampleFormat::KIND_FLOAT:
			if(m_format.m_bits == 32)
			{
				auto p = reinterpret_cast<const float*>(raw);
				for(size_t i=0; i<nsamples; i++)
				{
					idata->m_samples[i] = p[i*2];
					qdata->m_samples[i] = p[i*2 + 1];
				}
			}
			else
			{
				auto p = reinterpret_cast<const double*>(raw);
				for(size_t i=0; i<nsamples; i++)
				{
					idata->m_samples[i] = p[i*2];
					qdata->m_samples[i] = p[i*2 + 1];
				}
			}
			break;

		//Integer formats are normalized to +/- 1.0 full scale, matching the convention
		//ComplexImportFilter uses
		case SigMFSampleFormat::KIND_SIGNED:
			switch(m_format.m_bits)
			{
				case 8:
					{
						const float scale = 1.0f / 127.0f;
						auto p = reinterpret_cast<const int8_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = p[i*2] * scale;
							qdata->m_samples[i] = p[i*2 + 1] * scale;
						}
					}
					break;

				case 16:
					{
						const float scale = 1.0f / 32767.0f;
						auto p = reinterpret_cast<const int16_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = p[i*2] * scale;
							qdata->m_samples[i] = p[i*2 + 1] * scale;
						}
					}
					break;

				case 32:
					{
						const float scale = 1.0f / 2147483647.0f;
						auto p = reinterpret_cast<const int32_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = p[i*2] * scale;
							qdata->m_samples[i] = p[i*2 + 1] * scale;
						}
					}
					break;

				default:
					{
						const double scale = 1.0 / 9223372036854775807.0;
						auto p = reinterpret_cast<const int64_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = p[i*2] * scale;
							qdata->m_samples[i] = p[i*2 + 1] * scale;
						}
					}
					break;
			}
			break;

		//Unsigned formats are offset binary: midscale is zero
		case SigMFSampleFormat::KIND_UNSIGNED:
			switch(m_format.m_bits)
			{
				case 8:
					{
						const float scale = 1.0f / 127.0f;
						auto p = reinterpret_cast<const uint8_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = (static_cast<int>(p[i*2]) - 128) * scale;
							qdata->m_samples[i] = (static_cast<int>(p[i*2 + 1]) - 128) * scale;
						}
					}
					break;

				case 16:
					{
						const float scale = 1.0f / 32767.0f;
						auto p = reinterpret_cast<const uint16_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = (static_cast<int>(p[i*2]) - 32768) * scale;
							qdata->m_samples[i] = (static_cast<int>(p[i*2 + 1]) - 32768) * scale;
						}
					}
					break;

				case 32:
					{
						const double scale = 1.0 / 2147483647.0;
						auto p = reinterpret_cast<const uint32_t*>(raw);
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = (static_cast<int64_t>(p[i*2]) - 2147483648LL) * scale;
							qdata->m_samples[i] = (static_cast<int64_t>(p[i*2 + 1]) - 2147483648LL) * scale;
						}
					}
					break;

				default:
					{
						const double scale = 1.0 / 9223372036854775807.0;
						auto p = reinterpret_cast<const uint64_t*>(raw);
						const uint64_t mid = 9223372036854775808ULL;
						for(size_t i=0; i<nsamples; i++)
						{
							idata->m_samples[i] = (static_cast<double>(p[i*2]) - mid) * scale;
							qdata->m_samples[i] = (static_cast<double>(p[i*2 + 1]) - mid) * scale;
						}
					}
					break;
			}
			break;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Acquisition

bool SigMFSource::AcquireData()
{
	if(!m_valid)
		return false;

	//Sample buffers are reused between acquisitions (see below), so exactly one block may be
	//in flight. There are two ways to break that. The first is a queued-but-unpopped
	//waveform: the consumer has not read the buffers we are about to overwrite, so refuse
	//rather than corrupting a block that has already been handed over. The second is a
	//caller that submits GPU work reading these buffers and does not block before acquiring
	//again - PlayerSession does block, and anything that stops blocking has to give each
	//in-flight block its own buffers.
	{
		lock_guard<mutex> lock(m_pendingWaveformsMutex);
		if(!m_pendingWaveforms.empty())
		{
			LogError("SigMFSource: a waveform is still pending; sample buffers are reused "
				"between acquisitions, so overwriting them now would corrupt it\n");
			return false;
		}
	}

	//Wrap or stop when a full block is no longer available.
	//
	//Deliberately not emitting a short final block. Everything downstream is built around a
	//fixed transform length: a short block changes the bin count, which makes SpectrumReducer
	//discard its partial group and Waterfall reallocate its ring buffer. The tail of the
	//recording is at most one block, so nothing meaningful is lost by skipping it, whereas
	//emitting it stalls playback at the end of every pass.
	if(m_playCursor + m_blockSize > m_totalSamples)
	{
		if(!m_looping)
		{
			m_atEnd = true;
			m_triggerArmed = false;
			return false;
		}
		m_playCursor = 0;

		//A recording shorter than one block cannot be played at this transform length
		if(m_blockSize > m_totalSamples)
		{
			m_atEnd = true;
			return false;
		}
	}

	int64_t nsamples = m_blockSize;
	if(nsamples <= 0)
		return false;

	//Read the block. This is the whole reason for a driver rather than an import filter:
	//memory stays flat no matter how large the recording is.
	size_t bytesPerSample = m_format.BytesPerSample();
	size_t readlen = nsamples * bytesPerSample;

	//Decide where the bytes land before reading them.
	//
	//On the packed path that is the waveform's own pinned host memory, so the file data is
	//written exactly once on the CPU: pread fills the same buffer the host-to-device transfer
	//will read from. Both the staging vector and the conversion loop that used to sit between
	//them are gone, and what crosses the bus is the file's own bytes - four per sample for
	//ci16 rather than the eight two float arrays would need.
	//
	//On the CPU path the bytes cannot be a waveform until they are converted, so they land in
	//the staging buffer exactly as before.
	uint8_t* dst = nullptr;
	if(m_usePackedPath)
	{
		//Allocated once and reused, for the same reason the float pair below is; see the
		//comment there.
		if(!m_packedCap)
		{
			m_packedCap = new PackedIQWaveform(m_nickname + ".RX.iq");
			m_packedCap->m_format = m_packedFormat;
			m_packedCap->m_bias = m_packedBias;
			m_packedCap->m_scale = m_packedScale;
		}
		m_packedCap->ResizeComplexSamples(nsamples);

		//Ignoring the GPU copy rather than PrepareForCpuAccess(): every byte is about to be
		//overwritten, so faulting the previous block back from the device would be a
		//full-size transfer whose result is discarded on the next line.
		m_packedCap->m_samples.PrepareForCpuAccessIgnoringGpuData();

		dst = reinterpret_cast<uint8_t*>(m_packedCap->m_samples.GetCpuPointer());
		if(!dst)
		{
			LogError("SigMFSource: packed sample buffer has no host mapping\n");
			return false;
		}
	}
	else
	{
		if(m_readBuffer.size() < readlen)
			m_readBuffer.resize(readlen);
		dst = m_readBuffer.data();
	}

	off_t offset = m_dataStartByte + m_playCursor * static_cast<off_t>(bytesPerSample);
	size_t got = 0;
	double tReadStart = GetTime();
	while(got < readlen)
	{
		ssize_t r = pread(m_fd, dst + got, readlen - got, offset + got);
		if(r < 0)
		{
			if(errno == EINTR)
				continue;
			LogError("SigMFSource: read error at byte %" PRId64 " (%s)\n",
				static_cast<int64_t>(offset + got), strerror(errno));
			return false;
		}
		if(r == 0)
		{
			//Short file relative to what the metadata implied. Use what we got.
			LogWarning("SigMFSource: unexpected EOF at byte %" PRId64 "\n",
				static_cast<int64_t>(offset + got));
			break;
		}
		got += r;
	}
	m_tRead += GetTime() - tReadStart;

	size_t goodSamples = got / bytesPerSample;
	if(goodSamples == 0)
		return false;

	//Build the waveforms.
	//
	//FS_PER_SECOND is 1e15, a double, so it cannot be used with integer division or modulo
	//directly. It is exactly representable, so an integer copy is safe.
	const int64_t fsPerSecond = static_cast<int64_t>(FS_PER_SECOND);
	int64_t fs_per_sample = (m_sampleRate > 0) ? llround(FS_PER_SECOND / m_sampleRate) : 1;

	//Offset of this block from the start of the recording, so the timestamps advance as
	//playback proceeds rather than every block claiming to start at time zero
	int64_t blockOffsetFs = (m_sampleRate > 0)
		? llround(m_playCursor * (FS_PER_SECOND / m_sampleRate))
		: 0;
	int64_t startFs = m_startFemtoseconds + (blockOffsetFs % fsPerSecond);
	time_t startSec = m_startTimestamp + (blockOffsetFs / fsPerSecond);
	if(startFs >= fsPerSecond)
	{
		startFs -= fsPerSecond;
		startSec++;
	}

	auto chan = m_chan.get();
	SequenceSet s;

	//Packed path: the bytes are already where they need to be, so all that is left is to say
	//how many of them are real and when they were captured.
	//
	//Only stream 0 is published. There is no Q waveform to publish - both components are in
	//this one buffer, which is the entire point - and ComplexFFTFilter does not read input 1
	//when input 0 is packed. Leaving stream 1 unset also means PopPendingWaveform() never
	//calls SetData on it, so it stays null rather than holding something stale.
	if(m_usePackedPath)
	{
		auto pcap = m_packedCap;
		pcap->m_timescale = fs_per_sample;
		pcap->m_triggerPhase = 0;
		pcap->m_startTimestamp = startSec;
		pcap->m_startFemtoseconds = startFs;
		pcap->m_revision++;

		//Shrink to what was actually read. resize() never reallocates downwards, so a short
		//final block costs nothing and the sample count stays honest.
		pcap->ResizeComplexSamples(goodSamples);
		pcap->MarkSamplesModifiedFromCpu();

		m_samplesDelivered += goodSamples;
		m_samplesPlayed += goodSamples;

		s[StreamDescriptor(chan, 0)] = pcap;

		chan->UpdateCenterFrequency(GetExactCenterFrequency());
		m_playCursor += goodSamples;

		m_pendingWaveformsMutex.lock();
		m_pendingWaveforms.push_back(s);
		m_pendingWaveformsMutex.unlock();

		if(m_triggerOneShot)
			m_triggerArmed = false;

		return true;
	}

	//Reuse the same two waveforms for every acquisition.
	//
	//Allocating a fresh pair per block cost 280 us, which was 72% of playback wall clock
	//(DESIGN.md section 13). Nothing in this application returns waveforms to
	//m_analogWaveformPool - there is no HistoryManager, which is what does it in
	//ngscopeclient - so the pool was permanently empty and every AllocateAnalogWaveform()
	//built two AcceleratorBuffers from scratch: a vkAllocateMemory and vkMapMemory for
	//pinned host memory, a device buffer, and two vk::raii::Events each, plus the matching
	//frees when the previous pair was deleted.
	//
	//Handing the *same* pointers to the channel each time makes InstrumentChannel::SetData
	//early out (InstrumentChannel.cpp:146-147) instead of deleting, so the buffers survive
	//from block to block. The channel stays the owner and frees them when it is destroyed;
	//these are non-owning pointers and must not be deleted here.
	//
	//Constructed directly rather than through Oscilloscope::AllocateAnalogWaveform(), which
	//checked that same empty pool and then did exactly this.
	if(!m_icap)
	{
		m_icap = new UniformAnalogWaveform(m_nickname + ".RX.i");
		m_qcap = new UniformAnalogWaveform(m_nickname + ".RX.q");
	}
	auto icap = m_icap;
	auto qcap = m_qcap;

	for(auto w : {icap, qcap})
	{
		w->m_timescale = fs_per_sample;
		w->m_triggerPhase = 0;
		w->m_startTimestamp = startSec;
		w->m_startFemtoseconds = startFs;
		w->m_revision++;
		w->Resize(goodSamples);
		w->PrepareForCpuAccess();
	}

	double tConvertStart = GetTime();
	ConvertSamples(m_readBuffer.data(), goodSamples, icap, qcap);
	m_tConvert += GetTime() - tConvertStart;

	//Two counters, incremented together and reset differently on purpose. m_samplesDelivered
	//is an ingest statistic that ResetIngestStats() zeroes; m_samplesPlayed is the playback
	//timebase and nothing is allowed to zero it. See the Playback timebase section of the
	//header for why conflating them breaks the waterfall's time axis.
	m_samplesDelivered += goodSamples;
	m_samplesPlayed += goodSamples;

	icap->MarkSamplesModifiedFromCpu();
	qcap->MarkSamplesModifiedFromCpu();

	s[StreamDescriptor(chan, 0)] = icap;
	s[StreamDescriptor(chan, 1)] = qcap;

	//Downstream complex filters read the center frequency from this scalar stream. It is a
	//float and cannot hold a GHz carrier exactly; use GetExactCenterFrequency() for
	//anything that needs precision.
	chan->UpdateCenterFrequency(GetExactCenterFrequency());

	m_playCursor += goodSamples;

	m_pendingWaveformsMutex.lock();
	m_pendingWaveforms.push_back(s);
	m_pendingWaveformsMutex.unlock();

	if(m_triggerOneShot)
		m_triggerArmed = false;

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Playback control

bool SigMFSource::PopPendingWaveform()
{
	lock_guard<mutex> lock(m_pendingWaveformsMutex);
	if(m_pendingWaveforms.empty())
		return false;

	SequenceSet set = *m_pendingWaveforms.begin();
	for(auto it : set)
		it.first.m_channel->SetData(it.second, it.first.m_stream);
	m_pendingWaveforms.pop_front();

	return true;
}

void SigMFSource::Start()
{
	m_triggerArmed = true;
	m_triggerOneShot = false;
	m_atEnd = false;
}

void SigMFSource::StartSingleTrigger()
{
	m_triggerArmed = true;
	m_triggerOneShot = true;
	m_atEnd = false;
}

void SigMFSource::Stop()
{
	m_triggerArmed = false;
	m_triggerOneShot = false;
}

void SigMFSource::SetSampleDepth(uint64_t depth)
{
	if(depth > 0)
		m_blockSize = depth;
}
