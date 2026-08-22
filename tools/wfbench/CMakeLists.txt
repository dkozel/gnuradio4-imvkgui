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

########################################################################
# The correctness gate
#
# All six checks in Verify.h behind one command, against the generated recording rather than
# an external dataset. This is the only automated check that exercises the GPU path, and so
# the only thing that would notice the NVIDIA Xid-32 fault returning (DESIGN.md section 3,
# R7) - which is exactly what went unnoticed through the last submodule bump.
#
# It needs a working Vulkan device. On a machine without one it fails rather than skipping,
# which is the right answer for this project: every target here is a GPU target.

add_dependencies(wfbench testdata)

add_test(NAME wfbench-verify
	COMMAND wfbench
		--file "${IMCUFOSPHOR_TEST_RECORDING}.sigmf-meta"
		--verify
		--expect-tone 100125000)

set_tests_properties(wfbench-verify PROPERTIES
	LABELS gpu
	TIMEOUT 600)
