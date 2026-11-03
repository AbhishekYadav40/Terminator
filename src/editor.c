/* Terminator — editor.c
 * Line editor, history browsing, Ctrl+R search and Tab completion
 *  * (Design sections 7-8): everything handle_key does with a keypress.
 */
#include "common.h"

static int searching_history;
static char saved_search_input[INPUT_CAP];

/* Whether a Ctrl+R search is in progress: the screen draws the live
   input line with the "Enter search term: " label while one is. */
int history_search_active(void) { return searching_history; }
/* Pending numbered completion list: a digit selects one of the last
   ambiguous matches. */
static char completion_matches[64][PATH_MAX];
static size_t completion_count, completion_start, completion_prefix_len;


/* Put shared-history entry idx into the tab's input line. */
static void load_history_entry(Tab *t, int idx) {
    char buf[INPUT_CAP];
    if (hist_get(idx, buf, sizeof(buf))) {
        t->input_len = strnlen(buf, sizeof(t->input) - 1);
        memcpy(t->input, buf, t->input_len);
        t->input[t->input_len] = '\0';
    }
    t->cursor_pos = t->input_len;
}
static void history_up(Tab *t) {
    int count = hist_count();
    if (count == 0) return;
    if (t->hist_pos < 0 || t->hist_pos > count) t->hist_pos = count;
    if (t->hist_pos <= 0) return;
    t->hist_pos--;
    load_history_entry(t, t->hist_pos);
}

static void history_down(Tab *t) {
    int count = hist_count();
    if (t->hist_pos < 0) return;
    t->hist_pos++;
    if (t->hist_pos >= count) {
        t->input[0] = '\0';
        t->input_len = 0;
        t->hist_pos = count;
    } else load_history_entry(t, t->hist_pos);
    t->cursor_pos = t->input_len;
}

