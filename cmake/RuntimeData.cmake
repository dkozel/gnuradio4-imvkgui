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
	add_dependencies(${target} imcufosphorshaders halshaders protocolshaders
		ngcomputeshaders ngrendershaders)

	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			"-DSRC_DIRS=${IMCUFOSPHOR_SHADER_SOURCE_DIRS}|${extra_spv_dir}"
			"-DDEST_DIR=$<TARGET_FILE_DIR:${target}>/shaders"
			-P "${PROJECT_SOURCE_DIR}/cmake/CollectShaders.cmake"
		COMMENT "Collecting compute shaders into $<TARGET_FILE_DIR:${target}>/shaders"
		VERBATIM)
endfunction()

# TextureManager loads colour ramps through FindDataFile("icons/gradients/*.png"), which
# resolves relative to the binary's own directory. Same reasoning as the shaders above:
# upstream only makes these findable by installing them, and we do not install.
function(imcufosphor_attach_icons target)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${SCOPEHAL_APPS_DIR}/src/ngscopeclient/icons"
			"$<TARGET_FILE_DIR:${target}>/icons"
		COMMENT "Copying icons and colour ramps"
		VERBATIM)
endfunction()
