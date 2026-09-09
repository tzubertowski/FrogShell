#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"

#include "devmode.h"
#include "process.h"
#include "terminal.h"
#include "usbkbd.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

extern unsigned char fontdata8x8[64 * 16];

#define ROOT "/mnt/sdcard"
#define DEVICE_FILE "/tmp/tfdevice.env"
#define THEME_FILE "/mnt/sdcard/cubegm/skin/skin.txt"
#define KEYMAP_FILE "/mnt/sdcard/frogui/keymap.txt"
#define MAX_PATH 1024
#define MAX_ENTRIES 512
#define MAX_MARKED 128

enum { BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_A, BTN_B, BTN_L1, BTN_R1,
       BTN_X, BTN_Y, BTN_SELECT, BTN_START, BTN_COUNT };
static int key_bits[BTN_COUNT] = { 7, 5, 2, 3, 13, 14, 10, 11, 12, 15, 0, 1 };
static const char *key_names[BTN_COUNT] = {
    "LEFT", "RIGHT", "UP", "DOWN", "A", "B", "L1", "R1", "X", "Y", "SELECT", "START"
};

typedef struct { int w, h; uint32_t *canvas; uint16_t *output; } Screen;
typedef struct { char name[256]; int dir; off_t size; time_t modified; } Entry;
typedef struct { uint32_t text, accent, selected; } Theme;
typedef enum { MODE_NORMAL, MODE_ACTIONS, MODE_CONFIRM, MODE_CONFLICT, MODE_REWRITE, MODE_KEYBOARD, MODE_INFO, MODE_TERMINAL } Mode;
typedef enum { OP_COPY, OP_CUT } Op;

static volatile sig_atomic_t quit_requested;
static volatile uint32_t *raw_keys;
static uint32_t previous_keys;
static Screen screen;
static Theme theme = { 0xF4F4F4, 0x4A90D9, 0x101010 };
static stbtt_fontinfo font_info;
static unsigned char *font_buffer;
static float font_scale;
static int font_loaded;
/* Keep rasterized glyphs resident, just like the FrogUI/picoarch font path.
 * Reallocating a bitmap for every character on every frame caused both
 * flicker and missing/partially drawn glyphs on the target. */
struct glyph_cache { int valid, w, h, xoff, yoff; unsigned char *bmp; };
static struct glyph_cache glyphs[2][128];
static float glyph_scale[2] = { -1.0f, -1.0f };
static int glyph_baseline[2];
static Entry entries[MAX_ENTRIES];
static int entry_count, selected, scroll;
static char current[MAX_PATH] = ROOT;
static char marked[MAX_MARKED][MAX_PATH];
static int marked_count;
static char clipboard[MAX_PATH];
static char clipboard_paths[MAX_MARKED][MAX_PATH];
static int clipboard_count;
static Op clipboard_op;
static Mode mode;
static int menu_item, confirm_kind;
static int conflict_index, conflict_choice;
static char prompt[MAX_PATH], prompt_original[MAX_PATH];
static int keyboard_row, keyboard_col;
static int keyboard_symbols;   /* 0 = letters page, 1 = symbols page */
static int keyboard_for_terminal; /* keyboard session belongs to the terminal */
static int keyboard_shift;     /* caps state, unified for FM and terminal */
static int keyboard_ctrl;      /* sticky CTRL for the terminal virtual keyboard */
static int keyboard_alt;       /* sticky ALT for the terminal virtual keyboard */
static char status_text[160];
static int status_frames;
static char info_text[256];

/* Frame-skip state: when nothing visual changed, retro_run re-presents the
 * last converted buffer instead of redrawing the whole canvas. */
static int frame_dirty;   /* any input/process/terminal activity this frame */

/* SIGINT/SIGTERM handler kept for the process runner: when FrogShell is
 * killed externally the child process group is cleaned up in retro_deinit. */
static void die_signal(int sig) { (void)sig; quit_requested = 1; }
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static uint32_t parse_rgb(const char *s, uint32_t fallback) {
    char *end = NULL; unsigned long v = strtoul(s, &end, 0);
    return end != s ? (uint32_t)v & 0xFFFFFFu : fallback;
}

static void load_theme(void) {
    FILE *f = fopen(THEME_FILE, "r"); char line[128], k[64], v[64];
    if (!f) return;
    while (fgets(line, sizeof line, f) && sscanf(line, "%63[^=]=%63s", k, v) == 2) {
        if (!strcmp(k, "text_color")) theme.text = parse_rgb(v, theme.text);
        else if (!strcmp(k, "selection_color")) theme.accent = parse_rgb(v, theme.accent);
        else if (!strcmp(k, "sel_text_color")) theme.selected = parse_rgb(v, theme.selected);
    }
    fclose(f);
}

static int load_font_file(const char *name) {
    char path[MAX_PATH];
    const char *dirs[] = { "/mnt/sdcard/cubegm/fonts", "/mnt/sdcard/frogui/fonts" };
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0] && !f; i++) {
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        f = fopen(path, "rb");
    }
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return 0; }
    unsigned char *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); fclose(f); return 0; }
    fclose(f);
    if (!stbtt_InitFont(&font_info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) { free(buf); return 0; }
    free(font_buffer); font_buffer = buf; font_loaded = 1; font_scale = stbtt_ScaleForPixelHeight(&font_info, 18.0f); return 1;
}

static void load_selected_font(void) {
    char selected[128] = "";
    FILE *f = fopen("/mnt/sdcard/frogui/settings.txt", "r");
    char line[256], key[80], value[160];
    if (f) {
        while (fgets(line, sizeof line, f) && sscanf(line, "%79[^=]=%159s", key, value) == 2)
            if (!strcmp(key, "font")) { strncpy(selected, value, sizeof selected - 1); break; }
        fclose(f);
    }
    if ((!selected[0] || !load_font_file(selected)) && !load_font_file("GamePocket-Regular-ZeroKern.ttf"))
        load_font_file("monogram.ttf");
}

static void load_keymap(void) {
    FILE *f = fopen(KEYMAP_FILE, "r"); char line[96], name[32]; int bit;
    if (!f) return;
    while (fgets(line, sizeof line, f) && sscanf(line, "%31[^=]=%d", name, &bit) == 2)
        for (int i = 0; i < BTN_COUNT; i++) if (!strcmp(name, key_names[i]) && bit >= 0 && bit < 32) key_bits[i] = bit;
    fclose(f);
}

static volatile uint32_t *open_keys(void) {
    key_t k = ftok("/tmp/joy_key", 'a'); if (k == (key_t)-1) return NULL;
    int id = shmget(k, 4, 0666); if (id < 0) return NULL;
    void *p = shmat(id, NULL, 0); return p == (void *)-1 ? NULL : (volatile uint32_t *)p;
}

static uint32_t keys_now(void) { uint32_t out = 0, raw = raw_keys ? (*raw_keys & 0xFFFFu) : 0; for (int i = 0; i < BTN_COUNT; i++) if (raw & (1u << key_bits[i])) out |= 1u << i; return out; }
static bool pressed(uint32_t keys, int b) { return (keys & (1u << b)) && !(previous_keys & (1u << b)); }
static void set_status(const char *s) { strncpy(status_text, s, sizeof status_text - 1); status_text[sizeof status_text - 1] = 0; status_frames = 150; }
static bool path_contains(const char *parent, const char *child) { size_t n = strlen(parent); return !strcmp(parent, child) || (!strncmp(parent, child, n) && child[n] == '/'); }

