/*
 * vt.c — minimal VT/ANSI terminal emulator.
 *
 * Supported enough for line-oriented shells and full-screen editors:
 *   - UTF-8 input, CR/LF/BS/TAB, DEL
 *   - CSI cursor motion A/B/C/D/E/F/G/H/f/d/e/a/` and save/restore
 *   - erase J/K/X, insert/delete chars @/P, insert/delete lines L/M
 *   - scroll region r, scroll S/T, index/reverse-index
 *   - SGR colors/attributes including 256-color and truecolor
 *   - cursor visibility, alternate screen, DECCKM/keypad flags
 *   - DEC special graphics (rendered as ASCII box-drawing approximations)
 *
 * The parser is deliberately byte-oriented: invalid UTF-8 becomes U+FFFD and
 * is never re-emitted as a different byte sequence.  Wide characters occupy
 * two cells (lead + trail) so a later renderer can skip the trail cell.
 */
#include "vt.h"

#include <stdlib.h>
#include <string.h>

#define VT_RESPONSE_CAP 256

enum {
        VT_STATE_GROUND = 0,
        VT_STATE_ESC,
        VT_STATE_CSI,
        VT_STATE_OSC,
        VT_STATE_OSC_ESC,
        VT_STATE_STRING,
        VT_STATE_STRING_ESC,
        VT_STATE_CHARSET,
        VT_STATE_HASH,
};

enum {
        VT_CHARSET_ASCII = 0,
        VT_CHARSET_DEC_GRAPHICS = 1,
};

struct vt {
        int rows;
        int cols;

        vt_cell_t *main_cells;
        vt_cell_t *alt_cells;
        vt_cell_t *cells;
        int        alt_active;

        int cursor_x;
        int cursor_y;
        int cursor_visible;
        int autowrap;
        int origin_mode;
        int insert_mode;
        int newline_mode;

        int decckm;
        int keypad_app;

        int scroll_top;
        int scroll_bottom;

        uint32_t default_fg;
        uint32_t default_bg;
        uint32_t fg;
        uint32_t bg;
        uint8_t  attr;

        int      saved_x;
        int      saved_y;
        uint32_t saved_fg;
        uint32_t saved_bg;
        uint8_t  saved_attr;
        int      saved_origin;
        uint8_t  saved_charset[2];
        int      saved_active_charset;

        uint8_t charset[2];
        int     active_charset;

        uint32_t last_char;
        int      last_width;
        int      wrap_pending;

        int      dirty_all;
        uint8_t *dirty;

        int state;
        int charset_target;
        int csi_params[VT_MAX_PARAMS];
        int csi_count;
        int csi_cur;
        int csi_cur_valid;
        int csi_private;

        int      utf8_need;
        uint32_t utf8_cp;
        uint32_t utf8_min;

        char           response[VT_RESPONSE_CAP];
        size_t         response_len;
        vt_response_fn response_fn;
        void          *response_ctx;
};

static void vt_mark_row(vt_t *vt, int row);
static void vt_mark_all(vt_t *vt);
static void vt_scroll_up(vt_t *vt, int top, int bottom, int count);
static void vt_scroll_down(vt_t *vt, int top, int bottom, int count);
static void vt_fill_cells(vt_t *vt, int r0, int r1, int c0, int c1);
static void vt_erase_display(vt_t *vt, int mode);
static void vt_erase_line(vt_t *vt, int mode);
static void vt_parse_byte(vt_t *vt, uint8_t byte);
static void vt_csi_dispatch(vt_t *vt, uint8_t final);
static void vt_set_cursor(vt_t *vt, int row, int col);
static void vt_save_cursor(vt_t *vt);
static void vt_restore_cursor(vt_t *vt);

static int vt_clampi(int value, int lo, int hi)
{
        if (value < lo) return lo;
        if (value > hi) return hi;
        return value;
}

static vt_cell_t vt_blank_cell(const vt_t *vt)
{
        vt_cell_t cell;

        cell.ch    = 0;
        cell.fg    = vt->default_fg;
        cell.bg    = vt->bg;
        cell.attr  = 0;
        cell.width = VT_WIDTH_NARROW;
        return cell;
}

static vt_cell_t *vt_cell_mut(vt_t *vt, int row, int col)
{
        return &vt->cells[(size_t)row * (size_t)vt->cols + (size_t)col];
}

static void vt_mark_row(vt_t *vt, int row)
{
        if (row >= 0 && row < vt->rows) vt->dirty[row] = 1;
}

static void vt_mark_all(vt_t *vt)
{
        vt->dirty_all = 1;
}

static void vt_fill_cells(vt_t *vt, int r0, int r1, int c0, int c1)
{
        vt_cell_t blank = vt_blank_cell(vt);
        int       row;
        int       col;

        if (r0 < 0) r0 = 0;
        if (c0 < 0) c0 = 0;
        if (r1 > vt->rows) r1 = vt->rows;
        if (c1 > vt->cols) c1 = vt->cols;

        for (row = r0; row < r1; row++) {
                for (col = c0; col < c1; col++)
                        *vt_cell_mut(vt, row, col) = blank;
                vt_mark_row(vt, row);
        }
}

static void vt_clear_screen(vt_t *vt)
{
        vt_fill_cells(vt, 0, vt->rows, 0, vt->cols);
}

static size_t vt_utf8_len(uint32_t cp)
{
        if (cp < 0x80) return 1;
        if (cp < 0x800) return 2;
        if (cp < 0x10000) return 3;
        if (cp <= 0x10ffff) return 4;
        return 0;
}

size_t vt_utf8_encode(uint32_t cp, char *out, size_t cap)
{
        size_t len = vt_utf8_len(cp);

        if (out == NULL || len == 0 || cap < len) return 0;

        if (len == 1) {
                out[0] = (char)cp;
        } else if (len == 2) {
                out[0] = (char)(0xc0 | (cp >> 6));
                out[1] = (char)(0x80 | (cp & 0x3f));
        } else if (len == 3) {
                out[0] = (char)(0xe0 | (cp >> 12));
                out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                out[2] = (char)(0x80 | (cp & 0x3f));
        } else {
                out[0] = (char)(0xf0 | (cp >> 18));
                out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
                out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
                out[3] = (char)(0x80 | (cp & 0x3f));
        }
        return len;
}

