# Analisi media - Ratchet & Clank: Tools of Destruction (PS3)

Analisi di sola lettura. Script: `tools\analysis\media_probe.py`, `audio_entry_probe.py`, `audio_header_fields.py`. Output: `out\analysis\*.json`.

## 1. Audio

### Formati trovati

| Formato | Quantita | Posizione | Header verificato | Codec |
|---|---|---|---|---|
| `.xvag` globale | 73 | `packed\game\global_cached.psarc` (`/audio/global/`) | magic `XVAG`, chunk `fmat` a 0x20, encode type 6, 44100 Hz (72) / 48000 Hz (1) | Tipo 6: ADPCM a blocchi da 16 byte, flag 0x00 (inferito dalla struttura) |
| `.xvag` in `.dat` di livello | 121 | `streaming_dialogue*.dat` e simili nei livelli | encode 6 (76 totali) e 8 (45) | Tipo 8: **MPEG-1 Layer III (MP3)**, frame `FF FB D4 40` = 44.1 kHz, 256 kbps, joint stereo |
| `VAGp` in `.dat` | 301 | livelli (`built\levels\*`) | magic `VAGp`, versione 0, interleave 0 (mono) | **PS-ADPCM (VAG)**, 48000 Hz (267), 44099 Hz (34) |
| `.bnk` / `.bnkh` | 460 + 460 | `built/anark/frontend/texturebank_N` | magic `IGHW` | **Non audio**: banche di texture |
| `.pkg` / `.dat` `IGHW` | 300 + 352 | cartelle audio nei livelli | magic `IGHW` | Contenitori generici, contenuto non decodificato |

Totale file audio-correlati: 1164 voci nei manifest (`out\psarc\*.manifest.txt`).

### Stringhe in `elf\EBOOT.ELF`

| Offset | Stringa | Significato |
|---|---|---|
| 0x8cd2ba | "THIS SYSTEM ONLY SUPPORTS ADPCM AND PCM VOICE DATA!" | Voce: solo ADPCM e PCM |
| 0x8ceeb4 / 0x8cef28 | "snd_stream: xvag endian mismatch" / "unsupported xvag encode type" | Decoder XVAG nel gioco |
| 0x8cef58 | "RIFF", "WAVE", "XVAG", "VAGp", "pGAV" | Formati riconosciuti dallo stream |
| 0x8cf404 / 0x8cf48c | "snd_MP3DecodeBufferData", "ps3/libmp3/libmp3.cpp" | Decoder MP3 nel gioco |
| 0x8cf5ca | "LibMP3 stereo job", "LibMP3 surround job" | Job MP3 (WWS) |
| 0x8e1ca0 | "Wws_Job", "WwsJobDelay timed out" | Framework job WWS |
| 0x82bfb8 | "cellAudio", "cellSpurs", "cellSync" | Moduli importati |

Conteggi keyword: `mp3` 46, `vag` 18, `wws` 29, `libmp3` 10, `adec` 9, `bink` 5, `cellaudio` 1.

### Codec del gioco

- **ADPCM/VAG (PS-ADPCM)**: usato per la voce e per gli stream XVAG tipo 6 e i VAGp. Il pattern dei blocchi nei dati XVAG tipo 6 e il magic VAGp lo confermano.
- **MP3**: usato per gli stream XVAG tipo 8 (45 file nei livelli). La frame MPEG verificata e la stringa "LibMP3" lo confermano.
- **ATRAC3/ATRAC3+**: nessuna evidenza. Nessuna stringa, nessun file `.at3`, nessun import `cellAtrac`/`cellAdec`.

## 2. Video (Bink)

| Campo | Valore |
|---|---|
| File | 235 `.bik` in `built\movies\`, circa 8.96 GB totali |
| Tag / revisione | `BIK` + `i` (Bink 1, revisione `i`) |
| Risoluzioni piu comuni | 1280x720 (49), 876x214 (69), 380x280 (29), 424x256 (21), 480x480 (19), 316x180 (19) |
| FPS | 29.97 (NTSC) o 30.0; alcuni clip da 2 frame a 1.0 fps (`a_image_*`) |
| Tracce audio (campo @0x28) | > 0 in 52 file; 6 in `l00_s00_attr_fmv` (non verificato contro i descrittori) |

Decodifica: la EBOOT contiene "Bink Audio Thread", "Not a Bink file", "Error reading Bink header", quindi il decoder Bink e nel codice del gioco. `ps3recomp` ha gia lavoro su Bink tramite il job SPU `binkspu` (note in `docs\DIAGNOSTICS.md`, `libs\spurs\cellSpurs.c`, ereditate dal port LBP).

## 3. Cosa serve al runtime

| Componente | Stato in ps3recomp | Ruolo |
|---|---|---|
| `cellAudio` | Completo (WASAPI/SDL2) | Uscita audio |
| `cellSpurs` / `cellSpursJq` | Parziale / completo | Job SPU (MP3 WWS, binkspu) |
| `cellAdec` / `cellAtrac` | Parziale, nessuna decodifica reale | Non usati dal gioco (decodifica nella EBOOT) |
| Decoder ADPCM, MP3, Bink | Nella EBOOT | Serve solo fedelta dei job SPU e del file I/O |

### Rischi

1. Non assumere 460 banche audio: `.bnk` sono texture.
2. MP3 e Bink dipendono dalla fedelta dei job SPU (SPURS/WWS). Un job bloccato o non completato produce "WwsJobDelay timed out".
3. Il gioco usa 44.1 e 48 kHz: verificare il resampling in `cellAudio`.
4. 34 VAGp dichiarano 44099 Hz: probabile valore non standard, da verificare.
5. L'identificazione di encode type 6 come ADPCM e inferita dalla struttura dei dati, non da una stringa.
6. Circa 9 GB di video: prestazioni di I/O sul disco.
