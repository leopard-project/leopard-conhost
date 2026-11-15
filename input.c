/*
 * input.c — input endpoint discovery and key encoding.
 */
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vt.h"

static const char *const input_candidates[] = {
        "/dev/console",
        "/dev/input/seat0",
        "/dev/seat0",
        NULL,
};

static const char *const input_legacy_candidates[] = {
        "/dev/input/seat0",
        "/dev/seat0",
        NULL,
};

static int path_uses_events(const char *path)
{
        const char *format = getenv("CONHOST_INPUT_FORMAT");

        if (format != NULL) {
                if (strcmp(format, "events") == 0) return 1;
                if (strcmp(format, "bytes") == 0) return 0;
        }
        return strcmp(path, "/dev/console") == 0;
}

static int input_open_path(input_source_t *input, const char *path)
{
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY);

        if (fd < 0) return -1;
        input->fd         = fd;
        input->mode       = path_uses_events(path) ? INPUT_MODE_EVENTS
                                                   : INPUT_MODE_BYTES;
        input->eof        = 0;
        input->staged_len = 0;

        if (input->mode == INPUT_MODE_EVENTS) {
                /* Flush events queued before this consumer existed.  Login
                 * (leopard-getty) reads the legacy keyboard queue while the
                 * input core ring keeps a parallel copy, so without this the
                 * freshly started shell would receive the typed username and
                 * password as commands.  It also drops the password bytes that
                 * would otherwise sit in the kernel ring. */
                uint8_t discard[X3TP_EVENT_SIZE * 8];

                while (read(fd, discard, sizeof(discard)) > 0) {
                }
        }
        return 0;
}

int input_open(input_source_t *input)
{
        const char *env;
        int         i;

        if (input == NULL) return -1;
        input->fd         = -1;
        input->mode       = INPUT_MODE_BYTES;
        input->eof        = 0;
        input->staged_len = 0;

        env = getenv("CONHOST_INPUT");
        if (env != NULL && env[0] != '\0') {
                if (input_open_path(input, env) == 0) return 0;
        }

        for (i = 0; input_candidates[i] != NULL; i++) {
                if (input_open_path(input, input_candidates[i]) == 0) return 0;
        }

        input->fd = -1;
        return -1;
}

int input_open_legacy(input_source_t *input)
{
        const char *env;
        int         i;

        if (input == NULL) return -1;
        input->fd         = -1;
        input->mode       = INPUT_MODE_BYTES;
        input->eof        = 0;
        input->staged_len = 0;

        env = getenv("CONHOST_INPUT");
        if (env != NULL && env[0] != '\0' && strcmp(env, "/dev/console") != 0) {
                if (input_open_path(input, env) == 0) return 0;
        }
        for (i = 0; input_legacy_candidates[i] != NULL; i++) {
                if (input_open_path(input, input_legacy_candidates[i]) == 0) return 0;
        }
        return -1;
}

int input_is_event_source(const input_source_t *input)
{
        return input != NULL && input->fd >= 0 && input->mode == INPUT_MODE_EVENTS;
}

int input_fd(const input_source_t *input)
{
        return input != NULL ? input->fd : -1;
}

int input_read(input_source_t *input, void *buffer, size_t capacity)
{
        ssize_t result;

        if (input == NULL || input->fd < 0 || buffer == NULL || capacity == 0) return -1;

        result = read(input->fd, buffer, capacity);
        if (result > 0) return (int)result;
        if (result == 0) return 0;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1;
}

void input_close(input_source_t *input)
{
        if (input == NULL) return;
        if (input->fd >= 0) close(input->fd);
        input->fd         = -1;
        input->eof        = 0;
        input->staged_len = 0;
}

size_t input_read_events(input_source_t *input, x3tp_event_t *events, size_t max)
{
        size_t got = 0;

        if (input == NULL || input->fd < 0 || events == NULL || max == 0) return 0;

        for (;;) {
                /* Consume every whole record buffered locally first.  The
                 * kernel returns whole 24-byte records; staging still handles a
                 * short tail defensively across calls. */
                while (input->staged_len >= X3TP_EVENT_SIZE && got < max) {
                        x3tp_event_t record;

                        memcpy(&record, input->staging, sizeof(record));
                        memmove(input->staging, input->staging + X3TP_EVENT_SIZE,
                                input->staged_len - X3TP_EVENT_SIZE);
                        input->staged_len -= X3TP_EVENT_SIZE;

                        /* Drop unknown records: a fixed-size stream always
                         * resynchronises at the next 24-byte boundary. */
                        if (record.type == X3TP_EVENT_CHAR ||
                            record.type == X3TP_EVENT_KEYDOWN ||
                            record.type == X3TP_EVENT_KEYUP)
                                events[got++] = record;
                }
                if (got == max) break;

                if (input->staged_len == sizeof(input->staging)) {
                        /* Cannot make progress without discarding a record. */
                        memmove(input->staging, input->staging + X3TP_EVENT_SIZE,
                                input->staged_len - X3TP_EVENT_SIZE);
                        input->staged_len -= X3TP_EVENT_SIZE;
                }

                {
                        /* The ABI rejects read sizes below one record with
                         * -EINVAL, so always read a full record multiple. */
                        uint8_t chunk[X3TP_EVENT_SIZE * 8];
                        ssize_t result = read(input->fd, chunk, sizeof(chunk));

                        if (result > 0) {
                                size_t amount = (size_t)result;
                                size_t room   = sizeof(input->staging) - input->staged_len;

                                if (amount > room) amount = room;
                                memcpy(input->staging + input->staged_len, chunk, amount);
                                input->staged_len += amount;
                                continue;
                        }
                        if (result == 0) {
                                input->eof = 1;
                                break;
                        }
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        input->eof = 1;
                        break;
                }
        }

        return got;
}