static int vt_char_width(uint32_t cp)
{
        if (cp == 0) return 0;

        /* Combining marks occupy no cell. */
        if ((cp >= 0x0300 && cp <= 0x036f) ||
            (cp >= 0x1ab0 && cp <= 0x1aff) ||
            (cp >= 0x1dc0 && cp <= 0x1dff) ||
            (cp >= 0x20d0 && cp <= 0x20ff) ||
            (cp >= 0xfe20 && cp <= 0xfe2f))
                return 0;

        /* Wide East Asian / emoji ranges. */
        if ((cp >= 0x1100 && cp <= 0x115f) ||
            (cp >= 0x2e80 && cp <= 0xa4cf) ||
            (cp >= 0xac00 && cp <= 0xd7a3) ||
            (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0xfe10 && cp <= 0xfe19) ||
            (cp >= 0xfe30 && cp <= 0xfe6f) ||
            (cp >= 0xff00 && cp <= 0xff60) ||
            (cp >= 0xffe0 && cp <= 0xffe6) ||
            (cp >= 0x1f300 && cp <= 0x1f64f) ||
            (cp >= 0x1f900 && cp <= 0x1f9ff) ||
            (cp >= 0x20000 && cp <= 0x3fffd))
                return 2;

        return 1;
}

static uint32_t vt_dec_graphics(uint32_t cp)
{
        switch (cp) {
        case '`': return '+';
        case 'a': return '#';
        case 'b': return '#';
        case 'c': return '#';
        case 'd': return '#';
        case 'e': return '#';
        case 'f': return 'o';
        case 'g': return '+';
        case 'h': return '#';
        case 'i': return '#';
        case 'j': return '+';
        case 'k': return '+';
        case 'l': return '+';
        case 'm': return '+';
        case 'n': return '+';
        case 'o': return '-';
        case 'p': return '-';
        case 'q': return '-';
        case 'r': return '-';
        case 's': return '-';
        case 't': return '+';
        case 'u': return '+';
        case 'v': return '+';
        case 'w': return '+';
        case 'x': return '|';
        case 'y': return '<';
        case 'z': return '>';
        case '{': return 'p';
        case '|': return '!';
        case '}': return '#';
        case '~': return 'o';
        default:  return cp;
        }
}

static void vt_set_cursor(vt_t *vt, int row, int col)
{
        int old_row = vt->cursor_y;

        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (row >= vt->rows) row = vt->rows - 1;
        if (col >= vt->cols) col = vt->cols - 1;

        vt->cursor_y   = row;
        vt->cursor_x   = col;
        vt->wrap_pending = 0;

        if (vt->cursor_visible) {
                vt_mark_row(vt, old_row);
                vt_mark_row(vt, row);
        }
}

static void vt_save_cursor(vt_t *vt)
{
        vt->saved_x              = vt->cursor_x;
        vt->saved_y              = vt->cursor_y;
        vt->saved_fg             = vt->fg;
        vt->saved_bg             = vt->bg;
        vt->saved_attr           = vt->attr;
        vt->saved_origin         = vt->origin_mode;
        vt->saved_charset[0]     = vt->charset[0];
        vt->saved_charset[1]     = vt->charset[1];
        vt->saved_active_charset = vt->active_charset;
}

static void vt_restore_cursor(vt_t *vt)
{
        vt->fg             = vt->saved_fg;
        vt->bg             = vt->saved_bg;
        vt->attr           = vt->saved_attr;
        vt->origin_mode    = vt->saved_origin;
        vt->charset[0]     = vt->saved_charset[0];
        vt->charset[1]     = vt->saved_charset[1];
        vt->active_charset = vt->saved_active_charset;
        vt_set_cursor(vt, vt->saved_y, vt->saved_x);
}

static void vt_index_down(vt_t *vt)
{
        vt->wrap_pending = 0;
        if (vt->cursor_y == vt->scroll_bottom) {
                vt_scroll_up(vt, vt->scroll_top, vt->scroll_bottom, 1);
                vt_mark_row(vt, vt->cursor_y);
        } else if (vt->cursor_y < vt->rows - 1) {
                int old = vt->cursor_y;
                vt->cursor_y++;
                if (vt->cursor_visible) {
                        vt_mark_row(vt, old);
                        vt_mark_row(vt, vt->cursor_y);
                }
        }
}

static void vt_index_up(vt_t *vt)
{
        vt->wrap_pending = 0;
        if (vt->cursor_y == vt->scroll_top) {
                vt_scroll_down(vt, vt->scroll_top, vt->scroll_bottom, 1);
                vt_mark_row(vt, vt->cursor_y);
        } else if (vt->cursor_y > 0) {
                int old = vt->cursor_y;
                vt->cursor_y--;
                if (vt->cursor_visible) {
                        vt_mark_row(vt, old);
                        vt_mark_row(vt, vt->cursor_y);
                }
        }
}

static void vt_clear_wide_at(vt_t *vt, int row, int col)
{
        vt_cell_t *cell;

        if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols) return;
        cell = vt_cell_mut(vt, row, col);
        if (cell->width == VT_WIDTH_WIDE) {
                if (col + 1 < vt->cols) {
                        vt_cell_t *next = vt_cell_mut(vt, row, col + 1);
                        next->ch    = 0;
                        next->width = VT_WIDTH_NARROW;
                }
        } else if (cell->width == VT_WIDTH_TRAIL) {
                if (col > 0) {
                        vt_cell_t *prev = vt_cell_mut(vt, row, col - 1);
                        prev->ch    = 0;
                        prev->width = VT_WIDTH_NARROW;
                }
        }
}

