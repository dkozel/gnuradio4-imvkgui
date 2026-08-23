# The application (sigmf-spectrum) and the engine library underneath it (imcufosphor-core).

# Compiled into shaders-spv/, deliberately not shaders/: shaders/ next to the binary is the
# runtime search directory that the collection step below fills, and having glslc write into
# the same directory would make the two steps fight over it.
add_subdirectory(shaders shaders-spv)

########################################################################
# Engine
########################################################################

# The source, the filter graph and playback, with no dependency on a window.
#
# Split out from the application so that headless tools - tools/wfbench in particular - can
# drive the real pipeline rather than a reimplementation of it. A benchmark that measures
# something other than what the application runs is worse than no benchmark.
add_library(imcufosphor-core STATIC
	Annotation.cpp
	ComplexFFTFilter.cpp
	GpuTimer.cpp
	IqInjector.cpp
	PlayerSession.cpp
	RecordingClock.cpp
	SigMFSource.cpp
	SpectrumDensity.cpp
	SpectrumEngine.cpp
	SpectrumReducer.cpp
	)

target_include_directories(imcufosphor-core
PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
	)

# ComplexFFTFilter.h includes VulkanFFTPlan.h, which pulls in vkFFT.h. VkFFT needs this
# configuration and the glslang headers, which it does not include by a standard path.
# PUBLIC because anything including ComplexFFTFilter.h inherits the same requirement.
target_compile_definitions(imcufosphor-core PUBLIC -DVK_API_VERSION=10 -DVKFFT_BACKEND=0)
target_include_directories(imcufosphor-core PUBLIC ${glslang_INCLUDE_DIR}/glslang/Include)

# -Werror applies to our code only, never to the submodules. Third-party headers pulled in
# here (libsigmf and what it re-exports) are marked SYSTEM at the top level so their
# warnings do not trip this.
target_compile_options(imcufosphor-core PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

# scopeprotocols was a second shared library of 211 filters, of which this project used one
# (Waterfall). It is compiled into the vendored scopehal target now.
target_link_libraries(imcufosphor-core
PUBLIC
	scopehal
	libsigmf
	)

set_property(TARGET imcufosphor-core PROPERTY POSITION_INDEPENDENT_CODE ON)

########################################################################
# Display
########################################################################

# Everything that draws, and nothing that decides what to draw.
#
# Split out of the sigmf-spectrum executable so that a second host - the GNU Radio 4 blocks in
# gr-imcufosphor - can render the same panes without linking main.cpp. The areas were always
# independent of the application (they take a filter pointer, not a session), so this is a
# target boundary drawn around code that was already separable, not a refactor of it.
#
# The files stay in this directory. Moving them would break every #include for no gain.
add_library(imcufosphor-render STATIC
	AnalyzerPane.cpp
	AnnotationOverlay.cpp
	PlotAxis.cpp
	RenderHost.cpp
	SpectrumArea.cpp
	WaterfallArea.cpp
	)

target_include_directories(imcufosphor-render
PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	)

target_compile_options(imcufosphor-render PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

target_link_libraries(imcufosphor-render
PUBLIC
	imcufosphor-core
	imcufosphor-window
	)

set_property(TARGET imcufosphor-render PROPERTY POSITION_INDEPENDENT_CODE ON)

# Each of these headers compiled on its own, to prove it declares what it uses. See
# cmake/HeaderSelfCheck.cmake for why this is a build target and not a convention.
imcufosphor_header_selfcheck(imcufosphor-render-headercheck imcufosphor-render
	HEADERS
		${CMAKE_CURRENT_SOURCE_DIR}/AnalyzerPane.h
		${CMAKE_CURRENT_SOURCE_DIR}/AnalyzerSource.h
		${CMAKE_CURRENT_SOURCE_DIR}/Annotation.h
		${CMAKE_CURRENT_SOURCE_DIR}/AnnotationOverlay.h
		${CMAKE_CURRENT_SOURCE_DIR}/ComplexFFTFilter.h
		${CMAKE_CURRENT_SOURCE_DIR}/GpuTimer.h
		${CMAKE_CURRENT_SOURCE_DIR}/IqInjector.h
		${CMAKE_CURRENT_SOURCE_DIR}/PackedIQWaveform.h
		${CMAKE_CURRENT_SOURCE_DIR}/PlayerSession.h
		${CMAKE_CURRENT_SOURCE_DIR}/PlotAxis.h
		${CMAKE_CURRENT_SOURCE_DIR}/RecordingClock.h
		${CMAKE_CURRENT_SOURCE_DIR}/RenderHost.h
		${CMAKE_CURRENT_SOURCE_DIR}/RowHistory.h
		${CMAKE_CURRENT_SOURCE_DIR}/SigMFSource.h
		${CMAKE_CURRENT_SOURCE_DIR}/SpectrumArea.h
		${CMAKE_CURRENT_SOURCE_DIR}/SpectrumDensity.h
		${CMAKE_CURRENT_SOURCE_DIR}/SpectrumEngine.h
		${CMAKE_CURRENT_SOURCE_DIR}/SpectrumReducer.h
		${CMAKE_CURRENT_SOURCE_DIR}/WaterfallArea.h
	)

########################################################################
# Application
########################################################################

add_executable(sigmf-spectrum
	main.cpp
	MainWindow.cpp
	)

target_compile_options(sigmf-spectrum PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

target_link_libraries(sigmf-spectrum
	imcufosphor-render
	)

imcufosphor_attach_shaders(sigmf-spectrum "${IMCUFOSPHOR_APP_SPV_DIR}")
imcufosphor_attach_icons(sigmf-spectrum)

install(TARGETS sigmf-spectrum RUNTIME)
