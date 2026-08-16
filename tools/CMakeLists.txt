# Tools that link against the engine library.
#
# tools/vkmintest and tools/vkbisect are deliberately absent: they are minimal reproduction
# cases for the NVIDIA Xid-32 defect (DESIGN.md section 3, R7) and their value depends on
# having no dependency on this project's build. They stay hand-built from the g++ line in
# their header comments.

add_subdirectory(wfbench)
