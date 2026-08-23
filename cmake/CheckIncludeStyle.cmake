# Fails if any of our own sources reaches into a dependency by relative path.
#
# Run as `cmake -DROOTS=<dir>|<dir>|... -P CheckIncludeStyle.cmake`.
#
# Our code names upstream headers as <scopehal/Filter.h>, resolved through an include
# directory, rather than as "../../lib/scopehal/scopehal/Filter.h". The difference matters
# because the second form hardcodes where the header lives: it is what would have to be
# edited in thirty places the day those headers move, and it is why a reader cannot tell
# whether a dependency is one file or a whole library.
#
# This is a test rather than a convention, because a convention that is only written down
# gets violated by the next file someone adds.

string(REPLACE "|" ";" ROOT_LIST "${ROOTS}")

set(offenders "")
foreach(root ${ROOT_LIST})
	file(GLOB_RECURSE sources "${root}/*.h" "${root}/*.hpp" "${root}/*.cpp")
	foreach(src ${sources})
		file(STRINGS "${src}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]*\\.\\./")
		foreach(hit ${hits})
			string(STRIP "${hit}" hit)
			list(APPEND offenders "${src}: ${hit}")
		endforeach()
	endforeach()
endforeach()

if(offenders)
	list(LENGTH offenders n)
	message("Found ${n} relative include(s) escaping their own directory:")
	foreach(o ${offenders})
		message("  ${o}")
	endforeach()
	message(FATAL_ERROR
		"Name upstream headers through an include directory - <scopehal/Filter.h>, "
		"<scopeprotocols/Waterfall.h> - not by counting ../ segments.")
endif()

message("include style: no relative includes escape their directory")