static void vt_put_codepoint(vt_t *vt, uint32_t cp)
{
        int width;
        int row;
        int col;
        vt_cell_t *cell;

        if (cp == 0) return;
        width = vt_char_width(cp);
        if (width <= 0) return; /* combining marks are intentionally dropped */

        if (vt->wrap_pending) {
                if (vt->autowrap) {
                        vt->cursor_x = 0;
                        vt_index_down(vt);
                }
                vt->wrap_pending = 0;
        }

        if (width == 2 && vt->cursor_x >= vt->cols - 1) {
                if (vt->autowrap) {
                        vt->cursor_x = 0;
                        vt_index_down(vt);
                } else {
                        width = 1;
                }
        }

        row = vt->cursor_y;
        col = vt->cursor_x;

        if (vt->insert_mode) {
                size_t move = (size_t)(vt->cols - col - width) * sizeof(vt_cell_t);
                if (move > 0)
                        memmove(vt_cell_mut(vt, row, col + width),
                                vt_cell_mut(vt, row, col), move);
                vt_fill_cells(vt, row, row + 1, col, col + width);
        }

        vt_clear_wide_at(vt, row, col);
        if (width == 2) vt_clear_wide_at(vt, row, col + 1);

        cell        = vt_cell_mut(vt, row, col);
        cell->ch    = cp;
        cell->fg    = vt->fg;
        cell->bg    = vt->bg;
        cell->attr  = vt->attr;
        cell->width = (width == 2) ? VT_WIDTH_WIDE : VT_WIDTH_NARROW;

        if (width == 2) {
                vt_cell_t *trail = vt_cell_mut(vt, row, col + 1);
                trail->ch    = 0;
                trail->fg    = vt->fg;
                trail->bg    = vt->bg;
                trail->attr  = vt->attr;
                trail->width = VT_WIDTH_TRAIL;
        }

        vt->last_char  = cp;
        vt->last_width = width;
        vt_mark_row(vt, row);

        vt->cursor_x += width;
        if (vt->cursor_x >= vt->cols) {
                vt->cursor_x = vt->cols - 1;
                if (vt->autowrap) vt->wrap_pending = 1;
        }
}

static void vt_put_ascii(vt_t *vt, uint32_t cp)
{
        if (vt->charset[vt->active_charset] == VT_CHARSET_DEC_GRAPHICS)
                cp = vt_dec_graphics(cp);
        vt_put_codepoint(vt, cp);
}

static void vt_scroll_up(vt_t *vt, int top, int bottom, int count)
{
        int height;
        int row;

        if (top < 0) top = 0;
        if (bottom >= vt->rows) bottom = vt->rows - 1;
        if (top > bottom) return;

        height = bottom - top + 1;
        if (count <= 0) return;
        if (count > height) count = height;

        if (height > count) {
                memmove(vt_cell_mut(vt, top, 0),
                        vt_cell_mut(vt, top + count, 0),
                        (size_t)(height - count) * (size_t)vt->cols * sizeof(vt_cell_t));
        }
        vt_fill_cells(vt, bottom - count + 1, bottom + 1, 0, vt->cols);
        for (row = top; row <= bottom; row++) vt_mark_row(vt, row);
}

static void vt_scroll_down(vt_t *vt, int top, int bottom, int count)
{
        int height;
        int row;

        if (top < 0) top = 0;
        if (bottom >= vt->rows) bottom = vt->rows - 1;
        if (top > bottom) return;

        height = bottom - top + 1;
        if (count <= 0) return;
        if (count > height) count = height;

        if (height > count) {
                memmove(vt_cell_mut(vt, top + count, 0),
                        vt_cell_mut(vt, top, 0),
                        (size_t)(height - count) * (size_t)vt->cols * sizeof(vt_cell_t));
        }
        vt_fill_cells(vt, top, top + count, 0, vt->cols);
        for (row = top; row <= bottom; row++) vt_mark_row(vt, row);
}

static void vt_erase_line(vt_t *vt, int mode)
{
        int row = vt->cursor_y;

        if (mode == 0) {
                vt_fill_cells(vt, row, row + 1, vt->cursor_x, vt->cols);
        } else if (mode == 1) {
                vt_fill_cells(vt, row, row + 1, 0, vt->cursor_x + 1);
        } else if (mode == 2) {
                vt_fill_cells(vt, row, row + 1, 0, vt->cols);
        }
}

static void vt_erase_display(vt_t *vt, int mode)
{
        int row = vt->cursor_y;

        if (mode == 0) {
                vt_fill_cells(vt, row, row + 1, vt->cursor_x, vt->cols);
                vt_fill_cells(vt, row + 1, vt->rows, 0, vt->cols);
        } else if (mode == 1) {
                vt_fill_cells(vt, 0, row, 0, vt->cols);
                vt_fill_cells(vt, row, row + 1, 0, vt->cursor_x + 1);
        } else if (mode == 2 || mode == 3) {
                vt_clear_screen(vt);
        }
}

static uint32_t vt_ansi_color(int index)
{
        static const uint32_t base[16] = {
                0x000000, 0x800000, 0x008000, 0x808000,
                0x000080, 0x800080, 0x008080, 0xc0c0c0,
                0x808080, 0xff0000, 0x00ff00, 0xffff00,
                0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
        };

        if (index < 0) index = 0;
        if (index < 16) return base[index];

        if (index < 232) {
                int     n = index - 16;
                uint32_t r = (uint32_t)(n / 36);
                uint32_t g = (uint32_t)((n / 6) % 6);
                uint32_t b = (uint32_t)(n % 6);
                static const uint32_t level[6] = { 0, 95, 135, 175, 215, 255 };
                return (level[r] << 16) | (level[g] << 8) | level[b];
        }

        if (index < 256) {
                uint32_t v = (uint32_t)(8 + (index - 232) * 10);
                return (v << 16) | (v << 8) | v;
        }
        return 0xffffff;
}