static int read_geometry(int *w, int *h) {
    const char *ew = getenv("TF_PANEL_W"), *eh = getenv("TF_PANEL_H");
    *w = ew ? atoi(ew) : 640; *h = eh ? atoi(eh) : 480;
    FILE *f = fopen(DEVICE_FILE, "r"); char l[128], k[64], v[64];
    if (!f) return 0;
    while (fgets(l, sizeof l, f) && sscanf(l, "%63[^=]=%63s", k, v) == 2) { if (!ew && !strcmp(k, "TF_PANEL_W")) *w = atoi(v); else if (!eh && !strcmp(k, "TF_PANEL_H")) *h = atoi(v); }
    fclose(f); if (*w < 320 || *w > 1920) *w = 640; if (*h < 240 || *h > 1080) *h = 480; return 0;
}

static int screen_open(void) {
    memset(&screen, 0, sizeof screen); read_geometry(&screen.w, &screen.h);
    screen.canvas = calloc((size_t)screen.w * screen.h, sizeof(*screen.canvas));
    screen.output = calloc((size_t)screen.w * screen.h, sizeof(*screen.output));
    return screen.canvas && screen.output ? 0 : -1;
}

static void screen_close(void) { free(screen.canvas); free(screen.output); memset(&screen, 0, sizeof screen); }
/* Both supported 640x480 and 854x480 panels are 480p displays. Width alone is
 * not pixel density: treating 854 as a 2x canvas made SF3000 rows and glyphs
 * twice the R36HD size. Only scale up on genuinely taller output modes. */
static int ui_scale(void) { return screen.h >= 720 ? 2 : 1; }
static void clear(uint32_t c) { for (int y = 0; y < screen.h; y++) for (int x = 0; x < screen.w; x++) screen.canvas[(size_t)y * screen.w + x] = c; }
static void rect(int x, int y, int w, int h, uint32_t c) { if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; } if (x + w > screen.w) w = screen.w - x; if (y + h > screen.h) h = screen.h - y; if (w <= 0 || h <= 0) return; for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) screen.canvas[(size_t)yy * screen.w + xx] = c; }
static void glyph_cache_reset(int slot) {
    for (int i = 0; i < 128; i++) { free(glyphs[slot][i].bmp); glyphs[slot][i].bmp = NULL; glyphs[slot][i].valid = 0; }
}

static void text(int x, int y, const char *s, int scale, uint32_t c, int max) {
    int start = x;
    if (font_loaded) {
        int slot = scale > 1 ? 1 : 0;
        float sc = font_scale * (slot + 1);
        if (glyph_scale[slot] != sc) {
            glyph_cache_reset(slot); glyph_scale[slot] = sc;
            int ascent, descent, gap; stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &gap);
            glyph_baseline[slot] = (int)(ascent * sc);
        }
        int baseline = y + glyph_baseline[slot];
        for (; *s && x < start + max; s++) {
            unsigned char ch = (unsigned char)*s; if (ch >= 128) ch = '?';
            struct glyph_cache *g = &glyphs[slot][ch];
            if (!g->valid) {
                int gi = stbtt_FindGlyphIndex(&font_info, ch), x0, y0, x1, y1;
                if (!gi) { g->valid = 1; continue; }
                stbtt_GetGlyphBitmapBox(&font_info, gi, sc, sc, &x0, &y0, &x1, &y1);
                g->w = x1 - x0; g->h = y1 - y0; g->xoff = x0; g->yoff = y0;
                if (g->w > 0 && g->h > 0 && g->w <= 96 && g->h <= 96) {
                    g->bmp = malloc((size_t)g->w * g->h);
                    if (g->bmp) stbtt_MakeGlyphBitmap(&font_info, g->bmp, g->w, g->h, g->w, sc, sc, gi);
                }
                g->valid = 1;
            }
            if (g->bmp) for (int yy = 0; yy < g->h; yy++) for (int xx = 0; xx < g->w; xx++)
                if (g->bmp[yy * g->w + xx] > 60) rect(x + g->xoff + xx, baseline + g->yoff + yy, 1, 1, c);
            int ax, lsb; stbtt_GetCodepointHMetrics(&font_info, ch, &ax, &lsb); x += (int)(ax * sc) + scale;
        }
        return;
    }
    for (; *s && x + 8 * scale <= start + max; s++, x += 8 * scale) { unsigned char ch = (unsigned char)*s; if (ch >= 128) ch = '?'; for (int r = 0; r < 8; r++) for (int col = 0; col < 8; col++) if (fontdata8x8[ch * 8 + r] & (0x80u >> col)) rect(x + col * scale, y + r * scale, scale, scale, c); }
}

/* Width a string will occupy with the same metrics text() uses (for cursors). */
static int text_width(const char *s, int scale) {
    if (!font_loaded) return (int)strlen(s) * 8 * scale;
    int w = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s; if (ch >= 128) ch = '?';
        int ax, lsb; stbtt_GetCodepointHMetrics(&font_info, ch, &ax, &lsb);
        w += (int)((float)ax * font_scale * (scale > 1 ? 2 : 1)) + scale;
    }
    return w;
}

static retro_video_refresh_t video_cb;
static void present(void) {
    size_t count = (size_t)screen.w * screen.h;
    for (size_t i = 0; i < count; i++) {
        uint32_t c = screen.canvas[i];
        screen.output[i] = (uint16_t)(((c >> 8) & 0xF800u) |
                                      ((c >> 5) & 0x07E0u) |
                                      ((c >> 3) & 0x001Fu));
    }
    if (video_cb)
        video_cb(screen.output, (unsigned)screen.w, (unsigned)screen.h,
                 (size_t)screen.w * sizeof(*screen.output));
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static bool under_root(const char *p) { return !strncmp(p, ROOT, strlen(ROOT)) && (p[strlen(ROOT)] == 0 || p[strlen(ROOT)] == '/'); }
static void join_path(char *out, size_t n, const char *a, const char *b) { if (!strcmp(a, ROOT)) snprintf(out, n, "%s/%s", a, b); else snprintf(out, n, "%s/%s", a, b); }
static int entry_cmp(const void *a, const void *b) { const Entry *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name); }

static void scan(void) {
    entry_count = 0; DIR *d = opendir(current); if (!d) { set_status("Cannot open folder"); return; }
    if (strcmp(current, ROOT) != 0) { strcpy(entries[entry_count].name, ".. "); entries[entry_count].dir = 1; entry_count++; }
    struct dirent *e;
    while ((e = readdir(d)) && entry_count < MAX_ENTRIES) {
        if (e->d_name[0] == '.') continue;
        struct stat st; char p[MAX_PATH]; join_path(p, sizeof p, current, e->d_name); if (stat(p, &st) != 0) continue;
        strncpy(entries[entry_count].name, e->d_name, sizeof entries[entry_count].name - 1); entries[entry_count].name[sizeof entries[entry_count].name - 1] = 0;
        entries[entry_count].dir = S_ISDIR(st.st_mode); entries[entry_count].size = st.st_size; entries[entry_count].modified = st.st_mtime; entry_count++;
    }
    closedir(d); if (entry_count > 1) qsort(entries + (strcmp(current, ROOT) != 0), entry_count - (strcmp(current, ROOT) != 0), sizeof *entries, entry_cmp);
    if (selected >= entry_count) selected = entry_count ? entry_count - 1 : 0;
    if (selected < 0) selected = 0;
    scroll = selected >= 10 ? selected - 9 : 0;
    frame_dirty = 1;   /* listing changed: redraw even if selection did not */
}

