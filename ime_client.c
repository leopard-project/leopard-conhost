/*
 * ime_client.c — fail-open conhost transport to the terminal IME manager.
 *
 * Transport: one inherited stdin/stdout pipe pair.  conhost creates the pipes,
 * forks the manager with the child ends on fd 0/1, then writes key frames on
 * its write end and reads replies from its read end.  This is the kernel's
 * true blocking IPC primitive (pipefs has a wait queue and poll locks only the
 * pipe), unlike AF_UNIX reads/polls which yield-spin and may deadlock when both
 * peers poll.  No named socket, no bind/connect, no fd passing.
 */
#include "ime_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ime_protocol.h"

#define IME_MANAGER_PATH "/apps/system/leopard-ime.elf"

uint64_t ime_now_ms(void)
{
        struct timespec ts;

        memset(&ts, 0, sizeof(ts));
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int write_all(int fd, const void *data, size_t length)
{
        const uint8_t *bytes = (const uint8_t *)data;
        size_t         sent = 0;

        while (sent < length) {
                ssize_t result = write(fd, bytes + sent, length - sent);
                if (result < 0) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                if (result == 0) return -1;
                sent += (size_t)result;
        }
        return 0;
}

static int send_frame(ime_client_t *client, uint16_t type,
                      const void *payload, uint32_t length)
{
        uint8_t  frame[IME_FRAME_HEADER_SIZE + 64];
        ime_frame_header_t header;

        if (client == NULL || client->out_fd < 0) return -1;
        if (length > 64u) return -1;

        header.magic   = IME_PROTO_MAGIC;
        header.version = (uint16_t)IME_PROTO_VERSION;
        header.type    = type;
        header.length  = length;
        memcpy(frame, &header, sizeof(header));
        if (length != 0 && payload != NULL)
                memcpy(frame + sizeof(header), payload, length);

        return write_all(client->out_fd, frame, sizeof(header) + length);
}

static void clear_overlay(ime_client_t *client)
{
        client->overlay[0]  = '\0';
        client->overlay_len = 0;
}

static void disable(ime_client_t *client, uint64_t now_ms)
{
        (void)now_ms;
        if (client == NULL) return;
        if (client->in_fd >= 0) close(client->in_fd);
        if (client->out_fd >= 0) close(client->out_fd);
        client->in_fd      = -1;
        client->out_fd     = -1;
        client->enabled    = 0;
        client->active     = 0;
        client->composing  = 0;
        client->welcomed   = 0;
        client->sent_context = 0;
        client->rx_len     = 0;
        client->consecutive_timeouts = 0;
        clear_overlay(client);
}

/* Mark every unresolved entry as pass-through; resolved replies keep their
 * commit text so text the manager already produced is never lost. */
static void resolve_all_pass(ime_client_t *client)
{
        uint32_t seq;

        for (seq = client->head_seq; seq != client->next_seq; seq++) {
                ime_pending_t *entry = &client->pending[seq % IME_CLIENT_QUEUE];
                if (!entry->used) continue;
                if (!entry->ready) {
                        entry->ready      = 1;
                        entry->action     = IME_ACTION_PASS;
                        entry->output_len = 0;
                }
        }
}

static int queue_key(ime_client_t *client, const void *bytes, size_t len,
                     uint32_t *seq_out)
{
        uint32_t seq = client->next_seq;
        ime_pending_t *entry;

        if (seq - client->head_seq >= IME_CLIENT_QUEUE) return -1;
        if (len > sizeof(client->pending[0].original)) return -1;

        entry = &client->pending[seq % IME_CLIENT_QUEUE];
        memset(entry, 0, sizeof(*entry));
        entry->seq         = seq;
        entry->used        = 1;
        entry->length      = (uint8_t)len;
        if (len != 0 && bytes != NULL) memcpy(entry->original, bytes, len);

        *seq_out = seq;
        return 0;
}

void ime_client_init(ime_client_t *client)
{
        if (client == NULL) return;
        memset(client, 0, sizeof(*client));
        client->in_fd    = -1;
        client->out_fd   = -1;
        client->next_seq = 1;
        client->head_seq = 1;
        client->context  = 0;
        client->active   = 0;
        client->composing = 0;
        client->sent_context = 0xffffffffu;
}

int ime_client_start(ime_client_t *client, const char *manager_path)
{
        int c2m[2] = {-1, -1};
        int m2c[2] = {-1, -1};
        pid_t child;
        char *argv[2];
        char *envp[1] = { NULL };

        if (client == NULL || client->in_fd >= 0) return 0;
        if (manager_path == NULL || manager_path[0] == '\0')
                manager_path = IME_MANAGER_PATH;

        /* A live/PE root may not ship the IME manager.  Do not fork a child
         * that can only fail; the terminal runs fail-open without IME. */
        {
                int probe = open(manager_path, O_RDONLY);
                if (probe < 0) return -1;
                close(probe);
        }

        if (pipe(c2m) != 0 || pipe(m2c) != 0) {
                if (c2m[0] >= 0) close(c2m[0]);
                if (c2m[1] >= 0) close(c2m[1]);
                if (m2c[0] >= 0) close(m2c[0]);
                if (m2c[1] >= 0) close(m2c[1]);
                return -1;
        }

        child = fork();
        if (child < 0) {
                close(c2m[0]); close(c2m[1]);
                close(m2c[0]); close(m2c[1]);
                return -1;
        }

        if (child == 0) {
                int fd;
                int devnull;

                /* Manager reads keys on stdin and writes replies on stdout.
                 * Its stderr would otherwise land on the console/vt: keep the
                 * terminal clean and send lifecycle logs to /dev/null. */
                if (dup2(c2m[0], 0) < 0 || dup2(m2c[1], 1) < 0) _exit(126);
                devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                        (void)dup2(devnull, 2);
                        if (devnull > 2) close(devnull);
                }
                for (fd = 3; fd < 64; fd++) close(fd);

                argv[0] = (char *)manager_path;
                argv[1] = NULL;
                execve(manager_path, argv, envp);
                _exit(127);
        }

        close(c2m[0]);
        close(m2c[1]);
        client->in_fd    = m2c[0];
        client->out_fd   = c2m[1];
        client->enabled  = 1;
        client->active   = 0;
        client->composing = 0;
        client->welcomed = 0;
        client->pid      = (int)child;

        {
                ime_hello_t hello;

                hello.pid       = (uint32_t)getpid();
                hello.abi       = IME_PROTO_VERSION;
                hello.max_frame = IME_MAX_FRAME;
                if (send_frame(client, IME_MSG_HELLO, &hello, sizeof(hello)) != 0) {
                        disable(client, ime_now_ms());
                        return -1;
                }
        }
        return 0;
}

