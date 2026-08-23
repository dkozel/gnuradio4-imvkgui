/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of SigMFSource
 */
#ifndef SigMFSource_h
#define SigMFSource_h

#include <scopehal/scopehal.h>
#include <scopehal/ComplexChannel.h>

#include "sigmf_core_generated.h"
#include "sigmf.h"
#include "sigmf_helpers.h"

#include "Annotation.h"
#include "PackedIQWaveform.h"
#include "RecordingClock.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
	@brief The SigMF record shape we support

	Core namespace only. Adding a namespace here (e.g. sigmf::signal::DescrT for emitter
	metadata) is a one-line change rather than a refactor, which is why this lives in one
	place. See DESIGN.md section 7.1.

	Note that libsigmf's own sigmf::metadata_file_to_json() cannot be used with this type:
	it is not a template and hardcodes core+antenna+capture_details+signal. We parse into
	our own type with nlohmann::json directly instead, which is three lines.
 */
typedef sigmf::SigMF<
	sigmf::Global<sigmf::core::DescrT>,
	sigmf::Capture<sigmf::core::DescrT>,
	sigmf::Annotation<sigmf::core::DescrT> > SigMFRecord;

/**
	@brief Binary layout of one sample in a .sigmf-data file

	SigMF datatype strings have the grammar (c|r)(f|i|u)(8|16|32|64)(_le|_be)?, e.g.
	"cf32_le" or "ci16_le". Every recording in the demo dataset is ci16_le.
 */
class SigMFSampleFormat
{
public:
	enum Kind
	{
		KIND_FLOAT,
		KIND_SIGNED,
		KIND_UNSIGNED
	};

	SigMFSampleFormat()
	: m_complex(true)
	, m_kind(KIND_FLOAT)
	, m_bits(32)
	, m_littleEndian(true)
	{}

	/**
		@brief Parses a SigMF datatype string

		@param str			The datatype string
		@param errorOut		Set to a human readable reason on failure

		@return True on success
	 */
	bool Parse(const std::string& str, std::string& errorOut);

	///@brief Bytes occupied by one sample, counting both components if complex
	size_t BytesPerSample() const
	{ return (m_bits / 8) * (m_complex ? 2 : 1); }

	///@brief True if the format carries I and Q
	bool IsComplex() const
	{ return m_complex; }

	///@brief Human readable description, for error messages
	std::string ToString() const;

	/**
		@brief Maps this format onto a PackedIQFormat, if the GPU unpack shader can handle it

		@param formatOut	Component layout for PackedComplexWindow.glsl
		@param biasOut		Offset applied before scaling; nonzero only for offset binary
		@param scaleOut		Raw LSB to volts

		@return True if the format can take the packed path

		False means the caller has to fall back to ConvertSamples() on the CPU. That happens
		for the 64-bit formats, which would need shaderInt64 or fp64 in the shader for no
		practical gain, and for a file whose byte order is not the host's, which would need a
		second set of unpack paths for a case SigMF does not produce in practice.
	 */
	bool ToPackedFormat(PackedIQFormat& formatOut, float& biasOut, float& scaleOut) const;

	bool m_complex;
	Kind m_kind;
	int m_bits;
	bool m_littleEndian;
};

/**
	@brief Converts a record's annotations into the display-neutral model

	The one place SigMFRecord and AnnotationSet are both visible. Free rather than a method so
	that it can be tested against a hand-built record with no file and no Vulkan.

	@param rec				Parsed metadata
	@param totalSamples		Length of the recording, used to clamp ends that run past it
	@param out				Cleared, filled and finalized
 */
void BuildAnnotationSet(const SigMFRecord& rec, int64_t totalSamples, AnnotationSet& out);

/**
	@brief A SigMF recording, played into the filter graph

	Owns one ComplexChannel supplying I, Q and a center-frequency scalar (ComplexChannel.h:63),
	reads blocks of samples at the play cursor with pread(), and publishes them on that channel.

	@par Why this is not an Oscilloscope

	It was one until this commit, and DESIGN.md D1 still records the reasoning. That reasoning
	holds up as an argument against ComplexImportFilter - which reads an entire recording into
	memory in one fread and exposes no center frequency - but none of the three things it asks
	for came from the base class:

	  - bounded memory on a large recording is this class's own pread loop;
	  - the center-frequency stream is ComplexChannel's;
	  - run/stop/single is the three bools below.

	What the base class did supply was 47 overrides, of which six were ever called, and a
	vtable slot for every non-pure virtual in Oscilloscope and Instrument - AutoZero, Degauss,
	GetADCMode, GetInputMuxNames, GetProbeName, GetDigitalHysteresis, SerializeConfiguration.
	Measured, that was 98 of the 195 scopehal symbols this application referenced.

	IqInjector already demonstrated the alternative for live samples: a ComplexChannel with a
	null Oscilloscope* is a supported construction that the verification suite drives a real
	ComplexFFTFilter from. This is the same shape, with a file behind it instead of a port.
 */
