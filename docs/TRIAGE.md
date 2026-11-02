# Triage dell'EBOOT — Ratchet & Clank: Armi di Distruzione (BCES00052)

## Disco

| Campo | Valore |
|---|---|
| Serial | BCES00052 |
| APP_VER | 01.00 |
| PS3_SYSTEM_VER | 01.9300 |
| Aggiornamenti PSN | Nessuno (verificato con rusty-psn 0.7.1, 2026-10-20) |

Fonte: PARAM.SFO.

## File EBOOT

| File | Dimensione | SHA-256 |
|---|---|---|
| EBOOT.BIN | 10.925.832 byte | `209ADBF717391BD223941993526895F0E03BB5F98436CD67EEF92F593C08F7DD` |
| EBOOT.ELF | 10.923.400 byte | `cd2c37351e840fb31810f1be5fe9d7d03860d2c1d9b621f8ee55cdf67a8889ba` |

- EBOOT.BIN: SELF applicazione (self type 4), key revision 0x0001.
- Decifratura: `tools/self2elf.py`, set di chiavi KEY_APP per revision 0x0001 da `key_vault.cpp` di RPCS3.
- EBOOT.ELF: ELF64 big-endian, PPC64, tipo EXEC.

## Indirizzi principali

| Voce | Valore |
|---|---|
| Entry OPD | 0x859418 → codice 0x14360 |
| TOC entry | 0x88ff38 |
| TOC del modulo (r2) | 0x8afe5c |
| Regione OPD | 0x859408, dimensione 0x2eb30 |

## Segmenti LOAD

| vaddr | filesz | memsz | Flags |
|---|---|---|---|
| 0x10000 | 0x82c3c8 | — | 0x5 (R-X) |
| 0x840000 | 0x6d4e8 | — | 0x6 (RW) |
| 0x10000000 | 0x56b80 | — | 0x400004 |
| 0x10060000 | 0x14ef00 | 0x82c570 (bss 0x6dd670) | 0x600006 |

## Codice

| Voce | Valore |
|---|---|
| Funzioni | 23.910 |
| Codice | ~8,3 MB |
| Siti di chiamata `bl` | ~91.700 |
| Moduli PRX su disco | Nessuno: tutto il codice è nell'EBOOT |
| Import NP/online | Nessuno |

## Contenuto di PS3_GAME/USRDIR

Cartelle `built/` e `packed/`, sottocartelle `game`, `levels`, `movies`.

| Tipo | Quantità | Dimensione |
|---|---|---|
| .psarc | 64 | ~12,4 GB |
| .bik (Bink) | 235 | ~8,5 GB |
| .dat | 77 | ~1,8 GB |
| .pkg (piccoli) | 285 | — |

## Import firmware e copertura runtime

118 funzioni importate da 9 librerie. Copertura del runtime ps3recomp (fork soyjxck, commit 93dbfdd):

| Libreria | Copertura | Note |
|---|---|---|
| cellAudio | 7/7 | |
| cellGcmSys | 13/15 | |
| cellSpurs | 29/31 | |
| cellSync | 1/1 | |
| cellSysmodule | 3/3 | |
| cellSysutil | 18/19 | |
| sysPrxForUser | 10/19 | Nome-scan; le altre 9 sono in `runtime/ppu/ppu_sysprx.cpp` |
| sys_fs | 12/14 | Le altre 2 sono in `runtime/ppu/ppu_fs.cpp` |
| sys_io | 7/9 | |

### Funzioni mancanti (6)

| Funzione | Libreria | Nota |
|---|---|---|
| _cellGcmFunc15 | cellGcmSys | |
| cellSpursLFQueueDetachLv2EventQueue | cellSpurs | |
| cellSaveDataListAutoSave | cellSysutil | Savedata: serve un'implementazione reale |
| sys_game_process_exitspawn | sysPrxForUser | |
| cellPadInfoSensorMode | sys_io | Sixaxis: si possono usare stub |
| cellPadSetSensorMode | sys_io | Sixaxis: si possono usare stub |

Riutilizzabile: `_cellSpursLFQueuePushBody` è implementata in `dod3-recomp/src/kernel/spurs_lfqueue.cpp`.

## SPU

Il gioco usa i taskset SPURS (`cellSpursCreateTasksetWithAttrib`, `JoinTaskset`, `ShutdownTaskset`, event flag) e `cellSync`. Inventario dettagliato: `SPU_INVENTORY.md` (in corso).

## Confronto

| | Drakengard 3 | Ratchet & Clank: Armi di Distruzione |
|---|---|---|
| Funzioni | ~30.134 | 23.910 |
| Motore | Unreal Engine 3 | Insomniac |

## Prossimi passi

1. Implementare le 6 funzioni mancanti.
2. Completare l'inventario SPU (`SPU_INVENTORY.md`).
3. Primo lift PPU.
4. Build.
