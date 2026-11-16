# ui_copy_ppu_patch.cmake (09/11) -- hook on the game's memory copy func_0081A728 (ppu_recomp_004.cpp).
#
#   ui_copy_ppu_patch(<ppu_recomp_004.cpp in> <patched output file> <out var>)
#
# Renames the definition `void func_0081A728(ppu_context* ctx) {` to func_0081A728_lifted; src/stubs/ui_sync.cpp provides
# func_0081A728 and, for the copies the menus' UI quad writer func_00629180 makes (return addresses 0x6292F4 / 0x629530), notes
# where they go: a new UI pass starts over the same bytes, and in a menu it first waits for the renderer (vendor flicker).
# Every other caller goes straight to the lifted code. The output is rewritten only when its content changes.
function(ui_copy_ppu_patch orig outc outvar)
    file(READ "${orig}" txt)
    set(def "void func_0081A728(ppu_context* ctx) {\n")
    string(FIND "${txt}" "${def}" dpos)
    if(dpos LESS 0)
        message(FATAL_ERROR "ui_copy_ppu_patch: ${orig}: definition of func_0081A728 not found")
    endif()
    string(REPLACE "${def}" "void func_0081A728_lifted(ppu_context* ctx) {\n" txt "${txt}")
    get_filename_component(outdir "${outc}" DIRECTORY)
    file(MAKE_DIRECTORY "${outdir}")
    set(old "")
    if(EXISTS "${outc}")
        file(READ "${outc}" old)
    endif()
    if(NOT old STREQUAL txt)
        file(WRITE "${outc}" "${txt}")
        message(STATUS "  ui copy: func_0081A728 -> _lifted in ${outc}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(${outvar} "${outc}" PARENT_SCOPE)
endfunction()
