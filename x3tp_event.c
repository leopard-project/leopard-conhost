/*
 * x3tp_event.c — /dev/console record decoding.
 */
#include "x3tp_event.h"

#include <string.h>

#include "input.h"
#include "vt.h"

static size_t utf8_sequence_length(uint8_t lead)
{
        if (lead < 0x80) return 1;
        if (lead >= 0xc2 && lead <= 0xdf) return 2;
        if (lead >= 0xe0 && lead <= 0xef) return 3;
        if (lead >= 0xf0 && lead <= 0xf4) return 4;
        return 0;
}

size_t x3tp_event_utf8(uint32_t value, char out[4])
{
        uint8_t bytes[4];
        size_t  count = 0;
        size_t  want;
        size_t  i;

        if (out == NULL) return 0;

        for (i = 0; i < 4; i++) {
                uint8_t byte = (uint8_t)((value >> (i * 8)) & 0xffu);
                if (byte == 0) break;
                bytes[count++] = byte;
        }

        if (count > 0) {
                want = utf8_sequence_length(bytes[0]);
                if (want == count) {
                        int valid = 1;
                        for (i = 1; i < count; i++) {
                                if ((bytes[i] & 0xc0u) != 0x80u) {
                                        valid = 0;
                                        break;
                                }
                        }
                        if (valid) {
                                memcpy(out, bytes, count);
                                return count;
                        }
                }
        }

        /* Compatibility: a producer that stores a Unicode scalar value instead
         * of the packed UTF-8 bytes still gets a valid sequence. */
        if (value != 0 && value <= 0x10ffffu &&
            !(value >= 0xd800u && value <= 0xdfffu)) {
                size_t len = vt_utf8_encode(value, out, 4);
                if (len != 0) return len;
        }
        return 0;
}

static int key_kind(uint32_t code)
{
        switch (code) {
        case X3TP_KEYCODE_ESC:        return INPUT_KEY_ESCAPE;
        case X3TP_KEYCODE_BACKSPACE:  return INPUT_KEY_BACKSPACE;
        case X3TP_KEYCODE_TAB:        return INPUT_KEY_TAB;
        case X3TP_KEYCODE_ENTER:      return INPUT_KEY_ENTER;
        case X3TP_KEYCODE_SPACE:      return INPUT_KEY_CHAR;
        case X3TP_KEYCODE_F1:         return INPUT_KEY_F1;
        case X3TP_KEYCODE_F2:         return INPUT_KEY_F2;
        case X3TP_KEYCODE_F3:         return INPUT_KEY_F3;
        case X3TP_KEYCODE_F4:         return INPUT_KEY_F4;
        case X3TP_KEYCODE_F5:         return INPUT_KEY_F5;
        case X3TP_KEYCODE_F6:         return INPUT_KEY_F6;
        case X3TP_KEYCODE_F7:         return INPUT_KEY_F7;
        case X3TP_KEYCODE_F8:         return INPUT_KEY_F8;
        case X3TP_KEYCODE_F9:         return INPUT_KEY_F9;
        case X3TP_KEYCODE_F10:        return INPUT_KEY_F10;
        case X3TP_KEYCODE_F11:        return INPUT_KEY_F11;
        case X3TP_KEYCODE_F12:        return INPUT_KEY_F12;
        case X3TP_KEYCODE_HOME:       return INPUT_KEY_HOME;
        case X3TP_KEYCODE_UP:         return INPUT_KEY_UP;
        case X3TP_KEYCODE_PAGE_UP:    return INPUT_KEY_PAGE_UP;
        case X3TP_KEYCODE_LEFT:       return INPUT_KEY_LEFT;
        case X3TP_KEYCODE_RIGHT:      return INPUT_KEY_RIGHT;
        case X3TP_KEYCODE_END:        return INPUT_KEY_END;
        case X3TP_KEYCODE_DOWN:       return INPUT_KEY_DOWN;
        case X3TP_KEYCODE_PAGE_DOWN:  return INPUT_KEY_PAGE_DOWN;
        case X3TP_KEYCODE_INSERT:     return INPUT_KEY_INSERT;
        case X3TP_KEYCODE_DELETE:     return INPUT_KEY_DELETE;
        case X3TP_KEYCODE_KEYPAD_0:        return INPUT_KEY_KEYPAD_0;
        case X3TP_KEYCODE_KEYPAD_1:        return INPUT_KEY_KEYPAD_1;
        case X3TP_KEYCODE_KEYPAD_2:        return INPUT_KEY_KEYPAD_2;
        case X3TP_KEYCODE_KEYPAD_3:        return INPUT_KEY_KEYPAD_3;
        case X3TP_KEYCODE_KEYPAD_4:        return INPUT_KEY_KEYPAD_4;
        case X3TP_KEYCODE_KEYPAD_5:        return INPUT_KEY_KEYPAD_5;
        case X3TP_KEYCODE_KEYPAD_6:        return INPUT_KEY_KEYPAD_6;
        case X3TP_KEYCODE_KEYPAD_7:        return INPUT_KEY_KEYPAD_7;
        case X3TP_KEYCODE_KEYPAD_8:        return INPUT_KEY_KEYPAD_8;
        case X3TP_KEYCODE_KEYPAD_9:        return INPUT_KEY_KEYPAD_9;
        case X3TP_KEYCODE_KEYPAD_DECIMAL:  return INPUT_KEY_KEYPAD_DECIMAL;
        case X3TP_KEYCODE_KEYPAD_DIVIDE:   return INPUT_KEY_KEYPAD_DIVIDE;
        case X3TP_KEYCODE_KEYPAD_MULTIPLY: return INPUT_KEY_KEYPAD_MULTIPLY;
        case X3TP_KEYCODE_KEYPAD_MINUS:    return INPUT_KEY_KEYPAD_MINUS;
        case X3TP_KEYCODE_KEYPAD_PLUS:     return INPUT_KEY_KEYPAD_PLUS;
        case X3TP_KEYCODE_KEYPAD_ENTER:    return INPUT_KEY_KEYPAD_ENTER;
        case X3TP_KEYCODE_KEYPAD_EQUALS:   return INPUT_KEY_KEYPAD_EQUALS;
        default:                       return -1;
        }
}

