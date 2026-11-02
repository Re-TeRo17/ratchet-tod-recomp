# coll_sync_ppu_patch.cmake (sync59) -- gancio PPU della collisione sincrona nativa.
#
#   coll_sync_ppu_patch(<ppu_recomp_002.cpp originale> <file patchato di uscita> <var out>)
#
# Rinomina la definizione `void func_004A3288(ppu_context* ctx) {` (sottomissione dello SLOT sincrono 10612D80 alle raw SPU di
# collisione) in func_004A3288_lifted: func_004A3288 la fornisce src/stubs/coll_sync.cpp (stesso schema di lua_eh.cpp con --hook).
# I chiamanti, la function_table e i trampolini continuano a chiamare func_004A3288. L'originale (condiviso) non si tocca;
# il file di uscita e' riscritto SOLO se il contenuto cambia.
function(coll_sync_ppu_patch orig outc outvar)
    file(READ "${orig}" txt)
    set(def "void func_004A3288(ppu_context* ctx) {\n")
    string(FIND "${txt}" "${def}" dpos)
    if(dpos LESS 0)
        message(FATAL_ERROR "coll_sync_ppu_patch: ${orig}: definizione di func_004A3288 non trovata")
    endif()
    string(REPLACE "${def}" "void func_004A3288_lifted(ppu_context* ctx) {\n" txt "${txt}")
    get_filename_component(outdir "${outc}" DIRECTORY)
    file(MAKE_DIRECTORY "${outdir}")
    set(old "")
    if(EXISTS "${outc}")
        file(READ "${outc}" old)
    endif()
    if(NOT old STREQUAL txt)
        file(WRITE "${outc}" "${txt}")
        message(STATUS "  coll sync: func_004A3288 -> _lifted in ${outc}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(${outvar} "${outc}" PARENT_SCOPE)
endfunction()
