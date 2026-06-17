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
    b->wrap = 0;                    // truncate long lines by default; opt in via (set-line-wrap)
    b->text = store; b->cap = cap;
    b->gap_start = 0; b->gap_end = cap;
    b->point = 0;
    b->kind = RD_TEXT;
    b->canvas = 0; b->cv_w = 0; b->cv_h = 0;
#ifdef LM_BUILD
    // Text-property interval table starts empty. n_ivals is the only state
    // needed; the ivals[] array content is undefined until slots are filled.
    b->n_ivals = 0;
#endif
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
#ifdef LM_BUILD
    // Capture insert position and length BEFORE mutating the gap buffer, so the
    // interval adjustment sees the pre-insert text coordinate. Text coords are
    // gap-independent (logical positions), so p == b->point is correct here.
    int p = b->point;
    int n = 0; { const char *t = s; while (t[n]) { n++; } }
#endif
    gap_move(b, b->point);
    for (int i = 0; s[i]; i++) {
        if (b->gap_start == b->gap_end) { return; }   // full: drop the rest
        b->text[b->gap_start++] = s[i];
        b->point++;
    }
#ifdef LM_BUILD
    // Shift every interval boundary that is >= the insert point by +n.
    // Boundaries exactly at p also shift, which means newly inserted text falls
    // BEFORE the existing interval -- inserted text is property-free by default,
    // exactly as Emacs's plain insert behaves (no sticky front/rear props here).
    for (int i = 0; i < b->n_ivals; i++) {
        if (b->ivals[i].start >= p) { b->ivals[i].start += n; }
        if (b->ivals[i].end   >= p) { b->ivals[i].end   += n; }
    }
#endif
}

void rd_buf_delete(struct rd_buffer *b, int n)
{
#ifdef LM_BUILD
    // rd_buf_delete deletes n characters BEFORE point (backspace-style). The
    // deleted text range is [d0, d1) where d1 = point (before), d0 = point - n.
    // Capture these before the gap mutation so interval adjustment is correct.
    // Note: if n > point the gap_start check below clamps, but we use the
    // nominal n here for the boundary mapping -- intervals outside the actually
    // deleted range are unaffected anyway (they lie at d0 or below).
    int d1 = b->point;
    int d0 = d1 - n;
    if (d0 < 0) { d0 = 0; }
#endif
    gap_move(b, b->point);
    while (n-- > 0 && b->gap_start > 0) {       // deletion = the gap eats left
        b->gap_start--;
        b->point--;
    }
#ifdef LM_BUILD
    // Adjust interval boundaries for the deletion of [d0, d1):
    //   x >= d1       -> x - (d1 - d0)   (shift down by the deleted span)
    //   d0 < x < d1   -> d0              (clamp into the deleted range)
    //   x <= d0       -> x               (unaffected)
    // After adjustment, drop any interval that became empty (start >= end).
    int del = d1 - d0;   // actual characters removed (may be < n if buffer was short)
    int w = 0;           // write cursor for compaction
    for (int i = 0; i < b->n_ivals; i++) {
        int s = b->ivals[i].start;
        int e = b->ivals[i].end;
        // Map start boundary
        if      (s >= d1) { s -= del; }
        else if (s >  d0) { s  = d0; }
        // Map end boundary
        if      (e >= d1) { e -= del; }
        else if (e >  d0) { e  = d0; }
        // Drop intervals that collapsed to empty
        if (s >= e) { continue; }
        b->ivals[w] = b->ivals[i];
        b->ivals[w].start = s;
        b->ivals[w].end   = e;
        w++;
    }
    b->n_ivals = w;
#endif
}

void rd_buf_set_point(struct rd_buffer *b, int pos)
{
    int len = rd_buf_len(b);
    if (pos < 0) { pos = 0; }
    if (pos > len) { pos = len; }
    b->point = pos;
}

// ---- text-property interval store (LM_BUILD only) -------------------------
//
// This block compiles only when -DLM_BUILD is set (i.e. in the lm.elf build).
// The kernel build never defines LM_BUILD, so KTESTs are completely unaffected.
//
// Design overview (Emacs-faithful):
//   - Intervals are kept in b->ivals[0..n_ivals) sorted by start.
//   - They are non-overlapping and cover only propertized ranges.
//   - A position with no covering interval has no properties (Qnil plist).
//   - put-text-property merges (like Emacs); set-text-properties replaces;
//     remove-text-properties strips named keys.
//   - insert shifts boundaries; delete clamps and compacts.
//
// Implementation strategy: simplicity over cleverness. The bounded array
// (RD_MAX_INTERVALS = 256) keeps allocation and GC tracing trivial. For the
// use case (ANSI-colored ls output), 256 spans per buffer is ample.
#ifdef LM_BUILD

