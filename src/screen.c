/* Terminator — screen.c
 * Per-tab screen buffer and drawing (Design section 2):
 *  * append_screen routes text to the buffer, a capture file, a packet
 *  * pipe, or stdout; draw_screen renders the buffer with display-only
 *  * line wrapping.
 */
#include "common.h"

static pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;

static char prompt_buf[PATH_MAX + 32];

/* The prompt shows the tab's current directory, bash-style, with $HOME
   shortened to ~: `user@terminator:~> `, `user@terminator:/tmp> `.
   Continuation lines (open quote, heredoc body) show `> ` instead. */
void build_prompt(char *dst, size_t cap, const Tab *t) {
    if (t->multiline) { snprintf(dst, cap, "> "); return; }
    const char *cwd = t->cwd[0] ? t->cwd : "/";
    const char *home = getenv("HOME");
    char disp[PATH_MAX];
    if (home && home[0] && !strcmp(cwd, home)) snprintf(disp, sizeof(disp), "~");
    else if (home && home[0] && !strncmp(cwd, home, strlen(home)) && cwd[strlen(home)] == '/')
        snprintf(disp, sizeof(disp), "~%s", cwd + strlen(home));
    else snprintf(disp, sizeof(disp), "%s", cwd);
    snprintf(dst, cap, "user@terminator:%s> ", disp);
}


/* The prompt drawn on the live input line: while a history search is
   active the line is labelled "Enter search term: " so the term being
   typed shows right after the label, instead of at the normal prompt
   on the line below a stale label. */
static const char *live_prompt(Tab *t) {
    if (history_search_active()) return "Enter search term: ";
    build_prompt(prompt_buf, sizeof(prompt_buf), t);
    return prompt_buf;
}

void append_screen(Tab *t, const char *s, size_t n) {
    if (n == 0) return;
    if (t->capture_fd >= 0) { write_all_fd(t->capture_fd, s, n); return; }
    if (proc_child) { write_packet(child_resp_fd, 'T', s, (uint32_t)n); return; }
    if (batch_mode) { fwrite(s, 1, n, stdout); fflush(stdout); return; }
    pthread_mutex_lock(&screen_lock);
    screen_dirty = 1;
    t->scroll_offset = 0; /* new output snaps a scrolled view back to the tail */
    t->screen_gen++; t->sel_active = 0; t->sel_dragging = 0; /* new output invalidates a mouse selection */
    if (n >= MAX_SCREEN) { s += n - (MAX_SCREEN - 1); n = MAX_SCREEN - 1; }
    if (t->screen_len + n >= MAX_SCREEN) {
        size_t drop = t->screen_len + n - (MAX_SCREEN - 1);
        memmove(t->screen, t->screen + drop, t->screen_len - drop);
        t->screen_len -= drop;
        t->screen[t->screen_len] = '\0';
    }
    memcpy(t->screen + t->screen_len, s, n);
    t->screen_len += n;
    t->screen[t->screen_len] = '\0';
    pthread_mutex_unlock(&screen_lock);
}
void append_text(Tab *t, const char *s) { append_screen(t, s, strlen(s)); }

/* Expand tab characters to spaces (tab stops every 8 columns) for display:
   XDrawString draws a tab as nothing, so output like `printf 'A\tB\n'`
   showed "AB" with no gap. Expansion is display-only: the screen buffer
   and the input line keep the real tab characters. */
static char *expand_tabs(const char *s) {
    size_t outlen = 0;
    for (size_t k = 0; s[k]; k++) outlen += (s[k] == '\t') ? 8 - (outlen % 8) : 1;
    char *out = malloc(outlen + 1);
    if (!out) return NULL;
    size_t o = 0, col = 0;
    for (size_t k = 0; s[k]; k++) {
        if (s[k] == '\t') {
            size_t n = 8 - (col % 8);
            memset(out + o, ' ', n); o += n; col += n;
        } else { out[o++] = s[k]; col++; }
    }
    out[o] = '\0';
    return out;
}