static bool marked_path(const char *p) { for (int i = 0; i < marked_count; i++) if (!strcmp(marked[i], p)) return true; return false; }
static void toggle_mark(void) { if (selected >= entry_count || !strcmp(entries[selected].name, ".. ")) return; char p[MAX_PATH]; join_path(p, sizeof p, current, entries[selected].name); for (int i = 0; i < marked_count; i++) if (!strcmp(marked[i], p)) { memmove(marked[i], marked[i + 1], (size_t)(marked_count - i - 1) * sizeof marked[0]); marked_count--; set_status("Unmarked"); return; } if (marked_count < MAX_MARKED) { strcpy(marked[marked_count++], p); set_status("Marked"); } }
static int selected_paths(char out[][MAX_PATH], int cap) { int n = 0; if (marked_count) { for (int i = 0; i < marked_count && n < cap; i++) strcpy(out[n++], marked[i]); } else if (selected < entry_count && strcmp(entries[selected].name, ".. ")) { join_path(out[0], MAX_PATH, current, entries[selected].name); n = 1; } return n; }

static int copy_tree(const char *src, const char *dst) {
    struct stat st; if (lstat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) { if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) return -1; DIR *d = opendir(src); if (!d) return -1; struct dirent *e; int rc = 0; while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char a[MAX_PATH], b[MAX_PATH]; join_path(a, sizeof a, src, e->d_name); join_path(b, sizeof b, dst, e->d_name); if (copy_tree(a, b) != 0) { rc = -1; break; } } closedir(d); return rc; }
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777); if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); return -1; }
    char buf[32768]; ssize_t got; int rc = 0; while ((got = read(in, buf, sizeof buf)) > 0) { char *p = buf; while (got) { ssize_t wr = write(out, p, (size_t)got); if (wr <= 0) { rc = -1; break; } p += wr; got -= wr; } if (rc) break; } close(in); close(out); return rc;
}

static int remove_tree(const char *p) { struct stat st; if (lstat(p, &st) != 0) return -1; if (S_ISDIR(st.st_mode)) { DIR *d = opendir(p); if (!d) return -1; struct dirent *e; int rc = 0; while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[MAX_PATH]; join_path(c, sizeof c, p, e->d_name); if (remove_tree(c) != 0) rc = -1; } closedir(d); if (rmdir(p) != 0) rc = -1; return rc; } return unlink(p); }
static unsigned long long tree_size(const char *p) { struct stat st; if (lstat(p, &st) != 0) return 0; if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size; unsigned long long total = 0; DIR *d = opendir(p); if (!d) return 0; struct dirent *e; while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[MAX_PATH]; join_path(c, sizeof c, p, e->d_name); total += tree_size(c); } closedir(d); return total; }
static void size_label(unsigned long long bytes, char *out, size_t n) { if (bytes >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024)); else if (bytes >= 1024ULL * 1024) snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024)); else if (bytes >= 1024) snprintf(out, n, "%.1f KB", (double)bytes / 1024.0); else snprintf(out, n, "%llu B", bytes); }

static void unique_copy_name(const char *dst, char *out, size_t n) {
    if (access(dst, F_OK) != 0) { strncpy(out, dst, n - 1); out[n - 1] = 0; return; }
    const char *slash = strrchr(dst, '/');
    const char *name = slash ? slash + 1 : dst;
    size_t prefix = slash ? (size_t)(slash - dst + 1) : 0;
    const char *dot = strrchr(name, '.');
    size_t stem_len = (dot && dot != name) ? (size_t)(dot - name) : strlen(name);
    char stem[256], ext[256];
    if (stem_len >= sizeof stem) stem_len = sizeof stem - 1;
    memcpy(stem, name, stem_len); stem[stem_len] = 0;
    snprintf(ext, sizeof ext, "%s", (dot && dot != name) ? dot : "");
    for (int i = 1; i < 1000; i++) {
        snprintf(out, n, "%.*s%s (%d)%s", (int)prefix, dst, stem, i, ext);
        if (access(out, F_OK) != 0) return;
    }
    strncpy(out, dst, n - 1); out[n - 1] = 0;
}

/* policy: -1 ask, 0 skip, 1 overwrite, 2 keep both (auto-rename). */
static void paste_items(int start, int policy) {
    int rc = 0;
    for (int i = start; i < clipboard_count; i++) {
        char src[MAX_PATH], dst[MAX_PATH], final_dst[MAX_PATH];
        strcpy(src, clipboard_paths[i]);
        join_path(dst, sizeof dst, current, base(src));
        if (!under_root(dst) || !strcmp(src, dst)) { set_status("Already in this folder"); continue; }
        if (path_contains(src, dst)) { set_status("Cannot paste into itself"); rc = -1; continue; }
        if (access(dst, F_OK) == 0) {
            if (policy < 0) { conflict_index = i; mode = MODE_CONFLICT; return; }
            if (policy == 0) continue;
            if (policy == 2) unique_copy_name(dst, final_dst, sizeof final_dst);
            else { strcpy(final_dst, dst); if (remove_tree(final_dst) != 0) { rc = -1; continue; } }
        } else strcpy(final_dst, dst);
        if ((clipboard_op == OP_CUT ? rename(src, final_dst) : copy_tree(src, final_dst)) != 0) rc = -1;
    }
    if (rc == 0 && clipboard_op == OP_CUT) { clipboard[0] = 0; clipboard_count = 0; }
    set_status(rc == 0 ? "Paste complete" : "Some items failed"); scan(); mode = MODE_NORMAL;
}

static void clear_marks(void) { marked_count = 0; }
static const char *action_names[] = { "Copy", "Cut", "Paste", "Rename", "Delete", "New folder", "Info", "Cancel" };
static const int action_count = 8;
/* Developer Mode action labels inserted before Cancel when DEV is enabled. */
enum { DEV_ACTION_RUN = 0, DEV_ACTION_TERM_HERE, DEV_ACTION_TERMINAL, DEV_ACTION_COUNT };
static const char *dev_action_names[DEV_ACTION_COUNT] = { "Run", "Open terminal here", "Terminal" };
static int menu_action_count(void) { return action_count + (devmode_is_enabled() ? DEV_ACTION_COUNT : 0); }
static const char *menu_action_name(int i) {
    int base = action_count - 1; /* Cancel's index: DEV entries insert before it */
    int dev = devmode_is_enabled() ? DEV_ACTION_COUNT : 0;
    if (dev && i >= base && i < base + dev) return dev_action_names[i - base];
    if (dev) { if (i < base) return action_names[i]; return action_names[action_count - 1]; } /* Cancel last */
    return action_names[i]; /* no DEV: original 8-entry menu untouched */
}
static void begin_keyboard(const char *initial, const char *old) { strncpy(prompt, initial ? initial : "", sizeof prompt - 1); prompt[sizeof prompt - 1] = 0; strncpy(prompt_original, old ? old : "", sizeof prompt_original - 1); prompt_original[sizeof prompt_original - 1] = 0; keyboard_row = 1; keyboard_col = 0; keyboard_symbols = 0; keyboard_ctrl = 0; keyboard_alt = 0; keyboard_shift = 0; mode = MODE_KEYBOARD; }
static const char *kbd_rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM_-" };
/* Symbols page: 4 rows. Rows 2/3 keep their fixed key at col 9, so their text
 * covers cols 0-8 (9 chars). Rows 0/1 are full 10 chars. */
static const char *kbd_sym_rows[] = { "/.,;:!\"'`-", "()[]{}<>|&", "*=~+@#%^$", "?\\|~.-_+=" };
/* Unified 10-column layout for FM and terminal. Last cell of row 2 is the
 * shift/caps key (up-arrow glyph), last cell of row 3 is SYM; both stay in
 * place on the symbols page. Bottom row: CTRL ALT SPACE DEL ENTER. */