static void vt_sgr(vt_t *vt)
{
        int i;

        if (vt->csi_count == 0) {
                vt->attr = 0;
                vt->fg   = vt->default_fg;
                vt->bg   = vt->default_bg;
                return;
        }

        for (i = 0; i < vt->csi_count; i++) {
                int n = vt->csi_params[i];

                if (n == 0) {
                        vt->attr = 0;
                        vt->fg   = vt->default_fg;
                        vt->bg   = vt->default_bg;
                } else if (n == 1) {
                        vt->attr |= VT_ATTR_BOLD;
                } else if (n == 2) {
                        vt->attr |= VT_ATTR_DIM;
                } else if (n == 3) {
                        vt->attr |= VT_ATTR_ITALIC;
                } else if (n == 4) {
                        vt->attr |= VT_ATTR_UNDERLINE;
                } else if (n == 5 || n == 6) {
                        vt->attr |= VT_ATTR_BLINK;
                } else if (n == 7) {
                        vt->attr |= VT_ATTR_REVERSE;
                } else if (n == 8) {
                        vt->attr |= VT_ATTR_INVISIBLE;
                } else if (n == 9) {
                        vt->attr |= VT_ATTR_STRIKE;
                } else if (n == 21 || n == 22) {
                        vt->attr &= (uint8_t)~(VT_ATTR_BOLD | VT_ATTR_DIM);
                } else if (n == 23) {
                        vt->attr &= (uint8_t)~VT_ATTR_ITALIC;
                } else if (n == 24) {
                        vt->attr &= (uint8_t)~VT_ATTR_UNDERLINE;
                } else if (n == 25) {
                        vt->attr &= (uint8_t)~VT_ATTR_BLINK;
                } else if (n == 27) {
                        vt->attr &= (uint8_t)~VT_ATTR_REVERSE;
                } else if (n == 28) {
                        vt->attr &= (uint8_t)~VT_ATTR_INVISIBLE;
                } else if (n == 29) {
                        vt->attr &= (uint8_t)~VT_ATTR_STRIKE;
                } else if (n >= 30 && n <= 37) {
                        vt->fg = vt_ansi_color(n - 30);
                } else if (n == 39) {
                        vt->fg = vt->default_fg;
                } else if (n >= 40 && n <= 47) {
                        vt->bg = vt_ansi_color(n - 40);
                } else if (n == 49) {
                        vt->bg = vt->default_bg;
                } else if (n >= 90 && n <= 97) {
                        vt->fg = vt_ansi_color(n - 90 + 8);
                } else if (n >= 100 && n <= 107) {
                        vt->bg = vt_ansi_color(n - 100 + 8);
                } else if (n == 38 || n == 48) {
                        uint32_t color = 0;
                        int      have  = 0;

                        if (i + 2 < vt->csi_count && vt->csi_params[i + 1] == 5) {
                                color = vt_ansi_color(vt->csi_params[i + 2]);
                                have  = 1;
                                i += 2;
                        } else if (i + 4 < vt->csi_count && vt->csi_params[i + 1] == 2) {
                                uint32_t r = (uint32_t)vt_clampi(vt->csi_params[i + 2], 0, 255);
                                uint32_t g = (uint32_t)vt_clampi(vt->csi_params[i + 3], 0, 255);
                                uint32_t b = (uint32_t)vt_clampi(vt->csi_params[i + 4], 0, 255);
                                color = (r << 16) | (g << 8) | b;
                                have  = 1;
                                i += 4;
                        }

                        if (have) {
                                if (n == 38) vt->fg = color;
                                else vt->bg = color;
                        }
                }
        }
}

static int vt_csi_param(vt_t *vt, int index, int fallback)
{
        if (index < 0 || index >= vt->csi_count) return fallback;
        return vt->csi_params[index];
}

static int vt_csi_count_param(vt_t *vt, int index, int fallback)
{
        int value = vt_csi_param(vt, index, fallback);
        return value == 0 ? fallback : value;
}

static void vt_respond_bytes(vt_t *vt, const char *bytes, size_t len)
{
        size_t room;

        if (bytes == NULL || len == 0) return;
        room = VT_RESPONSE_CAP - vt->response_len;
        if (len > room) len = room;
        memcpy(vt->response + vt->response_len, bytes, len);
        vt->response_len += len;
}

static void vt_respond_char(vt_t *vt, char ch)
{
        vt_respond_bytes(vt, &ch, 1);
}

static void vt_respond_int(vt_t *vt, int value)
{
        char digits[12];
        int  count = 0;

        if (value < 0) value = 0;
        if (value == 0) {
                vt_respond_char(vt, '0');
                return;
        }
        while (value > 0 && count < (int)sizeof(digits)) {
                digits[count++] = (char)('0' + (value % 10));
                value /= 10;
        }
        while (count > 0) vt_respond_char(vt, digits[--count]);
}

static void vt_respond_cursor(vt_t *vt, int private_reply)
{
        vt_respond_bytes(vt, private_reply ? "\x1b[?" : "\x1b[", private_reply ? 3 : 2);
        vt_respond_int(vt, vt->cursor_y + 1);
        vt_respond_char(vt, ';');
        vt_respond_int(vt, vt->cursor_x + 1);
        vt_respond_char(vt, 'R');
}

static void vt_set_alt_screen(vt_t *vt, int on, int save_cursor, int clear)
{
        if (on) {
                if (vt->alt_active) {
                        if (clear) vt_clear_screen(vt);
                        return;
                }
                if (save_cursor) vt_save_cursor(vt);
                vt->alt_active = 1;
                vt->cells      = vt->alt_cells;
                if (clear) vt_clear_screen(vt);
                vt->cursor_x     = 0;
                vt->cursor_y     = 0;
                vt->wrap_pending = 0;
                vt->scroll_top   = 0;
                vt->scroll_bottom = vt->rows - 1;
                vt_mark_all(vt);
        } else {
                if (!vt->alt_active) return;
                vt->alt_active = 0;
                vt->cells      = vt->main_cells;
                vt->scroll_top = 0;
                vt->scroll_bottom = vt->rows - 1;
                if (save_cursor) vt_restore_cursor(vt);
                vt->wrap_pending = 0;
                vt_mark_all(vt);
        }
}

static void vt_set_mode(vt_t *vt, int mode, int on, int private_mode)
{
        if (private_mode == 1) {
                switch (mode) {
                case 1:
                        vt->decckm = on;
                        break;
                case 6:
                        vt->origin_mode = on;
                        vt_set_cursor(vt, on ? vt->scroll_top : 0, 0);
                        break;
                case 7:
                        vt->autowrap = on;
                        break;
                case 25:
                        if (vt->cursor_visible != on) {
                                vt->cursor_visible = on;
                                vt_mark_row(vt, vt->cursor_y);
                        }
                        break;
                case 47:
                case 1047:
                        vt_set_alt_screen(vt, on, 0, 1);
                        break;
                case 1048:
                        if (on) vt_save_cursor(vt);
                        else vt_restore_cursor(vt);
                        break;
                case 1049:
                        vt_set_alt_screen(vt, on, 1, 1);
                        break;
                default:
                        /* Mouse tracking, bracketed paste and focus reporting
                         * are accepted but not implemented by this input path. */
                        break;
                }
                return;
        }

        switch (mode) {
        case 4:
                vt->insert_mode = on;
                break;
        case 20:
                vt->newline_mode = on;
                break;
        default:
                break;
        }
}

