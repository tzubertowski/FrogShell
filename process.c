#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "process.h"

static pid_t child_pid = -1;
static pid_t child_pgid = -1;  /* the process group: survives the leader's reap */
static int   out_pipe[2] = { -1, -1 };  /* parent reads child stdout+stderr */
static int   in_pipe[2]  = { -1, -1 };  /* parent writes child stdin (unused now, kept open) */
static int   child_status = 0;
static bool  child_done = true;
static int   last_exit = -1;

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

/* Close every descriptor above 2 so the child does not inherit frontend fds
 * (canvas, fonts, shm, ...). Called after dup2, before exec. */
static void close_extra_fds(void) {
    int maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd <= 0) maxfd = 1024;
    for (int fd = 3; fd < maxfd; fd++) close(fd);
}

static pid_t spawn_child(char *const argv[], const char *cwd) {
    if (pipe(out_pipe) != 0) return -1;
    if (pipe(in_pipe) != 0) { close(out_pipe[0]); close(out_pipe[1]); out_pipe[0] = out_pipe[1] = -1; return -1; }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(in_pipe[0]); close(in_pipe[1]);
        out_pipe[0] = out_pipe[1] = in_pipe[0] = in_pipe[1] = -1;
        return -1;
    }
    if (pid == 0) {
        /* child */
        setpgid(0, 0);
        if (cwd && cwd[0]) chdir(cwd);
        /* Extend (not replace) PATH: keep whatever zhijack/picoarch set and
         * append the standard busybox locations plus the payload dir. */
        const char *cur = getenv("PATH");
        char pathbuf[512];
        if (cur && cur[0]) snprintf(pathbuf, sizeof pathbuf, "%s:/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/cubegm", cur);
        else snprintf(pathbuf, sizeof pathbuf, "/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/cubegm");
        setenv("PATH", pathbuf, 1);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close_extra_fds();   /* everything above fd 2 is now useless here */
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    /* parent: also set the group to close the setpgid race window; one of
     * the two calls fails with EACCES after exec which is harmless. */
    setpgid(pid, pid);
    close(in_pipe[0]); in_pipe[0] = -1;
    close(in_pipe[1]); in_pipe[1] = -1;  /* child stdin sees EOF, no hangs on read */
    close(out_pipe[1]); out_pipe[1] = -1;
    int fl = fcntl(out_pipe[0], F_GETFL, 0);
    if (fl >= 0) fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);
    child_pid = pid;
    child_pgid = pid;   /* the group id equals the leader's pid (setpgid(0,0)) */
    child_status = 0;
    child_done = false;
    last_exit = -1;
    return pid;
}

bool process_start_command(const char *command, const char *cwd) {
    if (process_is_running()) return false;
    char *argv[] = { "/bin/sh", "-c", (char *)command, NULL };
    return spawn_child(argv, cwd) > 0;
}

bool process_start_script(const char *path, const char *cwd) {
    if (process_is_running()) return false;
    char *argv[] = { "/bin/sh", (char *)path, NULL };
    return spawn_child(argv, cwd) > 0;
}

bool process_start_executable(const char *path, char *const argv[], const char *cwd) {
    if (process_is_running()) return false;
    /* Build a full argv with argv[0] = path for execv semantics. */
    int n = 0;
    if (argv) while (argv[n]) n++;
    char **av = calloc((size_t)n + 2, sizeof *av);
    if (!av) return false;
    av[0] = (char *)path;
    for (int i = 0; i < n; i++) av[i + 1] = argv[i];
    pid_t pid = spawn_child(av, cwd);
    free(av);
    return pid > 0;
}

bool process_is_running(void) { return child_pid > 0 && !child_done; }
int  process_exit_code(void)  { return child_done ? last_exit : -1; }

static int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);  /* e.g. 130 = SIGINT */
    return -1;
}

void process_poll(process_output_cb on_output) {
    if (child_pid > 0) {
        int status;
        pid_t r = waitpid(child_pid, &status, WNOHANG);
        if (r == child_pid) {
            child_status = status;
            child_done = true;
            child_pid = -1;
            last_exit = decode_status(child_status);  /* report even if a grandchild still holds the pipe */
            /* child_pgid is kept: descendants may still hold the group alive */
        }
    }
    if (out_pipe[0] >= 0) {
        char buf[2048];
        /* Cap the amount drained per frame: a continuously-writing command
         * (e.g. `yes`) can refill the pipe before every next read, so an
         * unbounded loop would never reach EAGAIN and monopolize retro_run().
         * The remainder is resumed on the next poll. */
        for (int chunks = 0; chunks < 4; chunks++) {
            ssize_t n = read(out_pipe[0], buf, sizeof buf);
            if (n > 0) { if (on_output) on_output(buf, (int)n); continue; }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                /* EOF / error: nothing more will arrive from this pipe. */
                close_fd(&out_pipe[0]);
            }
            break;  /* EAGAIN: no data yet */
        }
    }
}

static void kill_group(int sig) {
    /* Signal the retained group id: works even after the leader was reaped
     * (a `sleep 999 &` descendant still lives in the old group). */
    if (child_pgid > 0) kill(-child_pgid, sig);
}

void process_interrupt(void) {
    if (process_is_running()) kill_group(SIGINT);
}

void process_shutdown(void) {
    if (child_pgid > 0) {
        kill_group(SIGTERM);
        int waited = 0;
        while (!child_done && child_pid > 0 && waited < 100) { /* ~200ms grace at 2ms steps */
            int status;
            if (waitpid(child_pid, &status, WNOHANG) == child_pid) { child_status = status; child_done = true; break; }
            usleep(2000);
            waited++;
        }
        if (!child_done && child_pid > 0) { kill_group(SIGKILL); usleep(10000); }
        if (!child_done && child_pid > 0) {
            int status;
            if (waitpid(child_pid, &status, WNOHANG) == child_pid) child_status = status;
            child_done = true;  /* give up reaping: SIGKILL cannot be ignored */
        }
        /* After the grace window the whole group has been TERMed + KILLed;
         * any survivor would be immune to further signals too. */
        kill_group(SIGKILL);
        child_pgid = -1;
    }
    child_pid = -1;
    child_done = true;
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
}