#define KBD_COLS 10
enum { KBD_ACT_SPACE = 0, KBD_ACT_DEL, KBD_ACT_ENTER, KBD_ACT_SYM, KBD_ACT_SHIFT, KBD_ACT_CTRL, KBD_ACT_ALT };
static int kbd_page_rows(void) { return 4; }   /* 3 letter/symbol rows + 1 extra row 3 on letters */
/* Row text for the current page (letters or symbols): 4 rows each. */
static const char *kbd_row_text(int r) {
    return keyboard_symbols ? kbd_sym_rows[r & 3] : kbd_rows[r & 3];
}
/* Fixed-key cell (not a character): row 2 col 9 = caps, row 3 col 9 = SYM.
 * Rows 0-1 are all characters. Same on both pages. */
static int kbd_fixed_at(int r, int c) {
    if (c != 9 || r < 2) return 0;
    return r == 2 ? KBD_ACT_SHIFT : KBD_ACT_SYM;
}
/* Physical char for a grid cell, applying the shift (caps) state.
 * Letters toggle case; everything else is unaffected. */
static char kbd_char_at(int r, int c) {
    const char *src = kbd_row_text(r);
    char ch = src[c];
    if (ch >= 'A' && ch <= 'Z' && !keyboard_shift)
        ch = (char)(ch - 'A' + 'a');
    return ch;
}
/* Bottom modifier row: CTRL(2) ALT(2) SPACE(4) DEL(1) ENTER(1). */
static int kbd_fn_action_at(int col) {
    if (col <= 1) return KBD_ACT_CTRL;
    if (col <= 3) return KBD_ACT_ALT;
    if (col <= 6) return KBD_ACT_SPACE;
    if (col == 7) return KBD_ACT_DEL;
    return KBD_ACT_ENTER;
}

static void do_copy_or_cut(Op op) { char paths[MAX_MARKED][MAX_PATH]; int n = selected_paths(paths, MAX_MARKED); if (!n) { set_status("Nothing selected"); return; } clipboard_count = n; for (int i = 0; i < n; i++) strcpy(clipboard_paths[i], paths[i]); strcpy(clipboard, paths[0]); clipboard_op = op; set_status(op == OP_COPY ? "Copied to clipboard" : "Cut to clipboard"); }
static void do_paste(void) { if (!clipboard_count || !clipboard[0]) { set_status("Clipboard is empty"); return; } paste_items(0, -1); }
static void do_delete(void) { char paths[MAX_MARKED][MAX_PATH]; int n = selected_paths(paths, MAX_MARKED), rc = 0; for (int i = 0; i < n; i++) if (!under_root(paths[i]) || remove_tree(paths[i]) != 0) rc = -1; clear_marks(); set_status(rc ? "Delete failed" : "Deleted"); scan(); }
static void do_rename(const char *name) { char old[MAX_PATH], dst[MAX_PATH]; join_path(old, sizeof old, current, prompt_original); join_path(dst, sizeof dst, current, name); if (!name[0] || !under_root(dst) || rename(old, dst) != 0) set_status("Rename failed"); else set_status("Renamed"); scan(); }
static void do_new_folder(const char *name) { char dst[MAX_PATH]; join_path(dst, sizeof dst, current, name); if (!name[0] || !under_root(dst) || mkdir(dst, 0777) != 0) set_status("Create folder failed"); else set_status("Folder created"); scan(); }

/* On-screen keyboard, console-OSK style with a physical layout: letter grid
 * with a right rail (abc/ABC, SYM) and a bottom modifier row
 * (CTRL ALT SPACE DEL ENTER). Shared by the file manager and the terminal. */
static void draw_keyboard(int scale) {
    int rows = kbd_page_rows();
    /* Prompt panel */
    int panel_h = 40 * scale;
    int kb_x = 6 * scale, kb_w = screen.w - 12 * scale;
    int total_h = panel_h + 6 * scale + rows * (26 * scale + 3 * scale) + 26 * scale + 3 * scale + 14 * scale;
    int prompt_y = (screen.h - total_h) / 2;
    if (prompt_y < 4 * scale) prompt_y = 4 * scale;
    rect(kb_x, prompt_y, kb_w, panel_h, 0x303030);
    int carat = text_width(prompt, scale);
    text(kb_x + 10 * scale, prompt_y + 12 * scale, prompt, scale, theme.selected, kb_w - 40 * scale);
    rect(kb_x + 12 * scale + carat, prompt_y + 10 * scale, 8 * scale, 20 * scale, theme.accent); /* cursor */
    /* Key grid: 10 columns; fixed keys (caps arrow, SYM) integrated in their rows */
    int grid_y = prompt_y + panel_h + 6 * scale;
    int key_w = (kb_w - 8 * scale) / KBD_COLS;
    int key_h = 26 * scale;
    int gap = 3 * scale;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            int kx = kb_x + 4 * scale + c * key_w, ky = grid_y + r * (key_h + gap);
            bool active = r == keyboard_row;
            bool chosen = active && c == keyboard_col;
            int fixed = kbd_fixed_at(r, c);
            uint32_t box = chosen ? theme.accent : (active ? 0x404040 : 0x303030);
            if (fixed == KBD_ACT_SHIFT && keyboard_shift) box = chosen ? theme.accent : 0x606060;
            rect(kx, ky, key_w - 2 * scale, key_h, box);
            if (fixed == KBD_ACT_SHIFT) {
                /* Up-arrow caps glyph, drawn with rects (font-independent):
                 * filled when caps on, hollow when off. */
                uint32_t g = chosen ? theme.selected : theme.text;
                int cx = kx + (key_w - 2 * scale) / 2, cy = ky + key_h / 2;
                int aw = 9 * scale, ah = 12 * scale;
                for (int yy = 0; yy < ah; yy++) {
                    int half = aw * yy / ah;             /* triangle widens downward */
                    if (keyboard_shift) rect(cx - half / 2, cy - ah / 2 + yy, half > 0 ? half : 1, 1, g);
                    else if (yy % 3 == 0) rect(cx - half / 2, cy - ah / 2 + yy, half > 0 ? half : 1, 1, g);
                }
                /* stem */
                rect(cx - scale, cy + ah / 2 - 3 * scale, 2 * scale, 4 * scale, g);
                continue;
            }
            if (fixed == KBD_ACT_SYM) {
                text(kx + (key_w - 3 * 8 * scale) / 2 - scale, ky + (key_h - 14 * scale) / 2, "SYM", scale,
                     chosen ? theme.selected : theme.text, key_w);
                continue;
            }
            char glyph[2] = { kbd_char_at(r, c), '\0' };
            text(kx + (key_w - 8 * scale) / 2 - scale, ky + (key_h - 14 * scale) / 2, glyph, scale,
                 chosen ? theme.selected : theme.text, key_w);
        }
    }
    /* Bottom modifier row: CTRL(2) ALT(2) SPACE(4) DEL(1) ENTER(1) over 10 cols */
    int fn_y = grid_y + rows * (key_h + gap);
    bool fn_active = keyboard_row == rows;
    for (int c = 0; c < KBD_COLS; c++) {
        int a = kbd_fn_action_at(c);
        if (c == 0 || a != kbd_fn_action_at(c - 1)) {
            int span_end = c;
            while (span_end < KBD_COLS && kbd_fn_action_at(span_end) == a) span_end++;
            int span_w = (span_end - c) * key_w;
            int kx = kb_x + 4 * scale + c * key_w;
            bool chosen = fn_active && keyboard_col >= c && keyboard_col < span_end;
            bool latch = (a == KBD_ACT_CTRL && keyboard_ctrl) || (a == KBD_ACT_ALT && keyboard_alt);
            uint32_t box = chosen ? theme.accent : (latch ? 0x606060 : 0x282828);
            rect(kx, fn_y, span_w - 2 * scale, key_h, box);
            const char *label = a == KBD_ACT_CTRL ? "CTRL" : a == KBD_ACT_ALT ? "ALT"
                              : a == KBD_ACT_SPACE ? "SPACE" : a == KBD_ACT_DEL ? "DEL" : "ENTER";
            text(kx + (span_w - (int)strlen(label) * 8 * scale) / 2 - scale, fn_y + (key_h - 14 * scale) / 2, label, scale,
                 chosen ? theme.selected : theme.text, span_w);
        }
    }
    /* Hint bar */
    int hint_y = fn_y + key_h + 5 * scale;
    const char *hint = keyboard_for_terminal
        ? "A Type   X Space   B Back   L/R Hist   START Enter"
        : "A Type   X Space   B Cancel   START Save";
    text(kb_x + 8 * scale, hint_y, hint, scale, theme.text, kb_w - 16 * scale);
}

