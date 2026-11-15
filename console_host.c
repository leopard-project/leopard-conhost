/*
 * console_host.c — PTY terminal host.
 *
 *   shell/app <-> /dev/pts/N <-> pty master <-> conhost <-> framebuffer
 *
 * The shell runs in its own session on the PTY slave.  This process keeps the
 * master, feeds master bytes through the VT parser, renders dirty rows to the
 * framebuffer, and writes keyboard bytes back to the master.
 *
 * Primary keyboard source: /dev/console structured 24-byte x3tp_event_t
 * records.  The legacy XTTTP output buffer and XAPI_GETCH line editor remain
 * only as a fallback for apps that still use XAPI_INPUT/XAPI_PRINTF (notably
 * lpsh) and for kernels that do not expose /dev/console yet.
 */
#include "console_host.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ime_client.h"
#include "input.h"
#include "x3tp_event.h"
#include "pty.h"
#include "screen.h"
#include "vt.h"

#define LINE_CAP 256
#define DIRECT_CAP 512

typedef struct legacy_line {
        char   buffer[LINE_CAP];
        size_t length;
} legacy_line_t;

/* ---- XTTTP compatibility: output ------------------------------------- */

static void legacy_drain_output(vt_t *vt)
{
        char output[1024];
        int64_t read_rc;

        read_rc = (int64_t)enter_syscall(SXAH_READ_OUTPUT_BUFFER,
                                        (uint64_t)output, sizeof(output) - 1,
                                        0, 0, 0, 0);
        if (read_rc != 0) return;

        output[sizeof(output) - 1] = '\0';
        if (output[0] != '\0') {
                /* XTTTP legacy output bypasses the PTY line discipline, so
                 * OPOST/ONLCR never runs.  Normalize LF to CRLF here; fd-based
                 * PTY output is already normalized by the slave termios and
                 * does not pass through this path. */
                char cooked[sizeof(output) * 2];
                size_t in = 0, out = 0;
                while (in < sizeof(output) && output[in] != '\0') {
                        char ch = output[in++];
                        if (ch == '\n' && (out == 0 || cooked[out - 1] != '\r'))
                                cooked[out++] = '\r';
                        cooked[out++] = ch;
                        if (out + 2 >= sizeof(cooked)) break;
                }
                if (out != 0) vt_feed(vt, (const uint8_t *)cooked, out);
        }
        enter_syscall(SXAH_UNLOCK_OUTPUT_LOCK, 0, 0, 0, 0, 0, 0);
}

/* ---- XTTTP compatibility: line input --------------------------------- */

static int legacy_input_status(void)
{
        return (int)enter_syscall(SXAH_CHECK_INPUT_BUFFER, 0, 0, 0, 0, 0, 0);
}

static void legacy_feed_line(const legacy_line_t *line)
{
        char buffer[LINE_CAP + 2];
        size_t length = line->length;

        if (length > LINE_CAP) length = LINE_CAP;
        memcpy(buffer, line->buffer, length);
        buffer[length] = '\n';
        buffer[length + 1] = '\0';
        enter_syscall(SXAH_WRITE_INPUT_BUFFER, (uint64_t)buffer, 0, 0, 0, 0, 0);
}

static void legacy_input_char(vt_t *vt, legacy_line_t *line, char ch, int no_echo)
{
        if (ch == '\n' || ch == '\r') {
                static const char newline[] = "\r\n";
                if (!no_echo) vt_feed(vt, (const uint8_t *)newline, 2);
                legacy_feed_line(line);
                line->length = 0;
        } else if (ch == '\b' || ch == 127) {
                static const char erase[] = "\b \b";
                if (line->length > 0) {
                        line->length--;
                        if (!no_echo) vt_feed(vt, (const uint8_t *)erase, 3);
                }
        } else if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) {
                if (line->length < sizeof(line->buffer) - 1) {
                        line->buffer[line->length++] = ch;
                        if (!no_echo) vt_feed(vt, (const uint8_t *)&ch, 1);
                }
        }
}

/* Non-ASCII input is stored/echoed as raw UTF-8 bytes.  The legacy line
 * editor is byte-oriented, which is what XAPI_INPUT consumers expect. */
