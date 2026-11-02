# Raw SPU e thread group nel PPU di Tools of Destruction (BCES00052)

Analisi statica sola lettura di `elf/EBOOT.ELF` (VA = indirizzi guest; offset file = VA-0x10000 nel testo, `0x900000 + (VA-0x10060000)` per i dati 0x10060000+).
Script e output: `tools/analysis/*.py`, `out/analysis/` (grafo chiamate `graph.pkl`, `all_sc.json`, immagini in `out/analysis/raw_spu/`).
Nota: il binario ha 3 TOC (0x88FF38 gioco, 0x89FF20, 0x8AFE5C libs); le chiamate tra TOC passano da trampolini `std r2,0x28(r1); addis; addi; b target`.

## 1. Sintesi

| Domanda | Risposta |
|---|---|
| Programmi su **raw SPU** | **2**: `igCollBroadSpu` (broad phase) e `igcollspu` (narrow phase), collisioni Insomniac. Nessun altro uso raggiungibile di `sys_raw_spu_create` |
| Programmi su **thread group** (170/172/173..177) | Nessuno raggiungibile: wrapper presenti ma senza chiamanti (codice morto di libreria). I thread SPURS li crea il PRX cellSpurs |
| Origine del codice SPU raw | Blob **embedded nell'EBOOT** (dati RW), non da file, non ELF. Caricato dalla PPU via **DMA proxy MFC** (MMIO) in LS 0x60 |
| Immagini gia' inventariate? | **No**: stanno dentro l'intervallo (errato) dei jobmod #8/#9 di `SPU_INVENTORY.md`. Non sono moduli `C0DEC0DE` |
| Interrupt raw SPU | Non usati nel percorso vivo (handler 0x5A8920 irraggiungibile): la PPU fa polling/mailbox |

## 2. Funzioni PPU con syscall SPU (scansione `li r11,N ... sc`, 632 `sc` totali)

| Funzione (VA) | Syscall (sito `sc`) | Ruolo | Raggiungibile? | Chiamanti (a salire) |
|---|---|---|---|---|
| 0x5ABC00 | 169 (0x5ABC44), 330, 352 | init libreria SPU Wws: `sys_spu_initialize(6,2)`, mmapper, poi 0x5AB8D8/0x5AB848/0x5AC3F0/0x5AA3F8 | si | tramp 0x24F0C8 <- 0x51BB10 <- 0x392070 (`StageCore::GameDataThread`, `level_transitions`) |
| 0x5AB0C0 | 160 (0x5AB0DC) | wrapper `sys_raw_spu_create` | si | tramp 0x251198 <- **0x49D898** (str `igCollBroadSpu.elf`), **0x4A7FB0** (str `igcollspu.elf`) |
| 0x5AAF38 | - | registra programma in tabella slot (base 0x106766E8, 16 slot x 0x280) | si | tramp 0x250648 <- 0x49D898, 0x4A7FB0 |
| 0x5AB200 | MMIO | carica programma in LS con DMA proxy GET (chunk <=0x4000) | si | tramp 0x250E68 <- 0x49D898, 0x49DAC8, 0x4A7FB0 |
| 0x5AABA8 / 0x5AAC00 / 0x5AAC40 | MMIO | start (NPC<-entry, RunCntl=1) / stop (RunCntl=0) / puntatore blocco parametri | si | tramp <- 0x49D898, 0x49DAC8, 0x4A7FB0 |
| 0x5AB3E8 | 170 (0x5AB4B8), 172 | `sys_spu_thread_group_create` + `thread_initialize` (img in slot+0x98) | **no** | nessuno (nemmeno OPD referenziato) |
| 0x5AB558 | - | registra immagine in formato "segmenti" per il precedente | no | nessuno |
| 0x5AAD80 / DE8 / E40 / EA8 | 173 / 177 / 174 / 175 | group start / terminate / suspend / resume | no | nessuno |
| 0x5A8920 | 154,163,153,88 | handler interrupt raw SPU (EOI 88) | no | nessuno |
| 0x6C9240, 0x6C9468, 0x6CA020 | 151-154 | thread PPU di servizio raw SPU (macchina a stati) creato da 0x6CA020 | no (0x6CA020 senza chiamanti) | - |
| 0x7E47A8 | 190 (0x7E48D4), 130 | thread **"Job Manager Event Handler"**: server `spu_printf` (evento SPURS -> formatta -> risposta in mailbox) | si | creato da 0x7E48F0 (data 0x8AC8E4) |
| 0x82ED08 / 0x82ED48 / 0x82ED70 | 182 / 181 / loop | helper read/write LS 4 byte, usati dalla libc del server printf | si | 0x82F668 <- 0x82DC98 <- 0x826630 <- 0x7E47A8 |
| 0x7EB4C0 | 182 x12 | dump diagnostico (`commandlistchecker.cpp`, `$NPC`) leggendo LS dei thread SPURS | si (puntatore dati 0x1013FD08) | callback assert del job manager |

