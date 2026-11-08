# aspect_proj_ppu_patch.cmake (r121u) -- hook on the game's frustum (projection matrix) builder for TOD_ASPECT (Hor+).
#
#   aspect_proj_ppu_patch(<ppu_recomp_002.cpp in> <patched output file> <out var>)
#
# Renames the definition `void func_00597328(ppu_context* ctx) {` (builds a camera's projection at camera+0x110 from near, far
# and the frustum's left/right/bottom/top) to func_00597328_lifted; src/stubs/aspect_hor.cpp provides func_00597328, calls the
# lifted code and, with TOD_ASPECT on, widens the horizontal extent of 16:9 projections to the chosen ratio. Callers, the
# function table and trampolines keep calling func_00597328. The input may already be a patched copy (coll_sync); it is not
# touched. The output is rewritten only when its content changes.
function(aspect_proj_ppu_patch orig outc outvar)
    file(READ "${orig}" txt)
    set(def "void func_00597328(ppu_context* ctx) {\n")
    string(FIND "${txt}" "${def}" dpos)
    if(dpos LESS 0)
        message(FATAL_ERROR "aspect_proj_ppu_patch: ${orig}: definition of func_00597328 not found")
    endif()
    string(REPLACE "${def}" "void func_00597328_lifted(ppu_context* ctx) {\n" txt "${txt}")
    set(def2 "void func_005973E0(ppu_context* ctx) {\n")   # the same builder that also stores near/far at camera+0/+4
    string(FIND "${txt}" "${def2}" dpos2)
    if(dpos2 LESS 0)
        message(FATAL_ERROR "aspect_proj_ppu_patch: ${orig}: definition of func_005973E0 not found")
    endif()
    string(REPLACE "${def2}" "void func_005973E0_lifted(ppu_context* ctx) {\n" txt "${txt}")
    get_filename_component(outdir "${outc}" DIRECTORY)
    file(MAKE_DIRECTORY "${outdir}")
    set(old "")
    if(EXISTS "${outc}")
        file(READ "${outc}" old)
    endif()
    if(NOT old STREQUAL txt)
        file(WRITE "${outc}" "${txt}")
        message(STATUS "  aspect proj: func_00597328 -> _lifted in ${outc}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(${outvar} "${outc}" PARENT_SCOPE)
endfunction()
