# Frame rate e tempo di gioco - Ratchet & Clank: Armi di Distruzione (BCES00052)

Analisi statica (nessuna esecuzione). Script in `tools/analysis/fr_*.py`, output in `out/analysis/`.
Frequenza timebase PS3 = 79.800.000 Hz (costante float `79800000.0` e reciproco `1.2531328e-8`, `0x3257495D`, nelle TOC).

## 0. Nota preliminare: l'EBOOT ha TRE TOC

`r2` non e' unico (OPD = coppie codice/TOC a 32 bit): `0x88FF38`, `0x89FF20`, `0x8AFE5C`. Il valore `0x8AFE5C` di `docs/TRIAGE.md`
e' solo una delle tre. Il TOC di una funzione = quello dell'OPD precedente piu' vicino (`fr_toc.py of func_XXXXXXXX`).
Gli stub "TOC switch" (`std r2,0x28(r1); addis/addi r2; b target`, 16 byte) sono lifted come funzioni senza intestazione:
chiamate "mancanti" nel grafo statico sono quasi sempre stub (es. `0x251D68 -> 0x692D40`).

## 1. Come il gioco ritma i frame

Oggetto globale **Timer** a `0x10731D00` (BSS, "TS"):

| Offset | Significato (dedotto) |
|---|---|
| +0x04 | dt minimo / periodo di cap (s) |
| +0x08 | dt massimo (s) |
| +0x10/+0x40/+0x70/+0xA0 | 4 orologi: `+4` dt del frame (s), `+8` 1/dt, `+0x14` tempo totale (s), `+0` scala |
| +0xD0 | contatore frame |

- **Costruttore** `func_0069ACE8` (stub `0x24F0F8`, chiamato da main a lr `0x392360`): se `TS+8<=0` -> `0.08`; se `TS+4<=0` -> `0.0133333` (1/75).
  TS e' BSS (zero) quindi **default: min dt 1/75 s, max dt 0.08 s**. `func_0069AEC0` (stub `0x24F898`) li ri-clampa: `+4=max(+4,0.01333)`, `+8=min(+8,0.08)`.
  Costanti float in TOC `0x8AFE5C`: `-0x5904` = 0x8AA558 (0.0133333), `-0x5908` = 0x8AA554 (0.08). Usate solo da questi due.
- **Flag da riga di comando** (`func_0048A018`, argc/argv in globali `0x1036B0E8`/`0x1036B0EC`): `-capfps N` (TS+4=1/N), `-lockfps N` (TS+4=TS+8=1/N), `-60fps` (entrambi 1/60).
  Se `TS+8==TS+4` il gioco usa dt fisso (`func_0007E248`); altrimenti `dt=min(TS+0x14, 0.06)`.