Altri `sc` non SPU ma relativi: 141 (usleep), 100-108 (lwmutex/cond), 801-834 (fs/gcm).

### Sottosistemi che usano SPU (via SPURS joblist Wws, non raw)

`0x7E5498` crea il workload "Wws_Job" (`cellSpursAddWorkloadWithAttribute`, joblist.cpp). Chiamanti:

| Chiamante | Indizio (stringhe vicine) | Sottosistema |
|---|---|---|
| 0x5952D0 | `VertAnimSync timeout` | animazione vertici |
| 0x5C1E48 | `igGeomCullClip SPU Timeout!`, `geomcullclip0/1` | culling/clipping geometria |
| 0x5CFE60 | `DynamicJoint` | skinning/giunti |
| 0x5F0400, 0x5F2198 | `fxvUpdate` | FX/particelle |
| 0x607BE0, 0x608BD8 | `MobyParticleSync timeout`, `igCaustics` | particelle moby, caustiche |
| 0x5AC3F0 (da init 0x5ABC00) | - | joblist del manager |
| 0x6F3734 | `NextSynth PS3 (c)2006 SCEA` | audio synth |
| 0x703128 | `libmp3_wwsjob.cpp` | decodifica MP3 |
| Edge zlib | `edgeZlibTaskSet` (0x7E3ED8 <- 0x616D50 <- 0x5C4C78) | taskset SPURS (CreateTask), ELF 0x8EA200 |
| Raw SPU | `igCollBroadSpu.elf`, `igcollspu.elf` | **collisioni** (0x49D898, 0x4A7FB0) |

## 3. Cosa gira su ogni raw SPU

Sequenza (broad: 0x49D898; narrow: 0x4A7FB0): `raw_spu_create` -> `registra(code_EA, size, nome)` -> `load` (DMA proxy) -> `start`.
Argomenti trovati nei siti di chiamata (TOC letto dai slot):

| Programma | EA in guest | Offset file | Size caricata | LS base | Entry | Rilanci |
|---|---|---|---|---|---|---|
| igCollBroadSpu | 0x10175180 | 0xA15180 | 0x6DA0 (28.064 B) | 0x60..0x6E00 | LS 0x100 | stop+reload+start da 0x49DAC8 |
| igcollspu | 0x1017BF80 | 0xA1BF80 | 0x135D0 (79.312 B) | 0x60..0x13630 | LS 0x100 | mailbox: EA 0x1060ED00+0x4180 in In_Mbox |

