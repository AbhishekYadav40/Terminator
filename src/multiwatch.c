/* Terminator — multiwatch.c
 * multiWatch (Design section 6): the per-tab asynchronous state
 *  * machine that re-runs command lists every two seconds.
 */
#include "common.h"

MultiWatch multiwatches[MAX_TABS];


/* Asynchronous multiWatch. Commands run concurrently, then repeat on a two-second
   schedule. No blocking wait/poll is performed here; the X11 event loop services it. */
static long long monotonic_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void multiwatch_close_round(MultiWatch *mw) {
    for (int i = 0; i < mw->command_count; i++) {
        if (mw->fds[i] >= 0) { close(mw->fds[i]); mw->fds[i] = -1; }
        for (int k = 0; k < mw->npids[i]; k++) mw->pids[i][k] = 0;
        mw->process_done[i] = 1;
    }
    mw->round_started = 0;
}

void stop_multiwatch(int tab_index, int announce) {
    if (tab_index < 0 || tab_index >= MAX_TABS) return;
    MultiWatch *mw = &multiwatches[tab_index];
    if (!mw->active) return;
    mw->stopping = 1; mw->next_run_ms = 0;
    for (int i = 0; i < mw->command_count; i++) {
        if (mw->pgids[i] > 0) {
            /* Each watched pipeline has its own process group. */
            if (kill(-mw->pgids[i], SIGINT) < 0 && errno == ESRCH) {
                for (int k = 0; k < mw->npids[i]; k++)
                    if (mw->pids[i][k] > 0) kill(mw->pids[i][k], SIGINT);
            }
        }
    }
    (void)announce;
}

/* Launch one segment of watched command i. Returns 1 when a pipeline
   was launched, 0 when the segment could not run (parse/launch
   failure counts as a failed segment; the caller advances). */
static int multiwatch_launch_segment(MultiWatch *mw, int i, int seg) {
    Tab *t = &tabs[mw->tab_index];
    char segtext[INPUT_CAP];
    size_t sl = (size_t)mw->seg_len[i][seg];
    if (sl >= sizeof(segtext)) sl = sizeof(segtext) - 1;
    memcpy(segtext, mw->commands[i] + mw->seg_off[i][seg], sl); segtext[sl] = '\0';
    /* Commands are run by Terminator's own parser/pipeline machinery,
       with glob expansion relative to the tab's cwd. */
    char *tokens[MAX_TOKENS]; int ntok = 0;
    if (tokenize(segtext, tokens, &ntok, NULL) < 0 || ntok == 0) { free_tokens(tokens, ntok); return 0; }
    if (expand_globs(t, tokens, &ntok, NULL) < 0) { free_tokens(tokens, ntok); return 0; }
    Command cmds[MAX_CMDS]; int nc = 0;
    if (parse_commands(tokens, ntok, cmds, &nc) < 0) { free_tokens(tokens, ntok); return 0; }
    int fd = -1; pid_t pids[MAX_CMDS]; pid_t pgid = 0;
    int np = launch_pipeline_fds(t, cmds, nc, &fd, pids, &pgid);
    free_tokens(tokens, ntok);
    if (np < 0) return 0;
    mw->npids[i] = 0; mw->pgids[i] = 0; mw->fds[i] = -1;
    for (int k = 0; k < np; k++) mw->pids[i][k] = pids[k];
    mw->npids[i] = np; mw->pgids[i] = pgid; mw->fds[i] = fd; mw->process_done[i] = 0;
    return 1;
}

/* Start the next segment of watched command i, honouring && / || on
   the previous segment's status. Returns 1 when a segment is now
   running, 0 when the chain is exhausted (or stopping). A `&`
   separator inside a watched command runs sequentially like `;`:
   backgrounding within a 2-second round has no meaning. */
static int multiwatch_advance(MultiWatch *mw, int i) {
    if (mw->stopping) return 0;
    while (mw->seg_index[i] < mw->seg_count[i]) {
        int k = mw->seg_index[i];
        if (k > 0) {
            int prev_op = mw->seg_op[i][k - 1];
            if (prev_op == LIST_AND && mw->seg_status[i] != 0) { mw->seg_index[i]++; continue; }
            if (prev_op == LIST_OR && mw->seg_status[i] == 0) { mw->seg_index[i]++; continue; }
        }
        mw->seg_index[i]++;
        if (multiwatch_launch_segment(mw, i, k)) return 1;
        mw->seg_status[i] = 1; /* failed segment: keep chaining */
    }
    return 0;
}