- **Aggiornamento tempo** `func_0069AF80` (stub `0x252D88`, chiamato all'inizio di ogni iterazione dei loop di schermata): `elapsed = mftb - last`;
  orologio 0 (TS+0x10) = `min(elapsed, TS+8)`; orologi 1-3 = `clamp(elapsed, TS+4, TS+8)` (cioe' *con pavimento*), poi `secondi = ticks * 1.2531328e-8 * scala`.
- **Limitatore** `func_00692D40` (stub `0x251D68`, chiamato dopo `cellGcmSetWaitFlip` in `0x38F618/0x38FD38/0x396CD0/0x3982B0/0x398C00`):
  `periodo_ticks = TS[4] * 0.9 * 79.8e6` (0.9 = float in `0x8AA3D0`); `while (mftb - last < periodo) usleep(500us)` (syscall 141, `0x692DB8`);
  poi chiama `func_00505558` (fine frame: submit GCM + flip, misura `G+0x150`) e salva `last=mftb`.
  Default: `0.9/75 = 12 ms`. Il resto lo fa il VSYNC.
- **Flip**: `func_005372F0(modo)` (modo viene da main, struct su stack, = 0) -> `cellGcmSetFlipMode(VSYNC)`, `g[0]=0`.
  Il vblank handler registrato in `func_00537B80` (OPD 0x877400 = `func_00537008`) con `g[0]==0` esce subito: **nessun conteggio di vblank**
  (modi 1..4 userebbero `g[0]=1..3` = vblank per frame). `cellGcmSetFlip` a lr 0x537A44 e' immediato.
- **`func_00501980`** (usleep 50 us a lr `0x5056D8`) NON e' un limitatore: e' un'attesa di sincronizzazione RSX
  (polling di una label, maschera `0xF0000000` vs `G+0x2BC`), timeout `0x0BE420E0` tick = **2,5 s**, poi diagnostica.
- `G+0x150` (dt misurato in `func_00505558`, `G=[TOC+0x7498]`) e' letto solo da `func_0050E430` (statistiche): non guida la logica.

## 2. Logica legata ai frame o al dt?

**Dt misurato** (timebase), clampato a [1/75, 0.08] s. Non c'e' un passo fisso a 30 Hz. Il gioco e' un titolo a **60 fps** (vsync) con
cap a 75. Quindi ~55 fps presentati dal port **non** accelerano la logica: dt reale (~18 ms) e' dentro il range.
Rischio opposto: sopra 75 fps il pavimento (1/75) rallenta gli orologi 1-3 (a 120 fps, ~62% di velocita').

## 3. Punti di patch (stile DOD3_FPS)

Tutti i valori sono float big-endian nella RAM guest, da scrivere prima di `ppu_run` (il ctor non sovrascrive valori non nulli) o via override di `func_0069ACE8`.

| Obiettivo | Scrittura |
|---|---|
| (a) 30 fps "da console" (variabile) | `0x10731D04 = 1/30 (0x3D088889)`, `0x10731D08` invariato (0.08). Limitatore 30 ms + vsync a 2 vblank |
| (a') 30 fps con dt fisso | `0x10731D04 = 0x10731D08 = 0x3D088889` (equivale a `-lockfps 30`) |
| (b) 60 fps esatti | `0x10731D04 = 1/60 (0x3C888889)`, vblank sintetico a 16,667 ms (non 16 ms) |
| (c) 120 fps | `0x10731D04 = 1/120 (0x3C088889)` **e** `0x8AA558 = 0x3C088889` (pavimento); vblank del port a 120 Hz con timer ad alta risoluzione; `usleep(500us)` del port con granularita' <= 0,5 ms |
| (d) leva generica `TOD_FPS=n` | override registry di `func_0069ACE8`: scrive `TS+4=1/n`, `TS+8=0.08` (o `1/n` per dt fisso) e `0x8AA558=min(1/75,1/n)`, poi chiama l'originale |

Alternativa senza memoria: iniettare argv `-capfps N` / `-lockfps N` / `-60fps` (da verificare come il CRT `func_00014498` legge argc/argv).

## 4. Rischi

- **Default non verificati a runtime**: 1/75 e 0.08 derivano dal ctor (BSS = 0). Conviene loggare `TS+4/+8/+0x14` una volta.
- **>75 fps**: pavimento dt (patch 0x8AA558) e probabili costanti a 30/60 Hz in sottosistemi (93 costanti `30.0`, 27 `60.0` nelle TOC): animazioni/particelle/fisica a passo fisso non verificati.
- **Pipeline SPU/RSX per frame** (label, triple buffering, `G+0x2BC`): a 120 fps meno budget per frame; timeout 2,5 s del wait RSX.
- **Granularita' sleep Windows** (usleep 500 us nel limitatore): oltre 60 fps il limitatore puo' sforare e il ritmo dipende dal vblank del port.
- **dt fisso (lock)**: se il port scende sotto il rate il gioco rallenta; preferire il cap variabile.
- Filmati/cutscene e UI (`func_0038F618/0038FD38`, usano `TS+0x14` e dt clampato 0.06) non testati a rate diversi.

## 5. Script

`fr_consts.py` (float in ELF), `fr_index.py` (indice funzioni/call graph), `fr_stubs.py` (NID->nomi import), `fr_sys.py` (syscall tempo),
`fr_show.py` (funzione lifted senza boilerplate), `fr_toc.py` (TOC per funzione, valori, costanti float), `fr_users.py`, `fr_field.py`,
`fr_gfield.py`, `fr_stores.py` (scansione raw PPC), `fr_graph.py` (callers/callees/path).
