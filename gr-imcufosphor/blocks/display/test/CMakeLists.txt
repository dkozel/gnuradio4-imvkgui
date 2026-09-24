# Tests for the display sinks.
#
# All headless: nothing here publishes a RenderHost, so draw() returns without allocating and no
# Vulkan device is needed. That is deliberate rather than a limitation - the headless path is a
# supported mode that a flowgraph without a window actually takes, so testing it tests something
# real. GPU correctness is verified by tools/wfbench, which can synthesize known tones and check
# the resulting spectra.

gr_imcufosphor_add_ut_test(qa_IqLayout qa_IqLayout.cpp)
target_link_libraries(qa_IqLayout PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_MetaFromTags qa_MetaFromTags.cpp)
target_link_libraries(qa_MetaFromTags PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_AnnotationsFromTags qa_AnnotationsFromTags.cpp)
target_link_libraries(qa_AnnotationsFromTags PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_AnalyzerSink qa_AnalyzerSink.cpp)
target_link_libraries(qa_AnalyzerSink PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_SinglePaneSinks qa_SinglePaneSinks.cpp)
target_link_libraries(qa_SinglePaneSinks PRIVATE gr_imcufosphor::blocks_display_headers)

# The scope's capture path was split so that everything which decides what a record contains
# lives on the device-free side of AcceleratorBuffer's constructor. These two drive that side.
gr_imcufosphor_add_ut_test(qa_ScopeHistory qa_ScopeHistory.cpp)
target_link_libraries(qa_ScopeHistory PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_ScopeTrigger qa_ScopeTrigger.cpp)
target_link_libraries(qa_ScopeTrigger PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_ScopeSink qa_ScopeSink.cpp)
target_link_libraries(qa_ScopeSink PRIVATE gr_imcufosphor::blocks_display_headers)

gr_imcufosphor_add_ut_test(qa_IqRing qa_IqRing.cpp)
target_link_libraries(qa_IqRing PRIVATE gr_imcufosphor::blocks_display_headers)
