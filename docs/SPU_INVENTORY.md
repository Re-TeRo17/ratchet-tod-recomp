# Inventario programmi SPU - Ratchet & Clank: Tools of Destruction (BCES00052 v01.00)

Sorgente: `elf/EBOOT.ELF` (PPC64, 10.923.400 byte). Output grezzi in `out/spu/`.
Toolkit: `ps3recomp/tools`.

## 1. Riepilogo

| Categoria | Quantita | Dove | Note |
|---|---|---|---|
| ELF SPU embedded (EM_SPU) | **1** | file off. 0x8EA200 | Sony Edge zlib inflate task (SPURS task) |
| Moduli job WWS (magic `C0DEC0DE`) | **21** | file off. 0x989980..0xA46C00 (~1,36 MB sommati, con sovrapposizioni) | non sono ELF; modello "jobmod" (LS 0x4000), come in dod3 |
| Policy module / kernel WWS job | non individuato | - | vedi punti aperti |
| Programmi SPU da file dati | non verificabile senza ISO | stringhe suggeriscono .psarc / collision.dat | vedi sez. 4 |

Confronto dod3: 40 ELF SPU + 2 job raw. Qui 1 ELF + 21 jobmod. Struttura molto diversa.

## 2. ELF SPU embedded

| # | File | Off. EBOOT | Dim. | Entry | PT_LOAD | Identificazione | Tipo |
|---|---|---|---|---|---|---|---|
| 0 | `out/spu/images/spu_0000_at_008EA200.elf` | 0x8EA200 | 39.608 B | 0x30F0 | text 0x3000..0xBF00 (R-X, 36.608 B); data 0xBF00 filesz 0x20, memsz 0x2A9C0 (bss ~170 KB) | `.note SPUNAME = edgezlib_inflate_task.spu.elf`; versione `1.2.3.0-PS3-SPU-EDGE`; linka libspurs/libsync/libdma/libsputhread | **SPURS task** (taskset `edgeZlibTaskSet`, coda LFQueue) |

