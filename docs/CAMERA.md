# Camera: FOV e controllo mouse - Ratchet & Clank: Armi di Distruzione (BCES00052)

Analisi statica (nessuna esecuzione). Indirizzi = VA guest. Funzioni lifted = `func_XXXXXXXX` (8 cifre hex).
Helper: `tools/analysis/cam_dis.py` (disasm con risoluzione stub inter-TOC), `cam_callers.py` (chiamanti bl/b, anche via stub),
`cam_offs.py` (load/store con un dato offset), `cam_reg.py` (tabella registrazione Lua nome -> funzione), `cam_toc.py` (costanti TOC).
TOC: `0x88FF38` (camera, input, opzioni), `0x89FF20` (binding Lua), `0x8AFE5C` (pad/UI).

## 1. FOV

**`set_follow_cam(hero, dist, pitch, fov, height)`: confermato.** Nei `.lua` di `global_cached.psarc` (station, enemy1, gelatonium, arena,
meridian b1_start_cam_check, metropolis cameras, rykan main...) le chiamate sono `set_follow_cam(Ratchet1, 7.5, 12, 75.5, 1.65)`; `fov` va da
75.5 a 90 (meridian: 90/85). MOLTE chiamate usano solo 3 argomenti (`set_follow_cam(Clank1, 3, 20)`, boss_camera, stratus enemy):
`fov` manca e il FOV resta quello di default. `get_follow_cam_fov` e `update_camera_adjust_area/vol` (nel testo Lua dentro l'EBOOT, 0x1006155a)
interpolano `fov` con `CAMERA_ADJUST_TRACKER[cam].initfov`; se `fov==nil` lo azzerano (= "non impostare").

Registrazione Lua (tabella chiamata da `0x294E60`, TOC 0x89FF20, `lwz r4,name; lwz r5,opd` poi `bl 0x251218`):

| Lua | stringa | OPD | codice |
|---|---|---|---|
| `set_follow_cam` | 0x1001B4A8 | 0x86C0B0 | `0x2BFEC0` (wrapper lua_CFunction; fa `bl 0x2C0088 -> 0x28BD60`) |
| `get_follow_cam_fov` | 0x1001AA08 | 0x86C068 | `0x2BF590` (nativa `0x2843B0`) |
| `reset_follow_cam` | 0x1001B478 | 0x86C0A0 | `0x2BFC48` (nativa `0x28BC40`) |
| `set_follow_cam_indexed` | 0x1001A9F0 | 0x86C060 | `0x2BF348` |

Nativa `func_0028BD60(client, f1=dist, f2=pitch, f3=fov, f4=height)` (OPD 0x86B038): ottiene la struttura "follow cam target" = `base + 0x2150`
(stub `0x250B28 -> 0x683F8`) e scrive `+0x6C=dist (flag +0x78)`, `+0x70=pitch (flag +0x79)`, `+0x74=fov` **solo se fov>0** (flag `+0x7A`), height in `0x64(world+0x1FC0)`.
Quindi il valore e' solo un *target*: non e' li' che il FOV viene "usato".

Pipeline FOV (alta confidenza sul flusso, media sulle unita'):
1. `func_0006D468(cam, followTarget)` (chiamata da `0x713B0`, update follow cam): copia `dist/fov/pitch` target in `cam+0x130 / +0x134 / +0x138`.
2. `func_0006D850` (stub `0x252CF8`; chiamata da `0x713B0 @0x715F8` e da `get_follow_cam_fov`): smorza `cam+0x60` verso `cam+0x134`
   con `func_000107C0(cur,target,0,0,0,dt)`, dt = `[0x10731D00]+0xA4`. **`cam+0x60` = FOV corrente in gradi.** Il fallback (nessun fov nello script) e' il valore iniziale
   scritto dal costruttore `func_0006EC40` (`stfs f1,0x60(r31)` dopo una chiamata virtuale `0x6ECEC`).
3. **`func_0006CEE8`** (OPD 0x85C2F8, sync camera di gioco -> camera di render): `r7 = *(r3+0x6C)` (stub `0x10620 -> 0x63BF80`);
   `0x6CFB8: lfs f1,0x60(r7)`; `0x6CFBC: fmuls f0,f1,f6` con `f6 = 0.0174533` (deg->rad, TOC `0x88ABE8`); `0x6CFC4: stfs f0,0xB0(r29)`
   = **FOV in radianti della camera di render** (`+0xB4 = r7+0x5C` salvato accanto, probabile aspect/altro). Letto poi da `func_0006D010` (0x6D0EC)
   e `func_00061478` (0x61558), cioe' costruzione frustum/proiezione. Non ho trovato un `tan` esplicito (e' in libm inline o nella
   chiamata virtuale finale di 0x6CEE8): il punto di patch non ne ha bisogno.

**Punto di patch proposto per `TOD_FOV`**: override registry di `func_0006CEE8` (stile `TOD_FPS` in FRAMERATE.md):
```
cam = BE32(r3 + 0x6C);            // stesso r7 dell'originale
if (cam && is_follow_cam(cam)) {
    old = BEF32(cam + 0x60);
    BEF32(cam + 0x60) = clamp(old + TOD_FOV_DELTA, 20, 120);   // oppure old * scala
    orig(ctx);
    BEF32(cam + 0x60) = old;      // ripristina: lo stato di smorzamento (0x6D850) non deve accumulare l'offset
} else orig(ctx);
```
Perche' qui e non in `set_follow_cam`: copre anche le chiamate senza 4° argomento (default) e `reset_follow_cam`, e non rompe le interpolazioni Lua
(`get_follow_cam_fov` restituisce il valore non offsettato). Patchare la nativa `0x28BD60` (scrivere `fov+delta` a `0x28BE2C`) e' piu' semplice ma
manca i default e fa doppio offset nelle interpolazioni `update_camera_adjust_*` (initfov letto gia' offsettato).

Gameplay vs cutscene: `0x6CEE8` e' l'unico imbuto per TUTTE le camere (anche script `activate_cam`, `boss_register_cam`, `activate_skydive_cam`, `set_robowing_cam`).
Per limitarsi alla follow cam: override anche di `func_000713B0` (update follow cam, OPD 0x85C3E8), che salva `g_follow_cam = r3` e `g_follow_frame = u32[0x10731D00+0xD0]`
(contatore frame del Timer); in 0x6CEE8 applicare l'offset solo se `cam == g_follow_cam && frame == g_follow_frame`. (Non verificato che `0x713B0` giri solo con follow cam
attiva; la guardia sul frame lo copre.) Non ho trovato un campo "tipo camera" affidabile: `0x71680` (`lwz 0x74(r3)==4`) appartiene a un'altra classe (a `cam+0x74` la
camera ha un oggetto input con vptr, vedi sotto).
Confidenza: flusso/offset `cam+0x60` gradi **alta (~85%)**; "gradi verticali o orizzontali" non determinato; gating cutscene **media (~60%)**.
Costanti utili: FOV di gioco `fov` 75.5-90 -> aspetto 16:9 gia' considerato dallo script? (da verificare a runtime: confrontare `cam+0x60` e l'immagine).

