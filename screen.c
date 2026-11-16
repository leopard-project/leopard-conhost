/*
 * screen.c — framebuffer backend with a serial/text fallback.
 *
 * /dev/fb0 is the primary output path.  The ioctl payloads are defined locally
 * because the XACT CRT header currently exposes only TIOCSWINSZ; these are the
 * stable Linux framebuffer ABI values and match leopard-xsk/include/ioctl.h.
 * If the device or mmap is unavailable, dirty rows are emitted as plain text
 * through xapi_Printf so the PTY + VT code can still be tested.
 */
#include "screen.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font8x16.h"
#include "vt.h"
#include "xtuiapi.h"

#define CONHOST_FBIOGET_VSCREENINFO 0x4600
#define CONHOST_FBIOGET_FSCREENINFO 0x4602

struct conhost_fb_bitfield {
        uint32_t offset;
        uint32_t length;
        uint32_t msb_right;
};

struct conhost_fb_fix_screeninfo {
        char     id[16];
        uint64_t smem_start;
        uint32_t smem_len;
        uint32_t type;
        uint32_t type_aux;
        uint32_t visual;
        uint16_t xpanstep;
        uint16_t ypanstep;
        uint16_t ywrapstep;
        uint32_t line_length;
        uint64_t mmio_start;
        uint32_t mmio_len;
        uint32_t accel;
        uint16_t capabilities;
        uint16_t reserved[2];
};

struct conhost_fb_var_screeninfo {
        uint32_t xres;
        uint32_t yres;
        uint32_t xres_virtual;
        uint32_t yres_virtual;
        uint32_t xoffset;
        uint32_t yoffset;
        uint32_t bits_per_pixel;
        uint32_t grayscale;
        struct conhost_fb_bitfield red;
        struct conhost_fb_bitfield green;
        struct conhost_fb_bitfield blue;
        struct conhost_fb_bitfield transp;
        uint32_t nonstd;
        uint32_t activate;
        uint32_t height;
        uint32_t width;
        uint32_t accel_flags;
        uint32_t pixclock;
        uint32_t left_margin;
        uint32_t right_margin;
        uint32_t upper_margin;
        uint32_t lower_margin;
        uint32_t hsync_len;
        uint32_t vsync_len;
        uint32_t sync;
        uint32_t vmode;
        uint32_t rotate;
        uint32_t colorspace;
        uint32_t reserved[4];
};

static uint32_t pack_component(uint32_t value, uint32_t offset, uint32_t length)
{
        uint32_t max;
        uint32_t scaled;

        if (length == 0) return 0;
        if (length > 24) length = 24;
        max = (1u << length) - 1u;
        scaled = (value * max + 127u) / 255u;
        return (scaled & max) << offset;
}

static uint32_t pack_color(const screen_t *screen, uint32_t rgb)
{
        uint32_t red   = (rgb >> 16) & 0xffu;
        uint32_t green = (rgb >> 8) & 0xffu;
        uint32_t blue  = rgb & 0xffu;

        return pack_component(red, screen->red_off, screen->red_len) |
               pack_component(green, screen->green_off, screen->green_len) |
               pack_component(blue, screen->blue_off, screen->blue_len);
}

static void put_pixel(screen_t *screen, uint32_t x, uint32_t y, uint32_t color)
{
        uint8_t *pixel;

        if (screen->mem == NULL || x >= screen->xres || y >= screen->yres) return;
        pixel = screen->mem + (size_t)y * screen->line_length + (size_t)x * (screen->bpp / 8u);

        switch (screen->bpp) {
        case 16:
                *(uint16_t *)pixel = (uint16_t)color;
                break;
        case 24:
                pixel[0] = (uint8_t)(color & 0xffu);
                pixel[1] = (uint8_t)((color >> 8) & 0xffu);
                pixel[2] = (uint8_t)((color >> 16) & 0xffu);
                break;
        case 32:
                *(uint32_t *)pixel = color;
                break;
        default:
                break;
        }
}

static int glyph_index(uint32_t cp)
{
        if (cp >= ' ' && cp < 0x7f) return (int)cp;
        if (cp < 256) return (int)cp;

        /* The bundled 8x16 font is a single-byte console font.  Fold the most
         * common box-drawing characters to ASCII so borders remain readable. */
        switch (cp) {
        case 0x2500: case 0x2501: case 0x2504: case 0x2505:
        case 0x2550: return '-';
        case 0x2502: case 0x2503: case 0x2506: case 0x2507:
        case 0x2551: return '|';
        case 0x250c: case 0x2510: case 0x2514: case 0x2518:
        case 0x251c: case 0x2524: case 0x252c: case 0x2534:
        case 0x253c: case 0x2554: case 0x2557: case 0x255a:
        case 0x255d: case 0x2560: case 0x2563: case 0x2566:
        case 0x2569: case 0x256c: return '+';
        case 0x2022: return 'o';
        default: return '?';
        }
}

