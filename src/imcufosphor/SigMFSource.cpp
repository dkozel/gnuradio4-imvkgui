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
	, m_blockSize(65536)
	, m_looping(true)
	, m_atEnd(false)
	, m_triggerArmed(false)
	, m_triggerOneShot(false)
	, m_icap(nullptr)
	, m_qcap(nullptr)
	, m_startTimestamp(0)
	, m_startFemtoseconds(0)
	, m_tRead(0)
	, m_tConvert(0)
	, m_samplesDelivered(0)
{
	m_nickname = "sigmf";
	m_recordingName = "SigMF recording";

	//One complex channel, which gives us I, Q and the center frequency scalar
	auto chan = new ComplexChannel(
		this,
		"RX",
		"#4040ff",
		Unit(Unit::UNIT_FS),
		Unit(Unit::UNIT_VOLTS),
		m_channels.size());
	m_channels.push_back(chan);
	chan->SetDefaultDisplayName();

	//Samples are normalized to +/- 1.0 regardless of source format
	SetChannelVoltageRange(0, 0, 2);
	SetChannelVoltageRange(0, 1, 2);
	SetChannelOffset(0, 0, 0);
	SetChannelOffset(0, 1, 0);

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

	//Sample rate is optional in the spec. Every recording in the demo dataset has one, but
	//a missing rate is not fatal - it only means the frequency axis has no scale, so leave
	//it to the caller to supply a fallback.
	if(global.sample_rate.has_value() && (global.sample_rate.value() > 0))
		m_sampleRate = global.sample_rate.value();
	else
	{
		m_sampleRate = 0;
		LogWarning("SigMF recording has no core:sample_rate; a fallback must be supplied\n");
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

	//Skip any header the first capture declares
	if(!m_record.captures.empty())
	{
		auto& c0 = m_record.captures[0].get<sigmf::core::DescrT>();
		if(c0.header_bytes.has_value())
			m_dataStartByte = c0.header_bytes.value();
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

	//Recording start time, for waveform timestamps. Fall back to the data file mtime.
	GetTimestampOfFile(m_dataPath, m_startTimestamp, m_startFemtoseconds);

	m_recordingName = BaseName(m_dataPath);
	m_valid = true;

	LogTrace("Opened SigMF recording %s: %s, %" PRId64 " samples at %g Hz, %zu captures, %zu annotations\n",
		m_dataPath.c_str(),
		m_format.ToString().c_str(),
		m_totalSamples,
		m_sampleRate,
		m_record.captures.size(),
		m_record.annotations.size());
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
	return c.frequency.value_or(0.0);
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
	if(m_readBuffer.size() < readlen)
		m_readBuffer.resize(readlen);

	off_t offset = m_dataStartByte + m_playCursor * static_cast<off_t>(bytesPerSample);
	size_t got = 0;
	double tReadStart = GetTime();
	while(got < readlen)
	{
		ssize_t r = pread(m_fd, m_readBuffer.data() + got, readlen - got, offset + got);
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

	auto chan = dynamic_cast<ComplexChannel*>(GetChannel(0));

	//Reuse the same two waveforms for every acquisition.
	//
	//Allocating a fresh pair per block cost 280 us, which was 72% of playback wall clock
	//(DESIGN.md section 13). Nothing in this application returns waveforms to
	//m_analogWaveformPool - there is no HistoryManager, which is what does it in
	//ngscopeclient - so the pool is permanently empty and every AllocateAnalogWaveform()
	//built two AcceleratorBuffers from scratch: a vkAllocateMemory and vkMapMemory for
	//pinned host memory, a device buffer, and two vk::raii::Events each, plus the matching
	//frees when the previous pair was deleted.
	//
	//Handing the *same* pointers to the channel each time makes InstrumentChannel::SetData
	//early out (InstrumentChannel.cpp:146-147) instead of deleting, so the buffers survive
	//from block to block. The channel stays the owner and frees them when it is destroyed;
	//these are non-owning pointers and must not be deleted here.
	if(!m_icap)
	{
		m_icap = AllocateAnalogWaveform(m_nickname + ".RX.i");
		m_qcap = AllocateAnalogWaveform(m_nickname + ".RX.q");
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
	m_samplesDelivered += goodSamples;

	icap->MarkSamplesModifiedFromCpu();
	qcap->MarkSamplesModifiedFromCpu();

	SequenceSet s;
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

Oscilloscope::TriggerMode SigMFSource::PollTrigger()
{
	if(m_atEnd && !m_looping)
		return TRIGGER_MODE_STOP;
	if(!m_triggerArmed)
		return TRIGGER_MODE_STOP;

	//Data is always available from a file, so report triggered and let the caller block in
	//AcquireData(), the same contract the remote bridge drivers use
	return TRIGGER_MODE_TRIGGERED;
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

void SigMFSource::ForceTrigger()
{
	StartSingleTrigger();
}

bool SigMFSource::IsTriggerArmed()
{
	return m_triggerArmed;
}

void SigMFSource::PushTrigger()
{
	//no hardware trigger to configure
}

void SigMFSource::PullTrigger()
{
	//no hardware trigger to configure
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Instrument and channel configuration
//
// A recording has no adjustable hardware, so most of this is fixed. The Oscilloscope
// interface requires all of it regardless.

unsigned int SigMFSource::GetInstrumentTypes() const
{
	return INST_OSCILLOSCOPE;
}

uint32_t SigMFSource::GetInstrumentTypesForChannel([[maybe_unused]] size_t i) const
{
	return INST_OSCILLOSCOPE;
}

bool SigMFSource::IsChannelEnabled([[maybe_unused]] size_t i)
{
	return true;
}

void SigMFSource::EnableChannel([[maybe_unused]] size_t i)
{
}

void SigMFSource::DisableChannel([[maybe_unused]] size_t i)
{
}

OscilloscopeChannel::CouplingType SigMFSource::GetChannelCoupling([[maybe_unused]] size_t i)
{
	return OscilloscopeChannel::COUPLE_DC_50;
}

void SigMFSource::SetChannelCoupling(
	[[maybe_unused]] size_t i,
	[[maybe_unused]] OscilloscopeChannel::CouplingType type)
{
}

vector<OscilloscopeChannel::CouplingType> SigMFSource::GetAvailableCouplings([[maybe_unused]] size_t i)
{
	return { OscilloscopeChannel::COUPLE_DC_50 };
}

double SigMFSource::GetChannelAttenuation([[maybe_unused]] size_t i)
{
	return 1;
}

void SigMFSource::SetChannelAttenuation([[maybe_unused]] size_t i, [[maybe_unused]] double atten)
{
}

unsigned int SigMFSource::GetChannelBandwidthLimit([[maybe_unused]] size_t i)
{
	return 0;
}

void SigMFSource::SetChannelBandwidthLimit(
	[[maybe_unused]] size_t i,
	[[maybe_unused]] unsigned int limit_mhz)
{
}

float SigMFSource::GetChannelVoltageRange(size_t i, size_t stream)
{
	auto key = pair<size_t, size_t>(i, stream);
	if(m_channelVoltageRange.find(key) == m_channelVoltageRange.end())
		return 2;
	return m_channelVoltageRange[key];
}

void SigMFSource::SetChannelVoltageRange(size_t i, size_t stream, float range)
{
	m_channelVoltageRange[pair<size_t, size_t>(i, stream)] = range;
}

float SigMFSource::GetChannelOffset(size_t i, size_t stream)
{
	auto key = pair<size_t, size_t>(i, stream);
	if(m_channelOffset.find(key) == m_channelOffset.end())
		return 0;
	return m_channelOffset[key];
}

void SigMFSource::SetChannelOffset(size_t i, size_t stream, float offset)
{
	m_channelOffset[pair<size_t, size_t>(i, stream)] = offset;
}

OscilloscopeChannel* SigMFSource::GetExternalTrigger()
{
	return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Timebase
//
// The sample rate and depth come from the recording, not from us. SetSampleRate is ignored
// rather than honoured: resampling a file to a requested rate is not something a player
// should silently do.

vector<uint64_t> SigMFSource::GetSampleRatesNonInterleaved()
{
	return { static_cast<uint64_t>(m_sampleRate) };
}

vector<uint64_t> SigMFSource::GetSampleRatesInterleaved()
{
	return GetSampleRatesNonInterleaved();
}

uint64_t SigMFSource::GetSampleRate()
{
	return static_cast<uint64_t>(m_sampleRate);
}

void SigMFSource::SetSampleRate([[maybe_unused]] uint64_t rate)
{
}

vector<uint64_t> SigMFSource::GetSampleDepthsNonInterleaved()
{
	//Block sizes we are willing to deliver per acquisition. These are the knob that trades
	//FFT length headroom against latency; the reducer (DESIGN.md 7.3) sits downstream.
	return { 4096, 8192, 16384, 32768, 65536, 131072, 262144, 1048576 };
}

vector<uint64_t> SigMFSource::GetSampleDepthsInterleaved()
{
	return GetSampleDepthsNonInterleaved();
}

uint64_t SigMFSource::GetSampleDepth()
{
	return m_blockSize;
}

void SigMFSource::SetSampleDepth(uint64_t depth)
{
	if(depth > 0)
		m_blockSize = depth;
}

bool SigMFSource::IsInterleaving()
{
	return false;
}

bool SigMFSource::SetInterleaving([[maybe_unused]] bool combine)
{
	return false;
}

set<Oscilloscope::InterleaveConflict> SigMFSource::GetInterleaveConflicts()
{
	return {};
}

void SigMFSource::SetTriggerOffset([[maybe_unused]] int64_t offset)
{
}

int64_t SigMFSource::GetTriggerOffset()
{
	return 0;
}

bool SigMFSource::HasFrequencyControls()
{
	//The center frequency is a property of the recording, not something we can tune
	return false;
}

bool SigMFSource::HasTimebaseControls()
{
	return true;
}