// ---- plist helpers ----------------------------------------------------------
//
// A property list is a flat list of alternating key-value pairs:
//   (k1 v1 k2 v2 ...) or Qnil
// plist_get returns the first value for KEY (or Qnil).
// plist_put returns a new plist with KEY bound to VAL; the simplest correct
// implementation is to prepend a fresh (KEY VAL) pair at the front -- since
// plist_get returns the FIRST match, the prepended pair wins and the old one
// is shadowed (and eventually GC'd). This is safe and correct.

static Lobj plist_get(Lobj plist, Lobj key)
{
    // Walk (k v k v ...) stopping at the first k == key (interned symbols are
    // unique, so == is the right comparison -- no need for equal).
    while (IS_CONS(plist)) {
        Lobj k = CAR(plist);
        Lobj rest = CDR(plist);
        Lobj v = IS_CONS(rest) ? CAR(rest) : Qnil;
        if (k == key) { return v; }
        plist = IS_CONS(rest) ? CDR(rest) : Qnil;
    }
    return Qnil;
}

static Lobj plist_put(Lobj plist, Lobj key, Lobj val)
{
    // Prepend (key val) to plist. The old binding (if any) is shadowed --
    // plist_get returns the first match, so this is correct. Avoids walking
    // the whole list to find and update an existing entry.
    return make_cons(key, make_cons(val, plist));
}

// Remove all occurrences of KEY from plist (rebuild without them).
static Lobj plist_remove(Lobj plist, Lobj key)
{
    Lobj result = Qnil;
    Lobj tail = Qnil;
    while (IS_CONS(plist)) {
        Lobj k = CAR(plist);
        Lobj rest = CDR(plist);
        Lobj v = IS_CONS(rest) ? CAR(rest) : Qnil;
        if (k != key) {
            // Keep this pair: append (k v) to result.
            Lobj pair = make_cons(k, make_cons(v, Qnil));
            if (IS_NIL(result)) {
                result = pair;
                tail = CDR(pair);  // points at the inner Qnil
            } else {
                SETCDR(tail, pair);
                tail = CDR(pair);
            }
        }
        plist = IS_CONS(rest) ? CDR(rest) : Qnil;
    }
    return result;
}

// ---- interval table helpers -------------------------------------------------

// Find the index of the interval covering pos (start <= pos < end), or -1.
static int ival_find(struct rd_buffer *b, int pos)
{
    for (int i = 0; i < b->n_ivals; i++) {
        if (b->ivals[i].start <= pos && pos < b->ivals[i].end) { return i; }
    }
    return -1;
}

// Insert a new interval at index `at`, shifting the rest right by one.
// Returns 0 on success, -1 if the table is full.
static int ival_insert_at(struct rd_buffer *b, int at,
                          int start, int end, Lobj plist)
{
    if (b->n_ivals >= RD_MAX_INTERVALS) { return -1; }
    // Shift intervals at [at, n_ivals) right by one.
    for (int i = b->n_ivals; i > at; i--) {
        b->ivals[i] = b->ivals[i - 1];
    }
    b->ivals[at].start = start;
    b->ivals[at].end   = end;
    b->ivals[at].plist = plist;
    b->n_ivals++;
    return 0;
}

// Remove interval at index `at`, shifting the rest left.
static void ival_remove_at(struct rd_buffer *b, int at)
{
    for (int i = at; i < b->n_ivals - 1; i++) {
        b->ivals[i] = b->ivals[i + 1];
    }
    b->n_ivals--;
}

// Split interval at index `idx` at position `split_pos`, producing two
// adjacent intervals with the same plist. After the call, ivals[idx] covers
// [original_start, split_pos) and ivals[idx+1] covers [split_pos, original_end).
// Returns 0 on success, -1 if the table is too full for the extra slot.
static int ival_split_at(struct rd_buffer *b, int idx, int split_pos)
{
    if (b->n_ivals >= RD_MAX_INTERVALS) { return -1; }
    int orig_end = b->ivals[idx].end;
    Lobj pl = b->ivals[idx].plist;
    // Shrink the left half.
    b->ivals[idx].end = split_pos;
    // Insert the right half at idx+1.
    return ival_insert_at(b, idx + 1, split_pos, orig_end, pl);
}