static void draw_glyph(screen_t *screen, int px, int py, const unsigned char *glyph,
                       uint32_t fg, uint32_t bg, uint8_t attr, int cursor)
{
        uint32_t fgc = pack_color(screen, fg);
        uint32_t bgc = pack_color(screen, bg);
        int      row;
        int      col;

        if (cursor) {
                uint32_t tmp = fgc;
                fgc = bgc;
                bgc = tmp;
        }

        if (attr & VT_ATTR_DIM) {
                fgc = pack_color(screen, ((fg >> 1) & 0x7f7f7f));
        }

        for (row = 0; row < 16; row++) {
                unsigned int bits = glyph[row];
                if (attr & VT_ATTR_BOLD) bits |= bits >> 1;
                for (col = 0; col < 8; col++) {
                        uint32_t color = (bits & (0x80u >> col)) ? fgc : bgc;
                        put_pixel(screen, (uint32_t)(px + col), (uint32_t)(py + row), color);
                }
        }

        if (attr & VT_ATTR_UNDERLINE) {
                for (col = 0; col < 8; col++)
                        put_pixel(screen, (uint32_t)(px + col), (uint32_t)(py + 15), fgc);
        }
        if (attr & VT_ATTR_STRIKE) {
                for (col = 0; col < 8; col++)
                        put_pixel(screen, (uint32_t)(px + col), (uint32_t)(py + 7), fgc);
        }
}

static void draw_cell(screen_t *screen, int row, int col, const vt_cell_t *cell, int cursor)
{
        uint32_t fg;
        uint32_t bg;
        uint8_t  attr;
        int      index;
        int      px = col * 8;
        int      py = row * 16;

        if (cell == NULL) return;

        fg   = cell->fg;
        bg   = cell->bg;
        attr = cell->attr;

        if (attr & VT_ATTR_REVERSE) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
        }
        if (attr & VT_ATTR_INVISIBLE) fg = bg;

        index = glyph_index(cell->ch != 0 ? cell->ch : ' ');
        if (cell->ch == 0) {
                /* Blank cells still need their background painted.  A block
                 * cursor over a blank cell inverts the two colors. */
                int y;
                int x;
                uint32_t bgc;
                uint32_t fgc;

                if (cursor) {
                        uint32_t tmp = fg;
                        fg = bg;
                        bg = tmp;
                }
                if (attr & VT_ATTR_DIM) fg = (fg >> 1) & 0x7f7f7f;
                bgc = pack_color(screen, bg);
                fgc = pack_color(screen, fg);
                for (y = 0; y < 16; y++)
                        for (x = 0; x < 8; x++)
                                put_pixel(screen, (uint32_t)(px + x), (uint32_t)(py + y), bgc);
                if ((attr & VT_ATTR_UNDERLINE) || cursor) {
                        for (x = 0; x < 8; x++)
                                put_pixel(screen, (uint32_t)(px + x), (uint32_t)(py + 15), fgc);
                }
                return;
        }

        draw_glyph(screen, px, py, conhost_font8x16[index & 0xff], fg, bg, attr, cursor);
}

static void render_row(screen_t *screen, vt_t *vt, int row, int draw_cursor)
{
        int col;
        int cols = vt_cols(vt);

        if (cols > screen->cols) cols = screen->cols;

        for (col = 0; col < cols; col++) {
                const vt_cell_t *cell = vt_cell(vt, row, col);
                if (cell == NULL) break;
                if (cell->width == VT_WIDTH_TRAIL) continue;
                draw_cell(screen, row, col, cell, 0);
        }

        if (!draw_cursor) return;

        col = vt_cursor_col(vt);
        if (col < 0) col = 0;
        if (col >= cols) col = cols - 1;
        if (row >= 0) {
                const vt_cell_t *cell = vt_cell(vt, row, col);
                if (cell != NULL) {
                        if (cell->width == VT_WIDTH_TRAIL && col > 0) {
                                col--;
                                cell = vt_cell(vt, row, col);
                        }
                        draw_cell(screen, row, col, cell, 1);
                }
        }
}

