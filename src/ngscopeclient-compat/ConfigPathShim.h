/**
	@file
	@brief Redirects the lifted PreferenceManager's config directory away from ngscopeclient's.

	PreferenceManager::DeterminePath() hardcodes the string "ngscopeclient":

	    CreateDirectory("~/.config");
	    CreateDirectory("~/.config/ngscopeclient");
	    m_configDir = ExpandPath("~/.config/ngscopeclient");
	    m_filePath  = ExpandPath("~/.config/ngscopeclient/preferences.yml");

	(PreferenceManager.cpp:112-119, and the %APPDATA% equivalent above it.)

	PreferenceManager is a singleton whose destructor calls SavePreferences(), so linking
	it unmodified means our process reads ngscopeclient's real preference file at static
	init and then overwrites it at exit with our nine-preference schema - silently
	destroying the user's ngscopeclient settings.

	Decision D4 forbids editing the submodule, so instead we interpose on the two scopehal
	helpers that path string is fed through, for this one translation unit only. The
	function-like macros below are not re-expanded when the preprocessor rescans their own
	replacement text, so each resolves to the real scopehal function with a rewritten
	argument.

	This header must be force-included (-include) rather than included by the .cpp, and it
	pulls in ngscopeclient.h first on purpose: the macros must not be visible when
	scopehal.h:306-307 declares ExpandPath() and CreateDirectory(), or the declarations
	themselves would be rewritten.

	See notes/R2-lifted-primitives.md.
 */
#ifndef ConfigPathShim_h
#define ConfigPathShim_h

//Must come first: this is where ExpandPath() and CreateDirectory() are declared.
#include "ngscopeclient.h"

#include <string>

namespace imcufosphor
{
	/**
		@brief Rewrites an ngscopeclient config path into the imcufosphor equivalent.

		Anything that does not mention ngscopeclient is passed through untouched.
	 */
	inline std::string RemapConfigPath(const std::string& path)
	{
		const std::string from = "ngscopeclient";
		const std::string to = "imcufosphor";

		std::string out = path;
		for(size_t pos = out.find(from); pos != std::string::npos; pos = out.find(from, pos + to.length()))
			out.replace(pos, from.length(), to);
		return out;
	}
}

//Windows takes a different path through DeterminePath(): it builds a wide string with
//PathCombineW(..., L"ngscopeclient") and never calls either helper, so these macros would
//not bite there. CreateDirectory is also a windows.h macro, so defining it would break the
//build outright. Left unsolved until someone builds for Windows.
#ifndef _WIN32
#define ExpandPath(p) ExpandPath(::imcufosphor::RemapConfigPath(p))
#define CreateDirectory(p) CreateDirectory(::imcufosphor::RemapConfigPath(p))
#endif

#endif
