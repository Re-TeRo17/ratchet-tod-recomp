cmake_minimum_required(VERSION 3.20)
project(tod
    VERSION 0.0.1
    LANGUAGES C CXX
    DESCRIPTION "Ratchet & Clank: Tools of Destruction (PS3, BCES00052 1.00) recompiled for Windows"
)
# Modelled on dod3-recomp/CMakeLists.txt and the SDK's templates/project/CMakeLists.txt.
# The game's code is NOT in this repository: tools/lift_ppu.ps1 and tools/lift_spu.ps1 generate it from
# the user's own elf/EBOOT.ELF into recompiled/ and spu/ (see README.md, "Building from source").

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR: the toolkit checkout (submodule ps3recomp/ by default).
# RECOMP_DIR:    the lifter's output for this game (tools/lift_ppu.ps1 -> recompiled/).
if(NOT DEFINED PS3RECOMP_DIR)
    set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ps3recomp")
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${PS3RECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "No ps3recomp at ${PS3RECOMP_DIR}. Fetch the submodule "
        "(git submodule update --init) or pass -DPS3RECOMP_DIR=<checkout>.")
endif()
if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}. Run tools/lift_ppu.ps1 first, "
        "or point -DRECOMP_DIR= at an existing lift.")
endif()

# The lifted translation units (globbed; re-run CMake after a fresh lift).
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR "${RECOMP_DIR} has a ppu_recomp.h but no .c/.cpp beside it.")
endif()
# q89: native synchronous collision slot (SPU_COLL_SYNC): PPU hook on func_004A3288 (patched copy of ppu_recomp_002.cpp in build/gen/ppu)
option(TOD_COLL_SYNC "Synchronous collision slot: native execution in the PPU thread (SPU_COLL_SYNC=1/2 at run time, 0 = original)" ON)
# The native collision path runs generated tables (ps3recomp/runtime/spu/spu_coll_l2_gen_*.inc, spu_coll_l2_img.inc,
# spu_coll_sync_regs.inc): a mechanical translation of the game's SPU code, so NOT in this repository. Without them the
# option switches itself off and the game's own lifted collision SPU programs run (same result, slower).
if(TOD_COLL_SYNC AND NOT EXISTS "${PS3RECOMP_DIR}/runtime/spu/spu_coll_l2_img.inc")
    message(STATUS "TOD_COLL_SYNC: generated collision tables not present (game-derived, not published): native collision path OFF")
    set(TOD_COLL_SYNC OFF)
endif()
if(TOD_COLL_SYNC AND EXISTS "${RECOMP_DIR}/ppu_recomp_002.cpp")
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/coll_sync_ppu_patch.cmake")
    coll_sync_ppu_patch("${RECOMP_DIR}/ppu_recomp_002.cpp" "${CMAKE_BINARY_DIR}/gen/ppu/ppu_recomp_002.cpp" COLL_SYNC_PPU_002)
    list(TRANSFORM RECOMP_SOURCES REPLACE "^${RECOMP_DIR}/ppu_recomp_002.cpp$" "${COLL_SYNC_PPU_002}")
endif()
# r121u: TOD_ASPECT Hor+ hook on the frustum builder (func_00597328) in ppu_recomp_002.cpp (on top of the coll_sync copy if any)
set(_P002 "")
foreach(_s IN LISTS RECOMP_SOURCES)
    if(_s MATCHES "ppu_recomp_002[.]cpp$")
        set(_P002 "${_s}")
    endif()
endforeach()
if(_P002)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/aspect_proj_ppu_patch.cmake")
    aspect_proj_ppu_patch("${_P002}" "${CMAKE_BINARY_DIR}/gen/ppu_aspect/ppu_recomp_002.cpp" ASPECT_PROJ_PPU_002)
    list(TRANSFORM RECOMP_SOURCES REPLACE "^${_P002}$" "${ASPECT_PROJ_PPU_002}")
endif()
# 1.2.1: TOD_ARMOR_LOOK / TOD_REMOVE_HELMET hooks (armor getter func_001E2110, head read in func_001E4CC0) in ppu_recomp_000.cpp
set(_P000 "")
foreach(_s IN LISTS RECOMP_SOURCES)
    if(_s MATCHES "ppu_recomp_000[.]cpp$")
        set(_P000 "${_s}")
    endif()