static void vt_csi_dispatch(vt_t *vt, uint8_t final)
{
        int n;
        int row;
        int col;

        switch (final) {
        case 'A':
                vt_set_cursor(vt, vt->cursor_y - vt_csi_count_param(vt, 0, 1), vt->cursor_x);
                break;
        case 'B':
                vt_set_cursor(vt, vt->cursor_y + vt_csi_count_param(vt, 0, 1), vt->cursor_x);
                break;
        case 'C':
                vt_set_cursor(vt, vt->cursor_y, vt->cursor_x + vt_csi_count_param(vt, 0, 1));
                break;
        case 'D':
                vt_set_cursor(vt, vt->cursor_y, vt->cursor_x - vt_csi_count_param(vt, 0, 1));
                break;
        case 'E':
                vt_set_cursor(vt, vt->cursor_y + vt_csi_count_param(vt, 0, 1), 0);
                break;
        case 'F':
                vt_set_cursor(vt, vt->cursor_y - vt_csi_count_param(vt, 0, 1), 0);
                break;
        case 'G':
        case '`':
                vt_set_cursor(vt, vt->cursor_y, vt_csi_count_param(vt, 0, 1) - 1);
                break;
        case 'H':
        case 'f':
                row = vt_csi_count_param(vt, 0, 1) - 1;
                col = vt_csi_count_param(vt, 1, 1) - 1;
                if (vt->origin_mode) {
                        row += vt->scroll_top;
                        if (row > vt->scroll_bottom) row = vt->scroll_bottom;
                }
                vt_set_cursor(vt, row, col);
                break;
        case 'd':
                row = vt_csi_count_param(vt, 0, 1) - 1;
                if (vt->origin_mode) {
                        row += vt->scroll_top;
                        if (row > vt->scroll_bottom) row = vt->scroll_bottom;
                }
                vt_set_cursor(vt, row, vt->cursor_x);
                break;
        case 'e':
                vt_set_cursor(vt, vt->cursor_y + vt_csi_count_param(vt, 0, 1), vt->cursor_x);
                break;
        case 'a':
                vt_set_cursor(vt, vt->cursor_y, vt->cursor_x + vt_csi_count_param(vt, 0, 1));
                break;
        case 'I':
                n = vt_csi_count_param(vt, 0, 1);
                col = vt->cursor_x;
                while (n-- > 0) col = (col / 8 + 1) * 8;
                vt_set_cursor(vt, vt->cursor_y, col);
                break;
        case 'Z':
                n = vt_csi_count_param(vt, 0, 1);
                col = vt->cursor_x;
                while (n-- > 0) col = ((col + 7) / 8 - 1) * 8;
                if (col < 0) col = 0;
                vt_set_cursor(vt, vt->cursor_y, col);
                break;
        case 'J':
                vt_erase_display(vt, vt_csi_param(vt, 0, 0));
                break;
        case 'K':
                vt_erase_line(vt, vt_csi_param(vt, 0, 0));
                break;
        case 'X':
                n = vt_csi_count_param(vt, 0, 1);
                if (vt->cursor_x + n > vt->cols) n = vt->cols - vt->cursor_x;
                vt_fill_cells(vt, vt->cursor_y, vt->cursor_y + 1,
                              vt->cursor_x, vt->cursor_x + n);
                break;
        case '@':
                n = vt_csi_count_param(vt, 0, 1);
                if (vt->cursor_x + n > vt->cols) n = vt->cols - vt->cursor_x;
                if (n > 0) {
                        size_t move = (size_t)(vt->cols - vt->cursor_x - n) * sizeof(vt_cell_t);
                        if (move > 0)
                                memmove(vt_cell_mut(vt, vt->cursor_y, vt->cursor_x + n),
                                        vt_cell_mut(vt, vt->cursor_y, vt->cursor_x), move);
                        vt_fill_cells(vt, vt->cursor_y, vt->cursor_y + 1,
                                      vt->cursor_x, vt->cursor_x + n);
                }
                break;
        case 'P':
                n = vt_csi_count_param(vt, 0, 1);
                if (vt->cursor_x + n > vt->cols) n = vt->cols - vt->cursor_x;
                if (n > 0) {
                        size_t move = (size_t)(vt->cols - vt->cursor_x - n) * sizeof(vt_cell_t);
                        if (move > 0)
                                memmove(vt_cell_mut(vt, vt->cursor_y, vt->cursor_x),
                                        vt_cell_mut(vt, vt->cursor_y, vt->cursor_x + n), move);
                        vt_fill_cells(vt, vt->cursor_y, vt->cursor_y + 1,
                                      vt->cols - n, vt->cols);
                }
                break;
        case 'L':
                n = vt_csi_count_param(vt, 0, 1);
                if (vt->cursor_y >= vt->scroll_top && vt->cursor_y <= vt->scroll_bottom)
                        vt_scroll_down(vt, vt->cursor_y, vt->scroll_bottom, n);
                break;
        case 'M':
                n = vt_csi_count_param(vt, 0, 1);
                if (vt->cursor_y >= vt->scroll_top && vt->cursor_y <= vt->scroll_bottom)
                        vt_scroll_up(vt, vt->cursor_y, vt->scroll_bottom, n);
                break;
        case 'S':
                vt_scroll_up(vt, vt->scroll_top, vt->scroll_bottom,
                             vt_csi_count_param(vt, 0, 1));
                break;
        case 'T':
                vt_scroll_down(vt, vt->scroll_top, vt->scroll_bottom,
                               vt_csi_count_param(vt, 0, 1));
                break;
        case 'r':
                vt->scroll_top = vt_csi_count_param(vt, 0, 1) - 1;
                vt->scroll_bottom = vt_csi_count_param(vt, 1, vt->rows) - 1;
                vt->scroll_top = vt_clampi(vt->scroll_top, 0, vt->rows - 1);
                vt->scroll_bottom = vt_clampi(vt->scroll_bottom, 0, vt->rows - 1);
                if (vt->scroll_top >= vt->scroll_bottom) {
                        vt->scroll_top = 0;
                        vt->scroll_bottom = vt->rows - 1;
                }
                vt_set_cursor(vt, vt->origin_mode ? vt->scroll_top : 0, 0);
                break;
        case 's':
                vt_save_cursor(vt);
                break;
        case 'u':
                vt_restore_cursor(vt);
                break;
        case 'm':
                vt_sgr(vt);
                break;
        case 'h':
        case 'l':
                for (n = 0; n < vt->csi_count; n++) {
                        if (vt->csi_params[n] != 0)
                                vt_set_mode(vt, vt->csi_params[n], final == 'h',
                                            vt->csi_private);
                }
                break;
        case 'b':
                n = vt_csi_count_param(vt, 0, 1);
                while (n-- > 0 && vt->last_char != 0) vt_put_codepoint(vt, vt->last_char);
                break;
        case 'c':
                if (vt->csi_private == 2) {
                        vt_respond_bytes(vt, "\x1b[>0;0;0c", 9);
                } else {
                        vt_respond_bytes(vt, "\x1b[?1;2c", 7);
                }
                break;
        case 'n':
                n = vt_csi_param(vt, 0, 0);
                if (n == 5) {
                        vt_respond_bytes(vt, "\x1b[0n", 4);
                } else if (n == 6) {
                        vt_respond_cursor(vt, vt->csi_private == 1);
                }
                break;
        case 't':
                n = vt_csi_param(vt, 0, 0);
                if (n == 18) {
                        vt_respond_bytes(vt, "\x1b[8;", 4);
                        vt_respond_int(vt, vt->rows);
                        vt_respond_char(vt, ';');
                        vt_respond_int(vt, vt->cols);
                        vt_respond_char(vt, 't');
                } else if (n == 14) {
                        vt_respond_bytes(vt, "\x1b[4;", 4);
                        vt_respond_int(vt, vt->rows * 16);
                        vt_respond_char(vt, ';');
                        vt_respond_int(vt, vt->cols * 8);
                        vt_respond_char(vt, 't');
                }
                break;
        default:
                break;
        }
}

