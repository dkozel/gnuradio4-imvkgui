/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of RecordingClock
 */

#include <scopehal/scopehal.h>

#include "RecordingClock.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace std;

///@brief Femtoseconds in one second, as an integer. FS_PER_SECOND in scopehal is a double.
static const int64_t g_fsPerSecond = 1000000000000000LL;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction

RecordingClock::RecordingClock()
	: m_sampleRate(0)
	, m_haveAbsolute(false)
{
}

void RecordingClock::Clear()
{
	m_captures.clear();
	m_sampleRate = 0;
	m_haveAbsolute = false;
}

void RecordingClock::AddCapture(int64_t sampleStart, const string& datetime)
{
	CaptureEpoch c;
	c.sampleStart = (sampleStart > 0) ? sampleStart : 0;

	//An absent core:datetime is legal and common, so it is not worth a warning. Note that
	//libsigmf gives this field as a bare std::string rather than the Optional<> its siblings
	//get, so "absent" and "present but empty" are the same value here and both land in this
	//branch - which is what we want, but is worth knowing when reading the generated header.
	if(!datetime.empty())
	{
		string err;
		if(ParseIso8601(datetime, c.epochSec, c.epochFs, err))
			c.hasEpoch = true;

		//Present but unparseable is a different matter: the recording is claiming to know when
		//it was taken and we cannot read the claim, so say so rather than silently falling
		//back to relative time.
		else
		{
			LogWarning("SigMF capture at sample %" PRId64 " has core:datetime \"%s\" which "
				"could not be parsed (%s); times in this capture will be relative\n",
				c.sampleStart, datetime.c_str(), err.c_str());
		}
	}

	if(c.hasEpoch)
		m_haveAbsolute = true;

	m_captures.push_back(c);
}

