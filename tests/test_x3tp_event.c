/*
 * Host-side tests for the /dev/console record boundary.
 *
 * Build (from leopard-conhost/):
 *   cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -I. \
 *      tests/test_x3tp_event.c x3tp_event.c input.c vt.c -o /tmp/test_x3tp_event
 *
 * The target build is freestanding; this test only exercises the portable
 * mapping code with the host libc.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "x3tp_event.h"
#include "input.h"
#include "vt.h"

static void expect(const vt_t *vt, const x3tp_event_t *event,
                   const char *want, size_t want_len)
{
        char   out[16];
        size_t got = x3tp_event_encode(vt, event, out, sizeof(out));

        assert(got == want_len);
        assert(memcmp(out, want, want_len) == 0);
        (void)event;
}

int main(void)
{
        x3tp_event_t event;
        input_key_t   key;
        char          out[16];
        char          utf8[4];
        vt_t         *vt;

        assert(sizeof(x3tp_event_t) == 24);

        /* Packed UTF-8: ASCII. */
        memset(&event, 0, sizeof(event));
        event.type  = X3TP_EVENT_CHAR;
        event.value = 'a';
        expect(NULL, &event, "a", 1);

        /* Ctrl chords become C0 controls (PS/2 sends the letter + CTRL flag). */
        event.value = 'c';
        event.flags = X3TP_FLAG_CTRL;
        expect(NULL, &event, "\x03", 1);
        event.value = ' ';
        expect(NULL, &event, "\x00", 1);
        event.value = '?';
        expect(NULL, &event, "\x7f", 1);
        event.flags = 0;

        /* Backspace/erase contract: canonical PTY mode uses VERASE=0x7f, so an
         * unmodified CHAR U+0008 (Backspace, and future IME erase) must be
         * normalized to 0x7f.  Ctrl+H and DEL stay as they are. */
        event.value = 0x08;
        expect(NULL, &event, "\x7f", 1);
        event.flags = X3TP_FLAG_CTRL;
        expect(NULL, &event, "\x08", 1);
        event.flags = 0;
        event.value = 0x7f;
        expect(NULL, &event, "\x7f", 1);
        event.flags = X3TP_FLAG_CTRL;
        expect(NULL, &event, "\x7f", 1);
        event.flags = 0;

        /* The existing keycode and input_key paths also erase with 0x7f. */
        event.type  = X3TP_EVENT_KEYDOWN;
        event.value = 0x08;
        expect(NULL, &event, "\x7f", 1);
        key.kind = INPUT_KEY_BACKSPACE;
        key.ch   = 0;
        assert(input_encode_key(NULL, &key, out, sizeof(out)) == 1);
        assert((unsigned char)out[0] == 0x7f);

        /* Packed UTF-8: U+4F60 = e4 bd a0, little-endian packing. */
        event.value = 0x00a0bde4u;
        assert(x3tp_event_utf8(event.value, utf8) == 3);
        assert((unsigned char)utf8[0] == 0xe4);
        assert((unsigned char)utf8[1] == 0xbd);
        assert((unsigned char)utf8[2] == 0xa0);

        /* Scalar fallback for producers that store a code point. */
        assert(x3tp_event_utf8(0x4f60, utf8) == 3);
        assert((unsigned char)utf8[0] == 0xe4);

        /* Invalid scalar (surrogate) is rejected. */
        assert(x3tp_event_utf8(0xd800, utf8) == 0);
        assert(x3tp_event_utf8(0, utf8) == 0);

        /* Key releases must never emit bytes. */
        event.type  = X3TP_EVENT_KEYUP;
        event.value = X3TP_KEYCODE_UP;
        expect(NULL, &event, "", 0);

        /* DECCKM off: cursor keys are CSI. */
        vt = vt_create(24, 80);
        assert(vt != NULL);
        vt_reset(vt);

        memset(&event, 0, sizeof(event));
        event.type  = X3TP_EVENT_KEYDOWN;
        event.value = X3TP_KEYCODE_UP;
        expect(vt, &event, "\x1b[A", 3);

        event.value = X3TP_KEYCODE_HOME;
        expect(vt, &event, "\x1b[H", 3);

        event.value = X3TP_KEYCODE_DELETE;
        expect(vt, &event, "\x1b[3~", 4);

        event.value = X3TP_KEYCODE_TAB;
        event.flags = X3TP_FLAG_SHIFT;
        expect(vt, &event, "\x1b[Z", 3);
        event.flags = 0;

        event.value = X3TP_KEYCODE_F1;
        expect(vt, &event, "\x1bOP", 3);
        event.value = X3TP_KEYCODE_F10;
        expect(vt, &event, "\x1b[21~", 5);

        /* DECCKM on and application keypad. */
        vt_feed(vt, (const uint8_t *)"\x1b[?1h\x1b=", 8);
        assert(vt_decckm(vt) == 1);
        assert(vt_keypad_app(vt) == 1);

        event.value = X3TP_KEYCODE_UP;
        expect(vt, &event, "\x1bOA", 3);
        event.value = X3TP_KEYCODE_END;
        expect(vt, &event, "\x1bOF", 3);

        /* Keypad digits use the application-keypad form when enabled. */
        key.kind = INPUT_KEY_KEYPAD_5;
        key.ch   = 0;
        assert(input_encode_key(vt, &key, out, sizeof(out)) == 3);
        assert(memcmp(out, "\x1bOu", 3) == 0);

        event.value = X3TP_KEYCODE_KEYPAD_5;
        expect(vt, &event, "\x1bOu", 3);

        /* Unknown key codes produce nothing (never swallowed text elsewhere). */
        event.value = 9999;
        expect(NULL, &event, "", 0);

        /* Legacy ASCII control key codes stay usable. */
        event.value = 0x0d;
        expect(vt, &event, "\r", 1);
        event.value = 0x7f;
        expect(NULL, &event, "", 0);
        event.value = 0x1b;
        expect(vt, &event, "\x1b", 1);

        /* VT regression for the erase echo the PTY produces.
         *
         * Simple ECHOE form "\b \b": "abc" then erase -> "ab", cursor column 2.
         * 0x7f itself is DEL/no-op on output; erase lives on the input side. */
        vt_reset(vt);
        vt_feed(vt, (const uint8_t *)"abc\b \b", 6);
        assert(vt_cell(vt, 0, 0)->ch == 'a');
        assert(vt_cell(vt, 0, 1)->ch == 'b');
        assert(vt_cell(vt, 0, 2)->ch == ' ');
        assert(vt_cursor_col(vt) == 2);
        assert(vt_cursor_row(vt) == 0);

        /* Control-char ECHOE form: a control byte echoed as two cells ("^H")
         * is erased with 6 bytes "\b \b\b \b" and must still leave "ab" at
         * cursor column 2. */
        vt_reset(vt);
        vt_feed(vt, (const uint8_t *)"ab^H", 4);
        vt_feed(vt, (const uint8_t *)"\b \b\b \b", 6);
        assert(vt_cell(vt, 0, 0)->ch == 'a');
        assert(vt_cell(vt, 0, 1)->ch == 'b');
        assert(vt_cell(vt, 0, 2)->ch == ' ');
        assert(vt_cell(vt, 0, 3)->ch == ' ');
        assert(vt_cursor_col(vt) == 2);

        /* DEL on output is a no-op: it must not move the cursor or print. */
        vt_reset(vt);
        vt_feed(vt, (const uint8_t *)"ab\x7f", 3);
        assert(vt_cell(vt, 0, 2)->ch == 0);
        assert(vt_cursor_col(vt) == 2);

        /* Bounded fuzz: packed UTF-8 decoding must stay in bounds and never
         * produce more than four bytes. */
        {
                uint32_t seed = 0x12345678u;
                int      i;
                for (i = 0; i < 200000; i++) {
                        size_t n;
                        seed = seed * 1664525u + 1013904223u;
                        n    = x3tp_event_utf8(seed, utf8);
                        assert(n <= 4);
                }
        }

        /* Record reassembly: partial reads, unknown records and batching. */
        {
                int            fds[2];
                input_source_t source;
                x3tp_event_t  records[4];
                size_t         got;
                unsigned char  buffer[24 * 4];
                size_t         i;

                assert(pipe(fds) == 0);
                assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
                memset(&source, 0, sizeof(source));
                source.fd   = fds[0];
                source.mode = INPUT_MODE_EVENTS;

                memset(buffer, 0, sizeof(buffer));
                ((x3tp_event_t *)(void *)buffer)->type  = X3TP_EVENT_CHAR;
                ((x3tp_event_t *)(void *)buffer)->value = 'z';
                assert(write(fds[1], buffer, 10) == 10);
                assert(input_read_events(&source, records, 4) == 0);
                assert(write(fds[1], buffer + 10, 24 - 10) == (ssize_t)(24 - 10));
                got = input_read_events(&source, records, 4);
                assert(got == 1);
                assert(records[0].type == X3TP_EVENT_CHAR && records[0].value == 'z');

                /* Unknown type is dropped, valid neighbours survive. */
                memset(buffer, 0, sizeof(buffer));
                ((x3tp_event_t *)(void *)buffer)->type = 99;
                ((x3tp_event_t *)(void *)(buffer + 24))->type = X3TP_EVENT_KEYDOWN;
                ((x3tp_event_t *)(void *)(buffer + 24))->value = X3TP_KEYCODE_UP;
                ((x3tp_event_t *)(void *)(buffer + 48))->type = X3TP_EVENT_KEYUP;
                ((x3tp_event_t *)(void *)(buffer + 48))->value = X3TP_KEYCODE_UP;
                assert(write(fds[1], buffer, 72) == 72);
                got = input_read_events(&source, records, 4);
                assert(got == 2);
                assert(records[0].type == X3TP_EVENT_KEYDOWN);
                assert(records[1].type == X3TP_EVENT_KEYUP);

                /* EOF is reported through the source, not as a short read. */
                close(fds[1]);
                got = input_read_events(&source, records, 4);
                assert(got == 0 && source.eof == 1);

                close(fds[0]);
                (void)i;
        }

        vt_destroy(vt);
        puts("test_x3tp_event: ok");
        return 0;
}