static void vt_csi_start(vt_t *vt)
{
        int i;

        vt->state          = VT_STATE_CSI;
        vt->csi_count      = 0;
        vt->csi_cur        = 0;
        vt->csi_cur_valid  = 0;
        vt->csi_private    = 0;
        for (i = 0; i < VT_MAX_PARAMS; i++) vt->csi_params[i] = 0;
}

static void vt_csi_push(vt_t *vt, int value)
{
        if (vt->csi_count < VT_MAX_PARAMS)
                vt->csi_params[vt->csi_count++] = value;
}

static void vt_handle_csi_byte(vt_t *vt, uint8_t byte)
{
        if (byte >= '0' && byte <= '9') {
                vt->csi_cur = vt->csi_cur * 10 + (byte - '0');
                if (vt->csi_cur > 99999) vt->csi_cur = 99999;
                vt->csi_cur_valid = 1;
                return;
        }

        if (byte == ';' || byte == ':') {
                vt_csi_push(vt, vt->csi_cur_valid ? vt->csi_cur : 0);
                vt->csi_cur       = 0;
                vt->csi_cur_valid = 0;
                return;
        }

        if (vt->csi_count == 0 && !vt->csi_cur_valid) {
                if (byte == '?') { vt->csi_private = 1; return; }
                if (byte == '>') { vt->csi_private = 2; return; }
                if (byte == '<') { vt->csi_private = 3; return; }
                if (byte == '=') { vt->csi_private = 4; return; }
                if (byte == '!') { vt->csi_private = 5; return; }
        }

        if (byte >= 0x40 && byte <= 0x7e) {
                if (vt->csi_cur_valid) vt_csi_push(vt, vt->csi_cur);
                vt->csi_cur       = 0;
                vt->csi_cur_valid = 0;
                vt->state         = VT_STATE_GROUND;
                vt_csi_dispatch(vt, byte);
                return;
        }

        if (byte == 0x1b) {
                vt->state = VT_STATE_ESC;
                return;
        }

        /* Intermediate bytes (space, '!', etc.) are ignored. */
}

static void vt_process_codepoint(vt_t *vt, uint32_t cp)
{
        if (cp < 0x20 || cp == 0x7f) return;
        vt_put_codepoint(vt, cp);
}

static void vt_handle_ground_ascii(vt_t *vt, uint8_t byte)
{
        switch (byte) {
        case 0x00:
        case 0x07:
                return;
        case 0x08:
                if (vt->cursor_x > 0) vt_set_cursor(vt, vt->cursor_y, vt->cursor_x - 1);
                else vt->wrap_pending = 0;
                return;
        case 0x09: {
                int col = (vt->cursor_x / 8 + 1) * 8;
                vt_set_cursor(vt, vt->cursor_y, col);
                return;
        }
        case 0x0a:
        case 0x0b:
        case 0x0c:
                vt_index_down(vt);
                if (vt->newline_mode) vt_set_cursor(vt, vt->cursor_y, 0);
                return;
        case 0x0d:
                vt_set_cursor(vt, vt->cursor_y, 0);
                return;
        case 0x0e:
                vt->active_charset = 1;
                return;
        case 0x0f:
                vt->active_charset = 0;
                return;
        case 0x1b:
                vt->state = VT_STATE_ESC;
                return;
        case 0x7f:
                return;
        default:
                if (byte >= 0x20) vt_put_ascii(vt, byte);
                return;
        }
}

static void vt_handle_esc(vt_t *vt, uint8_t byte)
{
        vt->state = VT_STATE_GROUND;

        switch (byte) {
        case '[':
                vt_csi_start(vt);
                break;
        case ']':
                vt->state = VT_STATE_OSC;
                break;
        case 'P':
        case '^':
        case '_':
        case 'X':
                vt->state = VT_STATE_STRING;
                break;
        case '7':
                vt_save_cursor(vt);
                break;
        case '8':
                vt_restore_cursor(vt);
                break;
        case 'D':
                vt_index_down(vt);
                break;
        case 'E':
                vt_index_down(vt);
                vt_set_cursor(vt, vt->cursor_y, 0);
                break;
        case 'M':
                vt_index_up(vt);
                break;
        case 'H':
                break; /* horizontal tab set: fixed 8-column tabs */
        case '=':
                vt->keypad_app = 1;
                break;
        case '>':
                vt->keypad_app = 0;
                break;
        case 'c':
                vt_reset(vt);
                break;
        case '(':
        case ')':
        case '*':
        case '+':
                vt->charset_target = (byte == '(') ? 0 : (byte == ')' ? 1 : 2);
                vt->state = VT_STATE_CHARSET;
                break;
        case '#':
                vt->state = VT_STATE_HASH;
                break;
        case 'N':
        case 'O':
        case '\\':
                break;
        default:
                break;
        }
}

