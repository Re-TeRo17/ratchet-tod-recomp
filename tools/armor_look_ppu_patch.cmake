# armor_look_ppu_patch.cmake (1.2.1) -- hooks on the game's character model choice (TOD_ARMOR_LOOK, TOD_REMOVE_HELMET).
#
#   armor_look_ppu_patch(<ppu_recomp_000.cpp in> <patched output file> <out var>)
#
# Renames the definition `void func_001E2110(ppu_context* ctx) {` (returns the hero r3's equipped armor, [hero+0x1A68]+0x458;
# the per-frame hero update func_001E4CC0 shows the armor's mesh groups from it) to func_001E2110_lifted; src/stubs/armor_look.cpp
# provides func_001E2110 and, with TOD_ARMOR_LOOK = hidden, answers "no armor" to that one caller only (the armor,
# its protection and the save are unchanged). Callers, the function table and trampolines keep calling func_001E2110.
# The output is rewritten only when its content changes.
function(armor_look_ppu_patch orig outc outvar)
    file(READ "${orig}" txt)
    set(def "void func_001E2110(ppu_context* ctx) {\n")
    string(FIND "${txt}" "${def}" dpos)
    if(dpos LESS 0)
        message(FATAL_ERROR "armor_look_ppu_patch: ${orig}: definition of func_001E2110 not found")
    endif()
    string(REPLACE "${def}" "void func_001E2110_lifted(ppu_context* ctx) {\n" txt "${txt}")
    # TOD_REMOVE_HELMET: the head-group read of hero+0x21CD in func_001E4CC0 goes through tod_helmet_flag (armor_look.cpp)
    set(hdef "void func_001E4CC0(ppu_context* ctx) {\n")
    set(hrd "        r9 = vm_read8(r31 + 0x21CD);\n")
    string(FIND "${txt}" "${hdef}" hpos)
    string(FIND "${txt}" "${hrd}" rpos)
    string(FIND "${txt}" "${hrd}" rlast REVERSE)
    if(hpos LESS 0 OR rpos LESS hpos OR NOT rpos EQUAL rlast)
        message(FATAL_ERROR "armor_look_ppu_patch: ${orig}: helmet read in func_001E4CC0 not found exactly once")
    endif()
    string(REPLACE "${hrd}" "        r9 = tod_helmet_flag(vm_read8(r31 + 0x21CD));\n" txt "${txt}")
    string(REPLACE "${hdef}" "uint64_t tod_helmet_flag(uint64_t v);\n${hdef}" txt "${txt}")
    get_filename_component(outdir "${outc}" DIRECTORY)
    file(MAKE_DIRECTORY "${outdir}")
    set(old "")
    if(EXISTS "${outc}")
        file(READ "${outc}" old)
    endif()
    if(NOT old STREQUAL txt)
        file(WRITE "${outc}" "${txt}")
        message(STATUS "  armor look: func_001E2110 -> _lifted in ${outc}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(${outvar} "${outc}" PARENT_SCOPE)
endfunction()
