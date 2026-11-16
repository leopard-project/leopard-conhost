/*
 * console_host.c — pure PTY terminal host.
 *
 *   shell/app <-> /dev/pts/N <-> pty master <-> conhost <-> framebuffer
 *
 * The shell runs in its own session on the PTY slave.  This process keeps the
 * master, feeds master bytes through the VT parser, renders dirty rows to the
 * framebuffer, and writes keyboard bytes back to the master.
 *
 * Keyboard input is the /dev/console structured 24-byte x3tp_event_t stream.
 * All terminal I/O crosses the PTY.
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

#define DIRECT_CAP 512

/* ---- IME routing ------------------------------------------------------ */

typedef struct route_ctx {
        pty_session_t *pty;
        vt_t          *vt;
        ime_client_t  *ime;
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

/* ---- serial mirror ---------------------------------------------------- */

/* Headless/debug observability.  conhost renders PTY output to the framebuffer,
 * so QEMU serial logs (and the headless test suite that greps them) would
 * otherwise never see getty/shell/installer text.  Mirror the same master bytes
 * to the serial port; this is output-only, for diagnostics.
 *
 * xapi_OutputSerial() takes a NUL-terminated string and the kernel copies at
 * most XAPI_USER_STRING_MAX (4096) bytes, so feed small NUL-terminated chunks.
 * Embedded NULs are skipped (each printable run is emitted separately) instead
 * of reading past the bytes actually returned by read().  Password input stays
 * out of the log naturally: the PTY does not echo while ECHO is off, so no
 * secret bytes ever reach the master. */
static void serial_mirror(const char *data, size_t len)
{
        char   chunk[256];
        size_t offset = 0;

        while (offset < len) {
                size_t amount = len - offset;
                size_t start;
                size_t i;

                if (amount > sizeof(chunk) - 1) amount = sizeof(chunk) - 1;
                memcpy(chunk, data + offset, amount);
                chunk[amount] = '\0';

                start = 0;
                for (i = 0; i <= amount; i++) {
                        if (i != amount && chunk[i] != '\0') continue;
                        if (i > start) xapi_OutputSerial((WSTR)(chunk + start));
                        start = i + 1;
                }
                offset += amount;
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

int conhost_run_session(const char *shell, WSTR envp[])
{
        screen_t       screen;
        vt_t          *vt;
        input_source_t input;
        pty_session_t  pty;
        ime_client_t   ime;
        route_ctx_t    route;
        char           shell_var[512];
        char          *env[5];
        char           master_buffer[4096];
        int            child_done = 0;
        int            master_eof = 0;
        int            eof_spins = 0;

        (void)envp;
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
                uint32_t      ime_context = IME_CTX_TERMINAL;
                uint64_t      now_ms;

                if (child_done && master_eof) break;

                /* Focus/password context is sampled before poll so a password
                 * keystroke cannot be composed during the context window. */
                {
                        int no_echo = pty_no_echo(&pty);

                        ime_context = IME_CTX_TERMINAL;
                        if (no_echo) ime_context |= IME_CTX_PASSWORD;
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

                now_ms = ime_now_ms();

                if (poll_result > 0) {
                        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                                ssize_t got = read(pty.master, master_buffer, sizeof(master_buffer));
                                if (got > 0) {
                                        serial_mirror(master_buffer, (size_t)got);
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

                                        route.pty         = &pty;
                                        route.vt          = vt;
                                        route.ime         = &ime;
                                        route.ime_password = (ime_context & IME_CTX_PASSWORD) != 0;
                                        route.direct_len  = 0;

                                        do {
                                                count = input_read_events(&input, events, batch);
                                                for (i = 0; i < count; i++)
                                                        route_event(&route, &events[i]);
                                        } while (count == batch);

                                        route_flush(&route, now_ms);

                                        if (input.eof) {
                                                input_close(&input);
                                                xapi_Printf("leopard-conhost: /dev/console closed\n");
                                        }
                                } else {
                                        char keys[128];
                                        int  got = input_read(&input, keys, sizeof(keys));
                                        if (got > 0) {
                                                (void)pty_write(&pty, keys, (size_t)got);
                                        } else if (got == 0) {
                                                input_close(&input);
                                        }
                                }
                        }
                }

                /* IME pump runs even without readable input so deadlines and
                 * reconnects are serviced while the user is idle. */
                {
                        route.pty         = &pty;
                        route.vt          = vt;
                        route.ime         = &ime;
                        route.ime_password = (ime_context & IME_CTX_PASSWORD) != 0;
                        route_flush(&route, now_ms);
                }

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