static void vt_handle_charset(vt_t *vt, uint8_t byte)
{
        int charset = (byte == '0') ? VT_CHARSET_DEC_GRAPHICS : VT_CHARSET_ASCII;

        if (vt->charset_target == 0) vt->charset[0] = (uint8_t)charset;
        else if (vt->charset_target == 1) vt->charset[1] = (uint8_t)charset;
        vt->state = VT_STATE_GROUND;
}

static void vt_handle_utf8_byte(vt_t *vt, uint8_t byte)
{
        if (byte >= 0x80 && byte <= 0xbf) {
                vt->utf8_cp = (vt->utf8_cp << 6) | (uint32_t)(byte & 0x3f);
                vt->utf8_need--;
                if (vt->utf8_need == 0) {
                        uint32_t cp = vt->utf8_cp;
                        if (cp < vt->utf8_min || cp > 0x10ffff ||
                            (cp >= 0xd800 && cp <= 0xdfff))
                                cp = 0xfffd;
                        vt_process_codepoint(vt, cp);
                        vt->utf8_cp   = 0;
                        vt->utf8_min  = 0;
                }
                return;
        }

        /* Broken sequence: emit one replacement and reprocess this byte. */
        vt->utf8_need = 0;
        vt->utf8_cp   = 0;
        vt->utf8_min  = 0;
        vt_process_codepoint(vt, 0xfffd);
        vt_parse_byte(vt, byte);
}

static void vt_parse_byte(vt_t *vt, uint8_t byte)
{
        if (vt->state != VT_STATE_GROUND && byte == 0x1b) {
                /* ESC aborts most parser states.  OSC/DCS want ESC \ as ST. */
                if (vt->state == VT_STATE_OSC) {
                        vt->state = VT_STATE_OSC_ESC;
                        return;
                }
                if (vt->state == VT_STATE_STRING) {
                        vt->state = VT_STATE_STRING_ESC;
                        return;
                }
                vt->state = VT_STATE_ESC;
                return;
        }

        switch (vt->state) {
        case VT_STATE_GROUND:
                if (vt->utf8_need > 0) {
                        vt_handle_utf8_byte(vt, byte);
                        return;
                }
                if (byte < 0x80) {
                        vt_handle_ground_ascii(vt, byte);
                } else if (byte >= 0xc2 && byte <= 0xdf) {
                        vt->utf8_need = 1;
                        vt->utf8_cp   = byte & 0x1f;
                        vt->utf8_min  = 0x80;
                } else if (byte >= 0xe0 && byte <= 0xef) {
                        vt->utf8_need = 2;
                        vt->utf8_cp   = byte & 0x0f;
                        vt->utf8_min  = 0x800;
                } else if (byte >= 0xf0 && byte <= 0xf4) {
                        vt->utf8_need = 3;
                        vt->utf8_cp   = byte & 0x07;
                        vt->utf8_min  = 0x10000;
                } else {
                        vt_process_codepoint(vt, 0xfffd);
                }
                return;
        case VT_STATE_ESC:
                vt_handle_esc(vt, byte);
                return;
        case VT_STATE_CSI:
                vt_handle_csi_byte(vt, byte);
                return;
        case VT_STATE_OSC:
                if (byte == 0x07) vt->state = VT_STATE_GROUND;
                return;
        case VT_STATE_OSC_ESC:
                if (byte == '\\' || byte == 0x07) vt->state = VT_STATE_GROUND;
                else vt->state = VT_STATE_OSC;
                return;
        case VT_STATE_STRING:
                return;
        case VT_STATE_STRING_ESC:
                if (byte == '\\') vt->state = VT_STATE_GROUND;
                else vt->state = VT_STATE_STRING;
                return;
        case VT_STATE_CHARSET:
                vt_handle_charset(vt, byte);
                return;
        case VT_STATE_HASH:
                if (byte == '8') {
                        int row;
                        int col;
                        for (row = 0; row < vt->rows; row++) {
                                for (col = 0; col < vt->cols; col++) {
                                        vt_cell_t *cell = vt_cell_mut(vt, row, col);
                                        cell->ch    = 'E';
                                        cell->fg    = vt->default_fg;
                                        cell->bg    = vt->default_bg;
                                        cell->attr  = 0;
                                        cell->width = VT_WIDTH_NARROW;
                                }
                                vt_mark_row(vt, row);
                        }
                        vt_set_cursor(vt, 0, 0);
                }
                vt->state = VT_STATE_GROUND;
                return;
        default:
                vt->state = VT_STATE_GROUND;
                return;
        }
}

vt_t *vt_create(int rows, int cols)
{
        vt_t *vt;

        if (rows < 1) rows = 24;
        if (cols < 1) cols = 80;
        if (rows > 512) rows = 512;
        if (cols > 512) cols = 512;

        vt = (vt_t *)calloc(1, sizeof(*vt));
        if (vt == NULL) return NULL;

        vt->rows = rows;
        vt->cols = cols;
        vt->default_fg = 0xffffff;
        vt->default_bg = 0x000000;
        vt->fg = vt->default_fg;
        vt->bg = vt->default_bg;

        vt->main_cells = (vt_cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(vt_cell_t));
        vt->alt_cells  = (vt_cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(vt_cell_t));
        vt->dirty      = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
        if (vt->main_cells == NULL || vt->alt_cells == NULL || vt->dirty == NULL) {
                vt_destroy(vt);
                return NULL;
        }

        vt_reset(vt);
        return vt;
}

void vt_destroy(vt_t *vt)
{
        if (vt == NULL) return;
        free(vt->main_cells);
        free(vt->alt_cells);
        free(vt->dirty);
        free(vt);
}