/* Transient status toast, shared by both views. */
static void draw_status(int scale) {
    if (status_frames <= 0) return;
    int w = (int)strlen(status_text) * 8 * scale + 24 * scale;
    rect((screen.w - w) / 2, screen.h - 68 * scale, w, 28 * scale, theme.accent);
    text((screen.w - w) / 2 + 12 * scale, screen.h - 61 * scale, status_text, scale, theme.selected, w - 24 * scale);
}

static void draw(void) {
    int scale = ui_scale(), row_h = 42 * scale, header = 48 * scale;
    /* Terminal is a full-screen independent view: black background, nothing
     * of the file manager is drawn underneath it. The terminal on-screen
     * keyboard renders over the same black terminal backdrop. */
    if (mode == MODE_TERMINAL || (mode == MODE_KEYBOARD && keyboard_for_terminal)) {
        clear(0x000000);
        int line_h = 20 * scale;
        int footer_h = 34 * scale;
        int input_h = 28 * scale;
        int visible = (screen.h - footer_h - input_h) / line_h; if (visible < 1) visible = 1;
        int total = terminal_line_count();
        int first = total - visible - terminal_scroll_get();
        if (first < 0) first = 0;
        for (int i = 0; i < visible; i++) {
            int idx = first + i;
            if (idx < 0 || idx >= total) continue;
            text(12 * scale, 2 * scale + i * line_h, terminal_line(idx), scale, theme.text, screen.w - 24 * scale);
        }
        /* current input line above the footer */
        rect(0, screen.h - footer_h - input_h, screen.w, input_h, 0x181818);
        text(12 * scale, screen.h - footer_h - input_h + 5 * scale, terminal_prompt_line(), scale, theme.selected, screen.w - 24 * scale);
        char footerline[220];
        if (terminal_running()) snprintf(footerline, sizeof footerline, "DEV %s   A Keyboard   B Files   Y Interrupt", terminal_get_cwd());
        else snprintf(footerline, sizeof footerline, "DEV %s   A Keyboard   B Files", terminal_get_cwd());
        rect(0, screen.h - footer_h, screen.w, footer_h, 0x101010);
        text(12 * scale, screen.h - footer_h + 7 * scale, footerline, scale, theme.text, screen.w - 24 * scale);
        if (mode == MODE_KEYBOARD) draw_keyboard(scale);
        draw_status(scale);
        present();
        return;
    }
    clear(0x101010);
    rect(0, 0, screen.w, header, theme.accent); char title[120];
    if (devmode_is_enabled()) snprintf(title, sizeof title, "FROGSHELL DEV  %s", current);
    else snprintf(title, sizeof title, "FROGSHELL  %s", current);
    text(12, 9 * scale, title, scale, theme.selected, screen.w - 24);
    int visible = (screen.h - header - 42 * scale) / row_h; if (visible < 1) visible = 1; if (selected < scroll) scroll = selected; if (selected >= scroll + visible) scroll = selected - visible + 1;
    for (int i = 0; i < visible && scroll + i < entry_count; i++) {
        int idx = scroll + i, y = header + i * row_h; bool active = idx == selected; char p[MAX_PATH];
        join_path(p, sizeof p, current, entries[idx].name); uint32_t bg = active ? theme.accent : (i & 1 ? 0x202020 : 0x1B1B1B);
        rect(0, y, screen.w, row_h - 2, bg); if (marked_path(p)) rect(0, y, 5 * scale, row_h - 2, 0xF0C040);
        if (entries[idx].dir) { /* VitaShell-style folder marker; files intentionally have no icon. */ rect(12 * scale, y + 11 * scale, 22 * scale, 16 * scale, active ? theme.selected : theme.accent); rect(15 * scale, y + 8 * scale, 10 * scale, 5 * scale, active ? theme.selected : theme.accent); }
        char label[300], kind[80], stamp[32]; snprintf(label, sizeof label, "%s%s", entries[idx].name, entries[idx].dir ? "/" : "");
        if (entries[idx].dir) snprintf(kind, sizeof kind, "Folder"); else { const char *dot = strrchr(entries[idx].name, '.'); snprintf(kind, sizeof kind, "%s", dot && dot[1] ? dot + 1 : "File"); }
        struct tm *tmv = localtime(&entries[idx].modified); if (!tmv || !strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", tmv)) strcpy(stamp, "Unknown time");
        text(42 * scale, y + 4 * scale, label, scale, active ? theme.selected : theme.text, screen.w - 235 * scale);
        text(42 * scale, y + 24 * scale, kind, scale, active ? theme.selected : 0xA8A8A8, 100 * scale);
        text(screen.w - 190 * scale, y + 24 * scale, stamp, scale, active ? theme.selected : 0xA8A8A8, 178 * scale);
        if (!entries[idx].dir) { char sz[32]; snprintf(sz, sizeof sz, "%lld", (long long)entries[idx].size); text(screen.w - (int)strlen(sz) * 8 * scale - 12 * scale, y + 4 * scale, sz, scale, active ? theme.selected : 0xAAAAAA, 90 * scale); }
    }
    char footer[220]; snprintf(footer, sizeof footer, "A Open   B Back   X Menu   Y Mark   SELECT Paste   START New");
    rect(0, screen.h - 34 * scale, screen.w, 34 * scale, 0x181818);
    text(12 * scale, screen.h - 27 * scale, footer, scale, theme.text, screen.w - 24 * scale);
    draw_status(scale);
    if (mode == MODE_ACTIONS) { int menu_h_row = 30 * scale; int w = 250 * scale, h = menu_action_count() * menu_h_row + 20 * scale, x = (screen.w - w) / 2, y = (screen.h - h) / 2; if (y < 0) y = 4 * scale; rect(x, y, w, h, 0x303030); for (int i = 0; i < menu_action_count(); i++) { bool a = i == menu_item; if (a) rect(x + 4 * scale, y + 8 * scale + i * menu_h_row, w - 8 * scale, menu_h_row - 2, theme.accent); text(x + 18 * scale, y + 12 * scale + i * menu_h_row, menu_action_name(i), scale, a ? theme.selected : theme.text, w - 30 * scale); } }
    if (mode == MODE_CONFIRM) { int w = 430 * scale, x = (screen.w - w) / 2; rect(x, screen.h / 2 - 48 * scale, w, 96 * scale, 0x303030); text(x + 18 * scale, screen.h / 2 - 28 * scale, confirm_kind == 1 ? "Delete selected item(s)?" : "Paste into this folder?", scale, theme.text, w - 36 * scale); text(x + 18 * scale, screen.h / 2 + 10 * scale, "A YES   B CANCEL", scale, theme.selected, w - 36 * scale); }
    if (mode == MODE_CONFLICT) {
        int w = screen.w - 44 * scale, h = 168 * scale, x = (screen.w - w) / 2, y = (screen.h - h) / 2;
        rect(x, y, w, h, 0x303030); rect(x, y, w, 36 * scale, theme.accent);
        text(x + 14 * scale, y + 8 * scale, "ITEM ALREADY EXISTS", scale, theme.selected, w - 28 * scale);
        text(x + 14 * scale, y + 50 * scale, base(clipboard_paths[conflict_index]), scale, theme.text, w - 28 * scale);
        const char *choices[] = { "Skip", "Rewrite", "Number" };
        for (int i = 0; i < 3; i++) { int bx = x + 14 * scale + i * ((w - 28 * scale) / 3); if (i == conflict_choice) rect(bx, y + 86 * scale, (w - 42 * scale) / 3, 30 * scale, theme.accent); text(bx + 8 * scale, y + 94 * scale, choices[i], scale, i == conflict_choice ? theme.selected : theme.text, (w - 42 * scale) / 3 - 12 * scale); }
        text(x + 14 * scale, y + 132 * scale, "LEFT/RIGHT CHOOSE   A APPLY   B CANCEL", scale, theme.text, w - 28 * scale);
    }
    if (mode == MODE_REWRITE) {
        int w = screen.w - 56 * scale, h = 128 * scale, x = (screen.w - w) / 2, y = (screen.h - h) / 2;
        rect(x, y, w, h, 0x303030); rect(x, y, w, 34 * scale, 0xA83232);
        text(x + 14 * scale, y + 8 * scale, "REWRITE EXISTING ITEM?", scale, theme.selected, w - 28 * scale);
        text(x + 14 * scale, y + 50 * scale, base(clipboard_paths[conflict_index]), scale, theme.text, w - 28 * scale);
        text(x + 14 * scale, y + 88 * scale, "A YES   B NO", scale, theme.selected, w - 28 * scale);
    }
    if (mode == MODE_INFO) { int w = screen.w - 40 * scale; rect(20 * scale, screen.h / 2 - 70 * scale, w, 140 * scale, 0x303030); text(32 * scale, screen.h / 2 - 35 * scale, info_text, scale, theme.text, w - 24 * scale); text(32 * scale, screen.h / 2 + 10 * scale, "B CLOSE", scale, theme.selected, w - 24 * scale); }
    if (mode == MODE_KEYBOARD) draw_keyboard(scale);
    present();
}

/* Single exit paths for the on-screen keyboard sessions: submit the terminal
 * command or save the FM text, resetting ALL session state in one place. */
static void osk_submit_terminal(void) {
    terminal_submit(prompt);
    prompt[0] = 0;
    terminal_set_input("");
    keyboard_symbols = 0; keyboard_shift = 0; keyboard_ctrl = 0; keyboard_alt = 0;
    keyboard_for_terminal = 0;
    mode = MODE_TERMINAL;
}

static void osk_save_fm(void) {
    if (prompt_original[0]) do_rename(prompt); else do_new_folder(prompt);
    keyboard_symbols = 0; keyboard_shift = 0; keyboard_ctrl = 0; keyboard_alt = 0;
    mode = MODE_NORMAL;
}

static void kbd_fn_do(int action) {
    if (action == KBD_ACT_SPACE) { size_t n = strlen(prompt); if (n + 1 < sizeof prompt) { prompt[n] = ' '; prompt[n + 1] = 0; } }
    else if (action == KBD_ACT_DEL) { if (prompt[0]) prompt[strlen(prompt) - 1] = 0; }
    else if (action == KBD_ACT_SYM) {
        /* Toggle page; caps and SYM are fixed cells so the cursor stays put. */
        keyboard_symbols = !keyboard_symbols;
    }
    else if (action == KBD_ACT_ENTER) {
        /* ENTER: submit in terminal sessions, save in FM sessions. Same key,
         * same look everywhere; only the action differs by context. */
        if (keyboard_for_terminal) osk_submit_terminal();
        else osk_save_fm();
    }
    else if (action == KBD_ACT_SHIFT) keyboard_shift = !keyboard_shift;
    else if (action == KBD_ACT_CTRL) keyboard_ctrl = !keyboard_ctrl;
    else if (action == KBD_ACT_ALT) keyboard_alt = !keyboard_alt;
}

/* Segment start/end helpers for the bottom modifier row: keys span several
 * grid columns (CTRL 0-1, ALT 2-3, SPACE 4-6, DEL 7, ENTER 8-9). Navigation
 * must jump between KEYS, not columns, or moving inside a span looks like
 * the pad did not respond. */
static int kbd_seg_start(int col) { while (col > 0 && kbd_fn_action_at(col) == kbd_fn_action_at(col - 1)) col--; return col; }
static int kbd_seg_end(int col)  { while (col < KBD_COLS - 1 && kbd_fn_action_at(col + 1) == kbd_fn_action_at(col)) col++; return col + 1; }

static void keyboard_input(uint32_t k) {
    int rows = kbd_page_rows();
    /* Grid navigation: rows 0..3 are key rows, `rows` = modifier row. */
    if (pressed(k, BTN_UP)) keyboard_row = keyboard_row <= 0 ? rows : keyboard_row - 1;
    if (pressed(k, BTN_DOWN)) keyboard_row = (keyboard_row + 1) % (rows + 1);
    if (keyboard_row == rows) {
        /* Modifier row: jump key-to-key so every press visibly moves. */
        if (pressed(k, BTN_LEFT))  { int s = kbd_seg_start(keyboard_col); keyboard_col = s > 0 ? kbd_seg_start(s - 1) : kbd_seg_end(KBD_COLS - 1) - 1; }
        if (pressed(k, BTN_RIGHT)) { int e = kbd_seg_end(keyboard_col); keyboard_col = e < KBD_COLS ? e : kbd_seg_start(0); }
    } else {
        if (pressed(k, BTN_LEFT)) keyboard_col = (keyboard_col + KBD_COLS - 1) % KBD_COLS;
        if (pressed(k, BTN_RIGHT)) keyboard_col = (keyboard_col + 1) % KBD_COLS;
    }
    /* X doubles as SPACE without replacing the on-grid SPACE key. */
    if (pressed(k, BTN_X)) { size_t n = strlen(prompt); if (n + 1 < sizeof prompt) { prompt[n] = ' '; prompt[n + 1] = 0; } }
    if (pressed(k, BTN_A)) {
        if (keyboard_row < rows) {
            int fixed = kbd_fixed_at(keyboard_row, keyboard_col);
            if (fixed) kbd_fn_do(fixed);
            else {
                size_t n = strlen(prompt);
                char ch = kbd_char_at(keyboard_row, keyboard_col);
                if (ch && n + 1 < sizeof prompt) { prompt[n] = ch; prompt[n + 1] = 0; }
                keyboard_ctrl = keyboard_alt = 0;  /* modifiers are one-shot */
            }
        } else {
            kbd_fn_do(kbd_fn_action_at(keyboard_col));
        }
    }
    if (pressed(k, BTN_Y) && prompt[0]) prompt[strlen(prompt) - 1] = 0;
    if (keyboard_for_terminal) {
        /* START submits the command (instead of rename/new-folder). */
        if (pressed(k, BTN_START)) osk_submit_terminal();
        /* B keeps the edited text on the terminal prompt line. */
        if (pressed(k, BTN_B)) {
            terminal_set_input(prompt);
            keyboard_symbols = 0; keyboard_ctrl = 0; keyboard_alt = 0;
            keyboard_for_terminal = 0; mode = MODE_TERMINAL;
        }
        if (pressed(k, BTN_L1)) { terminal_history_move(-1); strncpy(prompt, terminal_input_text(), sizeof prompt - 1); prompt[sizeof prompt - 1] = 0; }
        if (pressed(k, BTN_R1)) { terminal_history_move(1); strncpy(prompt, terminal_input_text(), sizeof prompt - 1); prompt[sizeof prompt - 1] = 0; }
        return;
    }
    if (pressed(k, BTN_START)) osk_save_fm();
    if (pressed(k, BTN_B)) mode = MODE_NORMAL;
}

/* Developer Mode helpers: launch targets from the file manager. */
static int file_is_elf(const char *path) {
    unsigned char magic[4] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    return got == 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

/* Single smart Run: ELF binaries exec directly; everything else (.sh or
 * text) goes through /bin/sh with the path as a real argv element, so
 * spaces in paths are safe. */
static void dev_run_selected(void) {
    if (selected >= entry_count || entries[selected].dir) { set_status("Nothing runnable"); return; }
    char p[MAX_PATH]; join_path(p, sizeof p, current, entries[selected].name);
    if (process_is_running()) { set_status("A process is already running"); return; }
    terminal_set_cwd(current);
    int ok = file_is_elf(p) ? process_start_executable(p, NULL, current)
                            : process_start_script(p, current);
    if (!ok) { set_status("Failed to run"); return; }
    mode = MODE_TERMINAL;
    terminal_note_launched_path(p);
}

static void dev_open_terminal_here(void) {
    char p[MAX_PATH];
    if (selected < entry_count && entries[selected].dir && strcmp(entries[selected].name, ".. "))
        { join_path(p, sizeof p, current, entries[selected].name); terminal_set_cwd(p); }
    else
        terminal_set_cwd(current);
    mode = MODE_TERMINAL;
}

static void actions_input(uint32_t k) {
    int count = menu_action_count(), base = action_count - 1;
    /* DEV can be toggled while the menu is open: clamp the cursor into range. */
    if (menu_item >= count) menu_item = count - 1;
    if (pressed(k, BTN_UP)) menu_item = (menu_item + count - 1) % count;
    if (pressed(k, BTN_DOWN)) menu_item = (menu_item + 1) % count;
    if (pressed(k, BTN_B) || (menu_item == count - 1 && pressed(k, BTN_A))) { mode = MODE_NORMAL; return; }
    if (!pressed(k, BTN_A)) return;
    if (devmode_is_enabled() && menu_item >= base && menu_item < base + DEV_ACTION_COUNT) {
        switch (menu_item - base) {
        case DEV_ACTION_RUN: dev_run_selected(); break;
        case DEV_ACTION_TERM_HERE: dev_open_terminal_here(); break;
        case DEV_ACTION_TERMINAL: mode = MODE_TERMINAL; break;
        default: break;
        }
        return;
    }
    switch (menu_item) {
    case 0: do_copy_or_cut(OP_COPY); mode = MODE_NORMAL; break;
    case 1: do_copy_or_cut(OP_CUT); mode = MODE_NORMAL; break;
    case 2: mode = clipboard[0] ? MODE_CONFIRM : MODE_NORMAL; confirm_kind = 2; if (!clipboard[0]) set_status("Clipboard is empty"); break;
    case 3: if (selected < entry_count && strcmp(entries[selected].name, ".. ")) begin_keyboard(entries[selected].name, entries[selected].name); break;
    case 4: mode = MODE_CONFIRM; confirm_kind = 1; break;
    case 5: begin_keyboard("", NULL); break;
    case 6:
        if (selected < entry_count && strcmp(entries[selected].name, ".. ")) {
            char p[MAX_PATH], size[32]; join_path(p, sizeof p, current, entries[selected].name);
            size_label(tree_size(p), size, sizeof size);
            snprintf(info_text, sizeof info_text, "%s  %s  %s", entries[selected].name, entries[selected].dir ? "Folder" : "File", size);
        } else strcpy(info_text, "Nothing selected");
        mode = MODE_INFO; break;
    default: break;
    }
}

static void normal_input(uint32_t k) {
    int scale = ui_scale();
    int visible = (screen.h - 48 * scale - 34 * scale) / (42 * scale); if (visible < 1) visible = 1;
    if (pressed(k, BTN_UP) && selected > 0) selected--;
    if (pressed(k, BTN_DOWN) && selected + 1 < entry_count) selected++;
    if (pressed(k, BTN_L1)) selected -= visible;
    if (pressed(k, BTN_R1)) selected += visible;
    if (selected < 0) selected = 0;
    if (selected >= entry_count) selected = entry_count - 1;
    if (pressed(k, BTN_Y)) toggle_mark();
    if (pressed(k, BTN_SELECT)) { confirm_kind = 2; mode = clipboard[0] ? MODE_CONFIRM : MODE_NORMAL; if (!clipboard[0]) set_status("Clipboard is empty"); }
    if (pressed(k, BTN_X)) { menu_item = 0; mode = MODE_ACTIONS; }
    if (pressed(k, BTN_START)) begin_keyboard("", NULL);
    if (pressed(k, BTN_B)) { if (strcmp(current, ROOT) == 0) { quit_requested = 1; return; } char *s = strrchr(current, '/'); if (s && s != current) *s = 0; else strcpy(current, ROOT); scan(); }
    if (pressed(k, BTN_A) && selected < entry_count) { if (entries[selected].dir) { if (!strcmp(entries[selected].name, ".. ")) { char *s = strrchr(current, '/'); if (s && s != current) *s = 0; else strcpy(current, ROOT); } else { char p[MAX_PATH]; join_path(p, sizeof p, current, entries[selected].name); if (under_root(p)) strcpy(current, p); } scan(); } else { menu_item = 0; mode = MODE_ACTIONS; } }
}

static void terminal_input(uint32_t k) {
    if (pressed(k, BTN_B)) mode = MODE_NORMAL;
    else if (pressed(k, BTN_A)) { keyboard_for_terminal = 1; keyboard_symbols = 0; begin_keyboard(terminal_input_text(), NULL); }
    else if (pressed(k, BTN_Y)) terminal_interrupt();
    else if (pressed(k, BTN_L1)) terminal_scroll_set(terminal_scroll_get() + 1);
    else if (pressed(k, BTN_R1)) { int s = terminal_scroll_get() - 1; if (s < 0) s = 0; terminal_scroll_set(s); }
    /* UP/DOWN reserved for future history browsing on the prompt line */
}

/* Physical OTG keyboard: feeds whichever text context is active. In the
 * terminal view it types straight into the command line; inside the OSK it
 * feeds the same prompt the virtual keys use. */
static void physical_keyboard_input(void) {
    if (!usbkbd_connected()) return;
    for (;;) {
        int enter, bs, up, down, left, right, pgup, pgdn, ctrl_c;
        char ch = usbkbd_poll(&enter, &bs, &up, &down, &left, &right,
                              &pgup, &pgdn, &ctrl_c);
        if (!ch && !enter && !bs && !up && !down && !left && !right &&
            !pgup && !pgdn && !ctrl_c) break;
        frame_dirty = 1;   /* physical input changed something on screen */
        char *line = NULL;
        if (mode == MODE_TERMINAL) line = NULL;                    /* direct terminal input */
        else if (mode == MODE_KEYBOARD) line = prompt;             /* OSK prompt */
        if (ctrl_c) { if (mode == MODE_TERMINAL || keyboard_for_terminal) terminal_interrupt(); continue; }
        if (mode == MODE_TERMINAL) {
            /* type directly into the terminal line buffer */
            if (ch && ch >= 32) terminal_kbd_char(ch);
            else if (bs) terminal_kbd_backspace();
            else if (enter) terminal_kbd_submit();
            else if (up) terminal_history_move(-1);
            else if (down) terminal_history_move(1);
            else if (pgup) terminal_scroll_set(terminal_scroll_get() + 8);
            else if (pgdn) terminal_scroll_set(terminal_scroll_get() - 8);
            continue;
        }
        if (!line) continue;
        if (ch && ch >= 32) { size_t n = strlen(line); if (n + 1 < sizeof prompt) { line[n] = ch; line[n + 1] = 0; } }        else if (bs) { if (line[0]) line[strlen(line) - 1] = 0; }
        else if (enter) {
            if (keyboard_for_terminal) osk_submit_terminal();
            else osk_save_fm();
        } else if (up)   { if (keyboard_for_terminal) { terminal_history_move(-1); strncpy(prompt, terminal_input_text(), sizeof prompt - 1); prompt[sizeof prompt - 1] = 0; } }
        else if (down) { if (keyboard_for_terminal) { terminal_history_move(1); strncpy(prompt, terminal_input_text(), sizeof prompt - 1); prompt[sizeof prompt - 1] = 0; } }
    }
}

static void input_loop(void) {
    uint32_t k = keys_now();
    uint32_t quit_chord = (1u << BTN_START) | (1u << BTN_SELECT);
    if ((k & quit_chord) == quit_chord && (previous_keys & quit_chord) != quit_chord) { quit_requested = 1; previous_keys = k; return; }
    /* Any button edge or held state touching the UI marks the frame dirty. */
    if (k != previous_keys) frame_dirty = 1;
    /* Hidden Developer Mode chord: 2s hold TOGGLES it. While the chord is up
     * the individual L1/R1/X/Y handling is suppressed. */
    int dev_evt = devmode_chord_update(k, now_ms());
    if (dev_evt > 0) {
        set_status("Developer Mode Enabled");
        menu_item = 0;
    } else if (dev_evt < 0) {
        set_status("Developer Mode Disabled");
        if (mode == MODE_TERMINAL || (mode == MODE_KEYBOARD && keyboard_for_terminal)) mode = MODE_NORMAL;
        keyboard_for_terminal = 0;
        keyboard_symbols = 0; keyboard_ctrl = 0; keyboard_alt = 0;
        process_shutdown();  /* no orphan if DEV dies while a child runs */
    }
    if (devmode_chord_active()) { previous_keys = k; return; }
    physical_keyboard_input();
    if (mode == MODE_ACTIONS) actions_input(k); else if (mode == MODE_KEYBOARD) keyboard_input(k); else if (mode == MODE_CONFIRM) { if (pressed(k, BTN_A)) { if (confirm_kind == 1) { do_delete(); mode = MODE_NORMAL; } else do_paste(); } if (pressed(k, BTN_B)) mode = MODE_NORMAL; } else if (mode == MODE_CONFLICT) { if (pressed(k, BTN_LEFT)) conflict_choice = (conflict_choice + 2) % 3; if (pressed(k, BTN_RIGHT)) conflict_choice = (conflict_choice + 1) % 3; if (pressed(k, BTN_A)) { if (conflict_choice == 1) mode = MODE_REWRITE; else paste_items(conflict_index, conflict_choice == 2 ? 2 : 0); } if (pressed(k, BTN_B)) mode = MODE_NORMAL; } else if (mode == MODE_REWRITE) { if (pressed(k, BTN_A)) paste_items(conflict_index, 1); if (pressed(k, BTN_B)) mode = MODE_CONFLICT; } else if (mode == MODE_INFO) { if (pressed(k, BTN_B) || pressed(k, BTN_A)) mode = MODE_NORMAL; } else if (mode == MODE_TERMINAL) terminal_input(k); else normal_input(k); previous_keys = k;
}

static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof *info);
    info->library_name = "FrogShell";
    info->library_version = "1.0";
    info->valid_extensions = "";
}
void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof *info);
    info->geometry.base_width = info->geometry.max_width = (unsigned)screen.w;
    info->geometry.base_height = info->geometry.max_height = (unsigned)screen.h;
    info->geometry.aspect_ratio = (float)screen.w / screen.h;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}