// ---- rd_put_text_prop -------------------------------------------------------
//
// Set PROP=VAL on every character in [start, end), MERGING with existing
// properties (other keys in pre-existing intervals are preserved).
//
// Algorithm:
//   1. Clamp [start, end) to the buffer length.
//   2. Split any interval that straddles `start` or `end` so that every
//      interval is either fully inside or fully outside [start, end).
//   3. Walk the intervals that are now fully inside [start, end):
//      - Update their plist via plist_put.
//   4. For any sub-range of [start, end) NOT covered by an existing interval,
//      insert a new interval with plist = (prop val . Qnil).
//
// This keeps the array sorted and non-overlapping.

void rd_put_text_prop(struct rd_buffer *b, int start, int end,
                      Lobj prop, Lobj val)
{
    int len = rd_buf_len(b);
    // Clamp to valid text range.
    if (start < 0)   { start = 0; }
    if (end   > len) { end   = len; }
    if (start >= end) { return; }

    // Step 1: Split any interval straddling `start`.
    for (int i = 0; i < b->n_ivals; i++) {
        struct rd_interval *iv = &b->ivals[i];
        if (iv->start < start && iv->end > start) {
            // This interval straddles the left edge: split at `start`.
            if (ival_split_at(b, i, start) < 0) { return; } // table full
            // After the split, ivals[i] covers [iv->start, start) and
            // ivals[i+1] covers [start, old_end). Skip past the left half.
            i++;  // i now points at the right half; let the loop re-examine
            break;
        }
    }

    // Step 2: Split any interval straddling `end`.
    for (int i = 0; i < b->n_ivals; i++) {
        struct rd_interval *iv = &b->ivals[i];
        if (iv->start < end && iv->end > end) {
            if (ival_split_at(b, i, end) < 0) { return; }
            break;  // only one interval can straddle `end`
        }
    }

    // Step 3: Walk [start, end), updating covered intervals and inserting gaps.
    int pos = start;
    int i = 0;
    // Find the first interval that touches or follows pos.
    while (i < b->n_ivals && b->ivals[i].end <= pos) { i++; }

    while (pos < end) {
        if (i < b->n_ivals && b->ivals[i].start <= pos && b->ivals[i].end <= end) {
            // Fully covered interval: merge the property.
            if (b->ivals[i].start > pos) {
                // Gap before this interval: [pos, ivals[i].start) has no props.
                int gap_end = b->ivals[i].start;
                Lobj new_pl = make_cons(prop, make_cons(val, Qnil));
                if (ival_insert_at(b, i, pos, gap_end, new_pl) < 0) { return; }
                // i now points at the new interval; the old interval is at i+1.
                pos = gap_end;
                i++;  // fall through to process the old interval next iteration
                continue;
            }
            // Update the plist with the new property.
            b->ivals[i].plist = plist_put(b->ivals[i].plist, prop, val);
            pos = b->ivals[i].end;
            i++;
        } else if (i < b->n_ivals && b->ivals[i].start < end &&
                   b->ivals[i].start > pos) {
            // Gap before the next interval.
            int gap_end = b->ivals[i].start;
            if (gap_end > end) { gap_end = end; }
            Lobj new_pl = make_cons(prop, make_cons(val, Qnil));
            if (ival_insert_at(b, i, pos, gap_end, new_pl) < 0) { return; }
            pos = gap_end;
            i++;
        } else {
            // No more intervals in range: cover [pos, end) with a new one.
            Lobj new_pl = make_cons(prop, make_cons(val, Qnil));
            if (ival_insert_at(b, i, pos, end, new_pl) < 0) { return; }
            pos = end;
            i++;
        }
    }
}

// ---- rd_get_text_prop -------------------------------------------------------

Lobj rd_get_text_prop(struct rd_buffer *b, int pos, Lobj prop)
{
    int i = ival_find(b, pos);
    if (i < 0) { return Qnil; }
    return plist_get(b->ivals[i].plist, prop);
}

// ---- rd_text_props_at -------------------------------------------------------

Lobj rd_text_props_at(struct rd_buffer *b, int pos)
{
    int i = ival_find(b, pos);
    if (i < 0) { return Qnil; }
    return b->ivals[i].plist;
}

// ---- rd_set_text_props ------------------------------------------------------
//
// Replace (not merge) the entire plist over [start, end). This splits the
// boundary intervals exactly like rd_put_text_prop, then overwrites plists.

