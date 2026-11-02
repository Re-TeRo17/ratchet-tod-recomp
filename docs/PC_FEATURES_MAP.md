# PC features map: tasti, menu, FOV (analisi statica)

Sola lettura su `game\disc\PS3_GAME\USRDIR` e `elf\EBOOT.ELF`. Gli indici
sono quelli dei manifest `out\psarc\*.manifest.txt` (archivio `global_cached`
salvo diversa indicazione). Il formato di contenitore osservato e' `IGHW`
(magic a 4 byte, big-endian) in quasi tutti i file; significato del nome non
verificato.

## 1. Icone dei tasti e testi

| Risorsa | Archivio, indice | Dim. | Magic | Ruolo / stato |
|---|---|---|---|---|
| `/processed/texpacks/hud.tph` | global [39] | 6,8 KB | IGHW | Header atlante HUD. Contiene 20 tag `PAD0`..`PADF` a offset 0x30, poi record da 32 B a partire da 0x80. Record non decodificati. |
| `/processed/texpacks/hud.tp` | global [38] | 2,5 MB | IGHW | Atlante HUD (pixel). Formato interno non decodificato. |
| `/built/fonts/rcf_scea/font.dat` | global [1131] | 269 KB | IGHW | Tabella glifi (vedi sotto). Contiene anche i tag `PAD0`..`PADF`. |
| `/built/fonts/rcf_scea/font.tp` (+ `font.tph` [1133]) | global [1132] | 536 KB | IGHW | Atlante font SCEA. Ipotesi: contiene i glifi tasto; da confermare decodificando la texture. |
| `/built/fonts/rcf_scec|scej|scek|test/font.tp` | global [1135..1141] | 0,2-9,6 MB | IGHW | Font per regione/lingua. Non analizzati. |
| `/built/game/core_text_tags.<lang>.pkg` | global [1143..1157] (us = 1143) | 87-133 KB | IGHW | Testi di gioco, 15 lingue. Stringhe UTF-8 terminate da 0x00. |
| `/built/anark/frontend/anark_loc_text_tags.<lang>.pkg` | global [41..55] | 7-9 KB | IGHW | Testi del menu principale. |
| `/built/anark/pause/anark_loc_text_tags.<lang>.pkg` | global [87..101] | 11-16 KB | IGHW | Testi del menu di pausa. |
| `/processed/hud/metropolis_demo/metropolis.<lang>.tp` | global [1173..1187] | 8,4 MB ciascuno | non ispezionato | Atlanti HUD per lingua (demo). |