static void legacy_input_utf8(vt_t *vt, legacy_line_t *line, const char *bytes,
                              size_t len, int no_echo)
{
        size_t i;

        for (i = 0; i < len; i++) {
                if ((unsigned char)bytes[i] < 0x80) {
                        legacy_input_char(vt, line, bytes[i], no_echo);
                        continue;
                }
                if (line->length + 1 < sizeof(line->buffer)) {
                        line->buffer[line->length++] = bytes[i];
                        if (!no_echo) vt_feed(vt, (const uint8_t *)&bytes[i], 1);
                }
        }
}

static void legacy_input_bytes(vt_t *vt, legacy_line_t *line, int status,
                               const char *bytes, size_t len)
{
        size_t i;
        int    no_echo = (status & XTTTP_INPUT_STATUS_NO_ECHO) != 0;

        for (i = 0; i < len; i++) legacy_input_char(vt, line, bytes[i], no_echo);
}

static void legacy_input_event(vt_t *vt, legacy_line_t *line, int status,
                               const x3tp_event_t *event)
{
        int    no_echo = (status & XTTTP_INPUT_STATUS_NO_ECHO) != 0;
        char   utf8[4];
        size_t len;

        if (event == NULL) return;

        if (event->type == X3TP_EVENT_CHAR) {
                len = x3tp_event_utf8(event->value, utf8);
                if (len != 0) legacy_input_utf8(vt, line, utf8, len, no_echo);
                return;
        }
        if (event->type != X3TP_EVENT_KEYDOWN) return;

        switch (event->value) {
        case X3TP_KEYCODE_ENTER:
                legacy_input_char(vt, line, '\n', no_echo);
                break;
        case X3TP_KEYCODE_BACKSPACE:
                legacy_input_char(vt, line, 127, no_echo);
                break;
        case X3TP_KEYCODE_TAB:
                legacy_input_char(vt, line, '\t', no_echo);
                break;
        case X3TP_KEYCODE_SPACE:
                legacy_input_char(vt, line, ' ', no_echo);
                break;
        default:
                break;
        }
}

static void legacy_input_pump(vt_t *vt, legacy_line_t *line, int status)
{
        char ch = xapi_Getch();
        legacy_input_char(vt, line, ch, (status & XTTTP_INPUT_STATUS_NO_ECHO) != 0);
}

/* ---- IME routing ------------------------------------------------------ */

typedef struct route_ctx {
        pty_session_t *pty;
        vt_t          *vt;
        ime_client_t  *ime;
        legacy_line_t *line;
        int            legacy_status;
        int            ime_password;
        int            ctrl_down;
        char           direct[DIRECT_CAP];
        size_t         direct_len;
} route_ctx_t;

static void route_emit(void *context, const void *bytes, size_t len)
{
        route_ctx_t *route = (route_ctx_t *)context;

        if (route == NULL || route->pty == NULL || len == 0) return;
        (void)pty_write(route->pty, bytes, len);
}

static void route_direct(route_ctx_t *route, const char *bytes, size_t len)
{
        if (len == 0) return;
        if (route->direct_len + len > sizeof(route->direct)) {
                (void)pty_write(route->pty, route->direct, route->direct_len);
                route->direct_len = 0;
        }
        if (len <= sizeof(route->direct)) {
                memcpy(route->direct + route->direct_len, bytes, len);
                route->direct_len += len;
        }
}

static int route_legacy_waiting(const route_ctx_t *route)
{
        return route->legacy_status & XTTTP_INPUT_STATUS_WAITING;
}

/* Only an ASCII letter can start a compose run in the builtin engine while
 * the IME is on.  Everything else (space/enter/punctuation/digits/special
 * keys/Ctrl-Alt chords) is passed straight through by the engine, so the
 * client can send it to the PTY without asking the manager. */
static int route_starts_compose(const x3tp_event_t *event)
{
        uint32_t value;

        if (event->flags & (X3TP_FLAG_CTRL | X3TP_FLAG_ALT)) return 0;
        if (event->type != X3TP_EVENT_CHAR && event->type != X3TP_EVENT_KEYDOWN)
                return 0;

        value = event->value;
        if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
        return value >= 'a' && value <= 'z';
}

