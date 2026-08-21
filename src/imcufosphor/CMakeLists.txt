# The application (sigmf-spectrum) and the engine library underneath it (imcufosphor-core).
#
# Files under ${SCOPEHAL_APPS_DIR} are lifted verbatim from the scopehal-apps submodule
# and referenced in place, never copied, so that upstream updates are a submodule bump.
# Do not edit them; if a change is needed, subclass or wrap it. See DESIGN.md section 5.

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

# ComplexFFTFilter.h includes VulkanFFTPlan.h, which pulls in vkFFT.h. VkFFT needs the
# same configuration scopeprotocols uses (scopeprotocols/CMakeLists.txt:231) and the
# glslang headers, which VkFFT does not include by a standard path. PUBLIC because
# anything including ComplexFFTFilter.h inherits the same requirement.
target_compile_definitions(imcufosphor-core PUBLIC -DVK_API_VERSION=10 -DVKFFT_BACKEND=0)
target_include_directories(imcufosphor-core PUBLIC ${glslang_INCLUDE_DIR}/glslang/Include)

# -Werror applies to our code only, never to the submodules. Third-party headers pulled in
# here (libsigmf and what it re-exports) are marked SYSTEM at the top level so their
# warnings do not trip this.
target_compile_options(imcufosphor-core PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

target_link_libraries(imcufosphor-core
PUBLIC
	scopehal
	scopeprotocols
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
	ngscopeclient-compat
	)

set_property(TARGET imcufosphor-render PROPERTY POSITION_INDEPENDENT_CODE ON)

########################################################################
# Application
########################################################################

add_executable(sigmf-spectrum
	main.cpp
	MainWindow.cpp

	# Lifted display primitives go here as they are brought in, e.g.
	#   ${SCOPEHAL_APPS_DIR}/src/ngscopeclient/VulkanWindow.cpp
	#   ${SCOPEHAL_APPS_DIR}/src/ngscopeclient/TextureManager.cpp
	#   ${SCOPEHAL_APPS_DIR}/src/ngscopeclient/PreferenceManager.cpp
	)

target_compile_options(sigmf-spectrum PRIVATE ${IMCUFOSPHOR_WERROR_FLAGS})

target_link_libraries(sigmf-spectrum
	imcufosphor-render
	)

imcufosphor_attach_shaders(sigmf-spectrum "${IMCUFOSPHOR_APP_SPV_DIR}")
imcufosphor_attach_icons(sigmf-spectrum)

install(TARGETS sigmf-spectrum RUNTIME)