## 2. Controllo camera (stick destro)

Catena (alta confidenza sui passi 1-4):
1. `func_005A78F8` (pad update, TOC 0x8AFE5C): `bl 0x83976C` = `cellPadGetData` (a `0x5A7F7C`). Stato pad globale `P=0x10675610`, 4 slot da `0x64` byte a `P+0x288+i*0x64`:
   `+0 = LX, +4 = LY, +8 = RX, +0xC = RY` come float `(raw-128)*0.0078125` in [-1,1) (raw = `button[6],[7],[4],[5]` di CellPadData; costanti TOC 0x8A7FB8/BC).
   Nessuna dead zone qui (solo flag "stick oltre 0.75" per menu, 0x58 |= 0x2000...). Sixaxis a `+0x10..+0x1C` (`(raw-512)/512`).
2. Livello mapping `func_00664BF0`: copia i 4 assi dello slot in un oggetto logico, riordinati da una tabella di 4 byte `profile+0x40..0x43`
   (`0x664E00/E24/E48/E6C`): assi logici 0..3 = oggetto+`0x0C/0x10/0x14/0x18`. Getter `0x11B30 -> 0x662B78` = `*(float*)(obj+0x14+4*idx)`.
   La camera ha l'oggetto input **incorporato a `cam+0x74`** (init in `0x6EC40`): asse camera X = idx0 (`cam+0x88`), Y = idx1 (`cam+0x8C`).
3. **`func_00070CF8`** (OPD 0x85C3E0, chiamata da `0x713B0 @0x714B8`; follow cam): legge idx0/idx1; ognuno passa da `func_00662688` (stub `0x10650`, dead zone **0.35 -> 0.70**:
   0 sotto 0.35, rampa lineare, 1 sopra 0.70, segno preservato; costanti TOC 0x88AC58/0x88ACA0); poi coordinate polari `0x116C0` (atan2+modulo), *snap angolare* `0x10900(angle, 0.9, 4)`
   (verso i 4 assi, forza 0.9, TOC 0x88ACA4) e ritorno a cartesiano `0x13F50`: **l'input mouse sintetizzato come stick sarebbe deformato (dead zone ampia + snap)**.