class SigMFSource
{
public:

	/**
		@brief The waveforms published by one acquisition, keyed by the stream they go on

		Was Oscilloscope::SequenceSet (Oscilloscope.h:906). Same type, declared here because
		this class is the only thing that ever built one.
	 */
	typedef std::map<StreamDescriptor, WaveformBase*> SequenceSet;

	/**
		@brief Opens a SigMF recording

		@param metaPath		Path to the .sigmf-meta file
		@param dataPath		Path to the .sigmf-data file, or empty to derive it from metaPath

		Check IsValid() afterwards; construction does not throw.
	 */
	SigMFSource(const std::string& metaPath, const std::string& dataPath = "");
	virtual ~SigMFSource();

	//not copyable or assignable
	SigMFSource(const SigMFSource&) =delete;
	SigMFSource& operator=(const SigMFSource&) =delete;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Recording state

	///@brief True if the recording opened successfully
	bool IsValid() const
	{ return m_valid; }

	///@brief Human readable reason the recording could not be opened
	const std::string& GetErrorMessage() const
	{ return m_errorMessage; }

	/**
		@brief Center frequency at the play cursor, in Hz, at full precision

		Use this rather than the ComplexChannel scalar stream for anything that needs
		accuracy. That stream is a float, and a float cannot represent a GHz-scale carrier
		to better than ~128-256 Hz - a quarter of an FFT bin on the narrower recordings in
		the demo dataset. The scalar stream exists for downstream scopehal filters that
		expect it; our own axis labelling should use this.
	 */
	double GetExactCenterFrequency() const;

	///@brief Sample rate of the recording, Hz
	double GetRecordingSampleRate() const
	{ return m_sampleRate; }

	///@brief Total number of complex samples in the data file
	int64_t GetTotalSamples() const
	{ return m_totalSamples; }

	///@brief Parsed metadata, for annotation rendering later
	const SigMFRecord& GetRecord() const
	{ return m_record; }

	///@brief Sample format of the data file
	const SigMFSampleFormat& GetSampleFormat() const
	{ return m_format; }

