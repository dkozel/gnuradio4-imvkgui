# Copies every compiled .spv from SRC_DIRS into DEST_DIR.
#
# Invoked with "cmake -P" as a POST_BUILD step rather than being done at configure time,
# because the .spv files do not exist until glslc has run.
#
# This exists because imcufosphor does not install(). scopehal and scopeprotocols only
# put their shaders on FindDataFile()'s search path via "make install" into
# share/ngscopeclient; from an uninstalled build tree the .spv files sit in each
# subproject's own binary directory, where nothing looks for them. Gathering them into
# the binary's own directory satisfies the first entry of g_searchPaths
# (scopehal.cpp:821-828) with no absolute paths compiled into anything.

string(REPLACE "|" ";" SRC_DIRS "${SRC_DIRS}")

file(MAKE_DIRECTORY "${DEST_DIR}")

foreach(dir IN LISTS SRC_DIRS)
	file(GLOB spv "${dir}/*.spv")
	if(spv)
		# file(COPY) skips files that are already identical, so this is cheap on rebuild
		file(COPY ${spv} DESTINATION "${DEST_DIR}")
	endif()
endforeach()
