/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief ImGui, for code that draws and nothing else

	The smallest thing a file needs in order to say ImVec2 or ImDrawList.

	This exists because the obvious way to get ImGui in this project used to be
	`#include "ngscopeclient.h"`, and that header is an umbrella: 121 lines of which nine are
	load-bearing, plus a further 1081 lines of BERTState.h, OscilloscopeState.h,
	PowerSupplyState.h, MultimeterState.h, LoadState.h, FunctionGeneratorState.h, Marker.h,
	FontManager.h, GuiLogSink.h, Event.h and ImGuiDisabler.h - ngscopeclient's live-instrument
	session model, arriving in a file player that has no instruments.

	Note there is no `#define IMGUI_DEFINE_MATH_OPERATORS` here. It is a PUBLIC compile
	definition on the imcufosphor-window target instead, because it has to be identical in
	every translation unit that sees ImVec2: defining it in some and not others changes the
	type's operator set between objects, which is an ODR violation the linker will not
	diagnose.
 */
#ifndef ImGuiCompat_h
#define ImGuiCompat_h

#include <imgui.h>

#endif
