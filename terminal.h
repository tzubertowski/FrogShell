#ifndef FROGSHELL_TERMINAL_H
#define FROGSHELL_TERMINAL_H

#include <stdbool.h>

/* Integrated terminal for FrogShell Developer Mode.
 *
 * Bounded ring buffers only: 512 scrollback lines, 32 history entries.
 * 'yes' can flood output without growing RAM. The caller owns the render;
 * terminal_line_count()/terminal_line()/terminal_prompt_line() expose the
 * text to draw with the existing font/theme/scale system. */

#define TERM_LINES   512
#define TERM_COLS     128
#define TERM_HIST     32
#define TERM_INPUT    256

void terminal_init(void);
void terminal_free(void);
void terminal_note_launched_path(const char *path);
bool terminal_exit_pending(void);    /* `exit` builtin: leave the terminal view */

void terminal_update(void);                 /* poll process + pending exit line */
bool terminal_running(void);

void terminal_submit(const char *cmd);      /* builtins + process launch */
void terminal_interrupt(void);              /* SIGINT to running process */
void terminal_set_cwd(const char *cwd);
const char *terminal_get_cwd(void);
void terminal_output(const char *buf, int len);  /* process callback */

/* Render access */
int  terminal_line_count(void);
const char *terminal_line(int index);      /* 0..count-1, newest last */
const char *terminal_prompt_line(void);     /* current input + cursor pos */

/* Virtual keyboard integration */
void terminal_kbd_char(char c);            /* from on-screen keyboard */
void terminal_kbd_backspace(void);
void terminal_kbd_submit(void);
const char *terminal_input_text(void);
void terminal_set_input(const char *s);    /* restore edited text on cancel */

/* History navigation for the terminal keyboard row */
void terminal_history_move(int delta);
int  terminal_scroll_get(void);
void terminal_scroll_set(int lines);

#endif
