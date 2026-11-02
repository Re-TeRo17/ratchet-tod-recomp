/*
 * coll_sync.cpp (sync59) -- collisione SINCRONA nativa nel thread PPU (SPU_COLL_SYNC = 0 / 1 / 2).
 *
 * func_004A3288 = sottomissione dello SLOT sincrono (record da 160 B a [TOC+0x6CA4] = 10612D80) alle due raw SPU di collisione
 * (img 110 broad + img 111 narrow). Il chiamante (4A3E20 stadio 1, 4A3038 stadio 2, per 63DEF0) poi ATTENDE il completamento
 * (word16 rec+0xC bit 0x100, word16 rec+0x18 bit 0x8000). Questo file sostituisce func_004A3288 (la definizione lifted e' stata
 * rinominata func_004A3288_lifted da tools\coll_sync_ppu_patch.cmake) e, per i tipi 0..2, esegue il ticket in modo nativo nel
 * thread chiamante (spu_coll_sync.c: motore di coll53) invece di svegliare le raw SPU.
 *
 * Il "kick" del lifted e' una OR su una parola puntata da [(TOC[0x6C80] + 0x100000) + 0x10] (la linea di claim 10612E80): prima
 * |= 0x180, poi, dopo aver atteso (word & 0x40) == 0 e (*[*TOC[0x6CFC]] & 8) == 0, |= 2. Il lifted gira con quel puntatore DEVIATO
 * su una parola privata (nessuna raw SPU vede nulla), poi il puntatore e' ripristinato subito.
 *   modo 1: ticket nativo; log applicato; se rifiutato/errore -> kick vero (le raw SPU lavorano come sempre).
 *   modo 2: ticket nativo su copia, poi kick vero, attesa della chiusura delle raw SPU e confronto byte per byte del log con la memoria.
 * Il nativo non gira mai in parallelo con le raw SPU sullo stesso record (il kick e' sempre DOPO il nativo).
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#ifdef _WIN32
#include <intrin.h>
#endif

extern "C" {
int  spu_coll_sync_mode(void);
int  spu_coll_sync_prepare(void);
int  spu_coll_sync_run(uint32_t rec);
void spu_coll_sync_apply(void);
int  spu_coll_sync_compare(const uint8_t* rec_snap);
unsigned spu_coll_sync_waitms(void);
void spu_coll_sync_note(int what);
int64_t sys_memory_allocate(ppu_context* ctx);
}
void func_004A3288_lifted(ppu_context* ctx);

namespace {

uint32_t g_dummy;            /* parola guest privata (64 KB allocati una volta) */
std::atomic<int> g_busy{0};

uint32_t dummy_word(ppu_context* ctx)
{
    if (g_dummy) return g_dummy;
    ppu_context fake;
    memset(&fake, 0, sizeof fake);
    const uint32_t out = (uint32_t)ctx->gpr[1] - 0x800u;       /* parola di scarto sotto lo sp del chiamante */
    vm_write32(out, 0);
    fake.gpr[3] = 0x10000; fake.gpr[4] = 0x200; fake.gpr[5] = out;
    sys_memory_allocate(&fake);
    g_dummy = vm_read32(out);
    fprintf(stderr, "[coll-sync] parola privata del kick a guest 0x%08X\n", g_dummy);
    return g_dummy;
}

/* OR atomica (lwarx/stwcx. come il lifted) */
void rmw_or(ppu_context* ctx, uint32_t ea, uint32_t bits)
{
    for (;;) {
        const uint32_t v = vm_read32(ea);
        ctx->reserve_addr = ea; ctx->reserve_value = v;
        const int ok = (ctx->reserve_addr == ea) && ppu_stwcx32(ea, (uint32_t)ctx->reserve_value, v | bits);
        ctx->reserve_addr = 0;
        if (ok) return;
    }
}

/* il kick del lifted: |= 0x180 ; attesa ; |= 2 */
void real_kick(ppu_context* ctx, uint32_t greal, uint32_t toc)
{
    const uint32_t fp = vm_read32(vm_read32(toc + 0x6CFC));
    rmw_or(ctx, greal, 0x180);
    while ((vm_read32(greal) & 0x40u) != 0 || (vm_read32(fp) & 0x8u) != 0) {
#ifdef _WIN32
        _mm_pause();
#endif
    }
    rmw_or(ctx, greal, 2);
}

}  // namespace

void func_004A3288(ppu_context* ctx)
{
    static const int mode = spu_coll_sync_mode();
    if (mode == 0) { func_004A3288_lifted(ctx); return; }
    spu_coll_sync_note(0);
    if ((uint32_t)ctx->gpr[4] > 2u) { spu_coll_sync_note(2); func_004A3288_lifted(ctx); return; }          /* solo tipi 0..2 */
    int expect = 0;
    if (!g_busy.compare_exchange_strong(expect, 1)) { spu_coll_sync_note(2); func_004A3288_lifted(ctx); return; }
    struct Unbusy { ~Unbusy() { g_busy.store(0); } } unbusy;
    if (!spu_coll_sync_prepare()) { spu_coll_sync_note(2); func_004A3288_lifted(ctx); return; }
    const uint32_t toc = (uint32_t)ctx->gpr[2];
    const uint32_t base = vm_read32(toc + 0x6C80);
    const uint32_t gaddr = base + 0x100010u;
    const uint32_t greal = vm_read32(gaddr);
    if (greal < 0x10000u || greal >= 0xE0000000u || (greal & 3u)) { spu_coll_sync_note(2); func_004A3288_lifted(ctx); return; }
    const uint32_t dummy = dummy_word(ctx);
    if (!dummy) { spu_coll_sync_note(2); func_004A3288_lifted(ctx); return; }
    spu_coll_sync_note(1);

    /* 1. il lifted prepara record, mondo e flag; il kick e l'attesa vanno sulla parola privata */
    vm_write32(dummy, 0);
    vm_write32(gaddr, dummy);
    func_004A3288_lifted(ctx);
    vm_write32(gaddr, greal);
    vm_write32(dummy, 0);

    /* 2. ticket nativo sul record appena preparato (nulla e' scritto in memoria guest) */
    uint8_t* const vmb = vm_base;
    const uint32_t rec = vm_read32(toc + 0x6CA4);
    uint8_t snap[160];
    memcpy(snap, vmb + rec, 160);
    const int rc = spu_coll_sync_run(rec);
    if (rc == 0 && mode == 1) { spu_coll_sync_apply(); return; }

    /* 3. kick vero: le raw SPU fanno il lavoro (modo 2, o fallback del modo 1) */
    real_kick(ctx, greal, toc);
    if (rc != 0) { spu_coll_sync_note(2); return; }
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const uint16_t w0 = (uint16_t)vm_read16(rec + 0xC), w18 = (uint16_t)vm_read16(rec + 0x18);
        if ((w0 & 0x100u) && (w18 & 0x8000u)) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > (long)spu_coll_sync_waitms()) { spu_coll_sync_note(3); return; }
#ifdef _WIN32
        _mm_pause();
#endif
    }
    spu_coll_sync_compare(snap);
}
