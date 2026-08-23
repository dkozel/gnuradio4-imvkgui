# Compiles each public header on its own, to prove it declares what it uses.
#
#   imcufosphor_header_selfcheck(<target> <link-target> HEADERS <hdr>...)
#
# generates one translation unit per header containing nothing but an #include of it, and
# builds them all into an object library. A header that leans on its includer having read
# something else first fails here and nowhere else.
#
# This is not a style check. VulkanWindow.h upstream has no #include directives at all and
# names GLFWwindow, QueueHandle and ten vk::raii types; TextureManager.h names ImTextureID
# and GLFWimage without declaring either. Both compile fine in the application because every
# call site included a 1200-line umbrella first. The cost was that our own display headers
# had to include that umbrella too, and with it ngscopeclient's entire instrument-session
# model. Keeping our headers standalone is what stops that coming back.

function(imcufosphor_header_selfcheck target link_target)
	cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "HEADERS")

	set(sources "")
	foreach(header ${arg_HEADERS})
		get_filename_component(base "${header}" NAME_WE)
		set(generated "${CMAKE_CURRENT_BINARY_DIR}/selfcheck_${target}_${base}.cpp")

		# The include is by absolute path so this works regardless of the header's directory
		# being on the include path; what is under test is the header's own completeness.
		file(GENERATE OUTPUT "${generated}" CONTENT "#include \"${header}\"\n")

		list(APPEND sources "${generated}")
	endforeach()

	add_library(${target} OBJECT ${sources})
	target_link_libraries(${target} PRIVATE ${link_target})

	# Headers are included alone here, so anything unused in that context is expected.
	target_compile_options(${target} PRIVATE -Wno-unused-const-variable)
endfunction()
