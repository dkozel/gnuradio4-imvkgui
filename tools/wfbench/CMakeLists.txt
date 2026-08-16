# Headless throughput benchmark for the waterfall pipeline.

add_executable(wfbench
	wfbench.cpp
	Verify.cpp
	)

target_compile_options(wfbench PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

target_link_libraries(wfbench
	imcufosphor-core
	)

# wfbench drives real ComputePipelines, so it needs the same shader tree next to its binary
# that the application gets. Without this every pipeline fails to resolve its .spv.
imcufosphor_attach_shaders(wfbench "${IMCUFOSPHOR_APP_SPV_DIR}")