void rd_set_text_props(struct rd_buffer *b, int start, int end, Lobj plist)
{
    int len = rd_buf_len(b);
    if (start < 0)   { start = 0; }
    if (end   > len) { end   = len; }
    if (start >= end) { return; }

    // Split any interval straddling `start`.
    for (int i = 0; i < b->n_ivals; i++) {
        struct rd_interval *iv = &b->ivals[i];
        if (iv->start < start && iv->end > start) {
            if (ival_split_at(b, i, start) < 0) { return; }
            break;
        }
    }
    // Split any interval straddling `end`.
    for (int i = 0; i < b->n_ivals; i++) {
        struct rd_interval *iv = &b->ivals[i];
        if (iv->start < end && iv->end > end) {
            if (ival_split_at(b, i, end) < 0) { return; }
            break;
        }
    }

    // If plist is Qnil, remove all intervals fully inside [start, end).
    // Otherwise, update or create intervals to cover [start, end) with plist.
    int pos = start;
    int i = 0;
    while (i < b->n_ivals && b->ivals[i].end <= pos) { i++; }

    while (pos < end) {
        if (i < b->n_ivals && b->ivals[i].start <= pos &&
            b->ivals[i].end <= end) {
            if (b->ivals[i].start > pos) {
                // Gap: [pos, ivals[i].start) -- fill or skip.
                int gap_end = b->ivals[i].start;
                if (!IS_NIL(plist)) {
                    if (ival_insert_at(b, i, pos, gap_end, plist) < 0) { return; }
                    pos = gap_end;
                    i++;
                } else {
                    pos = gap_end;
                }
                continue;
            }
            if (IS_NIL(plist)) {
                // Remove this interval.
                int end_save = b->ivals[i].end;
                ival_remove_at(b, i);
                pos = end_save;
                // i unchanged (next interval shifted into position i)
            } else {
                b->ivals[i].plist = plist;
                pos = b->ivals[i].end;
                i++;
            }
        } else if (i < b->n_ivals && b->ivals[i].start < end &&
                   b->ivals[i].start > pos) {
            // Gap before the next interval.
            int gap_end = b->ivals[i].start;
            if (!IS_NIL(plist)) {
                if (ival_insert_at(b, i, pos, gap_end, plist) < 0) { return; }
                pos = gap_end;
                i++;
            } else {
                pos = gap_end;
            }
        } else {
            // No more intervals in range: cover [pos, end) with plist if non-nil.
            if (!IS_NIL(plist)) {
                if (ival_insert_at(b, i, pos, end, plist) < 0) { return; }
            }
            pos = end;
        }
    }
}

// ---- rd_remove_text_props ---------------------------------------------------
//
// Remove the keys listed in PROPS (a list of symbols, values ignored) from
// every interval intersecting [start, end). Intervals whose plist empties out
// are dropped from the table.

void rd_remove_text_props(struct rd_buffer *b, int start, int end, Lobj props)
{
    int len = rd_buf_len(b);
    if (start < 0)   { start = 0; }
    if (end   > len) { end   = len; }
    if (start >= end) { return; }

    // Walk intervals that overlap [start, end).
    for (int i = 0; i < b->n_ivals; ) {
        struct rd_interval *iv = &b->ivals[i];
        // Skip intervals entirely before or after [start, end).
        if (iv->end <= start || iv->start >= end) { i++; continue; }

        // Remove each key in PROPS from this interval's plist.
        Lobj pl = iv->plist;
        for (Lobj kp = props; IS_CONS(kp); kp = CDR(kp)) {
            Lobj key = CAR(kp);
            pl = plist_remove(pl, key);
        }

        if (IS_NIL(pl)) {
            // Plist is empty: drop this interval entirely.
            ival_remove_at(b, i);
            // Don't increment i: the next interval shifted into slot i.
        } else {
            iv->plist = pl;
            i++;
        }
    }
}

// ---- rd_buf_mark_props ------------------------------------------------------
//
// GC root phase: call mark(plist) for every interval so the mark-sweep
// collector can reach the property plists from the buffer. Without this hook,
// a GC between put-text-property and the next redisplay could free a face
// cons cell that is still referenced by an interval plist -- exactly the
// Emacs GC issue that motivated buffer text properties being GC roots.

void rd_buf_mark_props(struct rd_buffer *b, void (*mark)(Lobj))
{
    for (int i = 0; i < b->n_ivals; i++) {
        mark(b->ivals[i].plist);
    }
}

#endif  // LM_BUILD