static int multiwatch_start_round(MultiWatch *mw) {
    if (!mw || !mw->active || mw->stopping) return -1;
    mw->round_number++; mw->round_started = 1;
    for (int i = 0; i < mw->command_count; i++) {
        mw->npids[i] = 0; mw->pgids[i] = 0; mw->fds[i] = -1;
        mw->process_done[i] = 1; mw->header_written[i] = 0; mw->footer_written[i] = 0;
        mw->out_started[i] = 0; mw->out_last_nl[i] = 1;
        /* Split the (already expanded) watched command into list
           segments once per round; the segments run in order. */
        ListSeg segs[MW_MAX_SEGS];
        int nsegs = split_list(mw->commands[i], segs, MW_MAX_SEGS);
        mw->seg_count[i] = nsegs; mw->seg_index[i] = 0; mw->seg_status[i] = 0;
        for (int k = 0; k < nsegs; k++) {
            mw->seg_off[i][k] = (int)(segs[k].ptr - mw->commands[i]);
            mw->seg_len[i][k] = (int)segs[k].len;
            mw->seg_op[i][k] = segs[k].op;
        }
        (void)multiwatch_advance(mw, i);
    }
    mw->next_run_ms = monotonic_ms() + MULTIWATCH_INTERVAL_MS;
    return 0;
}

static void multiwatch_write_header(MultiWatch *mw, int i) {
    Tab *t = &tabs[mw->tab_index];
    char head[INPUT_CAP + 96];
    snprintf(head, sizeof(head), "\"%s\", %ld:\n----------------------------------------------------\n",
             mw->commands[i], (long)time(NULL));
    append_text(t, head);
    mw->header_written[i] = 1;
}

static void multiwatch_finish_command(MultiWatch *mw, int i) {
    if (mw->footer_written[i]) return;
    Tab *t = &tabs[mw->tab_index];
    if (!mw->header_written[i]) multiwatch_write_header(mw, i);
    if (mw->out_started[i] && !mw->out_last_nl[i]) append_text(t, "\n");
    append_text(t, "----------------------------------------------------\n");
    mw->footer_written[i] = 1;
}

void service_multiwatch(int tab_index) {
    if (tab_index < 0 || tab_index >= MAX_TABS) return;
    MultiWatch *mw = &multiwatches[tab_index];
    if (!mw->active) return;
    Tab *t = &tabs[mw->tab_index];
    int round_complete = 1;
    for (int i = 0; i < mw->command_count; i++) {
        if (mw->fds[i] >= 0) {
            char buf[OUTPUT_CHUNK];
            for (;;) {
                ssize_t n = read(mw->fds[i], buf, sizeof(buf));
                if (n > 0) {
                    if (!mw->header_written[i]) multiwatch_write_header(mw, i);
                    append_screen(t, buf, (size_t)n);
                    mw->out_started[i] = 1;
                    mw->out_last_nl[i] = (buf[n - 1] == '\n');
                } else if (n == 0) { close(mw->fds[i]); mw->fds[i] = -1; break; }
                else { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) break; close(mw->fds[i]); mw->fds[i] = -1; break; }
            }
        }
        if (!mw->process_done[i]) {
            int alive = 0;
            for (int k = 0; k < mw->npids[i]; k++) if (mw->pids[i][k] > 0) {
                int status = 0; pid_t r = waitpid(mw->pids[i][k], &status, WNOHANG);
                if (r == mw->pids[i][k] || (r < 0 && errno == ECHILD)) {
                    /* A pipeline's status is its last stage's, as in $?. */
                    if (k == mw->npids[i] - 1 && r == mw->pids[i][k]) {
                        if (WIFEXITED(status)) mw->seg_status[i] = WEXITSTATUS(status);
                        else if (WIFSIGNALED(status)) mw->seg_status[i] = 128 + WTERMSIG(status);
                    }
                    mw->pids[i][k] = 0;
                }
                else alive++;
            }
            if (!alive) mw->process_done[i] = 1;
        }
        if (mw->process_done[i] && mw->fds[i] < 0) {
            if (!multiwatch_advance(mw, i)) multiwatch_finish_command(mw, i);
        }
        if (!mw->process_done[i] || mw->fds[i] >= 0) round_complete = 0;
    }
    if (mw->stopping) {
        if (round_complete) {
            multiwatch_close_round(mw);
            memset(mw, 0, sizeof(*mw));
        }
        return;
    }
    if (mw->round_started && round_complete) {
        mw->round_started = 0;
        long long now = monotonic_ms();
        /* Keep a stable two-second cadence; skip missed slots. */
        while (mw->next_run_ms <= now) mw->next_run_ms += MULTIWATCH_INTERVAL_MS;
    }
    if (!mw->round_started && monotonic_ms() >= mw->next_run_ms) { (void)multiwatch_start_round(mw); }
}

