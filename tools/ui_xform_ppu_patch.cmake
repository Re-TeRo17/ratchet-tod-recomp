# ui_xform_ppu_patch.cmake (wide121) -- hook on the game's UI pixel transform.
#
#   ui_xform_ppu_patch(<ppu_recomp_003.cpp original> <patched output file> <out var>)
#
# Renames the definition `void func_00660CD8(ppu_context* ctx) {` (sets the HUD's pixel scale/offset at guest 0x10724BF4..C04,
# once per frame, before the widgets are drawn) to func_00660CD8_lifted; src/stubs/ui_xform.cpp provides func_00660CD8, calls the
# lifted code and, with TOD_ASPECT_UI on, narrows the transform to a 16:9 box in the middle of the wider frame. Callers, the
# function table and trampolines keep calling func_00660CD8. The original (shared) is not touched; the output is rewritten
# only when its content changes.
function(ui_xform_ppu_patch orig outc outvar)
    file(READ "${orig}" txt)
    set(def "void func_00660CD8(ppu_context* ctx) {\n")
    string(FIND "${txt}" "${def}" dpos)
    if(dpos LESS 0)
        message(FATAL_ERROR "ui_xform_ppu_patch: ${orig}: definition of func_00660CD8 not found")
    endif()
    string(REPLACE "${def}" "void func_00660CD8_lifted(ppu_context* ctx) {\n" txt "${txt}")
    get_filename_component(outdir "${outc}" DIRECTORY)
    file(MAKE_DIRECTORY "${outdir}")
    set(old "")
    if(EXISTS "${outc}")
        file(READ "${outc}" old)
    endif()
    if(NOT old STREQUAL txt)
        file(WRITE "${outc}" "${txt}")
        message(STATUS "  ui xform: func_00660CD8 -> _lifted in ${outc}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(${outvar} "${outc}" PARENT_SCOPE)
endfunction()