void vt_reset(vt_t *vt)
{
        if (vt == NULL) return;

        vt->cells        = vt->main_cells;
        vt->alt_active   = 0;
        vt->cursor_x     = 0;
        vt->cursor_y     = 0;
        vt->cursor_visible = 1;
        vt->autowrap     = 1;
        vt->origin_mode  = 0;
        vt->insert_mode  = 0;
        vt->newline_mode = 0;
        vt->decckm       = 0;
        vt->keypad_app   = 0;
        vt->scroll_top   = 0;
        vt->scroll_bottom = vt->rows - 1;
        vt->fg           = vt->default_fg;
        vt->bg           = vt->default_bg;
        vt->attr         = 0;
        vt->charset[0]   = VT_CHARSET_ASCII;
        vt->charset[1]   = VT_CHARSET_ASCII;
        vt->active_charset = 0;
        vt->last_char    = 0;
        vt->last_width   = 0;
        vt->wrap_pending = 0;
        vt->state        = VT_STATE_GROUND;
        vt->utf8_need    = 0;
        vt->utf8_cp      = 0;
        vt->utf8_min     = 0;
        vt->response_len = 0;
        vt_save_cursor(vt);

        /* Fill both grids with the default rendition. */
        vt_fill_cells(vt, 0, vt->rows, 0, vt->cols);
        vt->cells = vt->alt_cells;
        vt_fill_cells(vt, 0, vt->rows, 0, vt->cols);
        vt->cells = vt->main_cells;
        vt_mark_all(vt);
}

void vt_set_default_colors(vt_t *vt, uint32_t fg, uint32_t bg)
{
        if (vt == NULL) return;
        vt->default_fg = fg;
        vt->default_bg = bg;
        vt->fg         = fg;
        vt->bg         = bg;
        vt_mark_all(vt);
}

void vt_feed(vt_t *vt, const uint8_t *data, size_t len)
{
        size_t i;

        if (vt == NULL || data == NULL) return;
        for (i = 0; i < len; i++) vt_parse_byte(vt, data[i]);
}

int vt_rows(const vt_t *vt)
{
        return vt != NULL ? vt->rows : 0;
}

int vt_cols(const vt_t *vt)
{
        return vt != NULL ? vt->cols : 0;
}

int vt_cursor_row(const vt_t *vt)
{
        return vt != NULL ? vt->cursor_y : 0;
}

int vt_cursor_col(const vt_t *vt)
{
        return vt != NULL ? vt->cursor_x : 0;
}

int vt_cursor_visible(const vt_t *vt)
{
        return vt != NULL ? vt->cursor_visible : 0;
}

int vt_dirty_all(const vt_t *vt)
{
        return vt != NULL ? vt->dirty_all : 0;
}

int vt_row_dirty(const vt_t *vt, int row)
{
        if (vt == NULL || row < 0 || row >= vt->rows) return 0;
        return vt->dirty[row] != 0;
}

void vt_clear_dirty(vt_t *vt)
{
        if (vt == NULL) return;
        if (vt->dirty_all) {
                memset(vt->dirty, 0, (size_t)vt->rows);
                vt->dirty_all = 0;
        } else {
                memset(vt->dirty, 0, (size_t)vt->rows);
        }
}

void vt_mark_all_dirty(vt_t *vt)
{
        vt_mark_all(vt);
}

const vt_cell_t *vt_cell(const vt_t *vt, int row, int col)
{
        if (vt == NULL || row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
                return NULL;
        return &vt->cells[(size_t)row * (size_t)vt->cols + (size_t)col];
}

int vt_resize(vt_t *vt, int rows, int cols)
{
        vt_cell_t *new_main;
        vt_cell_t *new_alt;
        uint8_t   *new_dirty;
        int        y;
        int        x;

        if (vt == NULL || rows < 1 || cols < 1 || rows > 512 || cols > 512) return -1;
        if (rows == vt->rows && cols == vt->cols) return 0;

        new_main = (vt_cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(vt_cell_t));
        new_alt  = (vt_cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(vt_cell_t));
        new_dirty = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
        if (new_main == NULL || new_alt == NULL || new_dirty == NULL) {
                free(new_main);
                free(new_alt);
                free(new_dirty);
                return -1;
        }

        for (y = 0; y < rows; y++) {
                for (x = 0; x < cols; x++) {
                        vt_cell_t blank;
                        blank.ch    = 0;
                        blank.fg    = vt->default_fg;
                        blank.bg    = vt->default_bg;
                        blank.attr  = 0;
                        blank.width = VT_WIDTH_NARROW;
                        new_main[(size_t)y * (size_t)cols + (size_t)x] = blank;
                        new_alt[(size_t)y * (size_t)cols + (size_t)x] = blank;
                }
        }

        for (y = 0; y < (rows < vt->rows ? rows : vt->rows); y++) {
                int copy = cols < vt->cols ? cols : vt->cols;
                memcpy(&new_main[(size_t)y * (size_t)cols],
                       &vt->main_cells[(size_t)y * (size_t)vt->cols],
                       (size_t)copy * sizeof(vt_cell_t));
                memcpy(&new_alt[(size_t)y * (size_t)cols],
                       &vt->alt_cells[(size_t)y * (size_t)vt->cols],
                       (size_t)copy * sizeof(vt_cell_t));
        }

        free(vt->main_cells);
        free(vt->alt_cells);
        free(vt->dirty);
        vt->main_cells = new_main;
        vt->alt_cells  = new_alt;
        vt->dirty      = new_dirty;
        vt->cells      = vt->alt_active ? vt->alt_cells : vt->main_cells;
        vt->rows       = rows;
        vt->cols       = cols;

        if (vt->cursor_y >= vt->rows) vt->cursor_y = vt->rows - 1;
        if (vt->cursor_x >= vt->cols) vt->cursor_x = vt->cols - 1;
        vt->scroll_top    = 0;
        vt->scroll_bottom = vt->rows - 1;
        vt->wrap_pending  = 0;
        vt->dirty_all     = 1;
        return 0;
}

int vt_decckm(const vt_t *vt)
{
        return vt != NULL ? vt->decckm : 0;
}

int vt_keypad_app(const vt_t *vt)
{
        return vt != NULL ? vt->keypad_app : 0;
}

const char *vt_take_response(vt_t *vt, size_t *len)
{
        const char *result;

        if (vt == NULL || len == NULL) return NULL;
        if (vt->response_len == 0) {
                *len = 0;
                return NULL;
        }
        *len           = vt->response_len;
        result         = vt->response;
        vt->response_len = 0;
        return result;
}

void vt_set_response_callback(vt_t *vt, vt_response_fn fn, void *ctx)
{
        if (vt == NULL) return;
        vt->response_fn  = fn;
        vt->response_ctx = ctx;
}