/* ---- Mouse text selection ----
   A drag in the text area selects text in the same wrapped display
   layout draw_screen renders (logical lines split on \n, tabs expanded,
   wrapped at wrap_cols). The selection is two (segment row, column)
   endpoints plus the screen generation and wrap width it was made
   against; new output, a clear, or a resize invalidates it, and
   Ctrl+Shift+C copies it (editor.c falls back to the input line when
   there is no selection). Row indices count segments from the top of
   the layout, so a selection survives scrolling and tab switches. */
typedef struct { int line; size_t start; size_t len; const char *ptr; } SelSeg;
typedef struct {
    char **lines; size_t nlines;
    SelSeg *segs; size_t nsegs;
    size_t wrap_cols, first_row, total_rows;
} SelLayout;

static void free_layout(SelLayout *L) {
    if (L->lines) { for (size_t i = 0; i < L->nlines; i++) free(L->lines[i]); free(L->lines); }
    free(L->segs);
    memset(L, 0, sizeof(*L));
}

/* Rebuild the display layout for tab t, mirroring draw_screen's
   splitting, tab expansion, wrapping and first-row math exactly, so
   hit-testing and copying agree with what is drawn. */
static void build_layout(Tab *t, SelLayout *L) {
    memset(L, 0, sizeof(*L));
    const char *active_prompt = live_prompt(t);
    size_t prompt_len = strlen(active_prompt) + t->input_len;
    size_t total = t->screen_len + prompt_len + 2;
    char *combined = malloc(total);
    if (!combined) return;
    pthread_mutex_lock(&screen_lock);
    size_t pos = 0;
    if (t->screen_len) { memcpy(combined, t->screen, t->screen_len); pos = t->screen_len; }
    pthread_mutex_unlock(&screen_lock);
    if (pos > 0 && combined[pos - 1] != '\n') combined[pos++] = '\n';
    size_t plen = strlen(active_prompt);
    memcpy(combined + pos, active_prompt, plen); pos += plen;
    if (t->input_len) { memcpy(combined + pos, t->input, t->input_len); pos += t->input_len; }
    combined[pos] = '\0';
    size_t capacity = 0;
    char *save = NULL;
    for (char *line = strtok_r(combined, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (L->nlines == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            char **grown = realloc(L->lines, next * sizeof(*grown));
            if (!grown) break;
            L->lines = grown; capacity = next;
        }
        char *shown = expand_tabs(line);
        if (!shown) break;
        L->lines[L->nlines++] = shown;
    }
    free(combined);
    int char_w = (font_info && font_info->max_bounds.width > 0) ? font_info->max_bounds.width : 8;
    long wrap_long = (win_width - 20) / char_w;
    L->wrap_cols = wrap_long < 1 ? 1 : (size_t)wrap_long;
    int available = (win_height - TAB_HEIGHT - 4) / LINE_HEIGHT;
    if (available < 1) available = 1;
    size_t total_rows = 0;
    for (size_t i = 0; i < L->nlines; i++) { size_t ll = strlen(L->lines[i]); total_rows += ll ? (ll + L->wrap_cols - 1) / L->wrap_cols : 1; }
    L->total_rows = total_rows;
    size_t segcap = 0;
    for (size_t i = 0; i < L->nlines; i++) {
        size_t ll = strlen(L->lines[i]), off = 0;
        do {
            size_t sl = (ll - off > L->wrap_cols) ? L->wrap_cols : ll - off;
            if (L->nsegs == segcap) {
                size_t next = segcap ? segcap * 2 : 32;
                SelSeg *grown = realloc(L->segs, next * sizeof(*grown));
                if (!grown) break;
                L->segs = grown; segcap = next;
            }
            L->segs[L->nsegs].line = (int)i; L->segs[L->nsegs].start = off;
            L->segs[L->nsegs].len = sl; L->segs[L->nsegs].ptr = L->lines[i] + off;
            L->nsegs++;
            off += sl;
        } while (off < ll);
    }
    /* First visible row: the same math as draw_screen (scroll clamp,
       keep the cursor visible). */
    size_t cursor_row = total_rows ? total_rows - 1 : 0;
    if (L->nlines && t->cursor_pos <= t->input_len) {
        size_t before = 0;
        for (size_t i = 0; i + 1 < L->nlines; i++) { size_t ll = strlen(L->lines[i]); before += ll ? (ll + L->wrap_cols - 1) / L->wrap_cols : 1; }
        size_t last_len = strlen(L->lines[L->nlines - 1]);
        size_t last_rows = last_len ? (last_len + L->wrap_cols - 1) / L->wrap_cols : 1;
        size_t col = 0;
        for (const char *cp = active_prompt; *cp; cp++) col += (*cp == '\t') ? 8 - (col % 8) : 1;
        for (size_t k = 0; k < t->cursor_pos && k < t->input_len; k++) {
            if (t->input[k] == '\n') col = 0; /* pasted multi-line input: caret is on the last segment */
            else col += (t->input[k] == '\t') ? 8 - (col % 8) : 1;
        }
        size_t cursor_within = col / L->wrap_cols;
        if (cursor_within >= last_rows) cursor_within = last_rows - 1;
        cursor_row = before + cursor_within;
    }
    size_t first_row = total_rows > (size_t)available ? total_rows - (size_t)available : 0;
    if (t->scroll_offset) {
        size_t max_off = total_rows > (size_t)available ? total_rows - (size_t)available : 0;
        size_t off = (size_t)t->scroll_offset > max_off ? max_off : (size_t)t->scroll_offset;
        first_row = total_rows > (size_t)available ? total_rows - (size_t)available - off : 0;
    } else if (cursor_row < first_row) first_row = cursor_row;
    L->first_row = first_row;
}

/* Pixel position -> (segment row, column) in the display layout. */
static void sel_hit(Tab *t, SelLayout *L, int x, int y, size_t *row, size_t *col) {
    (void)t;
    if (L->nsegs == 0) { *row = 0; *col = 0; return; }
    /* A display row's glyphs sit above its baseline: row 0 spans
       y in [TAB_HEIGHT, TAB_HEIGHT + LINE_HEIGHT), row 1 the next
       band, and so on — measure from the top of the text area, not
       from the first baseline. */
    long r = (long)L->first_row + (y - TAB_HEIGHT) / LINE_HEIGHT;
    if (r < (long)L->first_row) r = (long)L->first_row;
    if ((size_t)r >= L->nsegs) r = (long)L->nsegs - 1;
    *row = (size_t)r;
    const SelSeg *sg = &L->segs[*row];
    size_t c = 0;
    if (font_info) {
        while (c < sg->len && 10 + XTextWidth(font_info, sg->ptr, (int)(c + 1)) <= x) c++;
    } else {
        c = x > 10 ? (size_t)((x - 10) / 8) : 0;
        if (c > sg->len) c = sg->len;
    }
    *col = c;
}

void clear_selection(Tab *t) { t->sel_active = 0; t->sel_dragging = 0; }

void sel_begin(Tab *t, int x, int y) {
    SelLayout L; build_layout(t, &L);
    size_t row, col; sel_hit(t, &L, x, y, &row, &col);
    t->sel_a_row = t->sel_b_row = row; t->sel_a_col = t->sel_b_col = col;
    t->sel_gen = t->screen_gen; t->sel_wrap = L.wrap_cols;
    t->sel_active = 1; t->sel_dragging = 1;
    free_layout(&L);
    screen_dirty = 1;
}

void sel_extend(Tab *t, int x, int y) {
    if (!t->sel_active) return;
    SelLayout L; build_layout(t, &L);
    size_t row, col; sel_hit(t, &L, x, y, &row, &col);
    t->sel_b_row = row; t->sel_b_col = col;
    free_layout(&L);
    screen_dirty = 1;
}

void sel_end(Tab *t) {
    t->sel_dragging = 0;
    if (t->sel_a_row == t->sel_b_row && t->sel_a_col == t->sel_b_col) clear_selection(t);
}

/* Copy the selected text into dst (segments of one logical line joined
   directly, logical lines joined with \n). Returns 1 when text was
   copied, 0 when there is no usable selection. */
int sel_copy_text(Tab *t, char *dst, size_t cap) {
    if (!t->sel_active || cap == 0) return 0;
    SelLayout L; build_layout(t, &L);
    if (t->screen_gen != t->sel_gen || L.wrap_cols != t->sel_wrap || L.nsegs == 0) {
        clear_selection(t); free_layout(&L); return 0;
    }
    size_t r1 = t->sel_a_row, c1 = t->sel_a_col, r2 = t->sel_b_row, c2 = t->sel_b_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        size_t tr = r1, tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc;
    }
    if (r1 >= L.nsegs) { free_layout(&L); return 0; }
    if (r2 >= L.nsegs) r2 = L.nsegs - 1;
    size_t out = 0; int started = 0, lines_seen = 0, prev_line = -1;
    for (size_t r = r1; r <= r2; r++) {
        const SelSeg *sg = &L.segs[r];
        size_t lo = (r == r1) ? c1 : 0, hi = (r == r2) ? c2 : sg->len;
        if (lo > sg->len) lo = sg->len;
        if (hi > sg->len) hi = sg->len;
        if (sg->line != prev_line) {
            lines_seen++;
            if (started && out + 1 < cap) dst[out++] = '\n';
            prev_line = sg->line;
        }
        if (hi > lo) {
            size_t n = hi - lo; if (out + n >= cap) n = cap - 1 - out;
            memcpy(dst + out, sg->ptr + lo, n); out += n; started = 1;
        }
    }
    if (out == 0 && lines_seen > 1) { dst[out++] = '\n'; } /* only a newline was selected */
    dst[out] = '\0';
    free_layout(&L);
    return out > 0;
}