endforeach()
if(_P000)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/armor_look_ppu_patch.cmake")
    armor_look_ppu_patch("${_P000}" "${CMAKE_BINARY_DIR}/gen/ppu_armor/ppu_recomp_000.cpp" ARMOR_LOOK_PPU_000)
    list(TRANSFORM RECOMP_SOURCES REPLACE "^${_P000}$" "${ARMOR_LOOK_PPU_000}")
endif()
# wide120: hook on the UI pixel transform (func_00660CD8, patched copy of ppu_recomp_003.cpp in build/gen/ppu); TOD_ASPECT_UI at run time
if(EXISTS "${RECOMP_DIR}/ppu_recomp_003.cpp")
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/ui_xform_ppu_patch.cmake")
    ui_xform_ppu_patch("${RECOMP_DIR}/ppu_recomp_003.cpp" "${CMAKE_BINARY_DIR}/gen/ppu/ppu_recomp_003.cpp" UI_XFORM_PPU_003)
    list(TRANSFORM RECOMP_SOURCES REPLACE "^${RECOMP_DIR}/ppu_recomp_003.cpp$" "${UI_XFORM_PPU_003}")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library (built from source, as dod3 does)
# ---------------------------------------------------------------------------
option(TOD_SYMBOLS "Debug info for the host code (Windows: /Z7 + /DEBUG)" ON)
if(WIN32 AND TOD_SYMBOLS)
    set(PS3RECOMP_DEBUG_INFO ON CACHE BOOL "Compile the runtime with debug info" FORCE)
endif()
# Controllers through SDL2 on Windows (the runtime's default there is XInput).
# SDL2 and zlib come from vcpkg (sdl2:x64-windows, zlib:x64-windows).
if(WIN32)
    set(PS3RECOMP_PAD_SDL2 ON CACHE BOOL "Windows: read controllers through SDL2 instead of XInput" FORCE)
endif()
set(PS3RECOMP_X86_LEVEL "x86-64-v3" CACHE STRING "-march for the ps3recomp runtime (main.cpp checks the CPU)")
# Keep the build machine's paths (and so the user name) out of the binary: __FILE__ becomes
# relative to the source tree. Must come before add_subdirectory so it covers the runtime too.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    if(MSVC)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.>")
    else()
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.>")
    endif()