void retro_init(void) {
    quit_requested = 0; previous_keys = 0;
    signal(SIGINT, die_signal);
    signal(SIGTERM, die_signal);
    load_theme(); load_selected_font(); load_keymap(); raw_keys = open_keys();
    if (screen_open() == 0) scan();
    devmode_refresh();
    terminal_init();
    usbkbd_init();
    frame_dirty = 1;   /* first frame must render */
}
void retro_deinit(void) {
    terminal_free();
    usbkbd_close();
    glyph_cache_reset(0); glyph_cache_reset(1);   /* free resident glyph bitmaps */
    free(font_buffer); font_buffer = NULL; font_loaded = 0;
    screen_close();
    if (raw_keys) shmdt((const void *)raw_keys);
    raw_keys = NULL;
}
bool retro_load_game(const struct retro_game_info *info) { (void)info; return screen.canvas != NULL; }
void retro_unload_game(void) {}
/* Frame-skip: when nothing visual changed (no new input edges, no fresh
 * process output, no active toast, same scroll/mode/selection), skip the
 * full canvas redraw and re-present the last converted RGB565 buffer. */
static Mode    last_drawn_mode;
static int     last_drawn_status;
static int     last_drawn_term_lines;
static int     last_drawn_term_scroll;
static int     last_drawn_selected;
static int     last_drawn_menu_item;
static int     last_drawn_devmode;

