# Block groups, and the aggregate target a consumer normally links.

# Registers a Boost.UT suite as a CTest test.
#
# Tests link the block headers rather than a library, because the blocks are header-only; the
# GPU-side display code they pull in comes with the render target attached to those headers.
# Every test here is headless by construction: nothing publishes a RenderHost, so draw() no-ops
# and no Vulkan device is required.
function(gr_imcufosphor_add_ut_test target_name source_file)
	add_executable(${target_name} ${source_file})
	target_link_libraries(${target_name} PRIVATE
		${GR_IMCU_GNURADIO4_TARGET}
		${GR_IMCU_BOOST_UT_TARGET})
	add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

add_subdirectory(display)

add_library(gr_imcufosphor_blocks_headers INTERFACE)
add_library(gr_imcufosphor::blocks_headers ALIAS gr_imcufosphor_blocks_headers)

target_link_libraries(gr_imcufosphor_blocks_headers INTERFACE
	gr_imcufosphor::blocks_display_headers)

# So the in-build alias and the imported name a consumer sees are the same string.
# Exported only in standalone mode; see the top-level CMakeLists for why.
if(PROJECT_IS_TOP_LEVEL)
	set_target_properties(gr_imcufosphor_blocks_headers PROPERTIES EXPORT_NAME blocks_headers)
	install(TARGETS gr_imcufosphor_blocks_headers EXPORT gr_imcufosphorTargets)
endif()