int ime_client_fd(const ime_client_t *client)
{
        return client != NULL ? client->in_fd : -1;
}

int ime_client_connected(const ime_client_t *client)
{
        return client != NULL && client->enabled && client->in_fd >= 0;
}

void ime_client_set_context(ime_client_t *client, uint32_t flags)
{
        ime_focus_t focus;

        if (client == NULL) return;
        client->context = flags;
        if (flags & IME_CTX_PASSWORD) {
                clear_overlay(client);
                client->composing = 0;
        }
        if (!ime_client_connected(client) || client->context == client->sent_context)
                return;

        focus.flags = flags;
        if (send_frame(client, IME_MSG_FOCUS, &focus, sizeof(focus)) != 0) {
                resolve_all_pass(client);
                disable(client, ime_now_ms());
                return;
        }
        client->sent_context = flags;
}

int ime_client_key(ime_client_t *client, uint32_t kind, uint32_t value,
                   uint32_t flags, const void *bytes, size_t len)
{
        ime_key_t key;
        uint32_t  seq;
        uint64_t  now_ms;

        if (client == NULL || !client->enabled || client->out_fd < 0) return 0;
        if (len > 32u) return 0;

        now_ms = ime_now_ms();
        if (now_ms == 0) return 0;
        if (queue_key(client, bytes, len, &seq) != 0) {
                resolve_all_pass(client);
                disable(client, now_ms);
                return 0;
        }

        key.seq    = seq;
        key.kind   = kind;
        key.value  = value;
        key.flags  = flags;
        key.length = (uint32_t)len;

        {
                uint8_t payload[sizeof(ime_key_t) + 32];
                memcpy(payload, &key, sizeof(key));
                if (len != 0 && bytes != NULL)
                        memcpy(payload + sizeof(key), bytes, len);

                if (send_frame(client, IME_MSG_KEY, payload,
                               (uint32_t)(sizeof(key) + len)) != 0) {
                        client->pending[seq % IME_CLIENT_QUEUE].used = 0;
                        resolve_all_pass(client);
                        disable(client, now_ms);
                        return 0;
                }
        }

        client->pending[seq % IME_CLIENT_QUEUE].deadline_ms =
                now_ms + IME_REPLY_TIMEOUT_MS;
        client->next_seq = seq + 1;
        return 1;
}