Formato dell'immagine (non ELF, non C0DEC0DE): header 0x30 B = 4x`ila`, +0x10 entry (0x100), +0x14 fine LS, +0x18 memsz (bss: 0x299E0 / 0x1C4D0), +0x1C inizio codice LS (0x90), +0x20/+0x24 offset/size blocco parametri, +0x28/+0x2C `DEADDEAD`. Il codice parte a LS 0x90 (= offset file 0x30); l'intera immagine va a LS 0x60.
Il loader 0x5AB200 fa: per chunk <=0x4000: `MFC_LSA`(0x43004), `EAH`(0x43008), `EAL`(0x4300C), `Size_Tag`(0x43010)=size<<16|tag0, `Class_CMD`(0x43014)=0x40 (GET), poll CMDStatus, poi `Prxy_QueryMask`(0x4321C)=1 e poll `TagStatus`(0x4322C). Poi start: `NPC`(0x44034)=0x100, `RunCntl`(0x4401C)=1.
Broad: la PPU scrive i parametri (0x4C B: EA strutture collisioni, **EA della finestra LS 0xE0000000+id<<20**, float) nel blocco in coda all'immagine, **in memoria EBOOT prima del DMA** (LS 0x6DB0..0x6E00). Il narrow riceve solo l'EA del descrittore via In_Mbox (0x4400C).

Verifica: decodifica con `spu_disasm.py` -> codice SPU valido (crt0, ctors, stack 0x3FFD0). `find_spu_functions.py` (immagini avvolte a base 0x60): broad 290 funzioni (86,2%), narrow 740 funzioni (98,1%), 0 `.word`.

| Immagine | SHA1 (16) | FNV-1a-64 intero (run LS) | FNV senza blocco parametri |
|---|---|---|---|
| igCollBroadSpu | 1407368981d7b5b0 | 0x99E8B2BAC2B91330 | 0xD1C5DB61212F9730 (LS 0x60..0x6DB0) |
| igcollspu | 96bb87ac92578f18 | 0xA91BC9992F359456 | - (nessun blocco scritto) |

File: `out/analysis/raw_spu/{igCollBroadSpu_ea10175180.bin, igcollspu_ea1017bf80.bin, *_ls0060.elf, *_functions.json, fingerprints.json}`.

### Mappa SPU-like nell'EBOOT (scansione euristica + `spu_disasm`)