// ---- named face resolver (LM_BUILD only) ------------------------------------
//
// rd_resolve_face is the bridge between a Lisp `face` text-property value and
// the numeric face id that a cell's .face byte carries.  Three input shapes:
//
//   nil / unknown symbol  ->  0 (the default face: guaranteed safe)
//   a face-name symbol    ->  look up via gfx_face_id() (lm_gfx.c owns the
//                             name->id registry)
//   a LIST of face names  ->  MERGE: start from the default face, walk the list
//                             left to right applying each face's SET attributes
//                             (fg_set, bg_set, bold, inverse, underline); later
//                             entries override earlier ones.  Then find an
//                             existing slot whose attributes match the merged
//                             result, or allocate the next free slot for it.
//
// The find-before-allocate step keeps the face table stable across multiple
// redisplay passes: if the same list appears on every character of a word, the
// second character resolves to the same id as the first (no runaway growth).
// When the table is full (n_faces_used == RD_NFACES) we fall back to 0 rather
// than silently stomping an existing face.

#ifdef LM_BUILD
int rd_resolve_face(struct rd_frame *f, Lobj v)
{
    if (IS_NIL(v)) { return 0; }

    // Fast path: a single face name symbol -- just look it up.
    if (IS_SYMBOL(v)) {
        int id = gfx_face_id(v);
        return (id >= 0 && id < RD_NFACES) ? id : 0;
    }

    // Merge path: a list (face1 face2 ...).
    // Start from a copy of the default face (index 0), which has all attributes
    // set, so the result is always fully specified even if none of the listed
    // faces carry fg or bg.
    if (IS_CONS(v)) {
        struct rd_face m = f->faces[0];   // working merged result; copy, not ptr
        for (Lobj p = v; IS_CONS(p); p = CDR(p)) {
            int id = gfx_face_id(CAR(p));
            if (id < 0 || id >= f->n_faces_used) { continue; }
            struct rd_face *s = &f->faces[id];
            // Each face in the list only OVERRIDES the attributes it explicitly
            // specifies (fg_set/bg_set == 1); unset attributes are inherited
            // from what was already accumulated -- the classic Emacs face merge.
            if (s->fg_set)    { m.fg = s->fg; m.fg_set = 1; }
            if (s->bg_set)    { m.bg = s->bg; m.bg_set = 1; }
            if (s->bold)      { m.bold = 1; }
            if (s->inverse)   { m.inverse = 1; }
            if (s->underline) { m.underline = 1; }
        }

        // Scan the already-allocated face slots for an identical entry.
        // This prevents the table from growing when the same list appears
        // on every character of a colored span -- redisplay calls us once
        // per character, so stability here is important.
        for (int i = 0; i < f->n_faces_used; i++) {
            struct rd_face *e = &f->faces[i];
            if (e->fg == m.fg && e->bg == m.bg &&
                e->bold == m.bold && e->inverse == m.inverse &&
                e->underline == m.underline &&
                e->fg_set == m.fg_set && e->bg_set == m.bg_set) {
                return i;
            }
        }
        // No existing slot matched: claim the next free one.
        if (f->n_faces_used < RD_NFACES) {
            f->faces[f->n_faces_used] = m;
            return f->n_faces_used++;
        }
        return 0;   // table full -- fall back to default rather than clobbering
    }

    return 0;
}
#endif  // LM_BUILD

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
    // 2 = region/selection bar.  Lisp can repaint or extend these via defface.
    //
    // All built-in faces specify both colors (fg_set=bg_set=1) and start with
    // all attribute bits cleared.  The fill loop for 3..RD_NFACES copies face 0
    // as a sane default so any unregistered slot is never garbage.
    f->faces[0].fg = 0x00D5C4A1; f->faces[0].bg = 0x001D2021;
    f->faces[0].fg_set = 1; f->faces[0].bg_set = 1;
    f->faces[0].bold = 0; f->faces[0].inverse = 0; f->faces[0].underline = 0;

    f->faces[1].fg = 0x001D2021; f->faces[1].bg = 0x00928374;
    f->faces[1].fg_set = 1; f->faces[1].bg_set = 1;
    f->faces[1].bold = 0; f->faces[1].inverse = 0; f->faces[1].underline = 0;

    f->faces[2].fg = 0x00FBF1C7; f->faces[2].bg = 0x00504945;   // selection bar
    f->faces[2].fg_set = 1; f->faces[2].bg_set = 1;
    f->faces[2].bold = 0; f->faces[2].inverse = 0; f->faces[2].underline = 0;

    // Slots 3..RD_NFACES-1: copy default (face 0) so unregistered ids are safe.
    for (int i = 3; i < RD_NFACES; i++) { f->faces[i] = f->faces[0]; }

    // n_faces_used is the single allocation cursor shared between:
    //   • lm_gfx's defface / face_alloc (which registers named faces)
    //   • rd_resolve_face's merge-allocator (which find-or-allocates merged slots)
    // Initialize to 3 (the three built-ins 0/1/2 are already committed).
    // lm_gfx_register then bumps it when it interns the built-in name symbols.
    f->n_faces_used = 3;
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

