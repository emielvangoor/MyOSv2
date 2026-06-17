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
//
// DUAL-BUILD GUARD NOTE (load-bearing):
// ======================================
// rd_core.c is compiled into BOTH the kernel (via CSRC := $(wildcard src/*.c))
// and the user-space lm.elf (via LM_CORE). The kernel test suite (src/tests.c)
// includes rd.h and calls rd_buf_insert/rd_buf_len/etc., but the kernel has no
// lm.h and no Lobj type. Therefore, every Lisp-touching piece of rd.h and
// rd_core.c is wrapped in #ifdef LM_BUILD ... #endif. The lm.elf compile rule
// passes -DLM_BUILD; the kernel build does not, so it sees the plain gap buffer
// + grid exactly as it always has. KTESTs are unaffected.
#pragma once
#include <stdint.h>

// Pull in the Lisp object type only for the lm.elf build (not the kernel).
// This must come before struct rd_buffer, which conditionally grows a
// text-property interval array that references Lobj.
#ifdef LM_BUILD
#include "lm.h"
#endif

// ---- Text-property intervals (LM_BUILD only) --------------------------------
//
// Each buffer can carry a small sorted array of non-overlapping intervals, each
// covering a character range [start, end) in TEXT COORDINATES (same as point --
// logical character positions, gap-independent). The plist is a standard Lisp
// property list (k v k v ...) storing arbitrary per-range properties, with
// 'face' being the display-relevant one. This is exactly Emacs's text-property
// mechanism: the buffer owns the intervals; the GC must trace them; insert and
// delete adjust the boundaries automatically.
//
// The fixed RD_MAX_INTERVALS cap (256 per buffer) keeps allocation freestanding
// and GC tracing trivial. If a buffer fills its quota, further put-text-property
// calls silently no-op the excess -- acceptable for the use case (ls output has
// a bounded number of color spans per screen).
#ifdef LM_BUILD
struct rd_interval {
    int start, end;   // [start, end) in text coordinates
    Lobj plist;       // Lisp property list: (k1 v1 k2 v2 ...) or Qnil
};
#define RD_MAX_INTERVALS 256
#endif

#define RD_CELL_W 20
#define RD_CELL_H 40                // the prerendered anti-aliased font's cell
                                    // (must match src/font_aa.h's FONT_AA_W/H)
#define RD_MAX_WIN 16               // window-tree node pool (per frame)
// 256 = the most a uint8_t cell.face can index. We register ~38 named faces
// (3 built-in + 16 fg + 16 bg + bold/underline/inverse) and rd_resolve_face
// allocates a slot per distinct MERGED combo (fg+bg+attrs) seen during render;
// 64 exhausted it (the bg row + attributes fell back to `default`), so use the
// full 256. find-or-allocate dedups, so per-character redisplay doesn't grow it.
#define RD_NFACES 256
#define RD_MAX_RECTS 32
#define RD_ECHO_MAX 10              // minibuffer: input + up to 9 candidates

// Buffer kinds: TEXT renders through the glyph grid; SURFACE (Phase 25.6)
// will blit a pixel canvas instead. The field exists now so 25.6 needs no
// structural surgery.
#define RD_TEXT    0
#define RD_SURFACE 1

// A named, themeable face: two 0x00RRGGBB colors plus attribute bits.
// fg_set / bg_set indicate whether this face explicitly specifies each color
// (1) or defers to the next face in the merge chain (0 = inherit). This lets
// a face like `bold` carry only bold=1 without clobbering fg/bg.
//
// The kernel renderer reads only fg and bg; the extra bytes are plain ints
// (no Lobj) so the struct is kernel-safe and needs NO #ifdef guard.
struct rd_face {
    uint32_t fg, bg;                             // 0x00RRGGBB each
    unsigned char bold, inverse, underline;      // attributes; inverse swaps fg/bg at paint time
    unsigned char fg_set, bg_set;                // 1 = this face specifies the color (else inherit)
};
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
#ifdef LM_BUILD
    // Text-property intervals: a sorted, non-overlapping array of intervals
    // each carrying a Lisp plist of character properties. Only present in the
    // lm.elf build where Lobj exists; the kernel build never sees this field.
    // The GC must trace every plist here (see rd_buf_mark_props + gc_collect).
    struct rd_interval ivals[RD_MAX_INTERVALS];
    int n_ivals;
#endif
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
    // High-water mark: the next free slot in faces[]. Built-in faces 0/1/2 are
    // allocated by rd_frame_init; lm_gfx's defface and rd_resolve_face's
    // merge-allocator BOTH claim slots from this single cursor so they never
    // collide. Plain int -- no guard needed.
    int n_faces_used;
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

// ---- text properties (LM_BUILD only) ----------------------------------------
//
// These functions are compiled into rd_core.c only when -DLM_BUILD is set
// (i.e. when building lm.elf). The kernel build never defines LM_BUILD and
// therefore never sees these declarations or their implementations. All
// position arguments are in TEXT COORDINATES (logical char positions, same
// as rd_buffer.point -- gap-independent).
#ifdef LM_BUILD
// Set PROP=VAL on every character in [start, end). Other properties on
// pre-existing intervals inside that range are PRESERVED (plist merge, like
// Emacs put-text-property). Silently no-ops if the interval table is full.
void rd_put_text_prop(struct rd_buffer *b, int start, int end,
                      Lobj prop, Lobj val);

// Return the value of PROP at character position POS, or Qnil if the position
// has no covering interval or PROP is absent from its plist.
Lobj rd_get_text_prop(struct rd_buffer *b, int pos, Lobj prop);

// Return the full property list at POS (the covering interval's plist), or
// Qnil if POS has no text properties.
Lobj rd_text_props_at(struct rd_buffer *b, int pos);

// Replace the entire property list over [start, end): every covered interval's
// plist becomes PLIST (no merge -- like Emacs set-text-properties).
void rd_set_text_props(struct rd_buffer *b, int start, int end, Lobj plist);

// Remove the keys listed in PROPS (a list of symbols; values are ignored, just
// like Emacs remove-text-properties) from every interval intersecting [start, end).
// Intervals whose plist becomes empty are dropped from the table.
void rd_remove_text_props(struct rd_buffer *b, int start, int end, Lobj props);

// GC root phase helper: call mark(plist) for every interval in the buffer so
// the mark-sweep collector can reach the property plists. Without this, a GC
// between put-text-property and redisplay could free a face cons cell.
void rd_buf_mark_props(struct rd_buffer *b, void (*mark)(Lobj));

// ---- face name resolver (LM_BUILD only) --------------------------------------
//
// rd_resolve_face maps a `face` text-property value to a concrete cell face id:
//   nil / unknown symbol -> 0 (default face)
//   a face-name symbol   -> the id registered by gfx_face_id()
//   a list of face names -> merge all named faces in order (later overrides),
//                           then find-or-allocate a slot for the merged result.
//
// gfx_face_id is the lookup half (lm_gfx.c owns the name->id registry).
// rd_resolve_face is the merge/resolve half (rd_core.c, guarded here).
// Together they are the bridge that lets `face = ansi-blue` (a Lisp symbol in
// a text-property) turn into a numeric slot id the cell grid understands.
int  rd_resolve_face(struct rd_frame *f, Lobj face_value);
int  gfx_face_id(Lobj name);   // implemented in lm_gfx.c
#endif
