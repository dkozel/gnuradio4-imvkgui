/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of SampleTime and RecordingClock
 */
#ifndef RecordingClock_h
#define RecordingClock_h

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

/**
	@brief An instant, split into whole seconds and femtoseconds within that second

	Split rather than a single integer because femtoseconds from the Unix epoch do not fit:
	int64 saturates at 9.22e18 fs, which is 9223 seconds, or two hours and 34 minutes after
	1970. Any code tempted to "simplify" this into one number should work that out first.

	The same split, with the same field meanings, as scopehal's WaveformBase::m_startTimestamp
	and m_startFemtoseconds, so a SampleTime can be assigned straight onto a waveform.

	@par Absolute versus relative

	@a absolute distinguishes "18:16:07.312 UTC on 22 July 2025" from "4.207 seconds into the
	recording". Both are useful and they are not interchangeable. A recording whose metadata
	carries no usable core:datetime yields relative times only, and saying so is the whole
	point of the flag: labelling dect6 with the mtime of its data file would put a 2022
	recording in 2026 with no indication that the number was invented.
 */
struct SampleTime
{
	///@brief Whole seconds. Unix time when @ref absolute, else seconds since the recording started.
	time_t sec = 0;

	///@brief Femtoseconds within that second, 0 to 1e15-1
	int64_t fs = 0;

	///@brief True if @ref sec is a wall clock time rather than an offset
	bool absolute = false;
};

/**
	@brief Maps a sample index in a recording to the instant it was captured

	@par Why recording coordinates and not playback coordinates

	Playback loops. A clock pinned to a free-running "samples played since start" counter
	would, after a minute of looping a three second file, claim to be showing data captured
	at T+60s that the recording never contained. Time is a property of the samples, not of
	how many times we have replayed them, so this is defined over recording coordinates -
	the same coordinates SigMF annotations and captures use - and is invariant to looping,
	pausing, seeking and replay speed.

	This also generalises without a special case: for a live source, recording coordinates and
	stream coordinates are the same thing and the total sample count is simply unbounded.

	@par Capture segments

	SigMF permits several captures, each with its own core:sample_start and core:datetime, and
	the spec does not require them to be contiguous in time - a scanning receiver revisiting a
	band leaves real gaps. So each segment carries its own epoch and times are computed within
	the segment containing the sample. Every recording in the demo dataset has exactly one
	capture, which is precisely why the multi-capture case has to be handled now rather than
	discovered later.

	@par Precision

	Times are computed from the sample index every call and never accumulated. Accumulation is
	the trap here: femtoseconds per sample is an integer in scopehal, and at 245.76 MS/s the
	true value is 4069010.4167 against a stored 4069010, so a running total drifts by about
	0.37 ms per hour. Recomputing from the index instead costs one division and is good to
	about a picosecond over a day, because a double holds sample indices below 9e15 exactly
	and the division introduces roughly one ulp.
 */
class RecordingClock
{
public:
	RecordingClock();

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Construction

	///@brief Forgets every capture segment and the sample rate
	void Clear();

	/**
		@brief Adds one capture segment

		@param sampleStart	Recording-relative index of the segment's first sample
		@param datetime		Value of core:datetime, or empty if the capture has none

		An unparseable or absent @a datetime is not an error: the segment is recorded without
		an epoch, and times within it come out relative. Pass the string through verbatim;
		parsing and complaining about it is this class's job, not the caller's.
	 */
	void AddCapture(int64_t sampleStart, const std::string& datetime);

	/**
		@brief Sorts the segments and fixes the sample rate. Call once, after the last AddCapture().

		@param sampleRate	Recording sample rate in Hz, which must be positive
	 */
	void Finalize(double sampleRate);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Queries

	///@brief True if at least one capture segment carries a usable wall clock epoch
	bool HasAbsoluteTime() const
	{ return m_haveAbsolute; }

	///@brief Number of capture segments
	size_t GetCaptureCount() const
	{ return m_captures.size(); }

	/**
		@brief Instant at which the given sample was captured

		@param recordingSample	Index into the recording, not the play cursor's delivered count

		Returns a relative time, with SampleTime::absolute false, when the containing segment
		has no epoch. The value is still meaningful - it is the offset from the start of the
		recording - so callers that only want elapsed time need not check the flag.
	 */
	SampleTime TimeOfSample(int64_t recordingSample) const;

	/**
		@brief Seconds from the start of the recording to the given sample

		Always relative and always available, for axis spans and durations where a wall clock
		is neither needed nor necessarily known.
	 */
	double SecondsIntoRecording(int64_t recordingSample) const;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Formatting

	/**
		@brief Renders an instant for display

		@param t			The instant
		@param subsecDigits	Digits after the decimal point, 0 to 15

		Absolute times come out as "2025-07-22 18:16:03.487" in UTC; relative ones as
		"+4.207 s". The two are deliberately unmistakable for each other.
	 */
	static std::string Format(const SampleTime& t, int subsecDigits = 3);

	/**
		@brief Parses an ISO 8601 / RFC 3339 timestamp into a split epoch

		@param str			The timestamp
		@param secOut		Whole seconds, Unix time
		@param fsOut		Femtoseconds within that second
		@param errorOut		Set to a human readable reason on failure

		@return True on success

		Handles rather more than the SigMF spec strictly requires, because real files need it.
		All four of these appear in the demo dataset:

		@verbatim
		2025-09-17T06:45:29Z                 whole seconds, zulu
		2025-07-22T18:16:03.0000000          seven fractional digits, no timezone at all
		2025-12-16T20:04:09.612882           microseconds, no timezone
		2026-02-12T16:08:21.787343+00:00     explicit offset
		@endverbatim

		A missing timezone is not conformant - core:datetime is specified as an RFC 3339 string
		and the offset is mandatory - but two of the eight demo recordings omit it, so it is
		read as UTC rather than rejected. Fractional digits beyond the fifteenth are dropped,
		since femtoseconds are the finest unit anything downstream can carry.
	 */
	static bool ParseIso8601(const std::string& str, time_t& secOut, int64_t& fsOut, std::string& errorOut);

protected:
	///@brief One capture segment and the instant its first sample was taken
	struct CaptureEpoch
	{
		///@brief Recording-relative index of this segment's first sample
		int64_t sampleStart = 0;

		///@brief Wall clock time of that sample, valid only if @ref hasEpoch
		time_t epochSec = 0;
		int64_t epochFs = 0;

		///@brief True if core:datetime was present and parseable
		bool hasEpoch = false;
	};

	///@brief Index of the segment containing the given sample, or SIZE_MAX if there are none
	size_t SegmentForSample(int64_t recordingSample) const;

	///@brief Capture segments, sorted by sampleStart once Finalize() has run
	std::vector<CaptureEpoch> m_captures;

	///@brief Sample rate in Hz. Positive once Finalize() has run with a usable rate.
	double m_sampleRate;

	///@brief True if any segment has an epoch
	bool m_haveAbsolute;
};

#endif
