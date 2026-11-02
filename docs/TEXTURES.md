# Texture dei tasti: formato IGHW, atlanti e mappa glifi

Analisi statica sola lettura su `game\disc\PS3_GAME\USRDIR\packed\game\global_uncached.psarc` (estratti con `tools\analysis\psarc_extract.py`) in `out\analysis\textures\raw\`. Decoder: `tools\analysis\ighw_tex.py`. Output: `out\analysis\textures\`.

## 1. Contenitore IGHW (big-endian)

Vale per `.tph` (header) e `.tp` (pixel):

| Offset | Campo |
|---|---|
| 0x00 | magic `IGHW` |
| 0x04 | versione `0x00010001` |
| 0x08 | numero di sezioni (1 per hud.tph/font.tph, 2 per i .tp) |
| 0x0C | offset della tabella sezioni (0x30 nei .tph, 0x40 nei .tp) |
| 0x10 | dimensione totale file |
| 0x18 | sentinella `DEADDEAD DEADDEAD` |
| 0x20 | tabella sezioni, voci da 16 B: `(tipo/id, offset, ?, dimensione)` |
| 0x30 | tag `PAD0`..`PADF` (20 voci da 4 B; collegamento ai record non trovato) |
| 0x80 | record texture da 32 B |

Sezione dati dei `.tp`: tipo `0x53`, offset `0x80`. Ogni record texture:

- w0 = offset del surface, relativo a 0x80 (quindi offset assoluto = 0x80 + w0)
- w1, byte 2 = formato GCM: `0x86` = DXT1, `0x88` = DXT5, `0xA8` = DXT5 con flag linear (0x20); i dati compressi non sono swizzled
- w5 = pitch/flag (non usato)
- w6 = (larghezza, altezza) come coppia u16 (es. `0x0100 0x0100` = 256x256)
- w4 = `0x0000AAE4`: remap standard ARGB
- I surface sono allineati a 128 B e includono la catena mip completa (verificato sugli span: 256x256 DXT5 = 87424 B).

## 2. Atlante tasti (font.tp)

`/built/fonts/rcf_scea/font.tph` (global [1133]) + `font.tp` (global [1132]). 4 texture:

| Rec | Offset (abs.) | Formato | Dim. | Contenuto |
|---|---|---|---|---|
| 0 | 0x000080 | DXT1 | 1024x512 | font testo (glifi ASCII/latini, `font_text_T0.png`) |
| 1 | 0x055600 | DXT1 | 512x256 | non ispezionato |
| 2 | 0x06AB80 | DXT1 | 128x128 | non ispezionato |
| 3 | 0x06D680 | DXT5 | 256x256 | **atlante tasti** (`font.png`) |

Il rec 3 contiene: L1, L2, R1, R2, SELECT, START, L3, L, R3, R, cerchio, croce, quadrato, triangolo, frecce D-pad.

### Mappa U+E001..U+E012 -> tasto

Coordinate da `font.dat` (record da 16 B a 0x40180, u16 BE). Byte larghezza/altezza a **+10/+11** (il documento precedente diceva +8/+9: errato). Verificato con `font_buttons.png`:

| Codepoint | x,y | w x h | Tasto (verificato a vista) |
|---|---|---|---|
| U+E001 | 116,91 | 52x52 | Cerchio |
| U+E002 | 172,91 | 52x52 | Quadrato |
| U+E003 | 4,147 | 52x52 | Croce |
| U+E004 | 60,147 | 52x52 | Triangolo |
| U+E005 | 4,39 | 52x39 | SELECT |
| U+E006 | 60,39 | 52x39 | START |
| U+E007 | 4,4 | 52x31 | L1 |
| U+E008 | 60,4 | 52x31 | L2 |
| U+E009 | 116,39 | 52x48 | L3 (click stick sinistro) |
| U+E00A | 172,39 | 52x48 | L (stick sinistro, direzionale) |
| U+E00B | 116,4 | 52x31 | R1 |
| U+E00C | 172,4 | 52x31 | R2 |
| U+E00D | 4,91 | 52x48 | R3 (click stick destro) |
| U+E00E | 60,91 | 52x48 | R (stick destro, direzionale) |
| U+E00F | 116,147 | 52x52 | D-pad giu' |
| U+E010 | 172,147 | 52x52 | D-pad su |
| U+E011 | 4,203 | 52x52 | D-pad destra |
| U+E012 | 60,203 | 52x52 | D-pad sinistra |

Le famiglie U+E101..U+E10D e i codepoint non mappati restano fuori dallo scopo.

## 3. Atlante HUD (hud.tp / hud.tph)

- `hud.tph` (39): 208 record (da 0x80 a 0x1A80), tutti DXT5 (formato `0x88` o `0xA8`).
- `hud.tp` (38): 2,5 MB di pixel, sezione dati da 0x80.
- Contact sheet di tutti i record: `out\analysis\textures\hud_sheet.png`.
- Tasti verificati (record): L1 = 98 (48x44), cerchio = 103, croce = 104, quadrato = 105, triangolo = 106, R1 = 187 (48x44). Immagini 2x: `hud_buttons.png`.
- Record 99..102 (128x128): controller PS3 con frecce di animazione. Record 107 (64x64): icona di rete. Record 175 (64x64): controller bianco. Record 174: timer.
- Tag PAD0..PADF nell'header: il collegamento tra tag e record non e' stato trovato. Restano non risolti.

## 4. Sostituzione (piano, non implementato)

Il modello di sostituzione resta lo stesso di dod3:
1. Pagina di riferimento: il rec 3 di font.tp (256x256 DXT5, catena mip 87424 B). Un atlante sostitutivo deve avere stesse dim., stesso formato e stesse coordinate in `font.dat`.
2. Hook all'upload della texture RSX: calcolare un hash (es. SHA-1) dei byte del surface e, se corrisponde a quello del rec 3, sostituire con l'atlante alternativo (xbox/PS4/PS5/tastiera), stesso layout DXT5.
3. Il testo non cambia: U+E001..U+E012 restano, la mappa glifo->tasto e' esterna.
4. Atlante HUD: stessa procedura per i record 98..106 e 187 se servono icone HUD.

## Stato

Decodifica riuscita per font.tp (rec 0 e 3) e per hud (tutti i record DXT5 visualizzati). Non verificati: rec 1 e 2 di font.tp, il collegamento PAD0..PADF e la sostituzione reale (non testata a runtime).
