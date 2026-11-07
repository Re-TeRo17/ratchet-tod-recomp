/*
 * The release's version and the update check (1.1).
 *
 * At start a background thread asks GitHub for the latest release of the port
 * (https://api.github.com/repos/Re-TeRo17/ratchet-tod-recomp/releases/latest, one
 * HTTPS GET, nothing about the player is sent; a few seconds' timeout). When its tag
 * is newer than TOD_RELEASE_VERSION, the F1 page and a short notice on screen say so.
 * Nothing is downloaded or installed. TOD_UPDATE_CHECK = 0 (tod.ini) turns it off.
 */
#pragma once

#define TOD_RELEASE_VERSION "1.2.0"
#define TOD_RELEASES_URL "https://github.com/Re-TeRo17/ratchet-tod-recomp/releases"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the check (once; does nothing with TOD_UPDATE_CHECK=0). */
void tod_update_check_start(void);

/* 0 not done yet / off, 1 up to date, 2 a newer release exists, -1 the check failed. */
int tod_update_state(void);

/* The newer release's tag ("1.02") when tod_update_state() == 2, else "". */
const char* tod_update_latest(void);

/* Opens the releases page in the browser. */
void tod_update_open_page(void);

#ifdef __cplusplus
}
#endif