/* Complete the last token from matching filesystem paths. */
static void complete_input(Tab *t) {
    if (t->cursor_pos == 0) return;
    size_t end = t->cursor_pos;
    size_t start = end;
    while (start > 0 && t->input[start - 1] != ' ' &&
           t->input[start - 1] != '\t' && t->input[start - 1] != '\n') start--;
    size_t plen = end - start;
    if (plen == 0 || plen + 2 >= INPUT_CAP) return;
    char prefix[INPUT_CAP], pattern[INPUT_CAP + PATH_MAX];
    memcpy(prefix, t->input + start, plen); prefix[plen] = '\0';
    /* Filenames are completed in the tab's own working directory. */
    if (prefix[0] == '/') snprintf(pattern, sizeof(pattern), "%s*", prefix);
    else if (prefix[0] == '~' && prefix[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(pattern, sizeof(pattern), "%s%s*", home ? home : "", prefix + 1);
    }
    else { size_t pl = strlen(prefix), pos = 0;
        for (size_t k = 0; t->cwd[k] && pos + 1 < sizeof(pattern); k++) pattern[pos++] = t->cwd[k];
        if (pos + 1 < sizeof(pattern)) pattern[pos++] = '/';
        for (size_t k = 0; k < pl && pos + 2 < sizeof(pattern); k++) pattern[pos++] = prefix[k];
        pattern[pos++] = '*'; pattern[pos] = '\0'; }
    glob_t matches; memset(&matches, 0, sizeof(matches)); int gr = glob(pattern, 0, NULL, &matches);
    if (gr != 0 || matches.gl_pathc == 0) { globfree(&matches); completion_count = 0; return; }
    /* Display forms: strip the cwd prefix we added so inserted text looks
       the way the user has been typing it. */
    char disp[64][PATH_MAX];
    size_t ndisp = matches.gl_pathc < 64 ? matches.gl_pathc : 64;
    char strip[PATH_MAX + 2] = "";
    if (prefix[0] != '/') snprintf(strip, sizeof(strip), "%s/", t->cwd);
    size_t striplen = strlen(strip);
    for (size_t i = 0; i < ndisp; i++) {
        const char *m = matches.gl_pathv[i];
        if (striplen && strncmp(m, strip, striplen) == 0) m += striplen;
        snprintf(disp[i], sizeof(disp[i]), "%s", m);
    }
    completion_start = start; completion_prefix_len = plen;
    if (ndisp == 1) {
        const char *match = disp[0]; size_t mlen = strlen(match); struct stat st;
        char full[PATH_MAX]; join_path(full, sizeof(full), t->cwd, match);
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        size_t suffix_len = t->input_len - end;
        if (start + mlen + (is_dir ? 1 : 0) + suffix_len < sizeof(t->input)) {
            memmove(t->input + start + mlen + (is_dir ? 1 : 0), t->input + end, suffix_len + 1);
            memcpy(t->input + start, match, mlen);
            if (is_dir) t->input[start + mlen] = '/';
            t->input_len = start + mlen + (is_dir ? 1 : 0) + suffix_len;
            t->input[t->input_len] = '\0';
            t->cursor_pos = start + mlen + (is_dir ? 1 : 0);
        }
        completion_count = 0;
    } else {
        /* Extend to the longest common prefix, then show a numbered list. */
        size_t common = strlen(disp[0]);
        for (size_t i = 1; i < ndisp; i++) { size_t j = 0; while (j < common && disp[i][j] && disp[0][j] == disp[i][j]) j++; common = j; }
        size_t suffix_len = t->input_len - end;
        if (common > plen && start + common + suffix_len < sizeof(t->input)) {
            memmove(t->input + start + common, t->input + end, suffix_len + 1);
            memcpy(t->input + start, disp[0], common);
            t->input_len = start + common + suffix_len;
            t->input[t->input_len] = '\0';
            t->cursor_pos = start + common;
            completion_prefix_len = common;
        }
        completion_count = ndisp;
        append_text(t, "\n");
        for (size_t i = 0; i < ndisp; i++) {
            snprintf(completion_matches[i], sizeof(completion_matches[i]), "%s", disp[i]);
            char b[PATH_MAX + 32]; snprintf(b, sizeof(b), "%zu) %s\n", i + 1, disp[i]); append_text(t, b);
        }
        append_text(t, "Select a match number, or continue typing: ");
    }
    globfree(&matches);
}

static void select_completion(Tab *t, unsigned int number) {
    if (!completion_count || number < 1 || number > completion_count) return;
    const char *match = completion_matches[number - 1];
    size_t mlen = strlen(match);
    size_t region_end = completion_start + completion_prefix_len;
    if (region_end > t->input_len) return;
    size_t suffix_len = t->input_len - region_end;
    char full[PATH_MAX]; join_path(full, sizeof(full), t->cwd, match);
    struct stat st;
    int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    if (completion_start + mlen + (is_dir ? 1 : 0) + suffix_len >= sizeof(t->input)) { completion_count = 0; return; }
    memmove(t->input + completion_start + mlen + (is_dir ? 1 : 0), t->input + region_end, suffix_len + 1);
    memcpy(t->input + completion_start, match, mlen);
    if (is_dir) t->input[completion_start + mlen] = '/';
    t->input_len = completion_start + mlen + (is_dir ? 1 : 0) + suffix_len;
    t->input[t->input_len] = '\0';
    t->cursor_pos = completion_start + mlen + (is_dir ? 1 : 0);
    completion_count = 0;
}


/* Enter key: echo the segment with the prompt (or the "> " continuation
   prompt), join continuation lines, and hand the completed command to the
   tab's executor. */
/* Append one input segment to the tab's pending (possibly multi-line)
   command. A trailing backslash is a pure continuation: it and the newline
   disappear (bash rule); continuation from an open quote/$(...) keeps the
   newline. Returns 1 when more input is needed. */
int pending_append(Tab *t, char *seg) {
    size_t sl = strlen(seg); int cont = 0;
    /* Inside a heredoc body, segments are data: a trailing backslash is
       kept verbatim instead of acting as a line continuation. */
    char delim[64]; int quoted = 0;
    int in_body = t->pending_cmd[0] && heredoc_open(t->pending_cmd, delim, sizeof(delim), &quoted) == 1;
    if (!in_body && sl > 0 && seg[sl - 1] == '\\') { seg[--sl] = '\0'; cont = 1; }
    size_t pl = strlen(t->pending_cmd);
    if (pl + sl + 2 < INPUT_CAP) { memcpy(t->pending_cmd + pl, seg, sl + 1); pl += sl; }
    if (cont || line_needs_more(t->pending_cmd)) {
        if (!cont && pl + 1 < INPUT_CAP) { t->pending_cmd[pl++] = '\n'; t->pending_cmd[pl] = '\0'; }
        return 1;
    }
    return 0;
}
void submit_current_line(Tab *t) {
    char prompt_echo[PATH_MAX + 32];
    build_prompt(prompt_echo, sizeof(prompt_echo), t);
    append_text(t, prompt_echo);
    append_screen(t, t->input, t->input_len);
    append_text(t, "\n");
    char segment[INPUT_CAP];
    snprintf(segment, sizeof(segment), "%s", t->input);
    t->input[0] = '\0'; t->input_len = 0; t->cursor_pos = 0;
    t->hist_pos = -1; completion_count = 0;
    if (pending_append(t, segment)) {
        t->multiline = 1;
        return;
    }
    t->multiline = 0;
    char full[INPUT_CAP];
    snprintf(full, sizeof(full), "%s", t->pending_cmd);
    t->pending_cmd[0] = '\0';
    if (!full[0]) return;
    if (t->busy || t->running) { append_text(t, "A command is still running in this tab. Wait for it to finish.\n"); return; }
    if (process_mode) {
        if (write_packet(t->cmd_fd, 'L', full, (uint32_t)strlen(full)) == 0) t->busy = 1;
    } else {
        snprintf(t->queue_line, sizeof(t->queue_line), "%s", full);
        __sync_synchronize();
        t->busy = 1;
        t->has_submit = 1;
    }
}

static void switch_tab(int dir) {
    for (int step = 1; step <= MAX_TABS; step++) {
        int idx = (current_tab + dir * step) % MAX_TABS; if (idx < 0) idx += MAX_TABS;
        if (tabs[idx].active) { current_tab = idx; return; }
    }
}

/* Ctrl+R (Task 9): print the most recent exact match; otherwise print the
   command(s) sharing the longest common substring with the term when that
   is longer than 2 characters; otherwise the no-match message. */
/* Length of the longest substring shared by two strings. */
static size_t common_substring_len(const char *a, size_t an, const char *b, size_t bn) {
    size_t best = 0;
    for (size_t i = 0; i < an; i++) for (size_t j = 0; j < bn; j++) {
        size_t k = 0;
        while (i + k < an && j + k < bn && a[i + k] == b[j + k]) k++;
        if (k > best) best = k;
    }
    return best;
}
static void do_history_search(Tab *t) {
    const char *term = t->input; int count = hist_count();
    char entry[INPUT_CAP];
    for (int i = count - 1; i >= 0; i--) {
        if (hist_get(i, entry, sizeof(entry)) && strcmp(entry, term) == 0) {
            append_text(t, entry); append_text(t, "\n");
            return;
        }
    }
    size_t qn = strlen(term), best = 0;
    for (int i = 0; i < count; i++) {
        if (!hist_get(i, entry, sizeof(entry))) continue;
        size_t k = common_substring_len(term, qn, entry, strlen(entry));
        if (k > best) best = k;
    }
    if (best > 2 && qn) {
        int printed = 0;
        for (int i = count - 1; i >= 0 && printed < 20; i--) {
            if (!hist_get(i, entry, sizeof(entry))) continue;
            size_t eb = common_substring_len(term, qn, entry, strlen(entry));
            if (eb == best) { append_text(t, entry); append_text(t, "\n"); printed++; }
        }
        if (printed) return;
    }
    append_text(t, "No match for search term in history\n");
}

void handle_key(XKeyEvent *ev) {
    Tab *t = &tabs[current_tab]; KeySym ks; char textbuf[64]; int n = XLookupString(ev, textbuf, sizeof(textbuf), &ks, NULL);
    int ctrl = (ev->state & ControlMask) != 0;
    if (key_matches(ev, ks, BIND_SCROLL_UP)) { scroll_tab(t, screen_page_rows()); return; }
    if (key_matches(ev, ks, BIND_SCROLL_DOWN)) { scroll_tab(t, -screen_page_rows()); return; }
    /* Modifier keys alone do nothing: swallowing them here also keeps a
       lone Shift press from snapping a scrolled view back to the tail
       just before Shift+PageUp/Down is pressed. */
    if (ks == XK_Shift_L || ks == XK_Shift_R || ks == XK_Control_L || ks == XK_Control_R ||
        ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L || ks == XK_Meta_R ||
        ks == XK_Super_L || ks == XK_Super_R || ks == XK_Caps_Lock || ks == XK_Num_Lock) return;
    t->scroll_offset = 0; /* any other key snaps a scrolled view back */
    /* Typing or editing invalidates a mouse selection (the text it
       covers may change); only the copy key itself may read it. */
    if (!key_matches(ev, ks, BIND_COPY)) clear_selection(t);
    if (searching_history) {
        if (ks == XK_Return || ks == XK_KP_Enter) {
            do_history_search(t);
            t->input[0] = 0; t->input_len = 0; t->cursor_pos = 0;
            searching_history = 0; return;
        }
        if (ks == XK_Escape) {
            snprintf(t->input, sizeof(t->input), "%s", saved_search_input);
            t->input_len = strlen(t->input); t->cursor_pos = t->input_len;
            searching_history = 0; return;
        }
        if (key_matches(ev, ks, BIND_INTERRUPT)) {
            t->input[0] = 0; t->input_len = 0; t->cursor_pos = 0;
            searching_history = 0; return;
        }
        /* other keys fall through to the normal editing code, editing the
           search term */
    } else {
        if (key_matches(ev, ks, BIND_INTERRUPT)) {
            if (process_mode) {
                /* Always signal the tab child: only it knows whether
                   a multiWatch is active, and only its interrupt
                   handler can stop one. (Skipping the signal when
                   the GUI looks idle left a watcher running forever,
                   appending output every 2 s.) The input line lives
                   in the GUI, so discard it here too — the child
                   cannot clear it. The child prints the ^C, so the
                   GUI must not print one as well. */
                if (t->child_pid > 0) kill(t->child_pid, SIGUSR1);
                else append_text(t, "^C\n");
                discard_input_line(t);
            }
            else interrupt_current_command(t);
            return;
        }
        if (key_matches(ev, ks, BIND_SUSPEND)) {
            if (process_mode) { if (t->child_pid > 0) kill(t->child_pid, SIGUSR2); }
            else suspend_foreground(t);
            return;
        }
        if (key_matches(ev, ks, BIND_COPY)) {
            /* A mouse selection is copied when one exists; otherwise
               the current input line is copied, as before. */
            if (!sel_copy_text(t, clipboard_text, sizeof(clipboard_text)))
                snprintf(clipboard_text, sizeof(clipboard_text), "%s", t->input);
            XSetSelectionOwner(dpy, clipboard_atom, win, CurrentTime);
            return;
        }
        if (key_matches(ev, ks, BIND_PASTE)) { request_clipboard_paste(); return; }
        if (key_matches(ev, ks, BIND_SEARCH)) {
            snprintf(saved_search_input, sizeof(saved_search_input), "%s", t->input);
            t->input[0] = 0; t->input_len = 0; t->cursor_pos = 0;
            searching_history = 1;
            /* No label is appended: the live input line is drawn with
               the "Enter search term: " prompt (screen.c), so the term
               being typed shows right after the label. */
            return;
        }
        if (key_matches(ev, ks, BIND_NEW_TAB)) { create_tab(); return; }
        if (key_matches(ev, ks, BIND_CLOSE_TAB)) { close_tab(current_tab); return; }
        if (key_matches(ev, ks, BIND_CLEAR)) { clear_tab_screen(t); return; }
        if (ctrl && (ks == XK_q || ks == XK_Q)) { running = 0; return; }
    }
    if (key_matches(ev, ks, BIND_EOF) && !searching_history) {
        /* EOF: exit the shell (close the tab, or quit on the last tab)
           whenever the tab is idle — even when text is typed but not
           yet executed, the unexecuted line is discarded, as EOF
           discards it in a real shell. While a command runs the tab
           cannot close, so Ctrl+D deletes the character under the
           cursor instead (the Delete key always does the same). */
        if (!t->running && !t->busy) {
            if (tab_count > 1) close_tab(current_tab); else running = 0;
        } else if (t->cursor_pos < t->input_len) {
            memmove(t->input + t->cursor_pos, t->input + t->cursor_pos + 1, t->input_len - t->cursor_pos);
            t->input_len--; t->input[t->input_len] = '\0';
        }
        return;
    }
    if (key_matches(ev, ks, BIND_LINE_START)) { t->cursor_pos = 0; return; }
    if (key_matches(ev, ks, BIND_LINE_END)) { t->cursor_pos = t->input_len; return; }
    if (key_matches(ev, ks, BIND_KILL_START)) {
        memmove(t->input, t->input + t->cursor_pos, t->input_len - t->cursor_pos + 1);
        t->input_len -= t->cursor_pos; t->cursor_pos = 0; return;
    }
    if (key_matches(ev, ks, BIND_KILL_END)) { t->input_len = t->cursor_pos; t->input[t->input_len] = '\0'; return; }
    if (key_matches(ev, ks, BIND_DELETE_WORD)) {
        size_t p = t->cursor_pos;
        while (p > 0 && (t->input[p - 1] == ' ' || t->input[p - 1] == '\t')) p--;
        while (p > 0 && t->input[p - 1] != ' ' && t->input[p - 1] != '\t') p--;
        memmove(t->input + p, t->input + t->cursor_pos, t->input_len - t->cursor_pos + 1);
        t->input_len -= t->cursor_pos - p; t->cursor_pos = p; return;
    }
    if (key_matches(ev, ks, BIND_CURSOR_LEFT)) { if (t->cursor_pos > 0) t->cursor_pos--; return; }
    if (key_matches(ev, ks, BIND_CURSOR_RIGHT)) { if (t->cursor_pos < t->input_len) t->cursor_pos++; return; }
    if (ks == XK_Home) { t->cursor_pos = 0; return; }
    if (ks == XK_End) { t->cursor_pos = t->input_len; return; }
    if (key_matches(ev, ks, BIND_HIST_PREV) && !searching_history) { history_up(t); return; }
    if (key_matches(ev, ks, BIND_HIST_NEXT) && !searching_history) { history_down(t); return; }
    if (ks == XK_Tab && !ctrl) { if (!searching_history) complete_input(t); return; }
    if (key_matches(ev, ks, BIND_TAB_NEXT)) { switch_tab(1); return; }
    if (key_matches(ev, ks, BIND_TAB_PREV)) { switch_tab(-1); return; }
    if (ks == XK_BackSpace) {
        if (t->cursor_pos > 0) { memmove(t->input + t->cursor_pos - 1, t->input + t->cursor_pos, t->input_len - t->cursor_pos + 1); t->cursor_pos--; t->input_len--; }
        if (searching_history) return;
        completion_count = 0;
        return;
    }
    if (ks == XK_Delete) {
        if (t->cursor_pos < t->input_len) { memmove(t->input + t->cursor_pos, t->input + t->cursor_pos + 1, t->input_len - t->cursor_pos); t->input_len--; }
        return;
    }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (!searching_history) submit_current_line(t);
        return;
    }
    if (!searching_history && completion_count > 1 && n == 1 && textbuf[0] >= '1' && textbuf[0] <= '9' && !ctrl) { select_completion(t, (unsigned int)(textbuf[0] - '0')); return; }
    if (completion_count > 1 && ks == XK_Escape) completion_count = 0;
    if (n > 0 && !ctrl) {
        size_t insert_n = (size_t)n;
        if (insert_n > INPUT_CAP - 1 - t->input_len) insert_n = INPUT_CAP - 1 - t->input_len;
        if (insert_n) {
            memmove(t->input + t->cursor_pos + insert_n, t->input + t->cursor_pos, t->input_len - t->cursor_pos + 1);
            memcpy(t->input + t->cursor_pos, textbuf, insert_n);
            t->input_len += insert_n; t->cursor_pos += insert_n;
            if (!searching_history) t->hist_pos = -1;
        }
    }
}
