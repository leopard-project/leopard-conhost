/*
 * input.h — keyboard input boundary.
 *
 * The primary source is the /dev/console event endpoint (24-byte
 * x3tp_event_t records).  A raw byte source (a seat device or an explicit
 * $CONHOST_INPUT path) is still supported so non-event endpoints can be
 * written straight to the PTY.  Key encoding is centralised here so the
 * event boundary does not leak into the pty/vt code.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "x3tp_event.h"

#define INPUT_STAGING_EVENTS 32

typedef struct vt vt_t;
typedef struct input_source {
        int           fd;
        int           mode;      /* INPUT_MODE_BYTES / INPUT_MODE_EVENTS */
        int           eof;
        size_t        staged_len;
        uint8_t       staging[X3TP_EVENT_SIZE * INPUT_STAGING_EVENTS];
} input_source_t;

enum {
        INPUT_MODE_BYTES  = 0,
        INPUT_MODE_EVENTS = 1,
};

/* Try /dev/console first (structured records), then the seat endpoints or
 * $CONHOST_INPUT (raw bytes).  Returns 0 when a source was opened, -1 when no
 * keyboard source is available. */
int  input_open(input_source_t *input);

int  input_fd(const input_source_t *input);
int  input_is_event_source(const input_source_t *input);

/* Non-blocking read.  Returns >0 bytes, 0 on EOF, -1 when no data is ready. */
int  input_read(input_source_t *input, void *buffer, size_t capacity);

/* Non-blocking structured read.  Returns the number of complete records
 * written to events (0 when no complete record is ready).  Sets input->eof on
 * EOF or a hard read error.  Never touches partial records. */
size_t input_read_events(input_source_t *input, x3tp_event_t *events, size_t max);

void input_close(input_source_t *input);

typedef enum {
        INPUT_KEY_CHAR = 0,
        INPUT_KEY_ENTER,
        INPUT_KEY_TAB,
        INPUT_KEY_BACKSPACE,
        INPUT_KEY_ESCAPE,
        INPUT_KEY_UP,
        INPUT_KEY_DOWN,
        INPUT_KEY_RIGHT,
        INPUT_KEY_LEFT,
        INPUT_KEY_HOME,
        INPUT_KEY_END,
        INPUT_KEY_INSERT,
        INPUT_KEY_DELETE,
        INPUT_KEY_PAGE_UP,
        INPUT_KEY_PAGE_DOWN,
        INPUT_KEY_F1,
        INPUT_KEY_F2,
        INPUT_KEY_F3,
        INPUT_KEY_F4,
        INPUT_KEY_F5,
        INPUT_KEY_F6,
        INPUT_KEY_F7,
        INPUT_KEY_F8,
        INPUT_KEY_F9,
        INPUT_KEY_F10,
        INPUT_KEY_F11,
        INPUT_KEY_F12,
        INPUT_KEY_KEYPAD_0,
        INPUT_KEY_KEYPAD_1,
        INPUT_KEY_KEYPAD_2,
        INPUT_KEY_KEYPAD_3,
        INPUT_KEY_KEYPAD_4,
        INPUT_KEY_KEYPAD_5,
        INPUT_KEY_KEYPAD_6,
        INPUT_KEY_KEYPAD_7,
        INPUT_KEY_KEYPAD_8,
        INPUT_KEY_KEYPAD_9,
        INPUT_KEY_KEYPAD_DECIMAL,
        INPUT_KEY_KEYPAD_DIVIDE,
        INPUT_KEY_KEYPAD_MULTIPLY,
        INPUT_KEY_KEYPAD_MINUS,
        INPUT_KEY_KEYPAD_PLUS,
        INPUT_KEY_KEYPAD_ENTER,
        INPUT_KEY_KEYPAD_EQUALS,
} input_key_kind_t;

typedef struct input_key {
        input_key_kind_t kind;
        uint32_t         ch; /* used when kind == INPUT_KEY_CHAR */
} input_key_t;

/* Encode a structured key according to DECCKM / application-keypad flags.
 * Returns bytes written, or 0 when the output buffer is too small. */
size_t input_encode_key(const vt_t *vt, const input_key_t *key, char *out, size_t cap);
