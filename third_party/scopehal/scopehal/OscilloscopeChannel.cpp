/***********************************************************************************************************************
*                                                                                                                      *
* libscopehal                                                                                                          *
*                                                                                                                      *
* Copyright (c) 2012-2026 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of OscilloscopeChannel

	@par Local modification

	Every accessor here used to be `if(m_instrument) return GetScope()->Something(...); else
	return <default>;` - a pass-through to the instrument that owns the channel.

	There are no instruments in this project. Every OscilloscopeChannel constructed anywhere in
	it passes a null scope: Filter.cpp:60 for every filter in the graph, and both ComplexChannel
	constructions, in SigMFSource and IqInjector. So all 34 of those branches were dead, and
	each one was a virtual call that made this file - and therefore the whole filter graph -
	depend on Oscilloscope being a complete type, which drags in Oscilloscope.cpp,
	Instrument.cpp, Trigger.cpp and EdgeTrigger.cpp.

	They are collapsed to the branch that actually ran. Coupling, attenuation, bandwidth limit,
	deskew, input mux and download progress now return the same defaults they always did.
 */

#include "scopehal.h"
#include "OscilloscopeChannel.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

OscilloscopeChannel::OscilloscopeChannel(
	Instrument* scope,
	const string& hwname,
	const string& color,
	Unit xunit,
	size_t index)
	: InstrumentChannel(scope, hwname, color, xunit, index)
	, m_downloadState(DownloadState::DOWNLOAD_UNKNOWN)
	, m_downloadProgress(0.0)
	, m_downloadStartTime(0.0)
	, m_refcount(0)
{
}

OscilloscopeChannel::OscilloscopeChannel(
	Instrument* scope,
	const string& hwname,
	const string& color,
	Unit xunit,
	Unit yunit,
	Stream::StreamType stype,
	size_t index)
	: InstrumentChannel(scope, hwname, color, xunit, yunit, stype, index)
	, m_downloadState(DownloadState::DOWNLOAD_UNKNOWN)
	, m_downloadProgress(0.0)
	, m_downloadStartTime(0.0)
	, m_refcount(0)
{
}

/**
	@brief Gives a channel a default display name if there's not one already.

	MUST NOT be called until the channel has been added to its parent scope.
 */
void OscilloscopeChannel::SetDefaultDisplayName()
{
	//If we have a scope, m_displayname is ignored.
	//Start out by pulling the name from hardware.
	//If it's not set, use our hardware name as the default.
}

OscilloscopeChannel::~OscilloscopeChannel()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers for calling scope functions

void OscilloscopeChannel::AddRef()
{
	if(m_refcount == 0)
		Enable();
	m_refcount ++;
}

void OscilloscopeChannel::Release()
{
	m_refcount --;
	if(m_refcount == 0)
		Disable();
}

float OscilloscopeChannel::GetOffset(size_t stream)
{
	return 0;
}

void OscilloscopeChannel::SetOffset(float offset, size_t stream)
{
}

bool OscilloscopeChannel::IsEnabled()
{
	return true;
}

void OscilloscopeChannel::Enable()
{
}

void OscilloscopeChannel::Disable()
{
}

OscilloscopeChannel::CouplingType OscilloscopeChannel::GetCoupling()
{
	return OscilloscopeChannel::COUPLE_SYNTHETIC;
}

vector<OscilloscopeChannel::CouplingType> OscilloscopeChannel::GetAvailableCouplings()
{
	vector<OscilloscopeChannel::CouplingType> ret;
	ret.push_back(COUPLE_SYNTHETIC);
	return ret;
}

void OscilloscopeChannel::SetCoupling(CouplingType type)
{
}

double OscilloscopeChannel::GetAttenuation()
{
	return 1;
}

void OscilloscopeChannel::SetAttenuation(double atten)
{
}

int OscilloscopeChannel::GetBandwidthLimit()
{
	return 0;
}

void OscilloscopeChannel::SetBandwidthLimit(int mhz)
{
}

bool OscilloscopeChannel::IsInverted([[maybe_unused]] size_t stream)
{
	return false;
}

float OscilloscopeChannel::GetVoltageRange(size_t stream)
{
	return 1;	//TODO: get from input
}

void OscilloscopeChannel::SetVoltageRange(float range, size_t stream)
{
}

void OscilloscopeChannel::SetDeskew(int64_t skew)
{
}

int64_t OscilloscopeChannel::GetDeskew()
{
	return 0;
}

void OscilloscopeChannel::SetDigitalHysteresis(float level)
{
}

void OscilloscopeChannel::SetDigitalThreshold(float level)
{
}

void OscilloscopeChannel::SetCenterFrequency(int64_t freq)
{
}

void OscilloscopeChannel::SetDisplayName(string name)
{
	InstrumentChannel::SetDisplayName(name);
}

string OscilloscopeChannel::GetDisplayName()
{
	//Use cached name if we have it
	auto cached = InstrumentChannel::GetDisplayName();
	if(!cached.empty())
		return cached;

	//If not, pull from hardware

	//No hardware? just use hwname
	else
		return m_hwname;
}

bool OscilloscopeChannel::CanInvert()
{
	return false;
}

void OscilloscopeChannel::Invert(bool invert)
{
}

bool OscilloscopeChannel::IsInverted()
{
	return false;
}

void OscilloscopeChannel::AutoZero()
{
}

bool OscilloscopeChannel::CanAutoZero()
{
	return false;
}

void OscilloscopeChannel::Degauss()
{
}

bool OscilloscopeChannel::CanDegauss()
{
	return false;
}

string OscilloscopeChannel::GetProbeName()
{
	return "";
}

bool OscilloscopeChannel::HasInputMux()
{
	return false;
}

size_t OscilloscopeChannel::GetInputMuxSetting()
{
	return 0;
}

void OscilloscopeChannel::SetInputMux(size_t select)
{
}

InstrumentChannel::DownloadState OscilloscopeChannel::GetDownloadState()
{
	return m_downloadState;
}

float OscilloscopeChannel::GetDownloadProgress()
{
	return m_downloadProgress;
}

double OscilloscopeChannel::GetDownloadStartTime()
{
	return m_downloadStartTime;
}

/**
	@brief Check if this channel is able to handle high rate (e.g. every mouse movement) offset changes

	@return True if yes (instrument is fast or driver rate-limits), false if GUI should rate limit
 */
bool OscilloscopeChannel::IsHighRateOffsetCapable()
{
	//No scope, we must be a filter or something so always high rate capable
	return true;
}