endif()
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Python (the venv if present) and the HLE NID table
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
    set(TOD_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
    set(TOD_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TOD_PYTHON "${Python3_EXECUTABLE}")
endif()

# Maps every firmware import NID onto the runtime's handler (gen_hle_nids.py
# --all, the canonical invocation). The six imports the runtime lacks are
# registered by src/stubs/missing_hle.cpp instead (ps3_hle_register_ctx).
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${TOD_PYTHON}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# PPU boot scaffold (compiled here: each file includes the lift's ppu_recomp.h)
# ---------------------------------------------------------------------------
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional -- the build links without them). tools\lift_spu.ps1
# fills spu\: the Wws_Job policy module (spu\spurs_pm_wwsjob, image id 2), the WWS job
# modules (spu\jobmod_*, ids 200..) and the registries spu_workloads.c / jobmods_register.c.
# ---------------------------------------------------------------------------
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # ring103: gancio nel prologo di igCollBroadSpu func_2640 (ticket ad anello della collisione, SPU_COLL_RING=1 a run time)
        set(COLL_RING_PATCHED "")
        if(TOD_COLL_SYNC)
            include("${CMAKE_CURRENT_SOURCE_DIR}/tools/coll_ring_patch.cmake")
            set(orig_igCollBroadSpu "${SPU_DIR}/igCollBroadSpu/spu_recomp.c")
            if(EXISTS "${orig_igCollBroadSpu}")
                coll_ring_patch("${orig_igCollBroadSpu}" "${CMAKE_BINARY_DIR}/gen/ring/igCollBroadSpu" COLL_RING_PATCHED)
                list(TRANSFORM SPU_LIFTED REPLACE "^${orig_igCollBroadSpu}$" "${COLL_RING_PATCHED}")
            endif()
        endif()
        # spu_dispatch_mt.c: the musttail indirect-branch path, excluded from the
        # runtime library on purpose; it must be built here, by clang.
        list(APPEND SPU_SOURCES ${SPU_LIFTED} "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        if(TOD_COLL_SYNC)
            list(APPEND SPU_SOURCES "${PS3RECOMP_DIR}/runtime/spu/spu_coll_sync.c")
        endif()
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c spu_raw_workloads.c taskset_policy_bytes.c dynovl_register.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------
# Dear ImGui (third_party/imgui, v1.92.9b, MIT) for the settings overlay.
set(TOD_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(TOD_IMGUI_SOURCES
    "${TOD_IMGUI_DIR}/imgui.cpp" "${TOD_IMGUI_DIR}/imgui_draw.cpp"
    "${TOD_IMGUI_DIR}/imgui_tables.cpp" "${TOD_IMGUI_DIR}/imgui_widgets.cpp"
    "${TOD_IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
set(PORT_SOURCES
    src/main.cpp
    src/stubs/missing_hle.cpp
    src/stubs/lua_eh.cpp
    src/stubs/ui_xform.cpp           # wide120: func_00660CD8 (HUD pixel transform) for TOD_ASPECT_UI
    src/stubs/aspect_hor.cpp         # r121u: TOD_ASPECT Hor+ (render camera FOV)
    src/stubs/armor_look.cpp         # 1.2.1: TOD_ARMOR_LOOK, TOD_REMOVE_HELMET (Ratchet's look with armor)
    src/kernel/edgezlib_native.cpp
    src/input/host_input.cpp
    src/input/key_bindings.cpp       # keyboard / mouse bindings (KEY_* in tod.ini, F1 page Controls tab)
    src/input/tod_bot.cpp           # TOD_BOT waypoint bot for benchmarks
    src/input/tod_autopilot.cpp     # TOD_AUTOPILOT: Ratchet plays by himself (tests)
    src/input/tod_cheat.cpp         # TOD_CHEAT cheat menu (god, heal, bolts, ammo, status)
    src/input/tod_dualsense.cpp     # DualSense adaptive triggers per weapon (+ speaker)
    src/patches/prompts.cpp          # button prompts in the pad's own glyphs
    src/patches/sixaxis_default.cpp  # SIXAXIS off by default (TOD_SIXAXIS)
    src/patches/level_streams.cpp
    src/patches/menu_archive.cpp     # patched Options archive built at start (TOD_MENU)
    src/patches/mods.cpp             # mods\ folder: files read instead of the game's (TOD_MODS)
    src/ui/tod_overlay.cpp           # F1 / SELECT held settings page (Dear ImGui over the D3D12 present)
    src/ui/tod_logv2_host.cpp        # logv2: host fields of the structured log (CPU/GPU/RAM, threads, position, F7 mark)
    ${TOD_IMGUI_SOURCES}
    src/ui/tod_settings_menu.cpp     # ANTIALIASING row of the game's Options page (host natives)
    src/discord/discord_rpc.cpp     # Discord Rich Presence (own thread, named-pipe IPC, no SDK)
    src/discord/discord_level.cpp   # what the status says: current level / state from guest memory
    src/settings.cpp                # tod.ini
    src/update/update_check.cpp     # 1.1: release version + background check for a newer release (TOD_UPDATE_CHECK)
    src/install/crypto.cpp          # first-run installer (tod --install)
    src/install/self.cpp
    src/install/iso.cpp
    src/install/install.cpp
)
if(TOD_COLL_SYNC)
    list(APPEND PORT_SOURCES src/stubs/coll_sync.cpp)   # q89: SPU_COLL_SYNC, func_004A3288 (synchronous collision slot)
endif()

# Exe icon: res/tod.ico (the project's own artwork); a local/tod.ico (gitignored, never committed) overrides it.
set(TOD_ICON_RC "")
set(TOD_ICON "${CMAKE_SOURCE_DIR}/res/tod.ico")
if(EXISTS "${CMAKE_SOURCE_DIR}/local/tod.ico")
    set(TOD_ICON "${CMAKE_SOURCE_DIR}/local/tod.ico")
endif()
if(WIN32 AND EXISTS "${TOD_ICON}")
    enable_language(RC)
    file(WRITE "${CMAKE_BINARY_DIR}/gen/tod_icon.rc"
         "101 ICON \"${TOD_ICON}\"
")
    set(TOD_ICON_RC "${CMAKE_BINARY_DIR}/gen/tod_icon.rc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TOD_ICON}")
endif()
add_executable(${PROJECT_NAME}
    ${PORT_SOURCES}
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
    ${TOD_ICON_RC}
)

# The lifted SPU code: same x86-64 level as the PPU code (main.cpp checks the CPU);
# TOD_SPU_X86_LEVEL=x86-64-v2 for the old SSE4.1 build.
set(TOD_SPU_X86_LEVEL "x86-64-v3" CACHE STRING "-march for the lifted SPU code on x86-64")
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    # The audio synth's DSP images (NextSynth: the streamed overlay and its jobmod
    # twin) may run host float ops on the synth's own thread (spu_helpers.h).
    foreach(img IN ITEMS dynovl_008A9B80635E2FAA jobmod_d941e2fd8ed9_e0080_at0099B080)
        if(EXISTS "${SPU_DIR}/${img}/spu_recomp.c")
            set_property(SOURCE "${SPU_DIR}/${img}/spu_recomp.c" APPEND PROPERTY COMPILE_DEFINITIONS SPU_FAST_FP_TLS)
        endif()
    endforeach()
    # The two raw collision SPU images (110 igCollBroadSpu, 111 igcollspu): their fa/fs/fm/fma/fms/fnms are the host's
    # float ops while their thread runs with MXCSR = RZ|DAZ|FTZ (spu_helpers.h SPU_RZ_FP_TLS; SPU_RZ_FP=0 turns it off).
    option(TOD_SPU_RZ_FP "Collision SPU images: host float ops in round-toward-zero mode instead of the exact FP path" ON)
    if(TOD_SPU_RZ_FP)
        foreach(img IN ITEMS igCollBroadSpu igcollspu)
            if(EXISTS "${SPU_DIR}/${img}/spu_recomp.c")
                set_property(SOURCE "${SPU_DIR}/${img}/spu_recomp.c" APPEND PROPERTY COMPILE_DEFINITIONS SPU_RZ_FP_TLS)
            endif()
        endforeach()
        if(COLL_RING_PATCHED)
            set_property(SOURCE "${COLL_RING_PATCHED}" APPEND PROPERTY COMPILE_DEFINITIONS SPU_RZ_FP_TLS)
        endif()
    endif()
    # wid102: SharedJobList1 job module 9f077839ea6c (4x4 matrix multiply loop at LS 0x4600): float ops as host RZ ops
    # inside the loop only. The module copy in SPU_DIR must be patched by tools/wid102_rz_apply.py (WID102_ENTER);
    # WID102_RZ=0 at run time restores the exact path.
    option(TOD_WID1_RZ "wid1 matrix job: host float ops in round-toward-zero mode inside its loop" ON)
    if(TOD_WID1_RZ)
        set(_w102 "${SPU_DIR}/jobmod_9f077839ea6c_e09B0_at00A3DB00/spu_recomp.c")
        if(EXISTS "${_w102}")
            file(STRINGS "${_w102}" _w102_hit REGEX "WID102_ENTER" LIMIT_COUNT 1)
            if(_w102_hit)
                set_property(SOURCE "${_w102}" APPEND PROPERTY COMPILE_DEFINITIONS SPU_RZ_FP_TLS)
            endif()
        endif()
    endif()
    if(TOD_COLL_SYNC)
        set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_coll_sync.c" APPEND PROPERTY COMPILE_DEFINITIONS SPU_RZ_FP_TLS)
    endif()
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-march=${TOD_SPU_X86_LEVEL};/clang:-fno-math-errno")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endif()
    # Debug info for the synth images only (TOD_SYMBOLS): the sampler's RVAs then
    # resolve to their lifted functions with llvm-symbolizer (PS3_SAMPLE_SPU).
    if(MSVC AND TOD_SYMBOLS)
        foreach(img IN ITEMS dynovl_008A9B80635E2FAA jobmod_d941e2fd8ed9_e0080_at0099B080)
            if(EXISTS "${SPU_DIR}/${img}/spu_recomp.c")
                set_property(SOURCE "${SPU_DIR}/${img}/spu_recomp.c" APPEND PROPERTY COMPILE_OPTIONS "/Z7")
            endif()
        endforeach()
    endif()
endif()

if(WIN32 AND TOD_SYMBOLS)
    set_source_files_properties(${PORT_SOURCES} ${PS3RECOMP_SCAFFOLD} ${HLE_NID_TABLE}
        PROPERTIES COMPILE_OPTIONS "/Z7")
    if(SPU_SOURCES)
        set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/Z7")
    endif()
    # /PDBALTPATH: the exe records "tod.pdb", not the absolute path of the build directory.
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:tod.pdb)
endif()

# lua_eh.cpp throws C++ exceptions through lifted code (Lua longjmp). /EHs, not
# /EHsc: with /EHsc extern "C" callees (ps3_indirect_call) are assumed not to
# throw and the catch can be optimised away. (Appended after the /Z7 block,
# which replaces COMPILE_OPTIONS.)
if(MSVC)
    set_property(SOURCE src/ui/tod_overlay.cpp APPEND PROPERTY COMPILE_OPTIONS /utf-8)   # UTF-8 source
    set_property(SOURCE src/ui/tod_settings_menu.cpp APPEND PROPERTY COMPILE_OPTIONS /utf-8)   # UTF-8 source (tod_menu_text.inc)
endif()
if(MSVC)
    set_property(SOURCE src/stubs/lua_eh.cpp APPEND PROPERTY COMPILE_OPTIONS /EHs /EHc-)  # /EHsc is in CMAKE_CXX_FLAGS: /EHs alone keeps c
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h
    "${PS3RECOMP_DIR}/libs/video"
    "${PS3RECOMP_DIR}/libs/input"              # cellPad.h (src/input/host_input.cpp)
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/gen"                  # prompts_data.h
    "${CMAKE_CURRENT_SOURCE_DIR}/src"           # settings.h, install/*.h
    "${TOD_IMGUI_DIR}"                          # imgui.h, backends/imgui_impl_dx12.h
)
target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime)
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp user32 gdi32 winmm d3d12 dxgi)
    # Console program by default while bringing the port up (logs on stdout/stderr).
    # -DTOD_CONSOLE=OFF links a windowed program.
    option(TOD_CONSOLE "Link tod.exe as a console program" ON)
    if(NOT TOD_CONSOLE)
        target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
