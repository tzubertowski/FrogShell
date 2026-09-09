#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "devmode.h"
#include "process.h"
#include "terminal.h"

#define MAX_PATH_LEN 1024

static char ring[TERM_LINES][TERM_COLS];
static int  ring_head;   /* next write slot */
static int  ring_count;  /* lines currently stored */
static char hist[TERM_HIST][TERM_INPUT];
static int  hist_count;
static int  hist_browse;   /* -1 = live input, >=0 = browsing history */
static char input_buf[TERM_INPUT];
static char saved_input[TERM_INPUT];
static char cwd[MAX_PATH_LEN];
static char prompt_line[TERM_COLS + TERM_INPUT];
static int  scroll_off;    /* 0 = bottom (live view) */
static bool have_reported;
static bool terminal_exit_requested;   /* `exit` builtin wants to leave */

static void line_push(const char *s) {
    strncpy(ring[ring_head], s, TERM_COLS - 1);
    ring[ring_head][TERM_COLS - 1] = 0;
    ring_head = (ring_head + 1) % TERM_LINES;
    if (ring_count < TERM_LINES) ring_count++;
    scroll_off = 0;  /* new output returns to live view */
}

/* Raw process output -> scrollback lines. Strips ANSI escape sequences and
 * control characters so they cannot corrupt the renderer. */
static char partial[TERM_COLS];
static int  partial_len;
static int  ansi_state;   /* 0 = normal, 1 = after ESC, 2 = inside CSI [... */

static void partial_reset(void) { partial_len = 0; ansi_state = 0; }

static void chunk_append(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (ansi_state == 1) {           /* ESC seen: '[' starts CSI, other final bytes end here */
            ansi_state = (c == '[') ? 2 : 0;
            continue;
        }
        if (ansi_state == 2) {           /* CSI: closed by 0x40-0x7E */
            if (c >= 0x40 && c <= 0x7E) ansi_state = 0;
            continue;
        }
        if (c == 0x1B) { ansi_state = 1; continue; }
        if (c == '\n') {
            partial[partial_len] = 0;
            line_push(partial);
            partial_len = 0;
            continue;
        }
        if (c == '\r') { partial_len = 0; continue; }  /* ignore CR */
        if (c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7F) continue;
        if (partial_len < TERM_COLS - 1) partial[partial_len++] = (char)c;
        else {
            partial[partial_len] = 0;
            line_push(partial);
            partial_len = 0;
            partial[partial_len++] = (char)c;   /* keep the wrapping char */
        }
    }
}

void terminal_output(const char *buf, int len) {
    chunk_append(buf, len);
}

static void report_exit(int code, int sig) {
    char line[80];
    if (sig > 0 && sig < 32)     snprintf(line, sizeof line, "[terminated SIG%d]", sig);
    else if (code == 0)          snprintf(line, sizeof line, "[exit 0]");
    else                         snprintf(line, sizeof line, "[exit %d]", code);
    line_push(line);
}

static void hist_push(const char *cmd) {
    if (!cmd[0]) return;
    if (hist_count && !strcmp(hist[hist_count - 1], cmd)) return;
    if (hist_count < TERM_HIST) strcpy(hist[hist_count++], cmd);
    else { memmove(hist, hist + 1, (TERM_HIST - 1) * sizeof hist[0]); strcpy(hist[TERM_HIST - 1], cmd); }
}

static void builtin_cd(const char *arg) {
    char target[MAX_PATH_LEN];
    while (*arg == ' ' || *arg == '\t') arg++;
    if (!arg[0]) strcpy(target, "/mnt/sdcard");
    else if (arg[0] == '/') snprintf(target, sizeof target, "%s", arg);
    else if (!strcmp(arg, "..")) {
        snprintf(target, sizeof target, "%s", cwd);
        char *s = strrchr(target, '/');
        if (s && s != target) *s = 0; else strcpy(target, "/");
    } else snprintf(target, sizeof target, "%s/%s", cwd, arg);
    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char msg[TERM_COLS];
        snprintf(msg, sizeof msg, "cd: cannot access %s", arg);
        line_push(msg);
        return;
    }
    strcpy(cwd, target);
}