static void handle_frame(ime_client_t *client, const ime_frame_header_t *header,
                         const uint8_t *payload)
{
        switch (header->type) {
        case IME_MSG_WELCOME:
                if (header->length >= sizeof(ime_welcome_t)) {
                        ime_welcome_t welcome;
                        memcpy(&welcome, payload, sizeof(welcome));
                        client->session  = welcome.session;
                        client->welcomed = 1;
                }
                break;
        case IME_MSG_REPLY:
                if (header->length >= sizeof(ime_reply_t)) {
                        ime_reply_t reply;
                        uint32_t    seq;
                        ime_pending_t *entry;

                        memcpy(&reply, payload, sizeof(reply));
                        seq = reply.seq;
                        if (seq - client->head_seq >= IME_CLIENT_QUEUE) break;
                        entry = &client->pending[seq % IME_CLIENT_QUEUE];
                        if (!entry->used || entry->seq != seq || entry->ready) break;

                        entry->action = (uint8_t)reply.action;
                        if (reply.action == IME_ACTION_COMMIT ||
                            reply.action == IME_ACTION_COMMIT_PASS) {
                                uint32_t copy = reply.length;
                                if (copy > header->length - sizeof(reply))
                                        copy = header->length - sizeof(reply);
                                if (copy > IME_MAX_COMMIT) copy = IME_MAX_COMMIT;
                                memcpy(entry->output, payload + sizeof(reply), copy);
                                entry->output_len = (uint16_t)copy;
                        } else {
                                entry->output_len = 0;
                        }
                        entry->ready = 1;
                        client->consecutive_timeouts = 0;
                }
                break;
        case IME_MSG_UI:
                if (header->length >= sizeof(ime_ui_t) &&
                    !(client->context & IME_CTX_PASSWORD)) {
                        ime_ui_t ui;
                        const uint8_t *cursor;
                        size_t   remaining;
                        size_t   out = 0;

                        memcpy(&ui, payload, sizeof(ui));
                        cursor    = payload + sizeof(ui);
                        remaining = header->length - sizeof(ui);
                        clear_overlay(client);

                        if (ui.preedit_len > remaining) ui.preedit_len = (uint32_t)remaining;
                        if ((ui.flags & IME_UI_HAS_PREEDIT) && ui.preedit_len != 0) {
                                size_t copy = ui.preedit_len;
                                if (copy > IME_OVERLAY_MAX - 1) copy = IME_OVERLAY_MAX - 1;
                                memcpy(client->overlay, cursor, copy);
                                out = copy;
                                client->overlay_len = copy;
                        }
                        cursor    += ui.preedit_len;
                        remaining -= ui.preedit_len;

                        if ((ui.flags & IME_UI_HAS_CANDIDATES) && ui.cand_count != 0) {
                                uint32_t i;
                                if (out + 2 < IME_OVERLAY_MAX) {
                                        client->overlay[out++] = ' ';
                                        client->overlay[out++] = ' ';
                                }
                                for (i = 0; i < ui.cand_count && remaining >= 2; i++) {
                                        uint16_t len;
                                        memcpy(&len, cursor, 2);
                                        cursor    += 2;
                                        remaining -= 2;
                                        if (len > remaining) len = (uint16_t)remaining;
                                        if (out + 4 + len >= IME_OVERLAY_MAX) break;
                                        client->overlay[out++] = '[';
                                        client->overlay[out++] = (char)('1' + (i % 9));
                                        client->overlay[out++] = ']';
                                        memcpy(client->overlay + out, cursor, len);
                                        out += len;
                                        client->overlay[out++] = ' ';
                                        cursor    += len;
                                        remaining -= len;
                                }
                                client->overlay_len = out;
                        }
                        client->overlay[out < IME_OVERLAY_MAX ? out : IME_OVERLAY_MAX - 1] = '\0';
                        client->composing = (ui.preedit_len != 0) || (ui.cand_count != 0);
                }
                break;
        case IME_MSG_STATE:
                if (header->length >= sizeof(ime_state_t)) {
                        ime_state_t state;
                        memcpy(&state, payload, sizeof(state));
                        client->active    = (state.flags & IME_STATE_ACTIVE) != 0;
                        client->composing = (state.flags & IME_STATE_COMPOSING) != 0;
                        if (!client->composing) clear_overlay(client);
                }
                break;
        case IME_MSG_BYE:
                resolve_all_pass(client);
                disable(client, ime_now_ms());
                break;
        default:
                break;
        }
}

