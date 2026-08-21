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