static int frame_changed(void) {
    if (frame_dirty) return 1;
    if (mode != last_drawn_mode) return 1;
    if (status_frames != last_drawn_status) return 1;
    if (devmode_is_enabled() != last_drawn_devmode) return 1;
    if (mode == MODE_TERMINAL || (mode == MODE_KEYBOARD && keyboard_for_terminal)) {
        if (terminal_line_count() != last_drawn_term_lines) return 1;
        if (terminal_scroll_get() != last_drawn_term_scroll) return 1;
    }
    if (mode == MODE_NORMAL || mode == MODE_ACTIONS) {
        if (selected != last_drawn_selected || menu_item != last_drawn_menu_item) return 1;
    }
    return 0;
}

static void frame_mark_drawn(void) {
    frame_dirty = 0;
    last_drawn_mode = mode;
    last_drawn_status = status_frames;
    last_drawn_devmode = devmode_is_enabled();
    last_drawn_term_lines = terminal_line_count();
    last_drawn_term_scroll = terminal_scroll_get();
    last_drawn_selected = selected;
    last_drawn_menu_item = menu_item;
}

void retro_run(void) {
    if (input_poll_cb) input_poll_cb();
    input_loop();
    if (status_frames > 0) status_frames--;
    /* Poll the child regardless of the active view so it is reaped and its
     * pipe drained even if the user browses the file manager mid-run. */
    if (devmode_is_enabled() && (mode == MODE_TERMINAL || (mode == MODE_KEYBOARD && keyboard_for_terminal))) {
        int before = terminal_line_count();
        terminal_update();
        if (terminal_line_count() != before) frame_dirty = 1;
        if (terminal_exit_pending()) mode = MODE_NORMAL;
    } else if (process_is_running()) {
        process_poll(NULL);   /* reap + drain without rendering terminal lines */
    }
    /* Wait for a process exit only when one is live: mark dirty so the
     * [exit N] line renders the moment it appears. */
    if (process_is_running()) frame_dirty = 1;
    if (frame_changed()) {
        draw();
        frame_mark_drawn();
    } else if (video_cb) {
        video_cb(screen.output, (unsigned)screen.w, (unsigned)screen.h,
                 (size_t)screen.w * sizeof(*screen.output));
    }
    if (quit_requested && environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}
void retro_reset(void) { scan(); }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
