/*
 * pty.h — PTY ownership boundary.
 *
 * The conhost creates the master/slave pair, puts the child in a new session,
 * makes the slave its controlling terminal, and execs the shell with the slave
 * on fds 0/1/2.  The parent keeps only the master.
 */
#pragma once

#include <sys/types.h>

typedef struct pty_session {
        int   master;
        pid_t child;
        int   rows;
        int   cols;
        int   reaped;
        int   status;
        int   term_fd;              /* lazily opened slave, for TCGETS only */
        char  slave_name[64];
} pty_session_t;

int pty_open_shell(pty_session_t *session, const char *shell,
                   char *const envp[], int rows, int cols);

/* Write bytes from the console into the shell's input queue. */
int pty_write(pty_session_t *session, const void *data, size_t len);

/* Ask the kernel to update the slave's winsize (which should raise SIGWINCH). */
int pty_set_winsize(pty_session_t *session, int rows, int cols);

/* 1 when the slave termios has ECHO cleared (password/no-echo input), 0 when
 * echo is on or the state cannot be queried.  Used only as IME context. */
int pty_no_echo(pty_session_t *session);

/* Non-blocking child poll: 1 = exited, 0 = still running, -1 = error/ECHILD. */
int pty_poll_child(pty_session_t *session);

/* Close the master.  The caller must have reaped or killed the child first. */
void pty_close(pty_session_t *session);
