# GNU Radio 4 blocks wrapping the imcufosphor Vulkan spectrum display.
#
# Structured as a standard gr4_modtool out-of-tree module - manifest, per-group header
# libraries, export set, package config - but normally built as a subdirectory of the
# imcufosphor superbuild so that the blocks can link the display targets directly instead of
# going through an install. See cmake/Dependencies.cmake.

cmake_minimum_required(VERSION 3.22)
project(gr_imcufosphor LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)

option(GR_IMCUFOSPHOR_ENABLE_TESTING "Build the block unit tests" ON)
option(GR_IMCUFOSPHOR_BUILD_APPS "Build the gr4-analyzer host application" ON)

set(GR_IMCUFOSPHOR_BOOST_UT_INCLUDE_DIR "" CACHE PATH
	"Directory containing boost/ut.hpp, if it is not installed system-wide")

if(PROJECT_IS_TOP_LEVEL)
	set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)
gr_imcufosphor_resolve_dependencies()

if(GR_IMCUFOSPHOR_ENABLE_TESTING)
	include(CTest)
	enable_testing()
endif()

add_subdirectory(blocks)

if(GR_IMCUFOSPHOR_BUILD_APPS)
	add_subdirectory(apps)
endif()

########################################################################
# Package config
########################################################################

# Only in standalone mode. As a subdirectory of the imcufosphor superbuild the block headers
# link imcufosphor-render, which the superbuild does not export, so an export set naming them
# cannot be generated - and would be pointless anyway, since the only consumer is the build
# that already has the targets. Splitting this module out later means adding install/export
# rules to imcufosphor, at which point this branch starts being taken.
if(NOT PROJECT_IS_TOP_LEVEL)
	return()
endif()

# Exported so that a consumer outside this build can find_package(gr_imcufosphor) and get the
# block headers with their include paths and gnuradio4 dependency already attached.

include(CMakePackageConfigHelpers)

set(GR_IMCUFOSPHOR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/gr_imcufosphor"
	CACHE STRING "Install location for gr_imcufosphor CMake package files")

install(EXPORT gr_imcufosphorTargets
	FILE gr_imcufosphorTargets.cmake
	NAMESPACE gr_imcufosphor::
	DESTINATION "${GR_IMCUFOSPHOR_INSTALL_CMAKEDIR}")

configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/gr_imcufosphorConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/gr_imcufosphorConfig.cmake"
	INSTALL_DESTINATION "${GR_IMCUFOSPHOR_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/gr_imcufosphorConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion)

install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/gr_imcufosphorConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/gr_imcufosphorConfigVersion.cmake"
	DESTINATION "${GR_IMCUFOSPHOR_INSTALL_CMAKEDIR}")