/* Selected column range [a, b) on display row `row`, or 0 if none. */
static int sel_range_for_row(Tab *t, size_t row, size_t seg_len, size_t *a, size_t *b) {
    if (!t->sel_active) return 0;
    size_t r1 = t->sel_a_row, c1 = t->sel_a_col, r2 = t->sel_b_row, c2 = t->sel_b_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        size_t tr = r1, tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc;
    }
    if (r1 == r2 && c1 == c2) return 0;
    if (row < r1 || row > r2) return 0;
    *a = (row == r1) ? c1 : 0;
    *b = (row == r2) ? c2 : seg_len;
    if (*a > seg_len) *a = seg_len;
    if (*b > seg_len) *b = seg_len;
    return *a < *b;
}

/* Draw one run of a segment, optionally inverted (selected text). */
static void draw_run(const char *s, int len, int x, int y, int inverse, int char_w) {
    if (len <= 0) return;
    if (!inverse) { XDrawString(dpy, win, gc, x, y, s, len); return; }
    int w = font_info ? XTextWidth(font_info, s, len) : len * char_w;
    int asc = font_info ? font_info->ascent : 13, desc = font_info ? font_info->descent : 4;
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, win, gc, x, y - asc, w, asc + desc);
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XDrawString(dpy, win, gc, x, y, s, len);
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
}


