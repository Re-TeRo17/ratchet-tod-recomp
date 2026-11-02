/*
 * The firmware imports ps3recomp's runtime does not implement (tools/hle_coverage.py,
 * docs/TRIAGE.md): logged stubs that return CELL_OK, so the game can run past them
 * while each gets a real implementation if it turns out to matter.
 *
 * Registration convention (same as dod3-recomp/src/kernel/hle.cpp and
 * ps3recomp/runtime/ppu/ppu_fs.cpp): a context-aware handler
 *     void fn(ppu_context* ctx)       -- args in ctx->gpr[3..10], result in ctx->gpr[3]
 * registered with ps3_hle_register_ctx(ps3_compute_nid("<name>"), "<name>", fn).
 * main.cpp calls ps3_load_prx_modules() BEFORE ppu_hle_init(); ctx handlers are
 * dispatched ahead of the generated table (ppu_hle.cpp), and the first
 * registration of a NID is the one called. gen_hle_nids.py only scans
 * ps3recomp/libs/**.c, so this file is NOT picked up by it: it must register
 * explicitly, as below.
 *
 * Without these, ppu_hle.cpp's unresolved path also answers CELL_OK (reported
 * once per NID); the point of the stubs is a named, per-call log line.
 */

#include "ppu_recomp.h"
#include "ps3emu/nid.h"          /* ps3_compute_nid */

#include <stdint.h>
#include <stdio.h>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
}

#define CELL_OK 0

/* One log line per call for the first few calls of each stub, then silence. */
#define TOD_STUB(NAME, NOTE)                                                              \
    static void stub_##NAME(ppu_context* ctx)                                             \
    {                                                                                     \
        static int n;                                                                     \
        if (n < 8) {                                                                      \
            n++;                                                                          \
            fprintf(stderr, "[hle-stub] " #NAME "(r3=0x%llX r4=0x%llX r5=0x%llX) -> CELL_OK  %s\n", \
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],     \
                    (unsigned long long)ctx->gpr[5], NOTE);                               \
        }                                                                                 \
        ctx->gpr[3] = CELL_OK;                                                            \
    }

TOD_STUB(_cellGcmFunc15, "")
TOD_STUB(cellSpursLFQueueDetachLv2EventQueue, "")
TOD_STUB(sys_game_process_exitspawn, "TODO: the game asked to exit/respawn; process keeps running")
TOD_STUB(cellPadInfoSensorMode, "(Sixaxis; stub is enough)")
TOD_STUB(cellPadSetSensorMode, "(Sixaxis; stub is enough)")

#define TOD_REG(NAME) ps3_hle_register_ctx(ps3_compute_nid(#NAME), #NAME, stub_##NAME)

extern "C" void tod_register_edgezlib(void);   /* src/kernel/edgezlib_native.cpp */

extern "C" void ps3_load_prx_modules(void)
{
    tod_register_edgezlib();
    TOD_REG(_cellGcmFunc15);
    TOD_REG(cellSpursLFQueueDetachLv2EventQueue);
    /* cellSaveDataListAutoSave: implemented in ps3recomp (libs/system/cellSaveData.c). */
    TOD_REG(sys_game_process_exitspawn);
    TOD_REG(cellPadInfoSensorMode);
    TOD_REG(cellPadSetSensorMode);
}
