#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include "usbkbd.h"

#define USBKBD_MAX_NODES 8
#define USBKBD_KEYBUF    32

static int fds[USBKBD_MAX_NODES];
static int fd_count;
static int connected;

/* US QWERTY scancode -> ASCII, no modifier */
static const char map_plain[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,
};
/* With SHIFT */
static const char map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,
};

/* Buffered decoded events: one slot per press. */
static char pending_chars[USBKBD_KEYBUF];
static int  pending_head, pending_tail;
static int  pending_flags[USBKBD_KEYBUF];

static int mod_shift, mod_ctrl, mod_alt;

static void push_event(int flag, char ch) {
    int next = (pending_head + 1) % USBKBD_KEYBUF;
    if (next == pending_tail) return;                 /* full: drop */
    pending_flags[pending_head] = flag;
    pending_chars[pending_head] = ch;
    pending_head = next;
}

/* Is this evdev node a keyboard? Check the key bitmap for KEY_Q/KEY_SPACE. */
static int node_is_keyboard(int fd) {
    unsigned char bits[KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    int q = KEY_Q / 8, s = KEY_SPACE / 8;
    return (bits[q] & (1u << (KEY_Q % 8))) && (bits[s] & (1u << (KEY_SPACE % 8)));
}

void usbkbd_init(void) {
    char path[32];
    for (int i = 0; i < USBKBD_MAX_NODES; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (node_is_keyboard(fd)) {
            fds[fd_count++] = fd;
            connected = 1;
        } else {
            close(fd);
        }
    }
}

void usbkbd_close(void) {
    for (int i = 0; i < fd_count; i++) close(fds[i]);
    fd_count = 0;
    connected = 0;
}

bool usbkbd_connected(void) { return connected; }

char usbkbd_poll(int *enter, int *backspace, int *up, int *down,
                 int *left, int *right, int *pgup, int *pgdn,
                 int *ctrl_c) {
    /* Always clear the flags first: an empty queue must report "no event",
     * never let the caller re-read stale stack values from last time. */
    *enter = 0; *backspace = 0; *up = 0; *down = 0;
    *left = 0; *right = 0; *pgup = 0; *pgdn = 0; *ctrl_c = 0;
    struct input_event ev;
    for (int i = 0; i < fd_count; i++) {
        while (read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != EV_KEY) continue;
            int code = (int)ev.code;
            int value = (int)ev.value;
            if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { mod_shift = value; continue; }
            if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)  { mod_ctrl = value;  continue; }
            if (code == KEY_LEFTALT || code == KEY_RIGHTALT)     { mod_alt = value;   continue; }
            if (value == 0) continue;                    /* key release */
            if (value == 2) continue;                    /* autorepeat: ignore */
            if (code == KEY_ENTER || code == KEY_KPENTER) { push_event(1, '\n'); continue; }
            if (code == KEY_BACKSPACE)  { push_event(2, 0);  continue; }
            if (code == KEY_UP)         { push_event(3, 0);  continue; }
            if (code == KEY_DOWN)       { push_event(4, 0);  continue; }
            if (code == KEY_LEFT)       { push_event(5, 0);  continue; }
            if (code == KEY_RIGHT)      { push_event(6, 0);  continue; }
            if (code == KEY_PAGEUP)     { push_event(7, 0);  continue; }
            if (code == KEY_PAGEDOWN)   { push_event(8, 0);  continue; }
            if (code == KEY_C && mod_ctrl) { push_event(9, 0); continue; }
            if (mod_ctrl || mod_alt) continue;          /* other combos: skip */
            if (code >= 128) continue;
            char ch = mod_shift ? map_shift[code] : map_plain[code];
            if (ch && ch >= 32) push_event(0, ch);   /* drop ESC/TAB and other control codes */
        }
    }
    if (pending_tail == pending_head) return 0;         /* nothing pending */
    int flag = pending_flags[pending_tail];
    char ch = pending_chars[pending_tail];
    pending_tail = (pending_tail + 1) % USBKBD_KEYBUF;
    *enter = flag == 1; *backspace = flag == 2; *up = flag == 3; *down = flag == 4;
    *left = flag == 5; *right = flag == 6; *pgup = flag == 7; *pgdn = flag == 8;
    *ctrl_c = flag == 9;
    return flag == 0 ? ch : 0;
}
