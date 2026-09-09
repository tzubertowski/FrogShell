#ifndef FROGSHELL_DEVMODE_H
#define FROGSHELL_DEVMODE_H

#include <stdint.h>

/* Developer Mode gate for FrogShell.
 *
 * Enabled by either a persistent flag on the SD card or a session-only flag
 * in /tmp created by a hidden button chord. Every capability that can run
 * commands or spawn processes must check devmode_is_enabled() first. */

#define DEVMODE_PERSISTENT_FLAG "/mnt/sdcard/frogui/developer.flag"
#define DEVMODE_SESSION_FLAG    "/tmp/treefrog_developer.flag"

/* Button bits (logical FrogShell BTN_* indices) for the hidden chord. */
#define DEVMODE_CHORD_L1 6
#define DEVMODE_CHORD_R1 7
#define DEVMODE_CHORD_X  8
#define DEVMODE_CHORD_Y  9

/* Hold time in milliseconds for the chord to latch Developer Mode. */
#define DEVMODE_CHORD_HOLD_MS 2000

int  devmode_is_enabled(void);
void devmode_refresh(void);
int  devmode_chord_update(uint32_t keys, int64_t now); /* +1 enabled, -1 disabled, 0 none */
int  devmode_chord_active(void);

#endif