// The cell face id for the character at text position `p`: read its `face`
// text property and resolve it (a face name, or a list of faces merged). In the
// kernel build (no Lisp) there are no text properties, so it's always face 0 --
// buffer text renders exactly as before until properties exist.
#ifdef LM_BUILD
static int char_face(struct rd_frame *f, struct rd_buffer *b, int p)
{
    static Lobj sym_face = 0;                 // interned once: the `face` property key
    if (sym_face == 0) { sym_face = intern("face"); }
    return rd_resolve_face(f, rd_get_text_prop(b, p, sym_face));
}
#else
static int char_face(struct rd_frame *f, struct rd_buffer *b, int p)
{ (void)f; (void)b; (void)p; return 0; }
#endif

// Lay out one leaf window's rect: text lines (truncated by default; wrapped
// when the buffer's line-wrap minor mode is on), then its modeline on its last row.
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

    int len = b ? rd_buf_len(b) : 0;

    if (b && b->wrap) {
        // Line-wrap minor mode: a logical line longer than the window flows
        // onto the following screen row(s) instead of being clipped at the
        // edge. `top_line` stays a LOGICAL-line anchor (set by the scroll
        // block above), so scrolling is unchanged: we begin at the start of
        // logical line `top_line` and fill screen rows top to bottom.
        //
        // Walk: advance `pos` to the first char of logical line `top_line`.
        int pos = 0, line = 0;
        while (line < w->top_line && pos < len) {
            if (rd_buf_char_at(b, pos++) == '\n') { line++; }
        }
        int row = 0, col = 0, p = pos;
        // Only render if we actually reached the requested start line (an
        // empty/short buffer may stop early -- then nothing to draw).
        if (line == w->top_line) {
            while (row < text_rows && p < len) {
                int c = rd_buf_char_at(b, p);
                if (c == '\n') {
                    // End of this logical line: cursor may sit on the newline
                    // (i.e. at end-of-line, before the '\n').
                    if (w == f->selected && b->point == p) {
                        f->cursor_col = w->x + col; f->cursor_row = w->y + row;
                    }
                    // Blank-fill the rest of this row, then drop to the next
                    // row to begin the next logical line.
                    for (; col < w->w; col++) {
                        put_cell(f, w->x + col, w->y + row, ' ', 0);
                    }
                    row++; col = 0; p++;
                    continue;
                }
                if (col >= w->w) {                // window edge reached: WRAP
                    row++; col = 0;               // continue the SAME logical line
                    if (row >= text_rows) { break; }
                }
                put_cell(f, w->x + col, w->y + row, c, char_face(f, b, p));
                if (w == f->selected && b->point == p) {
                    f->cursor_col = w->x + col; f->cursor_row = w->y + row;
                }
                col++; p++;
            }
            // Point at end of buffer (no trailing newline): place the cursor
            // after the last char, wrapping to a fresh row if we're at the edge.
            if (w == f->selected && b->point == p && row < text_rows) {
                if (col >= w->w) { row++; col = 0; }
                if (row < text_rows) {
                    f->cursor_col = w->x + col; f->cursor_row = w->y + row;
                }
            }
        }
        // Blank-fill the remainder of the current partial row, then every
        // unused row below it, so stale cells never linger.
        if (row < text_rows) {
            for (; col < w->w; col++) { put_cell(f, w->x + col, w->y + row, ' ', 0); }
            for (row++; row < text_rows; row++) {
                for (int c2 = 0; c2 < w->w; c2++) {
                    put_cell(f, w->x + c2, w->y + row, ' ', 0);
                }
            }
        }
        goto modeline;
    }

    // Walk the buffer line by line; render lines [top_line, top_line+rows).
    int pos = 0, line = 0;
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
                    put_cell(f, w->x + col, w->y + row, c, char_face(f, b, p));
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