static void serial_render_row(vt_t *vt, int row)
{
        char  line[2048];
        size_t used = 0;
        size_t last = 0;
        int    col;
        int    cols = vt_cols(vt);

        for (col = 0; col < cols; col++) {
                const vt_cell_t *cell = vt_cell(vt, row, col);
                uint32_t         cp;
                char             utf8[4];
                size_t           n;

                if (cell == NULL) break;
                if (cell->width == VT_WIDTH_TRAIL) continue;

                cp = cell->ch;
                if (cp == 0 || cp < 0x20) cp = ' ';
                n = vt_utf8_encode(cp, utf8, sizeof(utf8));
                if (n == 0 || used + n >= sizeof(line)) break;
                memcpy(line + used, utf8, n);
                used += n;
                if (cp != ' ') last = used;
        }

        line[last] = '\0';
        if (last != 0) xapi_Printf("%s\n", line);
}

int screen_init(screen_t *screen)
{
        struct conhost_fb_fix_screeninfo fix;
        struct conhost_fb_var_screeninfo var;
        size_t                            map_len;

        memset(screen, 0, sizeof(*screen));
        screen->fd = -1;
        screen->rows = 24;
        screen->cols = 80;

        screen->fd = open("/dev/fb0", O_RDWR);
        if (screen->fd >= 0) {
                memset(&fix, 0, sizeof(fix));
                memset(&var, 0, sizeof(var));

                if (ioctl(screen->fd, CONHOST_FBIOGET_FSCREENINFO, &fix) == 0 &&
                    ioctl(screen->fd, CONHOST_FBIOGET_VSCREENINFO, &var) == 0 &&
                    var.xres > 0 && var.yres > 0) {
                        screen->xres        = var.xres;
                        screen->yres        = var.yres;
                        screen->line_length = fix.line_length;
                        screen->bpp         = var.bits_per_pixel;
                        screen->rows        = (int)(var.yres / 16u);
                        screen->cols        = (int)(var.xres / 8u);
                        if (screen->rows < 1) screen->rows = 1;
                        if (screen->cols < 1) screen->cols = 1;

                        if (var.red.length && var.green.length && var.blue.length) {
                                screen->red_off   = var.red.offset;
                                screen->red_len   = var.red.length;
                                screen->green_off = var.green.offset;
                                screen->green_len = var.green.length;
                                screen->blue_off  = var.blue.offset;
                                screen->blue_len  = var.blue.length;
                        } else {
                                /* Common little-endian XRGB layout when the
                                 * driver leaves bitfields unset. */
                                screen->red_off   = 16;
                                screen->red_len   = 8;
                                screen->green_off = 8;
                                screen->green_len = 8;
                                screen->blue_off  = 0;
                                screen->blue_len  = 8;
                        }

                        if (screen->line_length == 0)
                                screen->line_length = var.xres * (var.bits_per_pixel / 8u);

                        map_len = fix.smem_len;
                        if (map_len == 0) {
                                uint32_t virtual_y = var.yres_virtual ? var.yres_virtual : var.yres;
                                map_len = (size_t)fix.line_length * virtual_y;
                        }

                        if (map_len > 0 &&
                            (var.bits_per_pixel == 16 || var.bits_per_pixel == 24 ||
                             var.bits_per_pixel == 32)) {
                                void *mapped = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                                                    MAP_SHARED, screen->fd, 0);
                                if (mapped != NULL && mapped != MAP_FAILED) {
                                        screen->mem      = (uint8_t *)mapped;
                                        screen->mem_len  = map_len;
                                        screen->fb_active = 1;
                                }
                        }
                }

                if (!screen->fb_active) {
                        close(screen->fd);
                        screen->fd = -1;
                }
        }

        if (!screen->fb_active)
                xapi_Printf("leopard-conhost: /dev/fb0 unavailable, serial fallback\n");

        return screen->fb_active ? 0 : 1;
}

void screen_shutdown(screen_t *screen)
{
        if (screen == NULL) return;
        if (screen->mem != NULL && screen->mem_len != 0)
                munmap(screen->mem, screen->mem_len);
        if (screen->fd >= 0)
                close(screen->fd);
        screen->mem = NULL;
        screen->mem_len = 0;
        screen->fd = -1;
        screen->fb_active = 0;
}

int screen_rows(const screen_t *screen)
{
        return screen != NULL ? screen->rows : 0;
}

int screen_cols(const screen_t *screen)
{
        return screen != NULL ? screen->cols : 0;
}

int screen_framebuffer_active(const screen_t *screen)
{
        return screen != NULL ? screen->fb_active : 0;
}

