# Throughput benchmark for the GR4 datapath. No window, so no ngscopeclient display code is
# needed beyond what imcufosphor-render already links.

add_executable(gr4-bench main.cpp)

target_link_libraries(gr4-bench PRIVATE
	gr_imcufosphor::blocks_display_headers
	${GR_IMCU_RENDER_TARGET}
	)

# Runs real compute pipelines, so it needs the shaders next to the binary exactly as the
# application does
if(COMMAND imcufosphor_attach_shaders)
	imcufosphor_attach_shaders(gr4-bench "${IMCUFOSPHOR_APP_SPV_DIR}")
	imcufosphor_attach_icons(gr4-bench)
endif()

install(TARGETS gr4-bench RUNTIME)