4. Velocita'/inversione: `func_0006D4A8` (OPD 0x85C318, solo da 0x70E54) = **yaw**: `ret = s * pi * (speed + 0.1) * x * cam[0x144]` con `s = -1 se G+0x14A8 != 0 altrimenti +1`,
   `cam+0x144 = 1.0` (scritto solo in `0x6ED3C`). `func_0006D518` (OPD 0x85C320, da 0x70E6C) = **pitch**: `-y*(speed+0.1)*s'` (s' da `G+0x14AC`) come frazione di interpolazione
   verso i limiti di pitch **-35 deg / +50 deg** (floats a `0x1006036C` / `0x10060370`), ritorna il pitch *obiettivo assoluto* in radianti. Poi `0x70FD8`: `angolo = yaw * dt * 1.0` con
   `func_0069B3A0(clock, v, k) = v*k*dt` (`0x125D0`), clock = `0x10731DA0` -> **dt = `*(float*)0x10731DA4`**; metà angolo (`*0.5`) -> quaternione -> rotazione attorno all'asse verticale.
   Il pitch invece passa da uno smorzamento esponenziale `0x12A10(..., rate=followTarget+0x60, dt=[0x10731D00]+0xA4)` a `0x710F4`.
   **Quindi la velocita' di yaw e' in rad/s x dt (indipendente dal frame rate: va bene a 60/120 fps, dt e' clampato a [1/75, 0.08], vedi FRAMERATE.md);
   anche il pitch usa dt (smorzamento).** Non serve compensare a meno di cambiare le costanti del Timer.
5. Flag "camera guidata dal giocatore" `0x101F5048 |= 0x10` (`0x70F28`: `[toc-0x533C]=0x101EFB20; +0x5528`) quando `|x'|>0 o |y'|>0` (valori PRIMA di 0x6D4A8): disattiva presumibilmente l'auto-recentering.

**Opzioni del menu** (struct `G = 0x101EFB20 + 0x10000 = 0x101FFB20`, `[toc-0x7914]`, `addis r9,r3,1`):

| Campo | Indirizzo | Significato | Nativa Lua |
|---|---|---|---|
| `G+0x14A8` | 0x10200FC8 | invert X (camera) | `set_x_inverted` (0x2C288 -> 0x26C98) / `is_x_inverted` |
| `G+0x14AC` | 0x10200FCC | invert Y (camera) | `set_y_inverted` (0x2C128 -> 0x26C60) |
| `G+0x14B0` | 0x10200FD0 | **camera speed**, float clamp [0,1] (`0x27678`) | `set_camera_speed` (0x2BFD8) / `get_camera_speed` (0x2C080 -> 0x26C50) |
| `G+0x14B4` | 0x10200FD4 | invert look X (aim/look) | `set_look_x_inverted` (0x2BE78 -> 0x26C18) |
| `G+0x14B8` | 0x10200FD8 | invert look Y | `set_look_y_inverted` (0x2BD18 -> 0x26BE0) |
| `G+0x14C4` | 0x10200FE4 | secondo slider 0..1 (non identificato) | `0x276A0` |

Stringhe menu: `GEN_TXT_MENU_CAMSPEED`, `INVERTX/Y`, `INVERTXLOOK/YLOOK`, `CONTROLSCHEME1/2`, `AIM`; flag da riga di comando `-cam_no_invert` (0x10027460, letto in `0x48A018`).
Velocita' camera = `pi*(speed+0.1)` rad/s a stick pieno (speed 0..1 -> 0.31..3.46 rad/s).

**Come alimentare il mouse (proposta, in ordine di preferenza)**
- **A) Iniezione diretta (consigliata, come dod3)**: override registry di `func_0006D4A8` (yaw) : `ret = orig(ctx) + sgn * dyaw_rad / dt`, con `dt = BEF32(0x10731DA4)`
  (l'angolo applicato e' `ret*dt`, quindi la rotazione del mouse risulta esatta, indipendente dal frame rate), `dyaw_rad` = delta mouse accumulato per frame x sensibilita'.
  Se si vuole rispettare l'opzione invert-X moltiplicare per `-1` quando `BE32(0x10200FC8)!=0`; la dead zone e lo snap sono a monte (non toccano il valore iniettato).
  In piu' impostare `*(u64*)0x101F5048 |= 0x10` quando c'e' movimento del mouse (equivale a "stick attivo").
  Pitch: override di `func_0006D518` sommando `dpitch_rad` (con segno) al pitch obiettivo restituito; **attenzione**: il risultato e' poi smorzato da `0x12A10`
  quindi il movimento mouse sara' leggermente morbido/in ritardo e va clampato a [-35,+50] deg (o allargare i floats `0x1006036C/70`). Da validare a runtime (confidenza media ~55%).
- B) Stick sintetico: via `cellPad_set_host_input` valori RX/RY (stato guest `P+0x288+8/0xC`). Sconsigliato: dead zone 0.35-0.70, snap a 4 direzioni e curva lineare
  rendono il mouse non lineare e quantizzato; utile solo per la tastiera (frecce) dove e' un on/off.