int screen_update_size(screen_t *screen)
{
        struct conhost_fb_fix_screeninfo fix;
        struct conhost_fb_var_screeninfo var;
        int                               rows;
        int                               cols;

        if (screen == NULL || !screen->fb_active || screen->fd < 0) return 0;
        memset(&fix, 0, sizeof(fix));
        memset(&var, 0, sizeof(var));

        if (ioctl(screen->fd, CONHOST_FBIOGET_FSCREENINFO, &fix) != 0 ||
            ioctl(screen->fd, CONHOST_FBIOGET_VSCREENINFO, &var) != 0)
                return 0;

        /* A stride or depth change would invalidate the existing mapping.  The
         * current conhost has no remap path, so keep the old geometry rather
         * than painting through a stale mapping. */
        if (var.bits_per_pixel != screen->bpp || fix.line_length != screen->line_length)
                return 0;

        rows = (int)(var.yres / 16u);
        cols = (int)(var.xres / 8u);
        if (rows < 1) rows = 1;
        if (cols < 1) cols = 1;
        if (rows == screen->rows && cols == screen->cols) return 0;

        screen->xres = var.xres;
        screen->yres = var.yres;
        screen->rows = rows;
        screen->cols = cols;
        return 1;
}

void screen_set_overlay(screen_t *screen, const char *text)
{
        size_t len = 0;

        if (screen == NULL) return;
        if (text != NULL) {
                while (text[len] != '\0' && len + 1 < sizeof(screen->overlay)) len++;
        }
        if (len == screen->overlay_len &&
            (len == 0 || memcmp(screen->overlay, text, len) == 0))
                return;

        if (len != 0) memcpy(screen->overlay, text, len);
        screen->overlay[len] = '\0';
        screen->overlay_len  = len;
        screen->overlay_dirty = 1;
}

static size_t overlay_decode(const char *text, size_t remaining, uint32_t *cp)
{
        const unsigned char *s = (const unsigned char *)text;

        if (remaining == 0) return 0;
        if (s[0] < 0x80) {
                *cp = s[0];
                return 1;
        }
        if ((s[0] & 0xe0) == 0xc0 && remaining >= 2 &&
            (s[1] & 0xc0) == 0x80) {
                *cp = ((uint32_t)(s[0] & 0x1f) << 6) | (uint32_t)(s[1] & 0x3f);
                return 2;
        }
        if ((s[0] & 0xf0) == 0xe0 && remaining >= 3 &&
            (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
                *cp = ((uint32_t)(s[0] & 0x0f) << 12) |
                      ((uint32_t)(s[1] & 0x3f) << 6) |
                      (uint32_t)(s[2] & 0x3f);
                return 3;
        }
        if ((s[0] & 0xf8) == 0xf0 && remaining >= 4 &&
            (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
            (s[3] & 0xc0) == 0x80) {
                *cp = ((uint32_t)(s[0] & 0x07) << 18) |
                      ((uint32_t)(s[1] & 0x3f) << 12) |
                      ((uint32_t)(s[2] & 0x3f) << 6) |
                      (uint32_t)(s[3] & 0x3f);
                return 4;
        }
        *cp = '?';
        return 1;
}

static void render_overlay(screen_t *screen, const char *text, size_t len)
{
        int             row  = screen->rows - 1;
        int             cols = screen->cols;
        int             col;
        size_t          used = 0;
        vt_cell_t       cell;

        cell.fg    = 0xffe08a;
        cell.bg    = 0x2b2410;
        cell.attr  = 0;
        cell.width = VT_WIDTH_NARROW;
        cell.ch    = ' ';

        for (col = 0; col < cols; col++)
                draw_cell(screen, row, col, &cell, 0);

        for (col = 0; col < cols && used < len; col++) {
                uint32_t cp;
                size_t   step = overlay_decode(text + used, len - used, &cp);

                if (step == 0) break;
                used += step;
                if (cp < 0x20 || cp == 0x7f) cp = ' ';
                cell.ch = cp;
                draw_cell(screen, row, col, &cell, 0);
        }
}

void screen_render(screen_t *screen, vt_t *vt)
{
        int all;
        int row;
        int cursor_row;

        if (screen == NULL || vt == NULL) return;

        all        = vt_dirty_all(vt);
        cursor_row = vt_cursor_row(vt);

        if (!screen->fb_active) {
                for (row = 0; row < vt_rows(vt) && row < screen->rows; row++) {
                        if (all || vt_row_dirty(vt, row))
                                serial_render_row(vt, row);
                }
                return;
        }

        for (row = 0; row < vt_rows(vt) && row < screen->rows; row++) {
                if (!all && !vt_row_dirty(vt, row)) continue;
                render_row(screen, vt, row,
                           vt_cursor_visible(vt) && row == cursor_row);
        }

        /* The status line is an overlay, not VT state: repaint it every frame
         * while active and once more after it disappears. */
        if (screen->overlay_len != 0 || screen->overlay_dirty) {
                render_overlay(screen, screen->overlay, screen->overlay_len);
                screen->overlay_dirty = 0;
        }
}
