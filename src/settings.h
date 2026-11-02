/*
 * tod.ini: the player's settings. KEY = VALUE lines; # or ; start a comment
 * (a whole line, or the end of a value). Every key is one of the environment
 * variables the port or the runtime already reads (TOD_FPS, RSX_SCALE, ...), so
 * the file is a friendlier spelling of the same switches. A variable that is
 * already set in the environment wins over the file. The file is looked for
 * beside the executable, then in the current directory. Same semantics as
 * dod3-recomp's dod3.ini (MIT, see THIRD_PARTY_NOTICES.md).
 *
 * tod_settings_load() must run first in main(), before anything reads getenv.
 */
#pragma once

/* Reads tod.ini and sets each key not already in the environment. Returns the
 * number of keys applied. argv0 is the program path (may be NULL). */
int tod_settings_load(const char* argv0);

/* The file that was read, else where it would be (beside the executable). */
const char* tod_settings_path();