endif()

# ---------------------------------------------------------------------------
# Button-prompt atlases (src/res/prompts, drawn by tools/prompt_atlas.py) as
# arrays in gen/prompts_data.h, for src/patches/prompts.cpp (as dod3 does).
# ---------------------------------------------------------------------------
set(_prompt_h "/* Generated from src/res/prompts by CMakeLists.txt. */\n")
foreach(_set IN ITEMS ps4 ps5 xbox switch keyboard)
    set(_png "${CMAKE_CURRENT_SOURCE_DIR}/src/res/prompts/${_set}.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_png}")
    file(READ "${_png}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    string(APPEND _prompt_h "static const unsigned char kPromptPng_${_set}[] = {${_bytes}};\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gen/prompts_data.h" CONTENT "${_prompt_h}" @ONLY)
# zlib (vcpkg zlib:x64-windows, already a runtime dependency) decodes the PNGs.
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
# ---------------------------------------------------------------------------
# Compiler flags (same split as dod3: clang-cl matches the first branch)
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /Zc:__cplusplus /experimental:c11atomics)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The lifted TUs: warnings off, no fp contraction, /bigobj. On x86-64 clang-cl,
# TOD_X86_LEVEL (default x86-64-v3: AVX2/FMA/BMI) -- same rationale as
# DOD3_X86_LEVEL in dod3-recomp. main.cpp checks the CPU at start (TOD_X86_V3).
# /Zc:tlsGuards- drops the on-demand TLS guard on the lifted code's thread-local.
if(MSVC)
    set(_recomp_opts "/w;/bigobj")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _recomp_opts "/Zc:tlsGuards-" "/clang:-fwrapv")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
            set(TOD_X86_LEVEL "x86-64-v3" CACHE STRING
                "-march for the lifted PPU code on x86-64 (empty: baseline x86-64)")
            if(TOD_X86_LEVEL)
                list(APPEND _recomp_opts "-march=${TOD_X86_LEVEL}"
                     "/clang:-ffp-contract=off" "/clang:-fno-math-errno")
                if(TOD_X86_LEVEL MATCHES "v3|v4|haswell|skylake|znver")
                    set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS TOD_X86_V3)
                endif()
            endif()
        endif()
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "${_recomp_opts}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off;-fwrapv")
endif()

