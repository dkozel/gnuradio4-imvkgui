/**
	@file
	@brief The preference schema for imcufosphor.

	This file stands in for ngscopeclient's PreferenceSchema.cpp. PreferenceManager.cpp is
	lifted unmodified, but it declares two things it does not define:

	  - PreferenceManager::m_instance, the singleton storage
	  - PreferenceManager::InitializeDefaults(), the schema

	Upstream puts both in PreferenceSchema.cpp (669 lines describing ngscopeclient's whole
	UI). We supply our own, containing only what the lifted primitives actually read.

	That set is small and entirely determined by VulkanWindow:

	  - VulkanWindow.cpp:145      Appearance.Windowing.startup_mode  (enum)
	  - VulkanWindow.cpp:179-187  Appearance.Startup.*               (nine invisible values)
	  - VulkanWindow.cpp:876-893  the same nine, written back by SaveWindowPositionAndSize()

	Every path is looked up by string through PreferenceCategory::GetLeaf(), which throws
	std::runtime_error on a miss, so an incomplete schema is a startup crash rather than a
	compile error. Any preference added here must keep its upstream identifier exactly -
	including "heigth", which is misspelled upstream in both the schema and the reader.

	Nothing else in the lift touches preferences: TextureManager has no preference
	references at all.

	See notes/R2-lifted-primitives.md.
 */

#include "ngscopeclient.h"
#include "PreferenceManager.h"
#include "PreferenceTypes.h"

PreferenceManager PreferenceManager::m_instance;

void PreferenceManager::InitializeDefaults()
{
	auto& appearance = this->m_treeRoot.AddCategory("Appearance");

		//How the window comes up. Read once, in the VulkanWindow constructor.
		auto& windowing = appearance.AddCategory("Windowing");
			windowing.AddPreference(
				Preference::Enum("startup_mode", STARTUP_MODE_WINDOWED)
					.Label("Window startup mode")
					.Description(
						"How the main window is opened at startup.\n"
						"\n"
						" - Windowed: a fixed 1280x720 window,\n"
						" - Maximized: maximized on the main screen,\n"
						" - Last State: restored to the last position and size.\n"
						)
					.EnumValue("Windowed", STARTUP_MODE_WINDOWED)
					.EnumValue("Maximized", STARTUP_MODE_MAXIMIZED)
					.EnumValue("Last State", STARTUP_MODE_LAST_STATE)
				);

		//Persisted window geometry. Invisible: these are state, not settings, and there is
		//no preference dialog in this application to show them in anyway.
		auto& startup = appearance.AddCategory("Startup");
			startup.AddPreference(Preference::Bool("startup_fullscreen", false).Invisible());
			startup.AddPreference(Preference::Bool("startup_maximized", false).Invisible());
			startup.AddPreference(Preference::Int("startup_pos_x", 0).Invisible());
			startup.AddPreference(Preference::Int("startup_pos_y", 0).Invisible());
			startup.AddPreference(Preference::Int("startup_size_width", 0).Invisible());
			startup.AddPreference(Preference::Int("startup_size_heigth", 0).Invisible());
			startup.AddPreference(Preference::String("monitor_name", "").Invisible());
			startup.AddPreference(Preference::Int("monitor_width", 0).Invisible());
			startup.AddPreference(Preference::Int("monitor_heigth", 0).Invisible());
}