static void route_event(route_ctx_t *route, const x3tp_event_t *event)
{
        char   bytes[16];
        size_t len;

        if (event->type == X3TP_EVENT_KEYDOWN &&
            event->value == X3TP_KEYCODE_CTRL) {
                route->ctrl_down = 1;
                return;
        }
        if (event->type == X3TP_EVENT_KEYUP &&
            event->value == X3TP_KEYCODE_CTRL) {
                route->ctrl_down = 0;
                return;
        }
        if (event->type == X3TP_EVENT_CHAR &&
            !(event->flags & X3TP_FLAG_CTRL)) {
                route->ctrl_down = 0;
        }

        len = x3tp_event_encode(route->vt, event, bytes, sizeof(bytes));

        /* Key releases carry no terminal bytes and are not queued. */
        if (event->type == X3TP_EVENT_KEYUP) return;

        if (route->ime != NULL && ime_client_connected(route->ime) &&
            !route->ime_password &&
            (event->type == X3TP_EVENT_CHAR ||
             event->type == X3TP_EVENT_KEYDOWN)) {
                uint32_t flags = 0;
                uint32_t kind  = event->type == X3TP_EVENT_CHAR
                                         ? IME_KEY_CHAR
                                         : IME_KEY_KEYDOWN;
                int      toggle = 0;

                if (route->ctrl_down && event->type == X3TP_EVENT_CHAR &&
                    event->value == 0x20u)
                        toggle = 1;
                if ((event->flags & X3TP_FLAG_CTRL) &&
                    event->type == X3TP_EVENT_CHAR && event->value == 0x20u)
                        toggle = 1;

                if (toggle) {
                        /* Fast path: keep the compose state locally so normal
                         * typing never crosses the process boundary while the
                         * IME is off.  Both sides flip on this same event. */
                        route->ime->active    = !route->ime->active;
                        route->ime->composing = 0;
                        flags |= IME_KEY_FLAG_TOGGLE;
                        (void)ime_client_key(route->ime, kind, event->value,
                                             flags, bytes, len);
                        return;
                }

                if (route->ime->active) {
                        int forward    = route->ime->composing != 0;
                        int provisional = 0;

                        /* IME on but not composing: only a letter needs the
                         * manager.  Space/Enter/punctuation and Ctrl chords
                         * take the direct PTY path, exactly like OJ's kernel
                         * filter returning false for direct input. */
                        if (!forward && route_starts_compose(event)) {
                                forward     = 1;
                                provisional = 1;
                                route->ime->composing = 1;
                        }

                        if (forward) {
                                if (ime_client_key(route->ime, kind, event->value,
                                                   flags, bytes, len))
                                        return;
                                if (provisional) route->ime->composing = 0;
                        }
                }
        }

        route_direct(route, bytes, len);
}

static void route_flush(route_ctx_t *route, uint64_t now_ms)
{
        if (route->ime != NULL) {
                ime_client_pump(route->ime, now_ms, route_emit, route);
                if (route->direct_len != 0) {
                        (void)pty_write(route->pty, route->direct, route->direct_len);
                        route->direct_len = 0;
                }
        } else if (route->direct_len != 0) {
                (void)pty_write(route->pty, route->direct, route->direct_len);
                route->direct_len = 0;
        }
}

/* ---- main session ----------------------------------------------------- */

static void build_environment(const char *shell, char *shell_var,
                              size_t shell_var_cap, char **env, size_t env_cap)
{
        size_t index = 0;

        snprintf(shell_var, shell_var_cap, "SHELL=%s", shell != NULL ? shell : "");

        if (index + 1 < env_cap) env[index++] = (char *)"TERM=xterm-256color";
        if (index + 1 < env_cap) env[index++] = (char *)"COLORTERM=truecolor";
        if (index + 1 < env_cap) env[index++] = (char *)"PATH=/apps/system:/apps/leopard";
        if (index + 1 < env_cap) env[index++] = shell_var;
        env[index] = NULL;
}

void conhost_mark_terminal(void)
{
        enter_syscall(SXAH_MARK_IS_TERMINAL, 0, 0, 0, 0, 0, 0);
}