size_t x3tp_event_encode(const struct vt *vt, const x3tp_event_t *event,
                          char *out, size_t cap)
{
        size_t used;

        if (event == NULL || out == NULL || cap == 0) return 0;
        if (event->type == X3TP_EVENT_CHAR) {
                char   utf8[4];
                size_t len = x3tp_event_utf8(event->value, utf8);

                if (len == 0 || len > cap) return 0;

                /* PS/2 reports Ctrl+letter as the letter plus a CTRL flag; the
                 * terminal byte stream needs the C0 control instead. */
                if ((event->flags & X3TP_FLAG_CTRL) && len == 1) {
                        unsigned char ch = (unsigned char)utf8[0];

                        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 1);
                        else if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 1);
                        else if (ch == '[') ch = 0x1b;
                        else if (ch == '\\') ch = 0x1c;
                        else if (ch == ']') ch = 0x1d;
                        else if (ch == '^') ch = 0x1e;
                        else if (ch == '_') ch = 0x1f;
                        else if (ch == '?') ch = 0x7f;
                        else if (ch == '@' || ch == '2') ch = 0x00;
                        else if (ch == '6') ch = 0x1e;
                        else if (ch == ' ') ch = 0x00;
                        utf8[0] = (char)ch;
                }

                memcpy(out, utf8, len);
                return len;
        }
        if (event->type != X3TP_EVENT_KEYDOWN)
                return 0; /* key releases produce no terminal bytes */

        if (event->value <= 0x7f) {
                uint32_t code = event->value;
                input_key_t key;

                /* Printable key codes are a compatibility fallback for
                 * byte-oriented sources; real text arrives as CHAR records. */
                switch (code) {
                case 0x08: key.kind = INPUT_KEY_BACKSPACE; break;
                case 0x09: key.kind = INPUT_KEY_TAB; break;
                case 0x0a:
                case 0x0d: key.kind = INPUT_KEY_ENTER; break;
                case 0x1b: key.kind = INPUT_KEY_ESCAPE; break;
                default:
                        if (code < 0x20u || code == 0x7fu) return 0;
                        key.kind = INPUT_KEY_CHAR;
                        key.ch   = code;
                        break;
                }
                if (key.kind != INPUT_KEY_CHAR) key.ch = 0;
                return input_encode_key(vt, &key, out, cap);
        }

        {
                int         kind = key_kind(event->value);
                input_key_t key;

                if (kind < 0) return 0;
                if (event->value == X3TP_KEYCODE_TAB &&
                    (event->flags & X3TP_FLAG_SHIFT)) {
                        if (cap < 3) return 0;
                        memcpy(out, "\x1b[Z", 3);
                        return 3;
                }
                key.kind = (input_key_kind_t)kind;
                key.ch   = (kind == INPUT_KEY_CHAR) ? 0x20u : 0;
                used     = input_encode_key(vt, &key, out, cap);
                return used;
        }
}

const char *x3tp_keycode_name(uint32_t code)
{
        static const char *const names[] = {
                "Esc", "Backspace", "Tab", "Enter", "CapsLock", "Shift",
                "Ctrl", "Alt", "Space", "F1", "F2", "F3", "F4", "F5", "F6",
                "F7", "F8", "F9", "F10", "F11", "F12", "NumLock", "ScrollLock",
                "Home", "Up", "PageUp", "Left", "Right", "End", "Down",
                "PageDown", "Insert", "Delete", "KP0", "KP1", "KP2", "KP3",
                "KP4", "KP5", "KP6", "KP7", "KP8", "KP9", "KPDecimal",
                "KPDivide", "KPMultiply", "KPMinus", "KPPlus", "KPEnter",
                "KPEquals",
        };

        if (code < X3TP_KEYCODE_ESC || code > X3TP_KEYCODE_MAX) return NULL;
        return names[code - X3TP_KEYCODE_ESC];
}