# TOD_PGO_PROFILE (OFF by default): clang -fprofile-instr-use=<.profdata> for the hot lifted PPU units only
# (TOD_PGO_UNITS, default ppu_recomp_002/003 = 76% of the main thread's guest time). Env TOD_PGO_PROFILE is the default
# (wt\logv2_build_rel.bat); empty / 0 / OFF = no PGO (the build is identical to the one without this block).
# Changing the value rebuilds only those units (+ relink). Stale profile: clang only warns (out-of-date) and drops it per function.
set(TOD_PGO_PROFILE "$ENV{TOD_PGO_PROFILE}" CACHE STRING "Path of a merged .profdata: -fprofile-instr-use on TOD_PGO_UNITS (empty/0/OFF = off)" FORCE)
set(TOD_PGO_UNITS "ppu_recomp_002;ppu_recomp_003" CACHE STRING "Lifted units that get TOD_PGO_PROFILE")
if(TOD_PGO_PROFILE AND NOT TOD_PGO_PROFILE MATCHES "^(0|OFF|off|NO|no)$" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(NOT EXISTS "${TOD_PGO_PROFILE}")
        message(FATAL_ERROR "TOD_PGO_PROFILE=${TOD_PGO_PROFILE}: file not found")
    endif()
    file(TO_CMAKE_PATH "${TOD_PGO_PROFILE}" _pgo_prof)
    if(MSVC)
        set(_pgo_opts "/clang:-fprofile-instr-use=${_pgo_prof}" "-Wno-profile-instr-out-of-date" "-Wno-profile-instr-unprofiled")
    else()
        set(_pgo_opts "-fprofile-instr-use=${_pgo_prof}" "-Wno-profile-instr-out-of-date" "-Wno-profile-instr-unprofiled")
    endif()
    set(_pgo_n 0)
    foreach(_src IN LISTS RECOMP_SOURCES)
        get_filename_component(_pgo_name "${_src}" NAME_WE)
        if(_pgo_name IN_LIST TOD_PGO_UNITS)
            set_property(SOURCE "${_src}" APPEND PROPERTY COMPILE_OPTIONS ${_pgo_opts})
            set_property(SOURCE "${_src}" APPEND PROPERTY OBJECT_DEPENDS "${_pgo_prof}")
            math(EXPR _pgo_n "${_pgo_n}+1")
        endif()
    endforeach()
    message(STATUS "PGO profile:     ${_pgo_prof} on ${_pgo_n} unit(s)")
endif()

# PPU_INLINE_VM: the lifted loads/stores inline the runtime's fast path
# (ps3recomp/runtime/ppu/ppu_vm_fast.h), as in dod3.
option(TOD_INLINE_VM "Inline the guest-memory fast path into the lifted code" ON)
if(TOD_INLINE_VM)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PPU_INLINE_VM)
endif()
set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/ppu")

# tod.ini (the documented settings template) next to the executable, once: an
# existing copy (the player's edits) is never overwritten by a rebuild.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tod.ini"
            "-DDST=$<TARGET_FILE_DIR:${PROJECT_NAME}>/tod.ini"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_if_missing.cmake"
    VERBATIM)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(FILES tod.ini DESTINATION bin)
