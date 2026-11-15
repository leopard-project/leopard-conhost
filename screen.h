/*
 * screen.h — output backend boundary.
 *
 * Primary backend: /dev/fb0 mapped with fb_fix_screeninfo/fb_var_screeninfo.
 * Fallback: dirty rows are emitted as plain text through xapi_Printf so the
 * PTY/VT path can be exercised before the kernel exposes a framebuffer device.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

typedef struct vt vt_t;

typedef struct screen {
        int      fd;
        int      fb_active;
        uint8_t *mem;
        size_t   mem_len;
        uint32_t xres;
        uint32_t yres;
        uint32_t line_length;
        uint32_t bpp;
        int      rows;
        int      cols;

        /* fb_var_screeninfo is kept only for channel packing. */
        uint32_t red_off;
        uint32_t red_len;
        uint32_t green_off;
        uint32_t green_len;
        uint32_t blue_off;
        uint32_t blue_len;

        /* IME preedit/candidate status line, painted over the last row. */
        char     overlay[192];
        size_t   overlay_len;
        int      overlay_dirty;
} screen_t;

int  screen_init(screen_t *screen);
void screen_shutdown(screen_t *screen);

int screen_rows(const screen_t *screen);
int screen_cols(const screen_t *screen);
int screen_framebuffer_active(const screen_t *screen);

/* Re-query the framebuffer size.  Returns 1 when the grid changed, 0 when it
 * did not (or when the mode change cannot be handled without a remap). */
int screen_update_size(screen_t *screen);

/* Set/clear the IME status line drawn over the bottom row.  Passing NULL or an
 * empty string restores the VT contents on the next render. */
void screen_set_overlay(screen_t *screen, const char *text);

/* Paint every dirty row.  The VT dirty flags are left untouched; callers use
 * vt_clear_dirty() after the render completes. */
void screen_render(screen_t *screen, vt_t *vt);
