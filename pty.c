/*
 * pty.c — create a PTY pair and start the shell on the slave.
 */
#include "pty.h"
#include "xtuiapi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* The XACT CRT's xposix/sys/ioctl.h only exports TIOCSWINSZ today.  These are
 * stable Linux ioctl numbers and match OpenXSKernel/include/ioctl.h. */
#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540e
#endif
#ifndef TCGETS
#define TCGETS 0x5401
#endif
#ifndef ECHO
#define ECHO 0000010
#endif

/* Matches OpenXSKernel/include/tty.h: struct termios. */
struct pty_termios {
        uint32_t c_iflag;
        uint32_t c_oflag;
        uint32_t c_cflag;
        uint32_t c_lflag;
        uint8_t  c_line;
        uint8_t  c_cc[20];
};

int pty_open_shell(pty_session_t *session, const char *shell,
                   char *const envp[], int rows, int cols)
{
        struct winsize ws;
        char          *slave_name;
        int            master;
        int            slave;
        pid_t          child;

        if (session == NULL || shell == NULL) return -1;

        memset(session, 0, sizeof(*session));
        session->master = -1;
        session->child  = -1;
        session->term_fd = -1;

        if (rows < 1) rows = 24;
        if (cols < 1) cols = 80;

        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0) return -1;

        if (grantpt(master) != 0 || unlockpt(master) != 0) {
                close(master);
                return -1;
        }

        slave_name = ptsname(master);
        if (slave_name == NULL || slave_name[0] == '\0') {
                close(master);
                return -1;
        }

        slave = open(slave_name, O_RDWR);
        if (slave < 0) {
                close(master);
                return -1;
        }

        ws.ws_row    = (unsigned short)rows;
        ws.ws_col    = (unsigned short)cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        (void)ioctl(slave, TIOCSWINSZ, &ws);

        child = fork();
        if (child < 0) {
                close(slave);
                close(master);
                return -1;
        }

        if (child == 0) {
                char *argv[2];
                int   fd;

                close(master);

                /* New session + controlling terminal.  If the kernel is still
                 * mid-implementation these calls may fail; the slave fds below
                 * are still usable, so continue rather than losing the shell. */
                (void)setsid();
                (void)ioctl(slave, TIOCSCTTY, (void *)0);
                (void)ioctl(slave, TIOCSWINSZ, &ws);

                if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0) {
                        static const char msg[] = "leopard-conhost: dup2 failed\n";
                        (void)write(2, msg, sizeof(msg) - 1);
                        _exit(126);
                }

                /* Do not leak the framebuffer or PTY master into the shell.
                 * dup2() already produced 0/1/2. */
                for (fd = 3; fd < 64; fd++) {
                        if (fd != slave) close(fd);
                }
                if (slave > 2) close(slave);

                argv[0] = (char *)shell;
                argv[1] = NULL;
                execve(shell, argv, envp);

                {
                        static const char msg[] = "leopard-conhost: cannot exec shell\n";
                        (void)write(2, msg, sizeof(msg) - 1);
                }
                _exit(127);
        }

        close(slave);
        session->master = master;
        session->child  = child;
        session->rows   = rows;
        session->cols   = cols;
        strncpy(session->slave_name, slave_name, sizeof(session->slave_name) - 1);
        session->slave_name[sizeof(session->slave_name) - 1] = '\0';
        return 0;
}

int pty_write(pty_session_t *session, const void *data, size_t len)
{
        const uint8_t *bytes = (const uint8_t *)data;
        size_t         offset = 0;

        if (session == NULL || session->master < 0 || data == NULL) return -1;

        while (offset < len) {
                ssize_t written = write(session->master, bytes + offset, len - offset);
                if (written < 0) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                if (written == 0) break;
                offset += (size_t)written;
        }
        return 0;
}

int pty_set_winsize(pty_session_t *session, int rows, int cols)
{
        struct winsize ws;

        if (session == NULL || session->master < 0) return -1;
        if (rows < 1) rows = 1;
        if (cols < 1) cols = 1;

        ws.ws_row    = (unsigned short)rows;
        ws.ws_col    = (unsigned short)cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;

        if (ioctl(session->master, TIOCSWINSZ, &ws) != 0) return -1;
        session->rows = rows;
        session->cols = cols;
        return 0;
}

int pty_no_echo(pty_session_t *session)
{
        struct pty_termios term;

        if (session == NULL || session->slave_name[0] == '\0') return 0;

        if (session->term_fd < 0) {
                session->term_fd = open(session->slave_name, O_RDONLY | O_NOCTTY | O_NONBLOCK);
                if (session->term_fd < 0) return 0;
        }

        memset(&term, 0, sizeof(term));
        if (ioctl(session->term_fd, TCGETS, &term) != 0) return 0;
        return (term.c_lflag & ECHO) == 0;
}

void pty_release_term_fd(pty_session_t *session)
{
        if (session == NULL || session->term_fd < 0) return;
        close(session->term_fd);
        session->term_fd = -1;
}

int pty_poll_child(pty_session_t *session)
{
        int status = 0;
        pid_t result;

        if (session == NULL) return -1;
        if (session->reaped) return 1;
        if (session->child < 0) return -1;

        result = waitpid(session->child, &status, WNOHANG);
        if (result == session->child) {
                session->reaped = 1;
                session->status = status;
                pty_release_term_fd(session);
                return 1;
        }
        if (result == 0) return 0;
        if (errno == ECHILD) {
                session->reaped = 1;
                pty_release_term_fd(session);
                return 1;
        }
        return -1;
}

void pty_close(pty_session_t *session)
{
        if (session == NULL) return;
        if (session->term_fd >= 0) {
                close(session->term_fd);
                session->term_fd = -1;
        }
        if (session->master >= 0) {
                close(session->master);
                session->master = -1;
        }
}
