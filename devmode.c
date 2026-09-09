#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devmode.h"

static int devmode_latched;
static int chord_held_now;
static int chord_event;  /* +1 = enabled this frame, -1 = disabled, 0 = none */

static int flag_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int devmode_is_enabled(void) {
    /* Latched state only: flags are re-read at init and on chord toggles, so
     * this stays a pure RAM check in the per-frame hot path. */
    return devmode_latched;
}

/* Latch the enabled state once (called from retro_init). */
void devmode_refresh(void) {
    if (flag_exists(DEVMODE_PERSISTENT_FLAG) || flag_exists(DEVMODE_SESSION_FLAG))
        devmode_latched = 1;
}

/* Hidden chord L1+R1+X+Y, held ~2s, TOGGLES Developer Mode. ON creates the
 * session flag. OFF only works for chord/session-enabled sessions: the
 * persistent SD flag always wins, so it cannot be toggled off from the pad. */
int devmode_chord_update(uint32_t keys, int64_t now) {
    static int64_t held_since;
    static int was_held;
    static int fired;
    const uint32_t chord = (1u << DEVMODE_CHORD_L1) | (1u << DEVMODE_CHORD_R1) |
                           (1u << DEVMODE_CHORD_X) | (1u << DEVMODE_CHORD_Y);
    int held = (keys & chord) == chord;
    chord_event = 0;
    if (held && !was_held) { held_since = now; fired = 0; }
    if (held && !fired && now - held_since >= DEVMODE_CHORD_HOLD_MS) {
        fired = 1;
        /* Re-read flags: a persistent flag dropped on the SD after boot must
         * win immediately (is_enabled is a pure latch for the hot path). */
        devmode_refresh();
        if (devmode_latched && !flag_exists(DEVMODE_PERSISTENT_FLAG)) {
            devmode_latched = 0;
            unlink(DEVMODE_SESSION_FLAG);
            chord_event = -1;
        } else if (!devmode_latched) {
            devmode_latched = 1;
            FILE *f = fopen(DEVMODE_SESSION_FLAG, "w");
            if (f) { fputs("treefrog developer session\n", f); fclose(f); }
            chord_event = 1;
        }
        /* latched ON + persistent flag: already on, nothing to do */
    }
    chord_held_now = held;
    was_held = held;
    return chord_event;
}

int devmode_chord_active(void) { return chord_held_now; }
