# Makes compiled shaders and colour ramps findable at runtime by an executable target.
#
# ComputePipeline resolves a path like "shaders/Foo.spv" through FindDataFile(), whose first
# search path is the directory holding the binary (scopehal.cpp:821-828). The remaining paths
# are all install prefixes. Since this project has no install() step and the submodules are
# not installed either, an uninstalled build finds nothing unless we put the .spv files next
# to the binary ourselves.
#
# Every executable that touches a ComputePipeline needs this, not just the application, so it
# lives here rather than being written out once per target.

# Gathers every .spv from the submodules and from ${extra_spv_dir} next to ${target}'s binary.
function(imcufosphor_attach_shaders target extra_spv_dir)
	# Was five targets over ~105 shaders: imcufosphorshaders, halshaders, protocolshaders,
	# ngcomputeshaders and ngrendershaders. Now three targets over fifteen - ours, the four
	# vendored scopehal ones, and the three vendored ngscopeclient ones.
	add_dependencies(${target} imcufosphorshaders halshaders ngshaders)

	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			"-DSRC_DIRS=${IMCUFOSPHOR_SHADER_SOURCE_DIRS}|${extra_spv_dir}"
			"-DDEST_DIR=$<TARGET_FILE_DIR:${target}>/shaders"
			# CMAKE_CURRENT_FUNCTION_LIST_DIR, not PROJECT_SOURCE_DIR: the latter resolves to
			# the nearest enclosing project(), so a caller inside a subproject - gr-imcufosphor
			# declares its own so it can be split out later - would look for the script under
			# that subproject instead of next to this file.
			-P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CollectShaders.cmake"
		COMMENT "Collecting compute shaders into $<TARGET_FILE_DIR:${target}>/shaders"
		VERBATIM)
endfunction()

# TextureManager loads colour ramps through FindDataFile("icons/gradients/*.png"), which
# resolves relative to the binary's own directory. Same reasoning as the shaders above:
# upstream only makes these findable by installing them, and we do not install.
#
# This used to copy the whole of ngscopeclient's icons directory - 8.8 MB, per executable
# target - to make one 5.6 KB file findable. It now copies the ramps we vendored.
function(imcufosphor_attach_icons target)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			# CMAKE_CURRENT_FUNCTION_LIST_DIR, not PROJECT_SOURCE_DIR, for the same reason as
			# CollectShaders.cmake above: the latter resolves to the nearest enclosing
			# project(), and gr-imcufosphor declares its own.
			"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/ngscopeclient/icons"
			"$<TARGET_FILE_DIR:${target}>/icons"
		COMMENT "Copying colour ramps"
		VERBATIM)
endfunction()
