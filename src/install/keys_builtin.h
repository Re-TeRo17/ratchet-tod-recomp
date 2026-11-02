/*
 * The key set that opens the disc's EBOOT.BIN, built in (as dod3-recomp's
 * src/install/keys_builtin.h does) and kept apart so it can be taken out.
 * The disc's EBOOT.BIN is an application SELF, key revision 0x0001; the keys
 * are the public application keys (RPCS3's KEY_APP) -- the same ones
 * tools/self2elf.py uses. The ELF that comes out is checked by its SHA-256,
 * so a wrong digit here fails the install, never the game.
 */
#pragma once

static const struct {
    unsigned key_revision;
    const char* erk;   /* 32 bytes, hex */
    const char* riv;   /* 16 bytes, hex */
} k_builtin_app_keys[] = {
    { 0x0001, "79481839C406A632BDB4AC093D73D99AE1587F24CE7E69192C1CD0010274A8AB", "6F0F25E1C8C4B7AE70DF968B04521DDA" },
};