**Codifica dei tasti nei testi.** Il tasto e' un codepoint UTF-8 della
Private Use Area. Esempio: `Press ` + `EE 80 84` + ` to exit` (U+E004).
Codepoint osservati nel testo us (conteggio occorrenze):
- U+E001..U+E012: 18 glifi tasto (con un'altra famiglia U+E101..U+E10D, non mappata).
- U+F008, F248, F555, F666, F777, F7B4, F800, F850, F8FF: non mappati.

**Mappa U+E001..U+E012 -> rettangoli in `font.dat`** (record da 16 B a
offset 0x40180: u16 codepoint, poi x, y in pixel; w = byte@+8, h = byte@+9;
interpretazione probabile, non confermata visivamente):

| Codepoint | x | y | w | h | | Codepoint | x | y | w | h |
|---|---|---|---|---|---|---|---|---|---|---|
| U+E001 | 116 | 91 | 52 | 52 | | U+E007 | 4 | 4 | 52 | 31 |
| U+E002 | 172 | 91 | 52 | 52 | | U+E008 | 60 | 4 | 52 | 31 |
| U+E003 | 4 | 147 | 52 | 52 | | U+E009 | 116 | 39 | 52 | 48 |
| U+E004 | 60 | 147 | 52 | 52 | | U+E00A | 172 | 39 | 52 | 48 |
| U+E005 | 4 | 39 | 52 | 39 | | U+E00B | 116 | 4 | 52 | 31 |
| U+E006 | 60 | 39 | 52 | 39 | | U+E00C | 172 | 4 | 52 | 31 |
| U+E00D | 4 | 91 | 52 | 48 | | U+E011 | 4 | 203 | 52 | 52 |
| U+E00E | 60 | 91 | 52 | 48 | | U+E012 | 60 | 203 | 52 | 52 |
| U+E00F | 116 | 147 | 52 | 52 | | | | | | |
| U+E010 | 172 | 147 | 52 | 52 | | | | | | |

Il glifo di ciascun tasto (Cerchio, Croce, ecc.) non e' ancora identificato:
serve decodificare `font.tp`.

Estratti in `out\analysis\extract\`: `1131__font.dat` (tabella glifi),
`1143__core_text_tags.us.pkg` (testo EN), `41__anark_loc_text_tags.us.pkg`,
`39__hud.tph`, `1130__font.tph`, plus header di `hud.tp` (4 KB) e `built.dat` (4 KB).

## 2. Menu, opzioni, script

| Elemento | Dove | Stato |
|---|---|---|
| Menu frontend con "GAME OPTIONS" | `/built/anark/frontend/built.dat` (global [40], 94,6 KB, IGHW) | Scena UI con `optionGroup`, `option1..6`, `barControl`, `adjustOption`, `moveOption`, `frontendHandler`, `onMainMenu`, `GetLanguage`. Non e' Lua. |
| Menu di pausa | `/built/anark/pause/built.dat` (global [86], 1,15 MB) | Non analizzato a fondo. |
| Opzioni esposte (stringhe) | come sopra + EBOOT | Velocita' camera, invert X/Y (modalita' sguardo), sottotitoli, volumi musica/effetti/voce, six-axis, lingua. |
| Opzioni grafiche | nessuna | Cercate `brightness`, `aspect`, `gamma`, `vsync`, `resolution`, `fov` nel frontend: 0 risultati. Nell'EBOOT: 1 `brightness` non legato al menu. |
| Script `.lc` | 524 `.lc` + 524 `.lua` | Script di gameplay: `/levels/*/scripts`, `/built/levels/*/scripts`, `/built/game/scripts`, `/data/configs`. Nessuno per menu o opzioni. |
| Versione Lua | header `.lc`: `1B 4C 75 61 50 01 04 04 04 06 08 09` | Byte di versione `0x50` = Lua 5.0. Gli altri byte di header non sono quelli standard. EBOOT contiene la stringa `Lua 5.0.2`. |
| Checksum | TOC PSARC: MD5 per entry | Testate 3 entry (1112, 1113, 39): il MD5 non coincide con il MD5 del contenuto ne' con quello del nome. I valori non compaiono nell'EBOOT. Meccanismo di verifica non trovato. |

**Possibilita' di aggiungere una pagina opzioni:** non dimostrata. Non esiste
un file `.lc` da sostituire per il menu. Per modificare la scena
`built.dat` servirebbe prima decodificarla. Il controllo dei pacchetti non e'
chiaro: il TOC non e' un MD5 semplice, quindi non si puo' dire se un file
modificato venga rifiutato.

## 3. FOV

- La FOV del follow camera e' il 4° argomento di `set_follow_cam(hero, dist, pitch, fov, height)`.
  Valori osservati: 75.5, 75.7, 80, 85, 90 (per livello, hardcoded).
- File chiave (archivio global, indice): `/levels/apogee space station/scripts/station.lua` [2249],
  `/levels/cobalia/scripts/enemy1.lua` [2255], `/levels/iris/scripts/cameras.lua` [2655],
  `/levels/metropolis/scripts/cameras.lua` [2817], `/levels/imperial fight fest/scripts/arena.lua` [2475],
  `/levels/meridian city/scripts/b1_start_cam_check.lua` [2727].
- Il codice di default (`get_follow_cam_fov`, `CAMERA_ADJUST_TRACKER`) e' presente come testo Lua nell'EBOOT,
  ma non nei PSARC scansionati.

## 4. Approcci raccomandati

**Icone dei tasti (Xbox, PS4, PS5, tastiera).** Modello dod3: sostituire
l'atlante in fase di upload della texture, identificandolo per contenuto.
1. Decodificare `font.tp` (e `hud.tp` se necessario) e confermare quale atlante
   contiene i tasti, con le coordinate di sopra.
2. Creare atlanti alternativi con gli stessi rettangoli (52 px) per ogni set.
3. Hook sull'upload: hash del contenuto, sostituzione.
4. Il testo non va modificato: i codici U+E001..U+E012 restano, la mappa
   glifo->tasto e' esterna. Le stringhe "Press START" sono texture separate (da trattare a parte, come fa dod3).

**Pagina grafica nelle opzioni.** Due strade:
- Patch della scena `built.dat` (come dod3 con gli script UnrealScript): piu' fedele ma
  richiede prima la decodifica della scena. Non consigliata come primo passo.
- Pagina nativa del port (overlay), che salva in un `.ini`, come in dod3. Piu' sicura,
  indipendente dal formato del gioco.

**FOV.** Intercettare la native `set_follow_cam` nel port e scalare o
sostituire il 4° argomento: un unico punto copre tutti i livelli, senza toccare
i `.lc`. Va verificato che `get_follow_cam_fov` restituisca un valore coerente.

**Sostituzione di script `.lc`.** Possibile solo se il loader legge file
sovrascritti prima dei PSARC (come l'overlay `patch101` di dod3). Non ancora
testato; verificare prima il controllo dei pacchetti.

## Strumenti creati

- `tools\analysis\psarc_extract.py`: estrae singole entry (per indice) con `psarc_scan.py`.
- `tools\analysis\check_psarc_md5.py`: confronta MD5 del TOC con contenuto e nome.
- `tools\analysis\grep_psarc.py`: ricerca in-memory sui file testuali degli archivi.

## Domande aperte

1. Quale glifo corrisponde a ciascun codepoint U+E001..U+E012 (decodificare `font.tp`).
2. Formato dei record 32 B di `hud.tph` e del testo in `font.tp`.
3. Meccanismo di verifica dei pacchetti (TOC MD5 non riconducibile; cercare un hash nel loader dell'EBOOT).
4. Struttura interna di `built.dat` (menu) per una eventuale pagina nella scena.
