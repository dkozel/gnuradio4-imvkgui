/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Where and how the main window comes up

	Ours, replacing ngscopeclient's preference system.

	@par What this replaces

	VulkanWindow read ten values through PreferenceManager: a startup mode, and nine fields of
	saved geometry. Supplying them cost 1604 lines of upstream code across Preference.cpp,
	PreferenceTree.cpp and PreferenceManager.cpp - a type-erased variant with a hand-rolled
	move, a builder DSL, a category tree, a path splitter and a YAML serializer - plus a
	70-line schema of our own to declare the ten, plus a 68-line preprocessor shim
	(ConfigPathShim.h) to stop the singleton's destructor rewriting the real ngscopeclient
	user's ~/.config file with our schema on exit.

	None of it was reachable as a preference: there is no preference dialog in this
	application, and nothing else in the project has a setting. It was a serialization
	framework being used as a struct.

	@par Persistence

	Deliberately absent. The struct carries defaults and VulkanWindow reads them; nothing
	writes a file. Losing the window position across runs is a small and obvious cost, and the
	right time to add persistence back is when someone notices, in whatever format the project
	then wants - not by keeping 1600 lines of YAML machinery against the possibility.
 */
#ifndef WindowGeometry_h
#define WindowGeometry_h

#include <string>

/**
	@brief How the main window is opened, and where it was last time

	Was the "Appearance.Windowing" and "Appearance.Startup" preference categories.
 */
class WindowGeometry
{
public:

	enum StartupMode
	{
		///@brief A fixed 1280x720 window
		STARTUP_WINDOWED,

		///@brief Maximized on the main screen
		STARTUP_MAXIMIZED,

		///@brief Restored to the last position and size
		STARTUP_LAST_STATE
	};

	///@brief How the window comes up. Read once, in the VulkanWindow constructor.
	StartupMode mode = STARTUP_WINDOWED;

	///@brief Saved window size, or 0 if none. Zero means "no saved geometry, maximize".
	int width = 0;
	int height = 0;

	///@brief Saved window position
	int x = 0;
	int y = 0;

	///@brief Saved window state
	bool fullscreen = false;
	bool maximized = false;

	/**
		@brief The monitor the window was last on

		Checked against the current monitor layout before a saved position is trusted, so that
		a window saved on a monitor that has since been unplugged does not open off screen.
	 */
	std::string monitorName;
	int monitorWidth = 0;
	int monitorHeight = 0;
};

#endif
