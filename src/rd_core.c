// rd_core.c -- the redisplay engine (Phase 25.3). See rd.h for the design.
// ========================================================================
//
// Reading order: gap buffer -> window tree -> layout (model -> cell grid) ->
// diff + paint (grid -> pixels + damage). The whole file is freestanding and
// side-effect-free toward the platform: pixels land in a caller buffer,
// damage rects go back to the caller. That is what lets the kernel KTESTs
// drive it as a pure function of its inputs.

#include "rd.h"
#include "font_aa.h"

// ---- tiny freestanding helpers ------------------------------------------

static int rd_slen(const char *s) { int n = 0; while (s[n]) { n++; } return n; }
// Bounded string copy; declared in rd.h so tests can call it directly.
void rd_scpy(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

// ---- gap buffer -----------------------------------------------------------
//
// text[0, gap_start) and text[gap_end, cap) hold the document; the hole
// between them is where insertion happens. Editing AT the gap is O(1);
// editing elsewhere first MOVES the gap there (one memmove-shaped loop).
// This is the storage Emacs has used for forty years -- cheap local edits,
// which is exactly what a REPL buffer does all day.

void rd_buf_init(struct rd_buffer *b, const char *name, char *store, int cap)
{
    rd_scpy(b->name, name, (int)sizeof(b->name));
    b->mode_line[0] = 0;            // no mode name until set-mode-line-name
    b->text = store; b->cap = cap;
    b->gap_start = 0; b->gap_end = cap;
    b->point = 0;
    b->kind = RD_TEXT;
    b->canvas = 0; b->cv_w = 0; b->cv_h = 0;
}

int rd_buf_len(const struct rd_buffer *b)
{
    return b->cap - (b->gap_end - b->gap_start);
}

int rd_buf_char_at(const struct rd_buffer *b, int pos)
{
    if (pos < 0 || pos >= rd_buf_len(b)) { return -1; }
    // Positions below the gap read directly; above it, skip over the hole.
    return (unsigned char)(pos < b->gap_start
                           ? b->text[pos]
                           : b->text[pos + (b->gap_end - b->gap_start)]);
}

// Slide the gap so that gap_start == pos (the precondition for editing there).
static void gap_move(struct rd_buffer *b, int pos)
{
    while (b->gap_start > pos) {               // gap moves left: char hops right
        b->gap_start--; b->gap_end--;
        b->text[b->gap_end] = b->text[b->gap_start];
    }
    while (b->gap_start < pos) {               // gap moves right: char hops left
        b->text[b->gap_start] = b->text[b->gap_end];
        b->gap_start++; b->gap_end++;
    }
}

void rd_buf_insert(struct rd_buffer *b, const char *s)
{
    gap_move(b, b->point);
    for (int i = 0; s[i]; i++) {
        if (b->gap_start == b->gap_end) { return; }   // full: drop the rest
        b->text[b->gap_start++] = s[i];
        b->point++;
    }
}

void rd_buf_delete(struct rd_buffer *b, int n)
{
    gap_move(b, b->point);
    while (n-- > 0 && b->gap_start > 0) {       // deletion = the gap eats left
        b->gap_start--;
        b->point--;
    }
}

void rd_buf_set_point(struct rd_buffer *b, int pos)
{
    int len = rd_buf_len(b);
    if (pos < 0) { pos = 0; }
    if (pos > len) { pos = len; }
    b->point = pos;
}

// ---- frame & window tree --------------------------------------------------

static struct rd_win *win_alloc(struct rd_frame *f)
{
    for (int i = 0; i < RD_MAX_WIN; i++) {
        if (!f->wins[i].used) {
            struct rd_win *w = &f->wins[i];
            w->used = 1; w->leaf = 1; w->vertical = 0; w->ratio_pct = 50;
            w->a = w->b = w->parent = 0;
            w->buf = 0; w->top_line = 0;
            w->x = w->y = w->w = w->h = 0;
            return w;
        }
    }
    return 0;
}

void rd_frame_init(struct rd_frame *f, int px_w, int px_h,
                   struct rd_cell *front, struct rd_cell *back,
                   struct rd_buffer *initial)
{
    f->px_w = px_w; f->px_h = px_h;
    f->cols = px_w / RD_CELL_W;
    f->rows = px_h / RD_CELL_H;
    for (int i = 0; i < RD_MAX_WIN; i++) { f->wins[i].used = 0; }
    f->root = f->selected = win_alloc(f);
    f->root->buf = initial;
    // Default faces: 0 = text (light on near-black), 1 = modeline (inverse),
    // 2 = reserved for region highlight later. Lisp can repaint these.
    f->faces[0].fg = 0x00D5C4A1; f->faces[0].bg = 0x001D2021;
    f->faces[1].fg = 0x001D2021; f->faces[1].bg = 0x00928374;
    f->faces[2].fg = 0x00FBF1C7; f->faces[2].bg = 0x00504945;   // selection bar
    for (int i = 3; i < RD_NFACES; i++) { f->faces[i] = f->faces[0]; }
    f->echo[0] = 0;
    f->echo_sel = -1;
    f->front = front; f->back = back;
    // Force a full first paint: make the front grid impossible cells.
    for (int i = 0; i < f->cols * f->rows; i++) {
        f->front[i].ch = 0xFF; f->front[i].face = 0xFF;
    }
    f->cursor_col = f->cursor_row = 0;
    f->cur_pcol = f->cur_prow = -1;
}

// Split the selected leaf in two. The selected window keeps the first half
// (top / left) and its buffer; the new leaf shows the same buffer until
// rd_set_buffer changes it -- exactly Emacs's C-x 2 / C-x 3.
struct rd_win *rd_split(struct rd_frame *f, int vertical)
{
    struct rd_win *leaf = f->selected;
    struct rd_win *a = win_alloc(f);
    struct rd_win *b = win_alloc(f);
    if (!a || !b) {
        if (a) { a->used = 0; }
        return 0;
    }
    a->buf = leaf->buf; a->top_line = leaf->top_line; a->parent = leaf;
    b->buf = leaf->buf; b->top_line = leaf->top_line; b->parent = leaf;
    leaf->leaf = 0; leaf->vertical = vertical; leaf->ratio_pct = 50;
    leaf->a = a; leaf->b = b;
    f->selected = a;
    return b;
}

// Delete the selected window: its sibling takes over the parent's rect (the
// parent split node becomes the sibling). Refuses on the last window.
int rd_win_delete(struct rd_frame *f)
{
    struct rd_win *dead = f->selected;
    struct rd_win *parent = dead->parent;
    if (!parent) { return -1; }                  // the only window
    struct rd_win *sib = (parent->a == dead) ? parent->b : parent->a;
    // The parent split node becomes the sibling (copy it up), so pointers to
    // the parent (its own parent's a/b) stay valid.
    parent->leaf = sib->leaf; parent->vertical = sib->vertical;
    parent->ratio_pct = sib->ratio_pct;
    parent->a = sib->a; parent->b = sib->b;
    parent->buf = sib->buf; parent->top_line = sib->top_line;
    if (sib->a) { sib->a->parent = parent; }
    if (sib->b) { sib->b->parent = parent; }
    sib->used = 0; dead->used = 0;
    // Select the first leaf under the survivor.
    struct rd_win *w = parent;
    while (!w->leaf) { w = w->a; }
    f->selected = w;
    return 0;
}

// Depth-first successor leaf (wrapping): the C-x o order.
static struct rd_win *first_leaf(struct rd_win *w)
{
    while (w && !w->leaf) { w = w->a; }
    return w;
}

void rd_other_window(struct rd_frame *f)
{
    // Walk up until we are someone's `a`, then down the sibling's left spine;
    // if we run off the top, wrap to the leftmost leaf.
    struct rd_win *w = f->selected;
    while (w->parent && w->parent->b == w) { w = w->parent; }
    if (w->parent) {
        f->selected = first_leaf(w->parent->b);
    } else {
        f->selected = first_leaf(f->root);
    }
}

void rd_set_buffer(struct rd_frame *f, struct rd_buffer *b)
{
    f->selected->buf = b;
    f->selected->top_line = 0;
}

// Collapse the window tree to a single window showing the selected buffer --
// Emacs's C-x 1 (delete-other-windows). The pool is just reused: free every
// node, then re-grow a lone root carrying the selected buffer + scroll.
void rd_delete_other(struct rd_frame *f)
{
    struct rd_buffer *b = f->selected->buf;
    int top = f->selected->top_line;
    for (int i = 0; i < RD_MAX_WIN; i++) { f->wins[i].used = 0; }
    f->root = f->selected = win_alloc(f);
    f->root->buf = b;
    f->root->top_line = top;
}

void rd_echo(struct rd_frame *f, const char *s)
{
    rd_scpy(f->echo, s, (int)sizeof(f->echo));
    f->echo_sel = -1;
}

void rd_echo_select(struct rd_frame *f, int line)
{
    f->echo_sel = line;
}

// How many lines the echo text wants (1..RD_ECHO_MAX). The echo area GROWS
// for multi-line content -- that is the whole minibuffer/vertico mechanism:
// Lisp just hands redisplay a few lines of text and a selected index.
static int echo_lines(const struct rd_frame *f)
{
    int n = 1;
    for (int i = 0; f->echo[i]; i++) {
        if (f->echo[i] == '\n') { n++; }
    }
    return n > RD_ECHO_MAX ? RD_ECHO_MAX : n;
}

// ---- layout: model -> back grid --------------------------------------------

static void put_cell(struct rd_frame *f, int col, int row, int ch, int face)
{
    if (col < 0 || col >= f->cols || row < 0 || row >= f->rows) { return; }
    f->back[row * f->cols + col].ch = (unsigned char)ch;
    f->back[row * f->cols + col].face = (unsigned char)face;
}

// Lay out one leaf window's rect: text lines (truncated, never wrapped),
// then its modeline on its last row.
static void layout_leaf(struct rd_frame *f, struct rd_win *w)
{
    struct rd_buffer *b = w->buf;
    int text_rows = w->h - 1;                    // last row is the modeline

    if (b && b->kind == RD_SURFACE) {
        // A surface window's content is the canvas, blitted in rd_redisplay
        // AFTER cell painting (so the blit wins). The cells underneath are
        // blanks; the modeline below still renders like any other window's.
        for (int row = 0; row < text_rows; row++) {
            for (int col = 0; col < w->w; col++) {
                put_cell(f, w->x + col, w->y + row, ' ', 0);
            }
        }
        goto modeline;
    }

    // Scroll the Emacs way: keep point on screen by nudging the window's
    // start line (top_line == Emacs's window-start). The machinery existed;
    // this is what actually moves it. We scroll minimally -- just enough to
    // bring point's line back into [top_line, top_line + text_rows) -- so a
    // REPL whose output grows downward stays pinned to the bottom, and moving
    // point up past the top scrolls up by the overflow.
    if (b) {
        int point_line = 0;
        for (int p = 0; p < b->point; p++) {
            if (rd_buf_char_at(b, p) == '\n') { point_line++; }
        }
        if (w->top_line > point_line) { w->top_line = point_line; }
        if (point_line >= w->top_line + text_rows) {
            w->top_line = point_line - text_rows + 1;
        }
        if (w->top_line < 0) { w->top_line = 0; }
    }

    // Walk the buffer line by line; render lines [top_line, top_line+rows).
    int pos = 0, line = 0, len = b ? rd_buf_len(b) : 0;
    for (int row = 0; row < text_rows; row++) {
        // Find the start of buffer line (w->top_line + row): advance `pos`.
        while (line < w->top_line + row && pos < len) {
            if (rd_buf_char_at(b, pos++) == '\n') { line++; }
        }
        int col = 0;
        int p = pos;
        if (line == w->top_line + row) {
            while (p < len) {
                int c = rd_buf_char_at(b, p);
                if (c == '\n') { break; }
                if (col < w->w) {                 // truncate at the window edge
                    put_cell(f, w->x + col, w->y + row, c, 0);
                }
                // The cursor lives where point is, in the selected window.
                if (b->point == p && w == f->selected && col < w->w) {
                    f->cursor_col = w->x + col; f->cursor_row = w->y + row;
                }
                col++; p++;
            }
            // Point at end of this line (incl. end of buffer).
            if (w == f->selected && b->point == p && col < w->w) {
                f->cursor_col = w->x + col; f->cursor_row = w->y + row;
            }
        }
        for (; col < w->w; col++) { put_cell(f, w->x + col, w->y + row, ' ', 0); }
    }

modeline:;
    // The modeline: "-- <name> " padded with dashes, in face 1.
    char ml[256];
    int n = 0;
    ml[n++] = '-'; ml[n++] = '-'; ml[n++] = ' ';
    const char *nm = b ? b->name : "?";
    for (int i = 0; nm[i] && n < (int)sizeof(ml) - 2; i++) { ml[n++] = nm[i]; }
    ml[n++] = ' ';
    // mode_line is set from Lisp by (set-mode-line-name) when a buffer
    // enters a major mode; paint "  (Name)" after the buffer name so the
    // active mode is visible in the mode line without querying Lisp.
    if (b && b->mode_line[0]) {
        if (n < (int)sizeof(ml) - 1) { ml[n++] = ' '; }
        if (n < (int)sizeof(ml) - 1) { ml[n++] = '('; }
        for (int i = 0; b->mode_line[i] && n < (int)sizeof(ml) - 3; i++) {
            ml[n++] = b->mode_line[i];
        }
        if (n < (int)sizeof(ml) - 2) { ml[n++] = ')'; }
        if (n < (int)sizeof(ml) - 1) { ml[n++] = ' '; }
    }
    while (n < w->w && n < (int)sizeof(ml) - 1) { ml[n++] = '-'; }
    ml[n] = 0;
    for (int col = 0; col < w->w; col++) {
        put_cell(f, w->x + col, w->y + w->h - 1, col < n ? ml[col] : '-', 1);
    }
}

// Assign rects (in cells) down the tree, then lay out every leaf.
static void layout_tree(struct rd_frame *f, struct rd_win *w,
                        int x, int y, int wd, int ht)
{
    w->x = x; w->y = y; w->w = wd; w->h = ht;
    if (w->leaf) {
        layout_leaf(f, w);
        return;
    }
    if (w->vertical) {                            // side by side
        int aw = wd * w->ratio_pct / 100;
        layout_tree(f, w->a, x, y, aw, ht);
        layout_tree(f, w->b, x + aw, y, wd - aw, ht);
    } else {                                      // stacked
        int ah = ht * w->ratio_pct / 100;
        layout_tree(f, w->a, x, y, wd, ah);
        layout_tree(f, w->b, x, y + ah, wd, ht - ah);
    }
}

void rd_layout(struct rd_frame *f)
{
    f->cursor_col = f->cursor_row = -1;
    // Window area = everything above the echo AREA, whose height follows its
    // content (1 line normally; up to RD_ECHO_MAX when the minibuffer shows
    // vertico-style candidates). Line echo_sel renders in face 2 -- the
    // selection bar.
    int elines = echo_lines(f);
    layout_tree(f, f->root, 0, 0, f->cols, f->rows - elines);
    const char *p = f->echo;
    for (int line = 0; line < elines; line++) {
        int row = f->rows - elines + line;
        int face = (line == f->echo_sel) ? 2 : 0;
        int col = 0;
        while (*p && *p != '\n' && col < f->cols) { put_cell(f, col++, row, *p++, face); }
        while (*p && *p != '\n') { p++; }           // clip an over-long line
        if (*p == '\n') { p++; }
        for (; col < f->cols; col++) { put_cell(f, col, row, ' ', face); }
    }
}

const struct rd_cell *rd_cell_at(const struct rd_frame *f, int col, int row)
{
    return &f->back[row * f->cols + col];
}

// ---- paint + diff -----------------------------------------------------------

// Paint one cell from the prerendered anti-aliased font: each glyph pixel is
// an 8-bit COVERAGE value, blended between the face's bg and fg with integer
// math -- out = bg + (fg-bg)*a/255, per channel. That single line is all of
// grayscale font anti-aliasing; the expensive part (rasterizing curves) was
// done once, on the host, by tools/gen_font.py. `invert` swaps fg/bg -- the
// cursor.
static inline uint32_t blend(uint32_t bg, uint32_t fg, uint32_t a)
{
    if (a == 0)   { return bg; }
    if (a == 255) { return fg; }
    int32_t ia = (int32_t)a;
    int32_t r = (int32_t)((bg >> 16) & 0xFF); r += (((int32_t)((fg >> 16) & 0xFF) - r) * ia) / 255;
    int32_t g = (int32_t)((bg >> 8)  & 0xFF); g += (((int32_t)((fg >> 8)  & 0xFF) - g) * ia) / 255;
    int32_t b = (int32_t)( bg        & 0xFF); b += (((int32_t)( fg        & 0xFF) - b) * ia) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void paint_cell(const struct rd_frame *f, uint32_t *fb, int stride,
                       int col, int row, const struct rd_cell *cell, int invert)
{
    const struct rd_face *face = &f->faces[cell->face < RD_NFACES ? cell->face : 0];
    uint32_t fg = invert ? face->bg : face->fg;
    uint32_t bg = invert ? face->fg : face->bg;
    int ch = (cell->ch >= FONT_AA_FIRST && cell->ch <= FONT_AA_LAST) ? cell->ch : '?';
    const uint8_t *glyph = font_aa[ch - FONT_AA_FIRST];
    for (int gy = 0; gy < RD_CELL_H; gy++) {
        uint32_t *out = fb + (row * RD_CELL_H + gy) * stride + col * RD_CELL_W;
        const uint8_t *arow = glyph + gy * RD_CELL_W;
        for (int gx = 0; gx < RD_CELL_W; gx++) {
            out[gx] = blend(bg, fg, arow[gx]);
        }
    }
}

int rd_redisplay(struct rd_frame *f, uint32_t *fb, int stride_px,
                 struct rd_rect *rects, int max_rects)
{
    rd_layout(f);

    int nrects = 0;
    for (int row = 0; row < f->rows; row++) {
        int run_start = -1;
        for (int col = 0; col <= f->cols; col++) {
            const struct rd_cell *bk = (col < f->cols) ? &f->back[row * f->cols + col] : 0;
            struct rd_cell *fr = (col < f->cols) ? &f->front[row * f->cols + col] : 0;
            // The cursor cell is "changed" only when the cursor ARRIVED or
            // LEFT it (cur_pcol/cur_prow live in the frame -- a static here
            // would be the exact cross-init bug 25.2 taught us about).
            int cursor_here = (col == f->cursor_col && row == f->cursor_row);
            int cursor_was  = (col == f->cur_pcol && row == f->cur_prow);
            int changed = bk && (bk->ch != fr->ch || bk->face != fr->face ||
                                 cursor_here != cursor_was);
            if (changed) {
                if (fb) { paint_cell(f, fb, stride_px, col, row, bk, cursor_here); }
                *fr = *bk;
                if (run_start < 0) { run_start = col; }
            } else if (run_start >= 0) {
                // Close the run of changed cells -> one damage rect per row run.
                if (nrects < max_rects) {
                    rects[nrects].x = run_start * RD_CELL_W;
                    rects[nrects].y = row * RD_CELL_H;
                    rects[nrects].w = (col - run_start) * RD_CELL_W;
                    rects[nrects].h = RD_CELL_H;
                    nrects++;
                } else if (max_rects > 0) {
                    // Out of slots: grow the last rect to cover (stay correct).
                    struct rd_rect *r = &rects[max_rects - 1];
                    int x2 = col * RD_CELL_W, y2 = (row + 1) * RD_CELL_H;
                    if (r->x > run_start * RD_CELL_W) { r->x = run_start * RD_CELL_W; }
                    if (r->y + r->h < y2) { r->h = y2 - r->y; }
                    if (r->x + r->w < x2) { r->w = x2 - r->x; }
                }
                run_start = -1;
            }
        }
    }
    f->cur_pcol = f->cursor_col; f->cur_prow = f->cursor_row;

    // Surface windows: blit each canvas over its window's text area and
    // damage the whole rect. No diffing for pixels -- the program may have
    // drawn anything since last time; one memcpy-shaped blit per redisplay
    // is the honest price of arbitrary graphics.
    for (int i = 0; i < RD_MAX_WIN; i++) {
        struct rd_win *w = &f->wins[i];
        if (!w->used || !w->leaf || !w->buf || w->buf->kind != RD_SURFACE ||
            !w->buf->canvas) { continue; }
        int px = w->x * RD_CELL_W, py = w->y * RD_CELL_H;
        int pw = w->w * RD_CELL_W, ph = (w->h - 1) * RD_CELL_H;
        int bw = w->buf->cv_w < pw ? w->buf->cv_w : pw;     // crop top-left
        int bh = w->buf->cv_h < ph ? w->buf->cv_h : ph;
        if (fb) {
            for (int y = 0; y < bh; y++) {
                uint32_t *dst = fb + (py + y) * stride_px + px;
                const uint32_t *src = w->buf->canvas + y * w->buf->cv_w;
                for (int x = 0; x < bw; x++) { dst[x] = src[x]; }
            }
        }
        if (nrects < max_rects) {
            rects[nrects].x = px; rects[nrects].y = py;
            rects[nrects].w = bw; rects[nrects].h = bh;
            nrects++;
        }
    }
    return nrects;
}
