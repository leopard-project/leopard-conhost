/*
 * x3tp_event.h — structured console input records (/dev/console).
 *
 * The kernel input endpoint exports fixed 24-byte records; conhost is the only
 * consumer that turns them back into terminal byte streams.  Keeping the record
 * layout and the key-code contract in one header makes the userspace/kernel
 * boundary explicit and testable.
 *
 * Record layout (little-endian, no padding):
 *
 *   uint32_t type;      X3TP_EVENT_*
 *   uint32_t value;     CHAR: packed UTF-8; KEY: key code
 *   uint32_t flags;     reserved for modifiers/repeat (ignored for now)
 *   uint32_t reserved;  must be ignored
 *   uint64_t sequence;  producer monotonic sequence (ignored by conhost)
 *
 * Printable text is always delivered as X3TP_EVENT_CHAR.  KEYDOWN/KEYUP
 * carry non-text keys only, using the key codes below (the kernel's
 * include/ps2/keyboard.h special_key_code enum, plus the keypad extension).
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#define X3TP_EVENT_CHAR    1
#define X3TP_EVENT_KEYDOWN 2
#define X3TP_EVENT_KEYUP   3

typedef struct x3tp_event {
        uint32_t type;
        uint32_t value;
        uint32_t flags;
        uint32_t reserved;
        uint64_t sequence;
} x3tp_event_t;

#define X3TP_EVENT_SIZE 24u
_Static_assert(sizeof(x3tp_event_t) == X3TP_EVENT_SIZE,
               "x3tp_event_t must stay 24 bytes (/dev/console read ABI)");

/* Modifier bits, matching leopard-xsk/include/x3tp.h exactly.  These
 * are informational: text always arrives as X3TP_EVENT_CHAR, and conhost only
 * uses CTRL to detect the IME toggle chord. */
#define X3TP_FLAG_SHIFT  (1u << 0)
#define X3TP_FLAG_CTRL   (1u << 1)
#define X3TP_FLAG_ALT    (1u << 2)
#define X3TP_FLAG_WIN    (1u << 3)
#define X3TP_FLAG_CAPS   (1u << 4)
#define X3TP_FLAG_NUM    (1u << 5)
#define X3TP_FLAG_SCROLL (1u << 6)

/* Key codes for X3TP_EVENT_KEYDOWN/KEYUP.
 *
 * Values 128..160 mirror leopard-xsk/include/ps2/keyboard.h
 * (enum special_key_code), which is what X3TP producers emit for
 * non-text keys.  161..177 are a conhost-side keypad extension; the current kernel
 * has no keypad key codes (numeric keypad arrives as CHAR digits), so those
 * entries are accepted but unreachable until the kernel appends them.
 * Printable characters must not be sent as key codes. */
#define X3TP_KEYCODE_ESC        128
#define X3TP_KEYCODE_BACKSPACE  129
#define X3TP_KEYCODE_TAB        130
#define X3TP_KEYCODE_ENTER      131
#define X3TP_KEYCODE_CAPS       132
#define X3TP_KEYCODE_SHIFT      133
#define X3TP_KEYCODE_CTRL       134
#define X3TP_KEYCODE_ALT        135
#define X3TP_KEYCODE_SPACE      136
#define X3TP_KEYCODE_F1         137
#define X3TP_KEYCODE_F2         138
#define X3TP_KEYCODE_F3         139
#define X3TP_KEYCODE_F4         140
#define X3TP_KEYCODE_F5         141
#define X3TP_KEYCODE_F6         142
#define X3TP_KEYCODE_F7         143
#define X3TP_KEYCODE_F8         144
#define X3TP_KEYCODE_F9         145
#define X3TP_KEYCODE_F10        146
#define X3TP_KEYCODE_F11        147
#define X3TP_KEYCODE_F12        148
#define X3TP_KEYCODE_NUML       149
#define X3TP_KEYCODE_SCROLL     150
#define X3TP_KEYCODE_HOME       151
#define X3TP_KEYCODE_UP         152
#define X3TP_KEYCODE_PAGE_UP    153
#define X3TP_KEYCODE_LEFT       154
#define X3TP_KEYCODE_RIGHT      155
#define X3TP_KEYCODE_END        156
#define X3TP_KEYCODE_DOWN       157
#define X3TP_KEYCODE_PAGE_DOWN  158
#define X3TP_KEYCODE_INSERT     159
#define X3TP_KEYCODE_DELETE     160

#define X3TP_KEYCODE_KEYPAD_0        161
#define X3TP_KEYCODE_KEYPAD_1        162
#define X3TP_KEYCODE_KEYPAD_2        163
#define X3TP_KEYCODE_KEYPAD_3        164
#define X3TP_KEYCODE_KEYPAD_4        165
#define X3TP_KEYCODE_KEYPAD_5        166
#define X3TP_KEYCODE_KEYPAD_6        167
#define X3TP_KEYCODE_KEYPAD_7        168
#define X3TP_KEYCODE_KEYPAD_8        169
#define X3TP_KEYCODE_KEYPAD_9        170
#define X3TP_KEYCODE_KEYPAD_DECIMAL  171
#define X3TP_KEYCODE_KEYPAD_DIVIDE   172
#define X3TP_KEYCODE_KEYPAD_MULTIPLY 173
#define X3TP_KEYCODE_KEYPAD_MINUS    174
#define X3TP_KEYCODE_KEYPAD_PLUS     175
#define X3TP_KEYCODE_KEYPAD_ENTER    176
#define X3TP_KEYCODE_KEYPAD_EQUALS   177
#define X3TP_KEYCODE_MAX             177

/* Encode one event as terminal bytes.  Honours DECCKM and application-keypad
 * state from the VT.  Returns bytes written, or 0 for events that produce no
 * output (key releases, unknown codes) or when the buffer is too small. */
struct vt;
size_t x3tp_event_encode(const struct vt *vt, const x3tp_event_t *event,
                          char *out, size_t cap);

/* Decode packed UTF-8 from an X3TP_EVENT_CHAR value.  Returns byte count
 * (1..4).  As a compatibility fallback a bare Unicode scalar value is accepted
 * when the packed form is not valid UTF-8; returns 0 when neither form works. */
size_t x3tp_event_utf8(uint32_t value, char out[4]);

/* Stable key code -> name, for tests and diagnostics.  Never returns key text. */
const char *x3tp_keycode_name(uint32_t code);