static void read_available(ime_client_t *client, uint64_t now_ms)
{
        for (;;) {
                struct pollfd pfd;
                ssize_t       got;

                if (client->in_fd < 0) return;
                pfd.fd      = client->in_fd;
                pfd.events  = POLLIN;
                pfd.revents = 0;
                if (poll(&pfd, 1, 0) <= 0) return;
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        resolve_all_pass(client);
                        disable(client, now_ms);
                        return;
                }
                if (!(pfd.revents & POLLIN)) return;

                if (client->rx_len >= sizeof(client->rx)) {
                        resolve_all_pass(client);
                        disable(client, now_ms);
                        return;
                }
                got = read(client->in_fd, client->rx + client->rx_len,
                           sizeof(client->rx) - client->rx_len);
                if (got <= 0) {
                        resolve_all_pass(client);
                        disable(client, now_ms);
                        return;
                }
                client->rx_len += (size_t)got;

                for (;;) {
                        ime_frame_header_t header;

                        if (client->rx_len < IME_FRAME_HEADER_SIZE) break;
                        memcpy(&header, client->rx, sizeof(header));
                        if (header.magic != IME_PROTO_MAGIC ||
                            header.version != IME_PROTO_VERSION ||
                            header.length > IME_MAX_FRAME - sizeof(header)) {
                                resolve_all_pass(client);
                                disable(client, now_ms);
                                return;
                        }
                        if (client->rx_len < sizeof(header) + header.length) break;

                        handle_frame(client, &header, client->rx + sizeof(header));
                        memmove(client->rx, client->rx + sizeof(header) + header.length,
                                client->rx_len - sizeof(header) - header.length);
                        client->rx_len -= sizeof(header) + header.length;
                }
        }
}

static void flush_ready(ime_client_t *client, ime_emit_fn emit, void *ctx)
{
        while (client->head_seq != client->next_seq) {
                ime_pending_t *entry = &client->pending[client->head_seq % IME_CLIENT_QUEUE];

                if (!entry->used) {
                        client->head_seq++;
                        continue;
                }
                if (!entry->ready) break;

                if (emit != NULL) {
                        if (entry->action == IME_ACTION_COMMIT ||
                            entry->action == IME_ACTION_COMMIT_PASS) {
                                if (entry->output_len != 0)
                                        emit(ctx, entry->output, entry->output_len);
                        }
                        if (entry->action == IME_ACTION_PASS ||
                            entry->action == IME_ACTION_COMMIT_PASS) {
                                if (entry->length != 0)
                                        emit(ctx, entry->original, entry->length);
                        }
                }

                entry->used = 0;
                client->head_seq++;
        }
}

void ime_client_pump(ime_client_t *client, uint64_t now_ms,
                     ime_emit_fn emit, void *ctx)
{
        uint32_t seq;

        if (client == NULL) return;

        if (client->in_fd >= 0) read_available(client, now_ms);

        /* Deadlines are checked from the oldest entry so later keys cannot
         * jump ahead of a stuck one. */
        for (seq = client->head_seq; seq != client->next_seq; seq++) {
                ime_pending_t *entry = &client->pending[seq % IME_CLIENT_QUEUE];

                if (!entry->used || entry->ready) continue;
                if (now_ms < entry->deadline_ms) continue;

                entry->ready      = 1;
                entry->action     = IME_ACTION_PASS;
                entry->output_len = 0;
                client->consecutive_timeouts++;
                if (client->consecutive_timeouts >= 3) {
                        resolve_all_pass(client);
                        disable(client, now_ms);
                        break;
                }
        }

        flush_ready(client, emit, ctx);
}

const char *ime_client_overlay(ime_client_t *client, size_t *len)
{
        if (client == NULL) return NULL;
        if (client->overlay_len == 0 || client->overlay[0] == '\0') return NULL;
        if (len != NULL) *len = client->overlay_len;
        return client->overlay;
}

void ime_client_close(ime_client_t *client)
{
        if (client == NULL) return;
        if (client->out_fd >= 0) (void)send_frame(client, IME_MSG_BYE, NULL, 0);
        disable(client, ime_now_ms());
}