void run_multiwatch_list(Tab *t, char cmds[][INPUT_CAP], int n) {
    int tab_index = (int)(t - tabs);
    if (tab_index < 0 || tab_index >= MAX_TABS) { t->last_status = 1; return; }
    if (multiwatches[tab_index].active) {
        append_text(t, "multiWatch: a watcher is already active in this tab; press Ctrl+C to stop it first.\n");
        t->last_status = 1; return;
    }
    if (n > MAX_MULTIWATCH_COMMANDS) n = MAX_MULTIWATCH_COMMANDS;
    /* A watched command is a single line re-run every round; it has no
       following lines to supply a heredoc body, so reject `<<` loudly
       instead of launching a command with no stdin. */
    for (int i = 0; i < n; i++) {
        char delim[64]; int quoted = 0;
        if (heredoc_find(cmds[i], delim, sizeof(delim), &quoted)) {
            append_text(t, "multiWatch: heredocs are not supported inside watched commands.\n");
            t->last_status = 2; return;
        }
    }
    MultiWatch *mw = &multiwatches[tab_index];
    memset(mw, 0, sizeof(*mw)); mw->active = 1; mw->tab_index = tab_index; mw->command_count = n;
    for (int i = 0; i < MAX_MULTIWATCH_COMMANDS; i++) mw->fds[i] = -1;
    for (int i = 0; i < n; i++) snprintf(mw->commands[i], sizeof(mw->commands[i]), "%s", cmds[i]);
    (void)multiwatch_start_round(mw);
    t->last_status = 0;
}

void run_multiwatch(Tab *t, char **tok, int ntok) {
    if (ntok < 2) {
        append_text(t, "Usage: multiWatch [\"command 1\", \"command 2\", ...]\n");
        t->last_status = 2; return;
    }
    char cmds[MAX_MULTIWATCH_COMMANDS][INPUT_CAP];
    int n = ntok - 1; if (n > MAX_MULTIWATCH_COMMANDS) n = MAX_MULTIWATCH_COMMANDS;
    for (int i = 0; i < n; i++) snprintf(cmds[i], sizeof(cmds[i]), "%s", tok[i + 1]);
    run_multiwatch_list(t, cmds, n);
}

/* Parse the spec's bracket form, multiWatch ["cmd1", "cmd2", ...], from the
   raw (already expanded) line. Returns the command count, or -1 when the
   line is not a bracket-form multiWatch. */
int parse_multiwatch_brackets(const char *line, char cmds[][INPUT_CAP], int maxn) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "multiWatch", 10) != 0) return -1;
    p += 10;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return -1;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < maxn) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (*p != '"' && *p != '\'') {
            const char *s = p;
            while (*p && *p != ',' && *p != ']') p++;
            size_t len = (size_t)(p - s);
            while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
            if (len >= INPUT_CAP) len = INPUT_CAP - 1;
            memcpy(cmds[n], s, len); cmds[n][len] = '\0'; n++;
            continue;
        }
        char q = *p++;
        size_t len = 0;
        while (*p && *p != q) {
            if (*p == '\\' && q == '"' && p[1]) p++;
            if (len + 1 < INPUT_CAP) cmds[n][len++] = *p;
            p++;
        }
        if (*p == q) p++;
        cmds[n][len] = '\0'; n++;
    }
    return n;
}
