# The display sinks.
#
# Header-only, which is the gnuradio4 convention for block modules, but unlike a typical one
# these headers pull in Vulkan, scopehal and a whole compute pipeline through
# imcufosphor-render. That is the deliberate trade: the blocks wrap the existing GPU display
# rather than reimplementing a CPU one, so anything including them inherits its dependencies.

add_library(gr_imcufosphor_blocks_display_headers INTERFACE)
add_library(gr_imcufosphor::blocks_display_headers ALIAS gr_imcufosphor_blocks_display_headers)

target_include_directories(gr_imcufosphor_blocks_display_headers INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(gr_imcufosphor_blocks_display_headers INTERFACE
	${GR_IMCU_GNURADIO4_TARGET}
	${GR_IMCU_RENDER_TARGET})

target_compile_features(gr_imcufosphor_blocks_display_headers INTERFACE cxx_std_23)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/gnuradio-4.0/imcufosphor
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gnuradio-4.0)

# Exported only in standalone mode; see the top-level CMakeLists for why
if(PROJECT_IS_TOP_LEVEL)
	set_target_properties(gr_imcufosphor_blocks_display_headers PROPERTIES
		EXPORT_NAME blocks_display_headers)
	install(TARGETS gr_imcufosphor_blocks_display_headers EXPORT gr_imcufosphorTargets)
endif()

if(GR_IMCUFOSPHOR_ENABLE_TESTING)
	add_subdirectory(test)
endif()
