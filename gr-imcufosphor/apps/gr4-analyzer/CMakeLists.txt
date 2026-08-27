# The example host: a GNU Radio flowgraph rendered through the imcufosphor display.

add_executable(gr4-analyzer
	main.cpp
	Gr4AnalyzerWindow.cpp
	)

target_link_libraries(gr4-analyzer PRIVATE
	gr_imcufosphor::blocks_display_headers
	${GR_IMCU_RENDER_TARGET}
	)

# The incubator's SoapyRx block is header-only but calls straight into libSoapySDR, so an
# application including it has to link that itself. Optional: without it the app builds with
# the file and generator sources only, which is what a machine with no SDR wants anyway.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SOAPYSDR IMPORTED_TARGET SoapySDR)

if(TARGET PkgConfig::SOAPYSDR)
	target_link_libraries(gr4-analyzer PRIVATE PkgConfig::SOAPYSDR)
	target_compile_definitions(gr4-analyzer PRIVATE GR4_ANALYZER_HAVE_SOAPYSDR=1)
	message(STATUS "gr4-analyzer: SoapySDR ${SOAPYSDR_VERSION} found, --soapy enabled")
else()
	message(STATUS "gr4-analyzer: SoapySDR not found, --soapy will be unavailable")
endif()

# gr-omnisig, for --omnisig. Optional in the same way SoapySDR is: it needs the DeepSig SDK, a CUDA
# toolkit and a licensed node, none of which a machine that only wants to look at a spectrum has.
#
# Not searched for by default, because find_package(gr_omnisig) would then fail loudly on every
# machine without it. Point GR_OMNISIG_DIR at the installed package to turn it on:
#
#   cmake -DGR_OMNISIG_DIR=/path/to/gr-omnisig/install/lib/cmake/gr_omnisig ...
#
# Two targets, and both are needed: the block headers, and exactly one engine implementation.
# gr_omnisig::engine is the real libomnisig; gr_omnisig::engine_stub is a fake for a machine without
# it. Linking both is a duplicate definition of makeContext().
set(GR_OMNISIG_DIR "" CACHE PATH "Directory containing gr_omnisigConfig.cmake, to enable --omnisig")

if(GR_OMNISIG_DIR)
	# find_package needs gnuradio4 discoverable as a CMake package, because gr_omnisig's own config
	# declares it as a dependency. This project otherwise finds gnuradio4 through pkg-config, so the
	# prefix has to be on CMAKE_PREFIX_PATH as well:
	#
	#   cmake -DCMAKE_PREFIX_PATH=<gnuradio4-install> -DGR_OMNISIG_DIR=... .
	find_package(gr_omnisig REQUIRED PATHS "${GR_OMNISIG_DIR}" NO_DEFAULT_PATH)

	# gr_omnisig::engine and NOT gr_omnisig::blocks_headers, deliberately.
	#
	# Both carry the block's include directory, and either would compile. The difference is that
	# blocks_headers also links gnuradio4::gnuradio-core, which propagates gnuradio4::gnuradio-options
	# - that is -Werror with -Wconversion, -Wsign-conversion, -Wold-style-cast and more. Applied to
	# this target it lands on the vendored scopehal headers, which do not compile clean under it and
	# are not ours to change. gnuradio4 arrives here through PkgConfig::GNURADIO4 already, so nothing
	# is lost by taking the include path from the engine target instead.
	#
	# The engine target is the real libomnisig one; gr_omnisig::engine_stub is the GPU-free fake, and
	# linking both is a duplicate definition of makeContext().
	target_link_libraries(gr4-analyzer PRIVATE gr_omnisig::engine)
	target_compile_definitions(gr4-analyzer PRIVATE GR4_ANALYZER_HAVE_OMNISIG=1)
	message(STATUS "gr4-analyzer: gr-omnisig found, --omnisig enabled")
else()
	message(STATUS "gr4-analyzer: GR_OMNISIG_DIR unset, --omnisig will be unavailable")
endif()

# Compute shaders and colour ramps are found relative to the binary at runtime (FindDataFile
# searches the executable's own directory first), so every executable needs its own copy. Only
# available in subdirectory mode, where the superbuild's helpers are in scope; a standalone
# build would need the installed data directory instead.
if(COMMAND imcufosphor_attach_shaders)
	imcufosphor_attach_shaders(gr4-analyzer "${IMCUFOSPHOR_APP_SPV_DIR}")
	imcufosphor_attach_icons(gr4-analyzer)
else()
	message(WARNING
		"imcufosphor_attach_shaders() not available: gr4-analyzer will not find its shaders "
		"unless they are installed somewhere FindDataFile() searches.")
endif()

install(TARGETS gr4-analyzer RUNTIME)