static void draw_tab_bar(void) {
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, win, gc, 0, 0, win_width, TAB_HEIGHT);
    int pos = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        int x = pos * TAB_WIDTH;
        if (i == current_tab) {
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XFillRectangle(dpy, win, gc, x, 0, TAB_WIDTH, TAB_HEIGHT);
            XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
        } else {
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XDrawRectangle(dpy, win, gc, x, 0, TAB_WIDTH, TAB_HEIGHT);
        }
        char label[80]; snprintf(label, sizeof(label), "Tab %d", pos + 1);
        XDrawString(dpy, win, gc, x + 10, 22, label, (int)strlen(label));
        XDrawString(dpy, win, gc, x + TAB_WIDTH - 22, 22, "x", 1);
        pos++;
    }
    int px = pos * TAB_WIDTH; XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawRectangle(dpy, win, gc, px, 0, TAB_WIDTH, TAB_HEIGHT);
    XDrawString(dpy, win, gc, px + 12, 22, "+", 1);
}

void draw_screen(void) {
    int scr = DefaultScreen(dpy);
    XSetForeground(dpy, gc, WhitePixel(dpy, scr));
    XFillRectangle(dpy, win, gc, 0, TAB_HEIGHT, win_width, win_height - TAB_HEIGHT);
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    Tab *t = &tabs[current_tab];
    pthread_mutex_lock(&screen_lock);

    /* Display saved commands/output first, then the active prompt on the last line. */
    const char *active_prompt = live_prompt(t);
    size_t prompt_len = strlen(active_prompt) + t->input_len;
    size_t total = t->screen_len + prompt_len + 2;
    char *combined = malloc(total);
    if (combined) {
        size_t pos = 0;
        if (t->screen_len) {
            memcpy(combined, t->screen, t->screen_len);
            pos = t->screen_len;
        }
        if (pos > 0 && combined[pos - 1] != '\n') combined[pos++] = '\n';
        size_t plen = strlen(active_prompt);
        memcpy(combined + pos, active_prompt, plen);
        pos += plen;
        if (t->input_len) {
            memcpy(combined + pos, t->input, t->input_len);
            pos += t->input_len;
        }
        combined[pos] = '\0';
        char **lines = NULL;
        size_t count = 0, capacity = 0;
        char *save = NULL;
        for (char *line = strtok_r(combined, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 32;
                char **grown = realloc(lines, next * sizeof(*lines));
                if (!grown) break;
                lines = grown;
                capacity = next;
            }
            /* Every displayed line is a heap copy with tabs expanded, so
               freeing the list is uniform whether or not a line had tabs. */
            char *shown = expand_tabs(line);
            if (!shown) break;
            lines[count++] = shown;
        }
        /* Wrap long lines at the window width: each logical line is drawn
           as one segment per row, so typing past the right edge continues
           on the next row instead of running off-screen. Wrapping is
           display-only; the input buffer stays one logical line. */
        int char_w = (font_info && font_info->max_bounds.width > 0) ? font_info->max_bounds.width : 8;
        long wrap_long = (win_width - 20) / char_w;
        size_t wrap_cols = wrap_long < 1 ? 1 : (size_t)wrap_long;
        int first_y = TAB_HEIGHT + LINE_HEIGHT, available = (win_height - TAB_HEIGHT - 4) / LINE_HEIGHT;
        if (available < 1) available = 1;
        /* Count display rows after wrapping, and find the cursor's row. */
        size_t total_rows = 0;
        for (size_t i = 0; i < count; i++) { size_t ll = strlen(lines[i]); total_rows += ll ? (ll + wrap_cols - 1) / wrap_cols : 1; }
        size_t cursor_row = total_rows ? total_rows - 1 : 0, cursor_within = 0, cursor_off = 0;
        if (count && t->cursor_pos <= t->input_len) {
            size_t before = 0;
            for (size_t i = 0; i + 1 < count; i++) { size_t ll = strlen(lines[i]); before += ll ? (ll + wrap_cols - 1) / wrap_cols : 1; }
            size_t last_len = strlen(lines[count - 1]);
            size_t last_rows = last_len ? (last_len + wrap_cols - 1) / wrap_cols : 1;
            /* The cursor column in expanded columns: the prompt plus the
               input up to the cursor, each tab advancing to the next
               multiple of 8, matching how the last line was expanded. */
            size_t col = 0;
            for (const char *cp = active_prompt; *cp; cp++) col += (*cp == '\t') ? 8 - (col % 8) : 1;
            for (size_t k = 0; k < t->cursor_pos && k < t->input_len; k++) {
                if (t->input[k] == '\n') col = 0; /* pasted multi-line input: caret is on the last segment */
                else col += (t->input[k] == '\t') ? 8 - (col % 8) : 1;
            }
            cursor_within = col / wrap_cols;
            if (cursor_within >= last_rows) { cursor_within = last_rows - 1; cursor_off = last_len - cursor_within * wrap_cols; }
            else cursor_off = col % wrap_cols;
            cursor_row = before + cursor_within;
        }
        size_t first_row = total_rows > (size_t)available ? total_rows - (size_t)available : 0;
        if (t->scroll_offset) {
            /* Scrolled up: show an older window of rows. The upper clamp
               lives here (content and window size change), so a stale
               offset can never point past the oldest row. */
            size_t max_off = total_rows > (size_t)available ? total_rows - (size_t)available : 0;
            if ((size_t)t->scroll_offset > max_off) t->scroll_offset = (int)max_off;
            first_row = total_rows > (size_t)available ? total_rows - (size_t)available - (size_t)t->scroll_offset : 0;
        } else if (cursor_row < first_row) first_row = cursor_row; /* keep the cursor visible */
        size_t row = 0; int y = first_y;
        for (size_t i = 0; i < count; i++) {
            size_t ll = strlen(lines[i]), off = 0;
            do {
                size_t sl = (ll - off > wrap_cols) ? wrap_cols : ll - off;
                if (row >= first_row && y < win_height) {
                    size_t ha, hb;
                    if (sel_range_for_row(t, row, sl, &ha, &hb)) {
                        /* Selected text draws inverted; split the segment
                           into unselected / selected / unselected runs. */
                        int xa = font_info ? 10 + XTextWidth(font_info, lines[i] + off, (int)ha) : 10 + (int)ha * char_w;
                        int xb = font_info ? 10 + XTextWidth(font_info, lines[i] + off, (int)hb) : 10 + (int)hb * char_w;
                        draw_run(lines[i] + off, (int)ha, 10, y, 0, char_w);
                        draw_run(lines[i] + off + ha, (int)(hb - ha), xa, y, 1, char_w);
                        draw_run(lines[i] + off + hb, (int)(sl - hb), xb, y, 0, char_w);
                    } else {
                        XDrawString(dpy, win, gc, 10, y, lines[i] + off, (int)sl);
                    }
                    y += LINE_HEIGHT;
                }
                row++; off += sl;
            } while (off < ll);
        }
        if (count && t->cursor_pos <= t->input_len && cursor_row >= first_row && font_info) {
            int cursor_y = first_y + (int)(cursor_row - first_row) * LINE_HEIGHT;
            int cx = 10 + XTextWidth(font_info, lines[count - 1] + cursor_within * wrap_cols, (int)cursor_off);
            XDrawLine(dpy, win, gc, cx, cursor_y - 12, cx, cursor_y + 3);
        }
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines); free(combined);
    }
    pthread_mutex_unlock(&screen_lock);
    draw_tab_bar();
    XFlush(dpy);
}


/* Clear the tab's saved screen. In a tab process this asks the GUI (which
   owns the screen) via an 'L' packet; in batch mode there is no screen. */
void clear_tab_screen(Tab *t) {
    if (batch_mode) return;
    if (proc_child) { write_packet(child_resp_fd, 'L', NULL, 0); return; }
    pthread_mutex_lock(&screen_lock);
    t->screen_len = 0; t->screen[0] = '\0'; t->scroll_offset = 0; screen_dirty = 1;
    t->screen_gen++; t->sel_active = 0; t->sel_dragging = 0;
    pthread_mutex_unlock(&screen_lock);
}

/* Scroll the current tab's view by rows display rows (positive = up into
   older output). The offset is clamped against the content in
   draw_screen; any new output or editing key snaps back to the tail. */
void scroll_tab(Tab *t, int rows) {
    t->scroll_offset += rows;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    screen_dirty = 1;
}

int screen_page_rows(void) {
    int available = (win_height - TAB_HEIGHT - 4) / LINE_HEIGHT;
    return available < 1 ? 1 : available;
}