`find_spu_functions.py`: 558 funzioni, copertura 88,2% (resto dati in .text), 9.152 istruzioni. `spu_ident.py` senza DB firme: 0 match (il DB non contiene questa build; l'identita viene dalla nota ELF).
Raccomandazione: HLE (come dod3, `edgezlib.cpp`) oppure lift normale.

## 3. Moduli job WWS (`C0DEC0DE`)

Header a `off = pos(magic) - 0x20`: 4 `ila` (0x42/0x43), `entry` @0x10, `size` @0x14, magic @0x20 (stesso formato atteso da `extract_jobmods.py`). Estratti in `out/spu/jobmods/` (SHA1 12 car. nel nome).

| # | Off. file | Entry | Size campo |
|---|---|---|---|
| 1 | 0x989980 | 0x80 | 0x116E0 |
| 2 | 0x99B080 | 0x80 | 0x10180 |
| 3 | 0x9AB380 | 0x13728 | 0x1B130 |
| 4 | 0x9C6580 | 0xA5E0 | 0xE8D0 |
| 5 | 0x9E4B00 | 0x1BDE8 | 0x1D510 |
| 6 | 0xA02000 | 0xDFF8 | 0xE1C0 |
| 7 | 0xA10200 | 0x2F88 | 0x30B20 |
| 8 | 0xA13680 | 0xD38 | 0x396A0 |
| 9 | 0xA14580 | 0x998 | 0xBB0 |
| 10 | 0xA2F580 | 0x31F0 | 0x46E0 |
| 11 | 0xA33C80 | 0x1DF0 | 0x36A0 |
| 12 | 0xA36180 | 0x1CD8 | 0x1EB0 |
| 13 | 0xA38080 | 0x960 | 0xC30 |
| 14 | 0xA38D00 | 0x18F0 | 0x8020 |
| 15 | 0xA3A980 | 0x16F0 | 0x18B0 |
| 16 | 0xA3C280 | 0x15A0 | 0x1C020 |
| 17 | 0xA3DB00 | 0x9B0 | 0xBB0 |
| 18 | 0xA3E700 | 0x2718 | 0x38E0 |
| 19 | 0xA42000 | 0x2A70 | 0x2CB0 |
| 20 | 0xA44D00 | 0x1730 | 0x10720 |
| 21 | 0xA46C00 | 0x7A60 | 0x28DC0 |

ATTENZIONE: il campo `size` include bss/dati non presenti nel file; molti intervalli `[off, off+size)` si sovrappongono al modulo successivo (es. #7 e #8, #16/#17/#18). I file in `out/spu/jobmods/` sono quindi tagliati "per size" e contengono byte dei vicini nella coda. La parte inizializzata reale va limitata al prossimo header/limite; da rifinire prima del lift. Il tool `extract_jobmods.py` non e utilizzabile tal quale: cerca zlib dentro SDATA, qui i moduli sono in chiaro nell'EBOOT.

Stringhe collegate (PPU): libreria Wws/PSG job manager ("WwsJob_IsAligned", "joblist.cpp", "commandlistchecker.cpp", audit buffer, `SPU Code: "%s"`), `jobmain.cpp` dentro i moduli #3 e #4, `*** SPU SYN_ASSERT! ***` nei moduli #1 e #2, LibMP3 1.25 (`libmp3_wwsjob.cpp`, job stereo/surround), Bink audio, `igGeomCullClip SPU Timeout!`, nomi build `temp\PS3_SPU_Release\igCollBroadSpu.elf` / `igcollspu.elf` (collisioni Insomniac: solo nomi, nessun ELF embedded corrispondente).

## 4. Evidenze di programmi SPU da file dati

Non e stato montato l'ISO (richiesta). Dalle stringhe dell'EBOOT:
- Archivi: `%s.psarc`, `/dev_bdvd/PS3_GAME/USRDIR`, `/app_home/%s/collision.dat`, `cinematic.dat`, `game.self`, `resm.self`, `%s_%d%d.self` (self = PPU, non SPU).
- Nessun path `*.spu` / `*.elf` / `*.bin` caricabile (gli unici `.elf` sono i nomi `igCollBroadSpu.elf`, `igcollspu.elf`, `edgezlib_inflate_task.spu.elf`).
- Possibile che altri jobmod stiano nei .psarc su disco (in dod3 stavano in SDATA): da verificare in fase successiva.

## 5. Uso dell'API SPU lato PPU

Import: `cellSpurs` (31), `cellSync` (1: `cellSyncLFQueueGetDirection`), nessun `sys_spu_*` importato da sysPrxForUser. Presente in-line (syscall `sc` con `li r11,N`, scansione del segmento testo):

| Syscall | N | Occorrenze | Indirizzo (es.) |
|---|---|---|---|
| sys_spu_initialize | 169 | 1 | 0x59BC44 |
| sys_spu_thread_group_create | 170 | 1 | 0x59B4B8 |
| sys_spu_thread_group_start/suspend/resume/terminate | 173/174/175/177 | 1 ciascuna | 0x59ADB0.. |
| sys_spu_thread_write_ls / read_ls | 181 / 182 | 1 / 14 | 0x81ED58 / 0x7DB574 |
| sys_spu_thread_write_spu_mb | 190 | 1 | 0x7D48D4 |
| **sys_raw_spu_create** | 160 | 1 | 0x59B0DC |
| **sys_raw_spu_set/get_int_mask, int_stat, read_puint_mb** | 151-154, 163 | 1-4 ciascuna | 0x598964, 0x6B9298.. |

`sys_spu_image_open` (156) / `sys_raw_spu` non importati come funzioni; non compare 156/157. Quindi il gioco usa un proprio runtime con raw SPU + interrupt (verosimilmente il job manager WWS/Insomniac) in aggiunta a SPURS. Il codice SPU lanciato su raw SPU non e ancora localizzato (probabilmente tra i 21 jobmod / kernel job).

Import cellSpurs: Initialize/Finalize, Taskset (create/attr/name/join/shutdown), CreateTask, EventFlag (init/attach/detach/wait/clear), LFQueue (init/attach/detach/PushBody), AddWorkloadWithAttribute, RemoveWorkload, ShutdownWorkload, WaitForWorkloadShutdown, SetPriorities, GetInfo, RequestIdleSpu, ReadyCountStore, SetExceptionEventHandler, Attach/DetachLv2EventQueue.

## 6. Confronto con dod3 e punti non supportati da ps3recomp

| Voce | Stato | Nota |
|---|---|---|
| Tutti gli import cellSpurs/cellSync tranne 2 | implementati in `runtime/.../cellSpurs.c`, `cellSync.c` | OK |
| `_cellSpursLFQueuePushBody` (NID 0x8a85674d), `cellSpursLFQueueDetachLv2EventQueue` (0x73e06f91) | **assenti in ps3recomp**; dod3 li ha in `src/kernel/spurs_lfqueue.cpp` (lift da libsre, `tools/gen_libsre.py`) | da portare/riscrivere nel progetto ToD (serve per la coda dell'Edge zlib) |
| Raw SPU (`spu_raw.c`: 150-154, 160, 161, 163) | supportato | verificare 164+/altre sys_raw_spu non coperte |
| SPURS workload con policy module (`spurs_policy.c`) | supportato, richiede il PM | PM del job manager WWS non ancora individuato |
| Task-set policy blob (`spurs_policy_blob_weak.c`) | stub vuoto (size 0) | dod3 lo estrae da libsre del firmware; serve per i taskset (Edge zlib) |
| jobmod WWS (`lift_jobmods.py`) | supportato in linea di principio | pipeline pensata per moduli da SDATA/zlib; qui adattare l'estrazione |
| `lift_spu.sh` di dod3 | non riusabile as-is | hardcode su hash/size EBOOT 1.01 e offset; va riscritto per ToD |

Bug/anomalie trovate nei tool:
- `nid_database` (DB di default) etichetta NID 0xe6f2c1e7 (sys_process_exit) con modulo `test_spu_lifted_start`: dato spurio nel DB.
- `spu_ident.py` su EBOOT intero: 0 immagini perche cerca `_binary_*`/header ELF via simboli; la scansione header (via `extract_spu_images`) trova l'ELF solo se si passa direttamente l'immagine.
- `triage_elf.py` richiede percorsi con slash `/` (confermato).

## 7. Punti aperti

1. Individuare il policy module (attr.pm) di `cellSpursAddWorkloadWithAttribute` e il codice usato con raw SPU (analisi PPU intorno a 0x59A000-0x59BC00 e 0x7C4000-0x7D5000).
2. Tagliare correttamente i 21 jobmod (limiti reali) e verificare con `spu_disasm.py`/`find_spu_functions.py`.
3. Controllare i `.psarc` su disco per ulteriori jobmod (senza montare l'ISO: leggere l'indice del file ISO).
4. Portare LFQueue PushBody/DetachLv2EventQueue.

## 8. Comandi eseguiti (PowerShell, cwd `tod-recomp`, `$env:PYTHONIOENCODING="utf-8"`)

```
$py=".venv\Scripts\python.exe"; $T="ps3recomp\tools"
& $py "$T\triage_elf.py" "elf/EBOOT.ELF"                      # out\spu\triage.txt
& $py "$T\extract_spu_images.py" elf\EBOOT.ELF --output out\spu\images   # 1 immagine
& $py "$T\spu_ident.py" elf\EBOOT.ELF                           # 0 immagini identificate
& $py "$T\spu_ident.py" out\spu\images\spu_0000_at_008EA200.elf
& $py "$T\find_spu_functions.py" out\spu\images\spu_0000_at_008EA200.elf --out out\spu\spu_0000_functions.json
```
Scansioni ad hoc (python -I): ricerca `C0DEC0DE` e estrazione in `out/spu/jobmods/`, grep stringhe (`out/spu/strings_spu.txt`), risoluzione NID import (`out/spu/imports_resolved.txt`, via `nid_database.get_default_db().lookup_nid`), scansione `sc` con `li r11,N` nel segmento testo (capstone per verifica).
