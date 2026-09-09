#ifndef FROGSHELL_USBKBD_H
#define FROGSHELL_USBKBD_H

#include <stdbool.h>

/* Physical USB keyboard support via OTG (/dev/input/event*, raw evdev).
 *
 * The kernel already exposes USB HID keyboards as evdev nodes; opening them
 * needs no extra daemons. One keyboard is tracked; if several are plugged,
 * events from any of them are accepted. */

void usbkbd_init(void);
void usbkbd_close(void);

/* Poll pending key events. Returns the next printable character, 0 = none.
 * Special keys are delivered through the out-params: enter, backspace,
 * arrows, pgup/pgdn (scrollback) and ctrl_c (interrupt). */
char usbkbd_poll(int *enter, int *backspace, int *up, int *down,
                 int *left, int *right, int *pgup, int *pgdn,
                 int *ctrl_c);

bool usbkbd_connected(void);

#endif