int conhost_run_session(const char *shell, WSTR envp[])
{
        screen_t       screen;
        vt_t          *vt;
        input_source_t input;
        pty_session_t  pty;
        ime_client_t   ime;
        legacy_line_t  line;
        route_ctx_t    route;
        char           shell_var[512];
        char          *env[5];
        char           master_buffer[4096];
        int            child_done = 0;
        int            master_eof = 0;
        int            eof_spins = 0;

        (void)envp;
        memset(&line, 0, sizeof(line));
        memset(&route, 0, sizeof(route));
        input.fd = -1;
        ime_client_init(&ime);

        build_environment(shell, shell_var, sizeof(shell_var), env, 5);

        (void)screen_init(&screen);

        vt = vt_create(screen_rows(&screen), screen_cols(&screen));
        if (vt == NULL) {
                xapi_Printf("leopard-conhost: cannot allocate terminal grid\n");
                screen_shutdown(&screen);
                return 1;
        }

        vt_set_default_colors(vt, 0xffffff, 0x0d1223);
        vt_reset(vt);

        if (pty_open_shell(&pty, shell, env, screen_rows(&screen), screen_cols(&screen)) != 0) {
                xapi_Printf("leopard-conhost: PTY setup failed\n");
                vt_destroy(vt);
                screen_shutdown(&screen);
                return 1;
        }

        (void)input_open(&input);
        if (ime_client_start(&ime, getenv("CONHOST_IME")) == 0)
                xapi_Printf("leopard-conhost: IME manager started on pipes\n");
        else
                xapi_Printf("leopard-conhost: IME unavailable, keys pass through\n");
        ime_client_set_context(&ime, IME_CTX_TERMINAL |
                               (pty_no_echo(&pty) ? IME_CTX_PASSWORD : 0));

        screen_render(&screen, vt);
        vt_clear_dirty(vt);

        while (1) {
                struct pollfd fds[3];
                int           nfds = 0;
                int           poll_result;
                int           input_index = -1;
                int           legacy_status;
                uint32_t      ime_context = IME_CTX_TERMINAL;
                uint64_t      now_ms;

                if (child_done && master_eof) break;

                /* Focus/password context is sampled before poll so a password
                 * keystroke cannot be composed during the context window. */
                {
                        int no_echo = pty_no_echo(&pty);

                        legacy_status = legacy_input_status();
                        ime_context   = IME_CTX_TERMINAL;
                        if (no_echo ||
                            ((legacy_status & XTTTP_INPUT_STATUS_WAITING) != 0 &&
                             (legacy_status & XTTTP_INPUT_STATUS_NO_ECHO) != 0))
                                ime_context |= IME_CTX_PASSWORD;
                        ime_client_set_context(&ime, ime_context);
                }

                fds[nfds].fd      = pty.master;
                fds[nfds].events  = POLLIN;
                fds[nfds].revents = 0;
                nfds++;

                if (input_fd(&input) >= 0) {
                        input_index = nfds;
                        fds[nfds].fd      = input_fd(&input);
                        fds[nfds].events  = POLLIN;
                        fds[nfds].revents = 0;
                        nfds++;
                }

                if (ime_client_fd(&ime) >= 0) {
                        fds[nfds].fd      = ime_client_fd(&ime);
                        fds[nfds].events  = POLLIN;
                        fds[nfds].revents = 0;
                        nfds++;
                }

                poll_result = poll(fds, (nfds_t)nfds, 20);
                if (poll_result < 0 && errno != EINTR)
                        break;

                now_ms        = ime_now_ms();
                legacy_status = legacy_input_status();

                if (poll_result > 0) {
                        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                                ssize_t got = read(pty.master, master_buffer, sizeof(master_buffer));
                                if (got > 0) {
                                        vt_feed(vt, (const uint8_t *)master_buffer, (size_t)got);
                                } else if (got == 0) {
                                        master_eof = 1;
                                } else if (errno == EIO) {
                                        master_eof = 1;
                                } else if (errno != EINTR) {
                                        master_eof = 1;
                                }
                        }

                        if (input_index >= 0 &&
                            (fds[input_index].revents & (POLLIN | POLLHUP | POLLERR))) {
                                if (input_is_event_source(&input)) {
                                        x3tp_event_t events[16];
                                        size_t        batch = sizeof(events) / sizeof(events[0]);
                                        size_t        count;
                                        size_t        i;

                                        route.pty           = &pty;
                                        route.vt            = vt;
                                        route.ime           = &ime;
                                        route.line          = &line;
                                        route.legacy_status = legacy_status;
                                        route.ime_password   = (ime_context & IME_CTX_PASSWORD) != 0;
                                        route.direct_len    = 0;

                                        do {
                                                count = input_read_events(&input, events, batch);
                                                for (i = 0; i < count; i++) {
                                                        if (!child_done && route_legacy_waiting(&route))
                                                                legacy_input_event(vt, &line,
                                                                                   legacy_status,
                                                                                   &events[i]);
                                                        else
                                                                route_event(&route, &events[i]);
                                                }
                                        } while (count == batch);

                                        if (!child_done && route_legacy_waiting(&route)) {
                                                /* Legacy apps get no IME: the line
                                                 * editor owns input while waiting. */
                                                route.ctrl_down = 0;
                                        } else {
                                                route_flush(&route, now_ms);
                                        }

                                        if (input.eof) {
                                                input_close(&input);
                                                if (input_open_legacy(&input) == 0)
                                                        xapi_Printf("leopard-conhost: /dev/console read failed, using legacy input\n");
                                                else
                                                        xapi_Printf("leopard-conhost: /dev/console unavailable, using XTTTP input\n");
                                        }
                                } else {
                                        char keys[128];
                                        int  got = input_read(&input, keys, sizeof(keys));
                                        if (got > 0) {
                                                if (!child_done &&
                                                    (legacy_status & XTTTP_INPUT_STATUS_WAITING) != 0) {
                                                        legacy_input_bytes(vt, &line,
                                                                           legacy_status,
                                                                           keys, (size_t)got);
                                                } else {
                                                        (void)pty_write(&pty, keys, (size_t)got);
                                                }
                                        } else if (got == 0) {
                                                input_close(&input);
                                        }
                                }
                        }
                }

                /* IME pump runs even without readable input so deadlines and
                 * reconnects are serviced while the user is idle. */
                {
                        route.pty           = &pty;
                        route.vt            = vt;
                        route.ime           = &ime;
                        route.line          = &line;
                        route.legacy_status = legacy_status;
                        route.ime_password  = (ime_context & IME_CTX_PASSWORD) != 0;
                        route_flush(&route, now_ms);
                }

                /* Legacy XAPI apps write into the XTTTP output buffer, not fd 1. */
                legacy_drain_output(vt);

                {
                        size_t      response_len = 0;
                        const char *response = vt_take_response(vt, &response_len);
                        if (response != NULL && response_len != 0)
                                (void)pty_write(&pty, response, response_len);
                }

                (void)screen_update_size(&screen);
                {
                        int rows = screen_rows(&screen);
                        int cols = screen_cols(&screen);
                        if (rows != vt_rows(vt) || cols != vt_cols(vt)) {
                                if (vt_resize(vt, rows, cols) == 0)
                                        (void)pty_set_winsize(&pty, rows, cols);
                        }
                }

                {
                        size_t      overlay_len = 0;
                        const char *overlay = ime_client_overlay(&ime, &overlay_len);
                        (void)overlay_len;
                        screen_set_overlay(&screen, overlay);
                }

                screen_render(&screen, vt);
                vt_clear_dirty(vt);

                {
                        int child = pty_poll_child(&pty);
                        if (child != 0) child_done = 1;
                }

                /* No input endpoint: use the old XTTTP line editor only while
                 * the app is explicitly waiting for XAPI input. */
                if (!child_done && input_fd(&input) < 0) {
                        if ((legacy_status & XTTTP_INPUT_STATUS_WAITING) != 0) {
                                legacy_input_pump(vt, &line, legacy_status);
                                screen_render(&screen, vt);
                                vt_clear_dirty(vt);
                        } else {
                                line.length = 0;
                        }
                }

                if (master_eof && !child_done) {
                        if (++eof_spins > 100) {
                                xapi_KillProcess((UINT64)pty.child);
                                (void)pty_poll_child(&pty);
                                child_done = 1;
                                break;
                        }
                        xapi_Sleep(10);
                } else {
                        eof_spins = 0;
                }
        }

        ime_client_close(&ime);
        input_close(&input);

        if (!pty.reaped) {
                int spins;
                xapi_KillProcess((UINT64)pty.child);
                for (spins = 0; spins < 50 && !pty.reaped; spins++) {
                        xapi_Sleep(10);
                        (void)pty_poll_child(&pty);
                }
        }

        pty_close(&pty);
        vt_destroy(vt);
        screen_shutdown(&screen);
        return 0;
}
