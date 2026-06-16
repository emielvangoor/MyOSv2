// rd.h -- the redisplay engine of the graphical Lisp machine (Phase 25.3).
// ========================================================================
//
// This is the view half of the Emacs architecture: Lisp (or a test) mutates
// BUFFERS, a WINDOW TREE and FACES, then calls rd_redisplay() and the screen
// is made to match. Like lm_core, this file's implementation (rd_core.c) is
// portable -- no libc, no kernel headers, no syscalls -- and is dual-built:
// into the kernel so KTEST can red-green layout and damage logic, and into
// /bin/lisp where it renders the real framebuffer.
//
// The design is GLYPH MATRICES (what curses calls double buffering and Emacs
// calls glyph matrices): redisplay lays the model out into a BACK grid of
// cells {character, face}, diffs it against the FRONT grid (what's on screen),
// paints only the changed cells into the pixel framebuffer, and reports the
// changed area as damage rects for the caller to gfx_flush. Damage minimality
// is not an optimization pass -- it falls out of the diff.
#pragma once
#include <stdint.h>

#define RD_CELL_W 20
#define RD_CELL_H 40                // the prerendered anti-aliased font's cell
                                    // (must match src/font_aa.h's FONT_AA_W/H)
#define RD_MAX_WIN 16               // window-tree node pool (per frame)
#define RD_NFACES 8
#define RD_MAX_RECTS 32
#define RD_ECHO_MAX 10              // minibuffer: input + up to 9 candidates

// Buffer kinds: TEXT renders through the glyph grid; SURFACE (Phase 25.6)
// will blit a pixel canvas instead. The field exists now so 25.6 needs no
// structural surgery.
#define RD_TEXT    0
#define RD_SURFACE 1

struct rd_face { uint32_t fg, bg; };          // 0x00RRGGBB each
struct rd_cell { unsigned char ch, face; };

// A gap buffer: the text is text[0,gap_start) + text[gap_end,cap). Insertion
// at the gap is O(1); moving the gap is a memmove. The classic editor store.
struct rd_buffer {
    char name[32];
    char mode_line[24];             // mode name shown in the mode line ("" = none)
    int  wrap;                      // line-wrap minor mode: 1 = wrap long lines, 0 = truncate (default)
    char *text;
    int  cap, gap_start, gap_end;
    int  point;                     // cursor position, in TEXT coordinates
    int  kind;                      // RD_TEXT / RD_SURFACE
    // RD_SURFACE only: a pixel canvas (0x00RRGGBB words) a program draws
    // into -- the EXWM move: an external renderer appearing as a buffer.
    uint32_t *canvas;
    int cv_w, cv_h;
};

struct rd_win {
    int used, leaf;
    // split node:
    int vertical;                   // 1 = side by side (split-right)
    int ratio_pct;                  // a's share of the rect, in percent
    struct rd_win *a, *b, *parent;
    // leaf node:
    struct rd_buffer *buf;
    int top_line;                   // first visible buffer line (scroll)
    // computed by layout (cells, frame-relative):
    int x, y, w, h;
};

struct rd_frame {
    int px_w, px_h;                 // pixels
    int cols, rows;                 // cells (rows includes the echo line)
    struct rd_win wins[RD_MAX_WIN];
    struct rd_win *root, *selected;
    struct rd_face faces[RD_NFACES];   // 0 default, 1 modeline, 2 cursor
    char echo[512];                 // multi-line: the minibuffer lives here
    int  echo_sel;                  // echo line drawn as the selection bar (-1 none)
    struct rd_cell *front, *back;   // cols*rows each, caller-allocated
    int cursor_col, cursor_row;     // computed by layout (selected's point)
    int cur_pcol, cur_prow;         // where the cursor was last painted
};

struct rd_rect { int x, y, w, h; };           // pixels

// ---- helpers ----
void rd_scpy(char *d, const char *s, int cap);   // bounded string copy (public for tests)

// ---- buffers ----
void rd_buf_init(struct rd_buffer *b, const char *name, char *store, int cap);
int  rd_buf_len(const struct rd_buffer *b);
int  rd_buf_char_at(const struct rd_buffer *b, int pos);   // -1 past end
void rd_buf_insert(struct rd_buffer *b, const char *s);    // at point
void rd_buf_delete(struct rd_buffer *b, int n);            // n chars before point
void rd_buf_set_point(struct rd_buffer *b, int pos);

// ---- frame / windows ----
void rd_frame_init(struct rd_frame *f, int px_w, int px_h,
                   struct rd_cell *front, struct rd_cell *back,
                   struct rd_buffer *initial);
struct rd_win *rd_split(struct rd_frame *f, int vertical);  // selected leaf -> 2
int  rd_win_delete(struct rd_frame *f);                     // remove selected
void rd_delete_other(struct rd_frame *f);                   // C-x 1: selected fills frame
void rd_other_window(struct rd_frame *f);                   // cycle selection
void rd_set_buffer(struct rd_frame *f, struct rd_buffer *b);
void rd_echo(struct rd_frame *f, const char *s);            // resets selection
void rd_echo_select(struct rd_frame *f, int line);          // vertico bar

// ---- redisplay ----
void rd_layout(struct rd_frame *f);                         // model -> back grid
const struct rd_cell *rd_cell_at(const struct rd_frame *f, int col, int row);
// Layout + diff + paint + damage. fb may be NULL (layout/diff only -- tests).
// Returns the number of damage rects written (merged per cell-row run).
int  rd_redisplay(struct rd_frame *f, uint32_t *fb, int stride_px,
                  struct rd_rect *rects, int max_rects);