| Regione (offset file) | VA | Cosa | Stato |
|---|---|---|---|
| 0x8EA200..0x8F3CB8 | 0x100501E0.. | ELF Edge zlib | noto |
| 0x8F3D00 (size 0x2B48, arrotondata 0x2B80) | 0x10053D00 | **Policy module Wws_Job** (getter 0x7EB490/0x7EB498, usato da 0x7E5498; gia' lifted in `spu/spurs_pm_wwsjob`) | noto |
| 0x8F6880 (~0x300) | 0x10056880 | secondo PM; getter 0x7EB4A8/0x7EB4B0 **senza chiamanti** | probabilmente non usato |
| 0x989980..0xA46C00 | - | 21 jobmod `C0DEC0DE` | noto, ma intervalli di `SPU_INVENTORY.md` sbagliati: i jobmod #8 (0xA13680) e #9 (0xA14580) sono corti (~0xF00 e ~0xBB0 B) e il resto fino a 0xA2F580 contiene i 2 raw sopra |
| 0xA15180, 0xA1BF80 | 0x10175180, 0x1017BF80 | **igCollBroadSpu, igcollspu** | **nuovi** |
| 0x976A00..0x984D00 (~58 KB) | 0x100D6A00..0x100E4D00 | blocco che decodifica al 100% come codice SPU (cwd/shufb/rotqby/fa/fcgt/selb, ~25 `bi $lr`), nessun riferimento PPU diretto al range (solo a dati vicini 0x100D6000-0x100D6480 e 0x100E4F00+) | **non identificato** (forse "SPU shaders"/codice caricato da job via DMA) |

## 4. Supporto ps3recomp

| Elemento | Stato nel runtime | Note per ToD |
|---|---|---|
| `sys_raw_spu_create/destroy` 160/161, int tag 150, int mask/stat 151-154, `read_puint_mb` 163, interrupt thread 84/88/89 | implementati (`runtime/spu/spu_raw.c`) | ToD usa solo 160 nel percorso vivo; sys_spu_initialize(6,2) e' solo log |
| Finestra MMIO 0xE0000000 (LS + problem state) | hook in `ppu_loader.cpp` (`vm_read32_slow`/`vm_write32_slow`, `spu_raw_is_reg`); LS = memoria guest | ok per lwz/stw usati dal gioco (RunCntl, NPC, MFC proxy, In_Mbox) |
| DMA proxy `Class_CMD` GET/PUT | sincrono, CMDStatus=0, QStatus 0x80000010, TagStatus=QueryMask | combacia col loader 0x5AB200 |
| Come parte il codice | `raw_spu_start` cerca l'immagine lifted per **fingerprint FNV-1a-64 dei run LS riempiti da GET** (`spu_workload_find_img`), PC da NPC; **nessun interprete** per le immagini non registrate (non gira) | serve lift + registrazione di entrambe le immagini |
| `sys_spu_initialize` 169 | handler che logga (la riga 2505 lo registra prima come stub, poi sovrascritto da 2506) | ok |
| Thread group 170/172/173/177, write/read_ls 181/182, write_spu_mb 190 | implementati in `lv2_register.c`; 174/175 stub | solo per SPURS/printf; i group "propri" sono codice morto |

### Cosa serve per questo gioco

1. **Lift** dei due raw: usare `out/analysis/raw_spu/*_ls0060.elf` (base 0x60, entry 0x100) con `spu_lifter.py --auto-functions`; assegnare image id nuovi (id 2 e' riservato al PM, vedi `tools/tod_spu.py`).
2. **Registrazione per fingerprint** in un nuovo TU (come `spu_workloads.c`): narrow = FNV dell'intero run `[0x60,0x13630)` (0xA91BC9992F359456). **Broad: problema**, il run include il blocco parametri (LS 0x6DB0..0x6E00) che la PPU riscrive prima del DMA, quindi l'hash intero cambia. Servono modifiche (fuori da questo task): in `raw_spu_start` provare anche il run troncato di `size` byte di coda (da header +0x24) o un prefisso fisso, oppure un hook per-gioco. Hash senza parametri: 0xD1C5DB61212F9730.
3. **Rilancio**: 0x49DAC8 fa RunCntl=0, ricarica LS, RunCntl=1. Il runtime su RunCntl!=1 imposta solo lo status; se il thread host e' ancora vivo (`started`) il nuovo start e' ignorato ("already up") mentre la LS viene sovrascritta. Verificare che il broad esca con `stop` prima del rilancio, o rendere RunCntl=0 un vero stop del thread.
4. **EA nella finestra LS di altri SPU**: i parametri del broad contengono 0xE0000000+id<<20; il DMA SPU deve trattare quegli EA come memoria guest normale (LS = guest memory: dovrebbe funzionare, da testare).
5. **PM Wws_Job e jobmod**: gia' coperti da `tod_spu.py`; correggere i limiti dei jobmod #8/#9 (vedi sopra).
6. **spu_printf server** (0x7E47A8): richiede gli eventi SPURS->PPU (event queue 0x80, port connect) e 181/182/190 sui thread SPURS; non indispensabile (solo assert/audit), il thread puo' restare bloccato in attesa.
7. Non necessari: interrupt raw SPU, thread group propri (codice morto).

## 5. Limiti dell'analisi

- Raggiungibilita' = grafo `bl`/`b` (con trampolini TOC) + puntatori a OPD in dati/TOC/`lis+addi`. Puntatori costruiti a runtime nell'heap potrebbero sfuggire (0x5AB3E8 & co. classificati morti con questo criterio).
- Il blocco SPU 0x976A00..0x984D00 e' classificato solo per decodifica; non e' stato collegato a un chiamante.