static size_t append_string(char *out, size_t used, size_t cap, const char *text)
{
        size_t len = strlen(text);

        if (used + len > cap) return used;
        memcpy(out + used, text, len);
        return used + len;
}

static size_t append_char(char *out, size_t used, size_t cap, char ch)
{
        if (used + 1 > cap) return used;
        out[used++] = ch;
        return used;
}

size_t input_encode_key(const vt_t *vt, const input_key_t *key, char *out, size_t cap)
{
        size_t used = 0;
        int    decckm;
        int    keypad;

        if (key == NULL || out == NULL || cap == 0) return 0;

        decckm = vt_decckm(vt);
        keypad = vt_keypad_app(vt);

        switch (key->kind) {
        case INPUT_KEY_CHAR:
                return vt_utf8_encode(key->ch, out, cap);
        case INPUT_KEY_ENTER:
                return append_char(out, used, cap, '\r');
        case INPUT_KEY_TAB:
                return append_char(out, used, cap, '\t');
        case INPUT_KEY_BACKSPACE:
                return append_char(out, used, cap, 0x7f);
        case INPUT_KEY_ESCAPE:
                return append_char(out, used, cap, 0x1b);
        case INPUT_KEY_UP:
                return append_string(out, used, cap, decckm ? "\x1bOA" : "\x1b[A");
        case INPUT_KEY_DOWN:
                return append_string(out, used, cap, decckm ? "\x1bOB" : "\x1b[B");
        case INPUT_KEY_RIGHT:
                return append_string(out, used, cap, decckm ? "\x1bOC" : "\x1b[C");
        case INPUT_KEY_LEFT:
                return append_string(out, used, cap, decckm ? "\x1bOD" : "\x1b[D");
        case INPUT_KEY_HOME:
                return append_string(out, used, cap, decckm ? "\x1bOH" : "\x1b[H");
        case INPUT_KEY_END:
                return append_string(out, used, cap, decckm ? "\x1bOF" : "\x1b[F");
        case INPUT_KEY_INSERT:
                return append_string(out, used, cap, "\x1b[2~");
        case INPUT_KEY_DELETE:
                return append_string(out, used, cap, "\x1b[3~");
        case INPUT_KEY_PAGE_UP:
                return append_string(out, used, cap, "\x1b[5~");
        case INPUT_KEY_PAGE_DOWN:
                return append_string(out, used, cap, "\x1b[6~");
        case INPUT_KEY_F1:
                return append_string(out, used, cap, "\x1bOP");
        case INPUT_KEY_F2:
                return append_string(out, used, cap, "\x1bOQ");
        case INPUT_KEY_F3:
                return append_string(out, used, cap, "\x1bOR");
        case INPUT_KEY_F4:
                return append_string(out, used, cap, "\x1bOS");
        case INPUT_KEY_F5:
                return append_string(out, used, cap, "\x1b[15~");
        case INPUT_KEY_F6:
                return append_string(out, used, cap, "\x1b[17~");
        case INPUT_KEY_F7:
                return append_string(out, used, cap, "\x1b[18~");
        case INPUT_KEY_F8:
                return append_string(out, used, cap, "\x1b[19~");
        case INPUT_KEY_F9:
                return append_string(out, used, cap, "\x1b[20~");
        case INPUT_KEY_F10:
                return append_string(out, used, cap, "\x1b[21~");
        case INPUT_KEY_F11:
                return append_string(out, used, cap, "\x1b[23~");
        case INPUT_KEY_F12:
                return append_string(out, used, cap, "\x1b[24~");
        case INPUT_KEY_KEYPAD_0:
        case INPUT_KEY_KEYPAD_1:
        case INPUT_KEY_KEYPAD_2:
        case INPUT_KEY_KEYPAD_3:
        case INPUT_KEY_KEYPAD_4:
        case INPUT_KEY_KEYPAD_5:
        case INPUT_KEY_KEYPAD_6:
        case INPUT_KEY_KEYPAD_7:
        case INPUT_KEY_KEYPAD_8:
        case INPUT_KEY_KEYPAD_9: {
                char digit = (char)('0' + (key->kind - INPUT_KEY_KEYPAD_0));
                if (keypad) {
                        used = append_string(out, used, cap, "\x1bO");
                        used = append_char(out, used, cap, (char)('p' + (digit - '0')));
                        return used;
                }
                return append_char(out, used, cap, digit);
        }
        case INPUT_KEY_KEYPAD_DECIMAL:
                return append_string(out, used, cap, keypad ? "\x1bOn" : ".");
        case INPUT_KEY_KEYPAD_DIVIDE:
                return append_string(out, used, cap, keypad ? "\x1bOo" : "/");
        case INPUT_KEY_KEYPAD_MULTIPLY:
                return append_string(out, used, cap, keypad ? "\x1bOj" : "*");
        case INPUT_KEY_KEYPAD_MINUS:
                return append_string(out, used, cap, keypad ? "\x1bOm" : "-");
        case INPUT_KEY_KEYPAD_PLUS:
                return append_string(out, used, cap, keypad ? "\x1bOk" : "+");
        case INPUT_KEY_KEYPAD_ENTER:
                return append_string(out, used, cap, keypad ? "\x1bOM" : "\r");
        case INPUT_KEY_KEYPAD_EQUALS:
                return append_string(out, used, cap, keypad ? "\x1bOX" : "=");
        default:
                return 0;
        }
}