static bool try_builtins(const char *cmd) {
    char word[64], rest[TERM_INPUT];
    int consumed = 0;
    if (sscanf(cmd, "%63s%n", word, &consumed) != 1) return true; /* empty line */
    snprintf(rest, sizeof rest, "%s", cmd + consumed);
    /* Case-insensitive: the OSK defaults to lowercase in terminal sessions. */
    if (!strcasecmp(word, "clear")) { ring_count = 0; ring_head = 0; scroll_off = 0; partial_reset(); return true; }
    if (!strcasecmp(word, "exit"))  {
        /* Interrupt a running process; ask the caller to leave the terminal. */
        if (process_is_running()) process_interrupt();
        terminal_exit_requested = 1;
        return true;
    }
    if (!strcasecmp(word, "cd"))   { builtin_cd(rest); return true; }
    return false;
}

void terminal_submit(const char *cmd) {
    char echo[TERM_COLS];
    snprintf(echo, sizeof echo, "$ %s", cmd);
    line_push(echo);
    hist_push(cmd);
    hist_browse = -1;
    if (try_builtins(cmd)) return;
    /* Capability gate: never spawn anything unless Developer Mode is on. */
    if (!devmode_is_enabled()) { line_push("developer mode disabled"); return; }
    if (process_is_running())  { line_push("a process is already running"); return; }
    have_reported = false;
    if (!process_start_command(cmd, cwd)) { line_push("failed to start /bin/sh"); have_reported = true; }
}

void terminal_interrupt(void) {
    if (process_is_running()) process_interrupt();
}

void terminal_note_launched_path(const char *path) {
    char echo[TERM_COLS];
    have_reported = false;
    snprintf(echo, sizeof echo, "$ %s", path);
    line_push(echo);
}

void terminal_update(void) {
    process_poll(terminal_output);
    if (!have_reported && !process_is_running() && process_exit_code() >= 0) {
        int code = process_exit_code();
        report_exit(code, code > 128 ? code - 128 : 0);   /* 128+sig = signaled */
        have_reported = true;
    }
}

void terminal_init(void) {
    ring_count = ring_head = 0;
    hist_count = 0;
    hist_browse = -1;
    input_buf[0] = 0;
    saved_input[0] = 0;
    strcpy(cwd, "/mnt/sdcard");
    scroll_off = 0;
    have_reported = true;   /* nothing has started yet */
    terminal_exit_requested = 0;
    partial_reset();
    process_shutdown();
    line_push("FrogShell DEV terminal - /bin/sh (busybox)");
    line_push("built-ins: cd, clear, exit");
}

bool terminal_exit_pending(void) {
    if (terminal_exit_requested) { terminal_exit_requested = 0; return true; }
    return false;
}

void terminal_free(void) {
    process_shutdown();
}

void terminal_set_cwd(const char *dir) { if (dir && dir[0]) snprintf(cwd, sizeof cwd, "%s", dir); }
const char *terminal_get_cwd(void) { return cwd; }

int terminal_line_count(void) { return ring_count; }
const char *terminal_line(int index) {
    if (index < 0 || index >= ring_count) return "";
    int slot = (ring_head - ring_count + index + 2 * TERM_LINES) % TERM_LINES;
    return ring[slot];
}

const char *terminal_prompt_line(void) {
    snprintf(prompt_line, sizeof prompt_line, "$ %s", input_buf);
    return prompt_line;
}

const char *terminal_input_text(void) { return input_buf; }

void terminal_set_input(const char *s) {
    snprintf(input_buf, sizeof input_buf, "%s", s ? s : "");
    hist_browse = -1;
}

void terminal_history_move(int delta) {
    if (!hist_count) return;
    if (hist_browse < 0) { strcpy(saved_input, input_buf); hist_browse = hist_count - 1; }
    else hist_browse += delta;
    if (hist_browse < 0) { hist_browse = -1; strcpy(input_buf, saved_input); return; }
    if (hist_browse >= hist_count) { hist_browse = hist_count - 1; return; } /* stay at oldest */
    strcpy(input_buf, hist[hist_browse]);
}

int terminal_scroll_get(void) { return scroll_off; }
void terminal_scroll_set(int lines) {
    if (lines < 0) lines = 0;
    if (lines > ring_count - 1) lines = ring_count > 0 ? ring_count - 1 : 0;
    scroll_off = lines;
}

void terminal_kbd_char(char c) {
    size_t n = strlen(input_buf);
    if (c && n + 1 < sizeof input_buf) { input_buf[n] = c; input_buf[n + 1] = 0; }
}

void terminal_kbd_backspace(void) {
    size_t n = strlen(input_buf);
    if (n) input_buf[n - 1] = 0;
}

void terminal_kbd_submit(void) {
    terminal_submit(input_buf);
    input_buf[0] = 0;
    hist_browse = -1;
}

bool terminal_running(void) { return process_is_running(); }