- C) Override di `0x11B30` (getter assi): condiviso con il resto del gioco, evitare.
Confidenza: catena pad -> camera e formule yaw/dt **alta (~80%)**; indirizzi opzioni **alta (~85%)**; pitch/smorzamento **media**.

## 3. Aim / strafe / first person

- Esiste una modalita' prima persona: flag Lua `HERO_FIRST_PERSON` (stringa 0x100009A8, usata con `check_flag`, es. metropolis/help.lua) e opzioni dedicate
  `invert look X/Y` (`G+0x14B4/0x14B8`) + `GEN_TXT_MENU_AIM`. Le classi camera che leggono `G+0x14B4/0x14B8` e `G+0x14B0` sono **diverse dalla follow cam**:
  `func_000749C8` (OPD 0x85C5A8), `func_00076AE8` (0x85C628), `func_000779B8` (0x85C690): stessa sorgente assi (`cam+0x74`), usano `dt = [0x10731D00]+0xA4` per accumulo/smorzamento
  (`0x74B20`: `lfs f27,0xA4(r10)`) e hanno un proprio yaw/pitch (`0x128(this)`). **Per queste camere NON basta agganciare 0x6D4A8/0x6D518**: serve un hook equivalente (es. dopo il
  calcolo dei due assi in `0x749C8` ~`0x74B00..0x74C00`); non analizzato in dettaglio (confidenza bassa sull'assegnazione di queste classi a "prima persona" vs "aim").
- Lock-strafe (L1): native `is_lock_strafe_active`, `lockstrafe`, `lookatlock`, `lookatpoint` (stringhe nel blocco nomi Lua): la camera resta la follow cam
  (nessun path pad separato trovato); il movimento e' relativo al bersaglio.
- Camere scriptate/cutscene: `activate_cam/deactivate_cam/is_active_cam`, `activate_skydive_cam`, `set_robowing_cam`, `boss_register_cam`, `camera_shake`: non usano il path stick;
  passano comunque da `0x6CEE8` (vedi FOV: usare la guardia su `0x713B0`).
- FOV in prima persona/aim: stesso imbuto `0x6CEE8` (se la camera e' scriptata il guard di sezione 1 la esclude).

## 4. Riepilogo indirizzi

| Cosa | Indirizzo |
|---|---|
| Lua `set_follow_cam` (wrapper / nativa) | 0x2BFEC0 (OPD 0x86C0B0) / 0x28BD60 (OPD 0x86B038) |
| Struct target follow cam | `world+0x2150` (stub 0x250B28 -> 0x683F8): dist +0x6C, pitch +0x70, fov +0x74, flag +0x78/79/7A |
| Update follow cam | 0x713B0 (OPD 0x85C3E8) |
| FOV smorzato (gradi) | `cam+0x60`, target `cam+0x134` (func 0x6D850 / 0x6D468) |
| Sync camera -> render, **patch FOV** | **0x6CEE8** (OPD 0x85C2F8), render fov rad a `+0xB0` |
| Input stick -> camera | pad 0x5A78F8 -> P=0x10675610 slot +8/+0xC -> mapping 0x664BF0 -> `cam+0x88/0x8C` -> 0x70CF8 |
| Yaw / pitch (hook mouse) | 0x6D4A8 / 0x6D518 |
| dt per la camera | `0x10731DA4` (Timer clock 3) |
| Opzioni camera | 0x10200FC8 (inv X), 0x10200FCC (inv Y), 0x10200FD0 (speed), 0x10200FD4/D8 (look inv) |
| Limiti pitch | 0x1006036C (-35), 0x10060370 (+50) |