	///@brief Path to the .sigmf-data file, which is not always the metadata basename
	const std::string& GetDataPath() const
	{ return m_dataPath; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Playback control

	///@brief Index of the next sample to be read
	int64_t GetPlayCursor() const
	{ return m_playCursor; }

	///@brief Moves the play cursor, clamped to the recording
	void SeekToSample(int64_t sample);

	///@brief True if playback restarts at the beginning on reaching the end
	bool GetLooping() const
	{ return m_looping; }

	void SetLooping(bool loop)
	{ m_looping = loop; }

	///@brief True once the cursor has run off the end with looping disabled
	bool AtEnd() const
	{ return m_atEnd; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Playback timebase
	//
	// Deliberately not in the ingest accounting section below, and deliberately not served by
	// GetSamplesDelivered(). That counter looks like it would do - it also counts samples, and
	// monotonically - but ResetIngestStats() zeroes it, so anything built on it silently jumps
	// backwards the first time someone adds a "reset stats" button. An instrumentation counter
	// must never be a time base. These two are the time base.

	/**
		@brief Samples handed downstream since the source was constructed

		Stream coordinates: monotonic, never reset, unaffected by looping or seeking. A
		difference between two of these is always a real elapsed duration, which is what makes
		it the right thing to measure a time axis span in.

		Contrast GetPlayCursor(), which is recording coordinates and wraps to zero on every
		loop - at high sample rates the whole recording goes past about once a second.
	 */
	int64_t GetSamplesPlayed() const
	{ return m_samplesPlayed; }

	/**
		@brief Maps recording sample indices to the instants they were captured

		Built once at open from the captures' core:datetime. Ask it rather than deriving
		timestamps from the waveform fields: those carry a fallback epoch for scopehal's
		benefit and cannot say whether the time is real.
	 */
	const RecordingClock& GetClock() const
	{ return m_clock; }

	/**
		@brief The recording's annotations, in display-neutral form

		Built once at open. notes/annotation-overlay-plan.md §A1 put this in PlayerSession;
		it lives here instead, beside GetClock(), because the two are the same kind of thing -
		a display-neutral view derived from the metadata at load - and the metadata is here.
		The property that actually mattered is unchanged: nothing downstream of this class
		sees libsigmf.
	 */
	const AnnotationSet& GetAnnotations() const
	{ return m_annotations; }

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Ingest cost accounting
	//
	// Reading and converting samples is host work that competes with nothing else in the
	// pipeline for the GPU, so it is invisible in any GPU-side measurement. At high sample
	// rates it is a plausible bottleneck in its own right - a scalar int16 to float
	// deinterleave at 245.76 MS/s writes about 2 GB/s - and the only way to know is to
	// count it separately from everything else AcquireData() does.

	///@brief Seconds spent in pread() since the last ResetIngestStats()
	double GetReadSeconds() const
	{ return m_tRead; }

	/**
		@brief Seconds spent in ConvertSamples() since the last ResetIngestStats()

		Always zero on the packed path: there is no CPU conversion to spend time in. Check
		IsUsingPackedPath() before reading anything into a low number here.
	 */
	double GetConvertSeconds() const
	{ return m_tConvert; }

	/**
		@brief True if samples reach the GPU in their on-disk format, unconverted

		When set, the file bytes are read straight into pinned memory and a shader does the
		unpacking, so neither ConvertSamples() nor the staging buffer is used and the bus
		carries the file's own bytes rather than two float arrays.
	 */
	bool IsUsingPackedPath() const
	{ return m_usePackedPath; }

	/**
		@brief Forces the CPU conversion path even when the format could be packed

		Exists so the two ingest paths can be measured against each other on the same
		recording and the same hardware, which is the only way to say what the packed path is
		worth. Not a runtime setting: call it before the first AcquireData(), since the two
		paths publish different waveforms on different streams and switching mid-playback
		would leave a stale waveform on stream 1.
	 */
	void SetPackedPathAllowed(bool allow)
	{
		m_packedPathAllowed = allow;
		m_usePackedPath = m_packedPathSupported && allow;
	}

	///@brief True if the recording's format could be packed, whatever SetPackedPathAllowed() says
	bool IsPackedPathSupported() const
	{ return m_packedPathSupported; }

	///@brief Complex samples delivered since the last ResetIngestStats()
	int64_t GetSamplesDelivered() const
	{ return m_samplesDelivered; }

	void ResetIngestStats()
	{
		m_tRead = 0;
		m_tConvert = 0;
		m_samplesDelivered = 0;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Playback
	//
	// What was 47 Oscilloscope overrides. Everything below is called by something.

	///@brief Display name of the recording, from the data file's basename
	std::string GetName() const
	{ return m_recordingName; }

	/**
		@brief The channel the filter graph reads from

		Streams 0, 1 and 2 are I, Q and the center frequency, matching what IqInjector exposes,
		so a consumer cannot tell a recording from a live port.
	 */
	ComplexChannel* GetChannel()
	{ return m_chan.get(); }

	/**
		@brief Reads one block at the play cursor and queues it

		The waveform is not visible to the graph until PopPendingWaveform() attaches it.
	 */
	bool AcquireData();

	/**
		@brief Attaches the oldest queued block to the channel

		Was Oscilloscope::PopPendingWaveform(). Reimplemented here because the queue it drains
		is one this class fills itself, and the fifteen lines of base class around it came
		attached to Instrument's configuration-serialization surface.

		@return True if a block was attached
	 */
	bool PopPendingWaveform();

	///@brief Arms playback for continuous acquisition
	void Start();

	///@brief Arms playback for a single block
	void StartSingleTrigger();

	///@brief Disarms playback
	void Stop();

	///@brief True while armed
	bool IsTriggerArmed() const
	{ return m_triggerArmed; }

	///@brief True once playback has run off the end without looping
	bool IsAtEnd() const
	{ return m_atEnd; }

	///@brief Complex samples delivered per AcquireData() call
	uint64_t GetSampleDepth() const
	{ return m_blockSize; }

	void SetSampleDepth(uint64_t depth);

protected:

	///@brief Loads and validates the metadata. Sets m_valid and m_errorMessage.
	void LoadMetadata(const std::string& metaPath, const std::string& dataPath);

	/**
		@brief Converts one block of raw file bytes into I and Q float samples

		@param raw			Interleaved sample data as read from the file
		@param nsamples		Number of complex samples in raw
		@param idata		I output, already resized
		@param qdata		Q output, already resized
	 */
	void ConvertSamples(
		const uint8_t* raw,
		size_t nsamples,
		UniformAnalogWaveform* idata,
		UniformAnalogWaveform* qdata);

	///@brief Index of the capture segment containing the given sample, or 0 if none
	size_t CaptureIndexForSample(int64_t sample) const;

	///@brief True if the recording opened successfully
	bool m_valid;

	///@brief Why the recording could not be opened
	std::string m_errorMessage;

	///@brief Parsed metadata
	SigMFRecord m_record;

	///@brief Binary layout of the data file
	SigMFSampleFormat m_format;

	///@brief Path to the .sigmf-data file
	std::string m_dataPath;

	///@brief Display name of the recording, from the data file's basename
	std::string m_recordingName;

	///@brief Short name prefixed to the waveforms' Vulkan debug names. Was Instrument's.
	std::string m_nickname;

	///@brief File descriptor for the data file, or -1
	int m_fd;

	///@brief Byte offset of the first sample, from core:header_bytes
	int64_t m_dataStartByte;

	///@brief Total complex samples available
	int64_t m_totalSamples;

	///@brief Sample rate, Hz
	double m_sampleRate;

	///@brief Sample index of the next read
	int64_t m_playCursor;

	///@brief Samples handed downstream since construction. Monotonic; ResetIngestStats() must not touch it.
	int64_t m_samplesPlayed;

	///@brief Recording sample index to wall clock time, from the captures' core:datetime
	RecordingClock m_clock;

	///@brief The record's annotations, converted out of libsigmf's types at load
	AnnotationSet m_annotations;

	///@brief Samples delivered per AcquireData call
	int64_t m_blockSize;

	///@brief True if playback wraps at the end
	bool m_looping;

	///@brief True once playback has run off the end without looping
	bool m_atEnd;

	///@brief True while armed
	bool m_triggerArmed;

	///@brief True if the current arm is a single shot
	bool m_triggerOneShot;

	/**
		@brief Scratch buffer for raw file data, reused between blocks

		Only used on the CPU conversion path. The packed path reads straight into the
		waveform's pinned host memory, so there is no staging copy to make.
	 */
	std::vector<uint8_t> m_readBuffer;

	//The I and Q waveforms, allocated once and reused for every acquisition. Non-owning:
	//the channel owns them, because it is the channel that will delete them. See
	//AcquireData() for why they are reused and what that costs in in-flight blocks.
	//
	//Null on the packed path, where m_packedCap replaces both.
	UniformAnalogWaveform* m_icap;
	UniformAnalogWaveform* m_qcap;

	/**
		@brief Raw interleaved samples, when the format can go to the GPU unconverted

		Null on the CPU conversion path. Reused between acquisitions and owned by the
		channel, exactly like m_icap and m_qcap.
	 */
	PackedIQWaveform* m_packedCap;

	///@brief True if this recording's format takes the packed path, i.e. supported and allowed
	bool m_usePackedPath;

	///@brief True if the format could be packed, regardless of whether it is allowed to be
	bool m_packedPathSupported;

	///@brief False if the caller has forced the CPU path for measurement
	bool m_packedPathAllowed;

	///@brief Component layout handed to the unpack shader. Meaningless unless m_usePackedPath.
	PackedIQFormat m_packedFormat;

	///@brief Offset applied to each raw component before scaling
	float m_packedBias;

	///@brief Raw LSB to volts
	float m_packedScale;

	///@brief Wall clock time of the recording start, whole seconds
	time_t m_startTimestamp;

	///@brief Wall clock time of the recording start, fractional part
	int64_t m_startFemtoseconds;

	///@brief Accumulated seconds in pread()
	double m_tRead;

	///@brief Accumulated seconds in ConvertSamples()
	double m_tConvert;

	///@brief Accumulated complex samples handed downstream
	int64_t m_samplesDelivered;

	/**
		@brief The channel the graph reads from

		Constructed with a null Oscilloscope*, exactly as IqInjector.cpp:34 does. Nothing
		downstream calls GetScope() on it, and nothing reads its voltage range - every
		GetVoltageRange() call in the display is on a filter, not on this.
	 */
	std::unique_ptr<ComplexChannel> m_chan;

	//Blocks read but not yet attached to the channel. Was Oscilloscope's; this class was
	//always the only thing that filled it.
	std::deque<SequenceSet> m_pendingWaveforms;
	std::mutex m_pendingWaveformsMutex;
};

#endif
