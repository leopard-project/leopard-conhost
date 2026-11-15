/*
 * vt.h — minimal VT/ANSI terminal state machine.
 *
 * The VT layer owns the character grid and knows nothing about the PTY or the
 * framebuffer.  A frontend feeds it raw bytes from the pty master and renders
 * the grid itself.  Dirty rows are reported per row so a frontend never has to
 * repaint the whole screen for a single keypress.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#define VT_MAX_PARAMS 16

/* Cell attributes.  Colors are stored as 0xRRGGBB. */
enum {
        VT_ATTR_BOLD      = 1u << 0,
        VT_ATTR_DIM       = 1u << 1,
        VT_ATTR_ITALIC    = 1u << 2,
        VT_ATTR_UNDERLINE = 1u << 3,
        VT_ATTR_BLINK     = 1u << 4,
        VT_ATTR_REVERSE   = 1u << 5,
        VT_ATTR_INVISIBLE = 1u << 6,
        VT_ATTR_STRIKE    = 1u << 7,
};

enum {
        VT_WIDTH_NARROW = 0,
        VT_WIDTH_WIDE   = 1,
        VT_WIDTH_TRAIL  = 2,
};

typedef struct {
        uint32_t ch;    /* Unicode scalar value, or 0 for a blank cell */
        uint32_t fg;
        uint32_t bg;
        uint8_t  attr;  /* VT_ATTR_* bits */
        uint8_t  width; /* VT_WIDTH_* */
} vt_cell_t;

typedef struct vt vt_t;

/* Optional CSI response sink (DA / DSR / size reports).  The frontend writes
 * the bytes back to the pty master. */
typedef void (*vt_response_fn)(void *ctx, const char *bytes, size_t len);

vt_t *vt_create(int rows, int cols);
void  vt_destroy(vt_t *vt);

/* Reset terminal state without changing the allocated grid size. */
void vt_reset(vt_t *vt);

void vt_set_default_colors(vt_t *vt, uint32_t fg, uint32_t bg);

/* Feed raw bytes from the pty master. */
void vt_feed(vt_t *vt, const uint8_t *data, size_t len);

int vt_rows(const vt_t *vt);
int vt_cols(const vt_t *vt);
int vt_cursor_row(const vt_t *vt);
int vt_cursor_col(const vt_t *vt);
int vt_cursor_visible(const vt_t *vt);

/* Dirty-row query.  A true vt_dirty_all() means every row is dirty. */
int  vt_dirty_all(const vt_t *vt);
int  vt_row_dirty(const vt_t *vt, int row);
void vt_clear_dirty(vt_t *vt);
void vt_mark_all_dirty(vt_t *vt);

const vt_cell_t *vt_cell(const vt_t *vt, int row, int col);

/* Resize the grid, preserving the top-left overlap when possible. */
int vt_resize(vt_t *vt, int rows, int cols);

/* DECCKM (application cursor keys) and application keypad flags. */
int vt_decckm(const vt_t *vt);
int vt_keypad_app(const vt_t *vt);

/* Responses generated while parsing (may be NULL/0).  The buffer is owned by
 * the vt and is reset by the call. */
const char *vt_take_response(vt_t *vt, size_t *len);
void        vt_set_response_callback(vt_t *vt, vt_response_fn fn, void *ctx);

/* UTF-8 helper shared by the VT and the input encoder.  Returns bytes written,
 * or 0 when the output buffer is too small. */
size_t vt_utf8_encode(uint32_t cp, char *out, size_t cap);
