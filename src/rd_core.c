// rd_core.c -- the redisplay engine (Phase 25.3). See rd.h for the design.
// ========================================================================
//
// Reading order: gap buffer -> window tree -> layout (model -> cell grid) ->
// diff + paint (grid -> pixels + damage). The whole file is freestanding and
// side-effect-free toward the platform: pixels land in a caller buffer,
// damage rects go back to the caller. That is what lets the kernel KTESTs
// drive it as a pure function of its inputs.

#include "rd.h"
#include "font8x8.h"

// ---- tiny freestanding helpers ------------------------------------------

static int rd_slen(const char *s) { int n = 0; while (s[n]) { n++; } return n; }
static void rd_scpy(char *d, const char *s, int cap)
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
    b->text = store; b->cap = cap;
    b->gap_start = 0; b->gap_end = cap;
    b->point = 0;
    b->kind = RD_TEXT;
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
    for (int i = 2; i < RD_NFACES; i++) { f->faces[i] = f->faces[0]; }
    f->echo[0] = 0;
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

void rd_echo(struct rd_frame *f, const char *s)
{
    rd_scpy(f->echo, s, (int)sizeof(f->echo));
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

    // The modeline: "-- <name> " padded with dashes, in face 1.
    char ml[256];
    int n = 0;
    ml[n++] = '-'; ml[n++] = '-'; ml[n++] = ' ';
    const char *nm = b ? b->name : "?";
    for (int i = 0; nm[i] && n < (int)sizeof(ml) - 2; i++) { ml[n++] = nm[i]; }
    ml[n++] = ' ';
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
    // Window area = everything above the echo line.
    layout_tree(f, f->root, 0, 0, f->cols, f->rows - 1);
    // The echo area: the frame's last cell row, default face.
    int row = f->rows - 1;
    for (int col = 0; col < f->cols; col++) {
        int c = col < rd_slen(f->echo) ? f->echo[col] : ' ';
        put_cell(f, col, row, c, 0);
    }
}

const struct rd_cell *rd_cell_at(const struct rd_frame *f, int col, int row)
{
    return &f->back[row * f->cols + col];
}

// ---- paint + diff -----------------------------------------------------------

// Paint one cell's 8x16 pixel block from the 8x8 font (each row twice).
// `invert` swaps fg/bg -- that's the cursor.
static void paint_cell(const struct rd_frame *f, uint32_t *fb, int stride,
                       int col, int row, const struct rd_cell *cell, int invert)
{
    const struct rd_face *face = &f->faces[cell->face < RD_NFACES ? cell->face : 0];
    uint32_t fg = invert ? face->bg : face->fg;
    uint32_t bg = invert ? face->fg : face->bg;
    const uint8_t *glyph = font8x8_basic[cell->ch < 128 ? cell->ch : '?'];
    for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = glyph[gy];
        for (int dy = 0; dy < 2; dy++) {          // each font row painted twice
            uint32_t *out = fb + (row * RD_CELL_H + gy * 2 + dy) * stride
                               + col * RD_CELL_W;
            for (int gx = 0; gx < RD_CELL_W; gx++) {
                out[gx] = (bits & (1u << gx)) ? fg : bg;
            }
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
    return nrects;
}