void RecordingClock::Finalize(double sampleRate)
{
	m_sampleRate = (sampleRate > 0) ? sampleRate : 0;

	//The spec says captures are in ascending sample_start order; real files are not obliged to
	//have read the spec, and SegmentForSample() below is written assuming the order holds.
	sort(m_captures.begin(), m_captures.end(),
		[](const CaptureEpoch& a, const CaptureEpoch& b)
		{ return a.sampleStart < b.sampleStart; });
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Queries

size_t RecordingClock::SegmentForSample(int64_t recordingSample) const
{
	//Linear scan. Capture counts are small - every recording in the demo dataset has exactly
	//one - and a binary search here would be more code guarding its own preconditions than it
	//would save. Same reasoning as SigMFSource::CaptureIndexForSample().
	size_t best = SIZE_MAX;
	for(size_t i=0; i<m_captures.size(); i++)
	{
		if(m_captures[i].sampleStart <= recordingSample)
			best = i;
		else
			break;
	}
	return best;
}

SampleTime RecordingClock::TimeOfSample(int64_t recordingSample) const
{
	SampleTime ret;
	if(m_sampleRate <= 0)
		return ret;

	if(recordingSample < 0)
		recordingSample = 0;

	size_t idx = SegmentForSample(recordingSample);
	bool haveEpoch = (idx != SIZE_MAX) && m_captures[idx].hasEpoch;

	//With an epoch, time is measured from the start of that capture segment, because that is
	//what the epoch dates. Without one, the only meaningful zero is the start of the
	//recording.
	int64_t offset = haveEpoch ? (recordingSample - m_captures[idx].sampleStart) : recordingSample;

	//Recomputed from the sample index rather than accumulated; see the class comment for why
	//that distinction is worth a paragraph.
	double secs = static_cast<double>(offset) / m_sampleRate;
	int64_t wholeSec = static_cast<int64_t>(floor(secs));
	int64_t fs = llround( (secs - static_cast<double>(wholeSec)) * static_cast<double>(g_fsPerSecond) );

	//Rounding can push the fraction up to a whole second
	if(fs >= g_fsPerSecond)
	{
		fs -= g_fsPerSecond;
		wholeSec++;
	}

	if(haveEpoch)
	{
		ret.absolute = true;
		ret.sec = m_captures[idx].epochSec + wholeSec;
		ret.fs = m_captures[idx].epochFs + fs;
		if(ret.fs >= g_fsPerSecond)
		{
			ret.fs -= g_fsPerSecond;
			ret.sec++;
		}
	}
	else
	{
		ret.absolute = false;
		ret.sec = wholeSec;
		ret.fs = fs;
	}

	return ret;
}

double RecordingClock::SecondsIntoRecording(int64_t recordingSample) const
{
	if(m_sampleRate <= 0)
		return 0;
	return static_cast<double>(recordingSample) / m_sampleRate;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Formatting

string RecordingClock::Format(const SampleTime& t, int subsecDigits)
{
	if(subsecDigits < 0)
		subsecDigits = 0;
	if(subsecDigits > 15)
		subsecDigits = 15;

	//Round the fraction to the requested width, carrying into the seconds so that 999.7 ms at
	//three digits reads as the next second rather than as ".1000"
	int64_t scale = 1;
	for(int i=0; i<15-subsecDigits; i++)
		scale *= 10;
	int64_t limit = 1;
	for(int i=0; i<subsecDigits; i++)
		limit *= 10;

	int64_t sec = t.sec;
	int64_t frac = (t.fs + scale/2) / scale;
	if(frac >= limit)
	{
		frac -= limit;
		sec++;
	}

	char fracbuf[24] = "";
	if(subsecDigits > 0)
		snprintf(fracbuf, sizeof(fracbuf), ".%0*" PRId64, subsecDigits, frac);

	if(t.absolute)
	{
		//UTC, always and explicitly. A timestamp rendered in whatever timezone the machine
		//running the display happens to be set to is not a thing anyone can correlate against
		//a log file from somewhere else.
		time_t tv = static_cast<time_t>(sec);
		struct tm tmv;
		if(gmtime_r(&tv, &tmv) == nullptr)
			return "(bad timestamp)";

		char datebuf[64];
		strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M:%S", &tmv);

		return string(datebuf) + fracbuf + " UTC";
	}

	//Relative. The leading sign is deliberate: it is what distinguishes an elapsed time from a
	//wall clock at a glance, which matters because the two look alike in a status line.
	char buf[64];
	snprintf(buf, sizeof(buf), "%+" PRId64, sec);
	return string(buf) + fracbuf + " s";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parsing

bool RecordingClock::ParseIso8601(const string& str, time_t& secOut, int64_t& fsOut, string& errorOut)
{
	//Trim, because a hand-edited metadata file may well have whitespace in it
	size_t b = str.find_first_not_of(" \t\r\n");
	if(b == string::npos)
	{
		errorOut = "empty";
		return false;
	}
	size_t e = str.find_last_not_of(" \t\r\n");
	string s = str.substr(b, e - b + 1);

	//Fixed-width date and time. Written with sscanf rather than std::get_time because the
	//latter cannot report how much of the string it consumed, and everything interesting here
	//is in the tail.
	int year = 0;
	int mon = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int sec = 0;
	int consumed = 0;
	if(sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n",
		&year, &mon, &day, &hour, &minute, &sec, &consumed) != 6)
	{
		//Some writers use a space where the spec calls for T
		if(sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d%n",
			&year, &mon, &day, &hour, &minute, &sec, &consumed) != 6)
		{
			errorOut = "not an ISO 8601 date and time";
			return false;
		}
	}

	//Range check before handing anything to timegm(), which normalises out-of-range fields
	//silently rather than complaining - so month 13 would quietly become January of the
	//following year instead of being reported as the malformed input it is.
	//
	//Second 60 is allowed: it is a leap second, and timegm() folds it into the next minute.
	if( (mon < 1) || (mon > 12) || (day < 1) || (day > 31) ||
		(hour > 23) || (minute > 59) || (sec > 60) ||
		(hour < 0) || (minute < 0) || (sec < 0) )
	{
		errorOut = "date or time field out of range";
		return false;
	}

	string tail = s.substr(consumed);
	size_t i = 0;

	//Fractional seconds, of any width. Scaled straight into femtoseconds a digit at a time so
	//that the field width never has to be known in advance; anything past the fifteenth digit
	//is finer than a femtosecond and is dropped.
	int64_t fs = 0;
	if( (i < tail.size()) && ((tail[i] == '.') || (tail[i] == ',')) )
	{
		i++;
		size_t digitStart = i;
		int64_t scale = g_fsPerSecond / 10;
		while( (i < tail.size()) && isdigit(static_cast<unsigned char>(tail[i])) )
		{
			if(scale > 0)
			{
				fs += (tail[i] - '0') * scale;
				scale /= 10;
			}
			i++;
		}

		if(i == digitStart)
		{
			errorOut = "decimal point with no digits after it";
			return false;
		}
	}

	//Timezone: Z, +HH:MM, +HHMM, +HH, or nothing at all
	int64_t tzOffsetSec = 0;
	bool haveTz = false;
	if(i < tail.size())
	{
		char c = tail[i];
		if( (c == 'Z') || (c == 'z') )
		{
			haveTz = true;
			i++;
		}

		else if( (c == '+') || (c == '-') )
		{
			int sign = (c == '-') ? -1 : 1;
			i++;

			string rest = tail.substr(i);
			int th = 0;
			int tm = 0;
			int n = 0;
			if(sscanf(rest.c_str(), "%2d:%2d%n", &th, &tm, &n) == 2)
				i += n;
			else if(sscanf(rest.c_str(), "%2d%2d%n", &th, &tm, &n) == 2)
				i += n;
			else if(sscanf(rest.c_str(), "%2d%n", &th, &n) == 1)
			{
				tm = 0;
				i += n;
			}
			else
			{
				errorOut = "malformed timezone offset";
				return false;
			}

			if( (th < 0) || (th > 23) || (tm < 0) || (tm > 59) )
			{
				errorOut = "timezone offset out of range";
				return false;
			}

			tzOffsetSec = sign * (th * 3600 + tm * 60);
			haveTz = true;
		}
	}

	if(i != tail.size())
	{
		errorOut = "trailing garbage after the timestamp";
		return false;
	}

	struct tm tmv;
	memset(&tmv, 0, sizeof(tmv));
	tmv.tm_year = year - 1900;
	tmv.tm_mon = mon - 1;
	tmv.tm_mday = day;
	tmv.tm_hour = hour;
	tmv.tm_min = minute;
	tmv.tm_sec = sec;

	//timegm() rather than mktime(): the fields are UTC once the offset below is applied, and
	//mktime() would interpret them in whatever timezone the machine is set to.
	time_t t = timegm(&tmv);
	if(t == static_cast<time_t>(-1))
	{
		errorOut = "date not representable";
		return false;
	}

	//The written time is local to the stated offset, so UTC is that time minus the offset
	t -= tzOffsetSec;

	if(!haveTz)
	{
		//Non-conformant: core:datetime is specified as an RFC 3339 string and the offset is
		//not optional there. Two of the eight demo recordings omit it, so reading it as UTC is
		//the useful thing to do - but a recording that was actually taken in a different
		//timezone is now wrong by that many hours, and only this warning will say so.
		LogWarning("SigMF core:datetime \"%s\" has no timezone, which RFC 3339 requires; "
			"reading it as UTC\n", s.c_str());
	}

	secOut = t;
	fsOut = fs;
	return true;
}
