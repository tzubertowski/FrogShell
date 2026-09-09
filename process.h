#ifndef FROGSHELL_PROCESS_H
#define FROGSHELL_PROCESS_H

#include <stdbool.h>

/* Non-blocking process runner for FrogShell Developer Mode.
 *
 * One child at a time, launched in its own process group with stdin/stdout/
 * stderr wired to pipes. The parent polls with O_NONBLOCK reads — never a
 * blocking read inside retro_run(). Callers must gate on devmode before
 * starting anything. */

typedef void (*process_output_cb)(const char *buf, int len);

bool process_start_command(const char *command, const char *cwd);
bool process_start_script(const char *path, const char *cwd);
bool process_start_executable(const char *path, char *const argv[], const char *cwd);

void process_poll(process_output_cb on_output);
void process_interrupt(void);   /* SIGINT to the child process group */
void process_shutdown(void);    /* kill + reap + close pipes */

bool process_is_running(void);
int  process_exit_code(void);    /* last finished child; -1 while running */

#endif
