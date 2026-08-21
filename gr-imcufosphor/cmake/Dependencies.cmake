# Dependency resolution, in two modes.
#
# This module is normally built as a subdirectory of the imcufosphor superbuild, where the
# display libraries are live CMake targets and gnuradio4 has already been found. It is also
# meant to be splittable out later into a standalone repository without restructuring, which is
# why every consumer below refers only to the two variables this file sets rather than naming a
# target directly.
#
#   subdirectory mode  imcufosphor-render and PkgConfig::GNURADIO4 come from the parent
#   standalone mode    gnuradio4 via pkg-config, imcufosphor via its installed package config
#
# The standalone branch needs imcufosphor to export an installed package, which it does not do
# yet. It is written out now anyway so that the split is a build-system change rather than a
# source change, and so the coupling stays visible.

include_guard(GLOBAL)

function(gr_imcufosphor_resolve_dependencies)

	if(PROJECT_IS_TOP_LEVEL)

		find_package(PkgConfig REQUIRED)
		pkg_check_modules(GR_IMCU_GR4 REQUIRED IMPORTED_TARGET gnuradio4)
		set(GR_IMCU_GNURADIO4_TARGET PkgConfig::GR_IMCU_GR4 PARENT_SCOPE)

		find_package(imcufosphor REQUIRED)
		set(GR_IMCU_RENDER_TARGET imcufosphor::render PARENT_SCOPE)

	else()

		# The parent found gnuradio4 before adding this directory; reusing its imported target
		# rather than probing again keeps one set of flags in the build.
		if(NOT TARGET PkgConfig::GNURADIO4)
			message(FATAL_ERROR
				"gr-imcufosphor was added as a subdirectory but PkgConfig::GNURADIO4 does not "
				"exist. The parent must pkg_check_modules(GNURADIO4 REQUIRED IMPORTED_TARGET "
				"gnuradio4) before add_subdirectory().")
		endif()
		set(GR_IMCU_GNURADIO4_TARGET PkgConfig::GNURADIO4 PARENT_SCOPE)

		if(NOT TARGET imcufosphor-render)
			message(FATAL_ERROR
				"gr-imcufosphor was added as a subdirectory but the imcufosphor-render target "
				"does not exist. Add src/imcufosphor before this directory.")
		endif()
		set(GR_IMCU_RENDER_TARGET imcufosphor-render PARENT_SCOPE)

	endif()

	if(GR_IMCUFOSPHOR_ENABLE_TESTING)
		gr_imcufosphor_find_boost_ut()
		set(GR_IMCU_BOOST_UT_TARGET ${GR_IMCU_BOOST_UT_TARGET} PARENT_SCOPE)
	endif()

endfunction()

# boost-ut, which gnuradio4 fetches into its own build tree rather than installing. Preferring
# an installed package, then a system header, then the copy inside the gnuradio4 build - the
# last is what actually exists on a developer machine today, and hardcoding only that would
# make the module unbuildable anywhere else.
function(gr_imcufosphor_find_boost_ut)

	find_package(boost_ut CONFIG QUIET)
	if(TARGET boost_ut::ut)
		set(GR_IMCU_BOOST_UT_TARGET boost_ut::ut PARENT_SCOPE)
		return()
	endif()

	find_path(GR_IMCU_BOOST_UT_INCLUDE
		NAMES boost/ut.hpp
		HINTS ${GR_IMCUFOSPHOR_BOOST_UT_INCLUDE_DIR}
		PATH_SUFFIXES boost-ut/include include)

	if(NOT GR_IMCU_BOOST_UT_INCLUDE)
		message(FATAL_ERROR
			"boost-ut headers not found (boost/ut.hpp). Install boost-ut, or point "
			"-DGR_IMCUFOSPHOR_BOOST_UT_INCLUDE_DIR at the copy in the gnuradio4 build tree, "
			"e.g. <gnuradio4-build>/projects/gnuradio4-core/_deps/ut-src/include")
	endif()

	add_library(gr_imcufosphor_boost_ut INTERFACE)
	target_include_directories(gr_imcufosphor_boost_ut SYSTEM INTERFACE ${GR_IMCU_BOOST_UT_INCLUDE})
	add_library(gr_imcufosphor::boost_ut ALIAS gr_imcufosphor_boost_ut)
	set(GR_IMCU_BOOST_UT_TARGET gr_imcufosphor::boost_ut PARENT_SCOPE)

endfunction()
