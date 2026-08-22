# Synthetic SigMF recording generator, used to give the verification suite test data.
#
# Deliberately links nothing. It exists so that `ctest` needs no external dataset, and a
# fixture generator that pulled in Vulkan or libsigmf could itself be the reason a test
# fails.

add_executable(mksigmf mksigmf.cpp)
target_compile_options(mksigmf PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

# The recording the tests run against. Generated at build time rather than committed: it is
# 4 MiB of samples that are fully described by the twenty lines that produce them.
set(IMCUFOSPHOR_TEST_RECORDING "${CMAKE_BINARY_DIR}/testdata/synthetic-tone" CACHE INTERNAL
	"Basename of the generated test recording, without the .sigmf-meta / .sigmf-data suffix")

add_custom_command(
	OUTPUT "${IMCUFOSPHOR_TEST_RECORDING}.sigmf-meta" "${IMCUFOSPHOR_TEST_RECORDING}.sigmf-data"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/testdata"
	COMMAND mksigmf "${IMCUFOSPHOR_TEST_RECORDING}"
	DEPENDS mksigmf
	COMMENT "Generating synthetic test recording"
	VERBATIM)

add_custom_target(testdata ALL
	DEPENDS "${IMCUFOSPHOR_TEST_RECORDING}.sigmf-meta" "${IMCUFOSPHOR_TEST_RECORDING}.sigmf-data")
