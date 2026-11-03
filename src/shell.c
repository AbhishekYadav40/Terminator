/* Terminator — shell.c
 * The shell engine (Design section 4): tokenize, pipelines,
 *  * redirection, expansion, substitution, built-ins, and execute_command.
 *  * This is the one engine used by typed lines, scripts, batch mode,
 *  * and both execution modes.
 */
#include "common.h"


/* Tokenizer: preserve backslash sequences inside double quotes unless the
   following character is one of $, `, \\, or ". Outside quotes, backslash
   escapes the next character. Single quotes preserve everything literally. */

/* qflags[i] (may be NULL) records, per token: bit 1 = the word had a
   quoted portion or an escaped glob character (never glob-expand it),
   bit 2 = the word started inside quotes (never a NAME=value assignment). */
int tokenize(const char *line, char **tokens, int *argc_out, int *qflags) {
    char *buf = strdup(line);
    if (!buf) return -1;
    int n = 0;
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        if (n >= MAX_TOKENS - 1) { for (int i=0;i<n;i++) free(tokens[i]); free(buf); return -1; }
        /* File-descriptor redirections: 2>file, 2>>file, 0<file. Only at a
           word start (spaces were skipped above), so `echo 2` is untouched. */
        if ((*p == '0' || *p == '1' || *p == '2') && (p[1] == '>' || p[1] == '<')) {
            char tmp[4];
            if (p[1] == '>' && p[2] == '>') { tmp[0] = *p; tmp[1] = '>'; tmp[2] = '>'; tmp[3] = 0; p += 3; }
            else { tmp[0] = *p; tmp[1] = p[1]; tmp[2] = 0; p += 2; }
            tokens[n] = strdup(tmp);
            if (qflags) qflags[n] = 0;
            if (!tokens[n]) { for (int i=0;i<n;i++) free(tokens[i]); free(buf); return -1; }
            n++;
            continue;
        }
        if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {
            const char *op = NULL; char tmp[2];
            if (*p == '>' && p[1] == '>') { op = ">>"; p += 2; }
            else if (*p == '<' && p[1] == '<') { op = "<<"; p += 2; }
            else if (*p == '|' && p[1] == '&') { op = "|&"; p += 2; }
            else if (*p == '&' && p[1] == '>') {
                if (p[2] == '>') { op = "&>>"; p += 3; } else { op = "&>"; p += 2; }
            }
            else { tmp[0] = *p++; tmp[1] = '\0'; op = tmp; }
            tokens[n] = strdup(op);
            if (qflags) qflags[n] = 0;
            if (!tokens[n]) { for (int i=0;i<n;i++) free(tokens[i]); free(buf); return -1; }
            n++;
            continue;
        }
        char word[INPUT_CAP]; size_t k = 0; char quote = 0; int qany = 0, qfirst = 0;
        while (*p) {
            if (!quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '|' || *p == '<' || *p == '>' || *p == '&')) break;
            if (quote == '\'') {
                if (*p == '\'') { quote = 0; p++; }
                else { if (k + 1 < sizeof(word)) { word[k++] = *p; qany = 1; if (k == 1) qfirst = 1; } p++; }
                continue;
            }
            if (quote == '"') {
                if (*p == '"') { quote = 0; p++; continue; }
                if (*p == '\\') {
                    char next = p[1];
                    if (next == '$' || next == '`' || next == '"' || next == '\\') {
                        if (k + 1 < sizeof(word)) { word[k++] = next; qany = 1; if (k == 1) qfirst = 1; }
                        p += 2;
                    } else {
                        /* Keep sequences like \n for printf to interpret. */
                        if (k + 1 < sizeof(word)) { word[k++] = *p; qany = 1; if (k == 1) qfirst = 1; }
                        p++;
                    }
                    continue;
                }
                if (k + 1 < sizeof(word)) { word[k++] = *p; qany = 1; if (k == 1) qfirst = 1; }
                p++;
                continue;
            }
            if (*p == '\'' || *p == '"') { quote = *p++; continue; }
            if (*p == '\\') {
                p++;
                if (*p && k + 1 < sizeof(word)) { word[k++] = *p; if (*p == '*' || *p == '?' || *p == '[') qany = 1; }
                if (*p) p++;
                continue;
            }
            if (k + 1 < sizeof(word)) word[k++] = *p;
            p++;
        }
        if (quote) { for (int i=0;i<n;i++) free(tokens[i]); free(buf); return -2; }
        word[k] = '\0';
        tokens[n] = strdup(word);
        if (qflags) qflags[n] = (qany ? 1 : 0) | (qfirst ? 2 : 0);
        if (!tokens[n]) { for (int i=0;i<n;i++) free(tokens[i]); free(buf); return -1; }
        n++;
    }
    tokens[n] = NULL;
    *argc_out = n;
    free(buf);
    return 0;
}
void free_tokens(char **tokens, int n) { for (int i=0;i<n;i++) free(tokens[i]); }


/* Decode <, >, >>, N<, N>, N>> (N = 0, 1 or 2) into a target fd,
   an input/output flag and an append flag. Returns 1 when s is one. */
static int redir_op(const char *s, int *target, int *is_input, int *append) {
    if (!strcmp(s, "<")) { *target = 0; *is_input = 1; *append = 0; return 1; }
    if (!strcmp(s, ">")) { *target = 1; *is_input = 0; *append = 0; return 1; }
    if (!strcmp(s, ">>")) { *target = 1; *is_input = 0; *append = 1; return 1; }
    if ((s[0] == '0' || s[0] == '1' || s[0] == '2') && (s[1] == '>' || s[1] == '<')) {
        *target = s[0] - '0';
        if (s[1] == '<') { *is_input = 1; *append = 0; return s[2] == 0; }
        *is_input = 0; *append = (s[2] == '>');
        return s[2] == 0 || (s[2] == '>' && s[3] == 0);
    }
    return 0;
}

static int push_redir(Command *c, int target, char *file, int append, int dup_fd, int close_fd, int both) {
    if (c->nredirs >= 8) return -1;
    Redir *r = &c->redirs[c->nredirs++];
    r->target_fd = target; r->file = file; r->append = append;
    r->dup_fd = dup_fd; r->close_fd = close_fd; r->both = both;
    return 0;
}

int parse_commands(char **tok, int ntok, Command *cmds, int *ncmd_out) {
    memset(cmds, 0, sizeof(Command) * MAX_CMDS); int nc = 1;
    for (int i = 0; i < ntok; i++) {
        if (!strcmp(tok[i], "|") || !strcmp(tok[i], "|&")) {
            if (cmds[nc-1].argc == 0 || nc >= MAX_CMDS) return -1;
            if (!strcmp(tok[i], "|&")) cmds[nc-1].pipe_stderr = 1;
            nc++; continue;
        }
        if (!strcmp(tok[i], "<<")) {
            /* Heredoc: the next token is the delimiter word; the body
               itself arrives separately (execute_command) and is attached
               after parsing. Error codes: -2 words after the heredoc on
               the same line, -3 a second heredoc in one command,
               -4 a missing or non-word delimiter. */
            if (i + 1 >= ntok) return -4;
            char *d = tok[i + 1];
            if (!strcmp(d, "<") || !strcmp(d, ">") || !strcmp(d, ">>") || !strcmp(d, "<<") ||
                !strcmp(d, "|") || !strcmp(d, "|&") || !strcmp(d, "&")) return -4;
            if (cmds[nc-1].has_heredoc) return -3;
            cmds[nc-1].has_heredoc = 1;
            i++;
            continue;
        }
        if (!strcmp(tok[i], "&>") || !strcmp(tok[i], "&>>")) {
            /* &>file: stdout and stderr to one file. &>2 / &>-: stdout
               duplicated to (or closed from) that fd, bash-style. */
            if (i + 1 >= ntok) return -5;
            int append = !strcmp(tok[i], "&>>");
            char *f = tok[++i];
            if ((f[0] == '0' || f[0] == '1' || f[0] == '2') && f[1] == 0) {
                if (push_redir(&cmds[nc-1], 1, NULL, 0, f[0] - '0', 0, 0) < 0) return -1;
            } else if (!strcmp(f, "-")) {
                if (push_redir(&cmds[nc-1], 1, NULL, 0, -1, 1, 0) < 0) return -1;
            } else {
                if (push_redir(&cmds[nc-1], 1, f, append, -1, 0, 1) < 0) return -1;
            }
            continue;
        }
        {
            int target = 0, is_input = 0, append = 0;
            if (redir_op(tok[i], &target, &is_input, &append)) {
                if (i + 1 >= ntok) return -5;
                char *f = tok[++i];
                if (!strcmp(f, "&")) {
                    /* N>&M / N>&- : duplicate fd M onto fd N, or close N. */
                    if (i + 1 >= ntok) return -5;
                    char *w = tok[++i];
                    if ((w[0] == '0' || w[0] == '1' || w[0] == '2') && w[1] == 0) {
                        if (push_redir(&cmds[nc-1], target, NULL, 0, w[0] - '0', 0, 0) < 0) return -1;
                    } else if (!strcmp(w, "-")) {
                        if (push_redir(&cmds[nc-1], target, NULL, 0, -1, 1, 0) < 0) return -1;
                    } else return -1;
                } else {
                    if (push_redir(&cmds[nc-1], target, f, append, -1, 0, 0) < 0) return -1;
                }
                continue;
            }
        }
        if (cmds[nc-1].has_heredoc) return -2;
        if (cmds[nc-1].argc >= MAX_TOKENS - 1) return -1;
        cmds[nc-1].argv[cmds[nc-1].argc++] = tok[i];
    }
    if (cmds[nc-1].argc == 0) return -1;
    for (int i=0;i<nc;i++) cmds[i].argv[cmds[i].argc] = NULL;
    *ncmd_out = nc; return 0;
}
void print_error(Tab *t, const char *what) { char b[512]; snprintf(b,sizeof(b),"%s: %s\n",what,strerror(errno)); append_text(t,b); }

int launch_pipeline_fds(Tab *t, Command *cmds, int nc, int *fd_out, pid_t *pids_out, pid_t *pgid_out) {
    for (int i = 0; i < nc; i++) {
        /* A heredoc needs its collected body, must feed the first
           command, and cannot share stdin with an input file. Callers
           that cannot supply a body (command substitution, multiWatch)
           never launch with one attached. */
        if (cmds[i].has_heredoc && cmds[i].heredoc == NULL) return -1;
        if (cmds[i].has_heredoc && i != 0) return -1;
        if (cmds[i].has_heredoc) for (int r = 0; r < cmds[i].nredirs; r++)
            if (cmds[i].redirs[r].target_fd == 0 && cmds[i].redirs[r].file) return -1;
    }
    int outpipe[2];
    if (pipe(outpipe) < 0) { print_error(t,"pipe"); return -1; }
    fcntl(outpipe[0], F_SETFL, fcntl(outpipe[0], F_GETFL) | O_NONBLOCK);
    int prev_read = -1; int pids[MAX_CMDS]; int np = 0; pid_t pgid = 0;
    for (int i=0;i<nc;i++) {
        int hpipe[2] = {-1, -1};
        if (cmds[i].has_heredoc) {
            /* Bodies are capped at INPUT_CAP bytes, well under a pipe
               buffer, so writing before the fork cannot block. */
            if (pipe(hpipe) < 0) {
                print_error(t, "pipe");
                if (prev_read >= 0) close(prev_read);
                close(outpipe[0]); close(outpipe[1]);
                return -1;
            }
            if (write_all_fd(hpipe[1], cmds[i].heredoc, cmds[i].heredoc_len) < 0) {
                close(hpipe[0]); close(hpipe[1]);
                if (prev_read >= 0) close(prev_read);
                close(outpipe[0]); close(outpipe[1]);
                return -1;
            }
            close(hpipe[1]); hpipe[1] = -1;
        }
        int next_pipe[2] = {-1,-1};
        if (i < nc-1 && pipe(next_pipe) < 0) { print_error(t,"pipe"); if(hpipe[0]>=0)close(hpipe[0]); if(prev_read>=0)close(prev_read); close(outpipe[0]);close(outpipe[1]); return -1; }
        pid_t pid = fork();
        if (pid < 0) { print_error(t,"fork"); if(hpipe[0]>=0)close(hpipe[0]); if(prev_read>=0)close(prev_read); if(next_pipe[0]>=0){close(next_pipe[0]);close(next_pipe[1]);} close(outpipe[0]);close(outpipe[1]); return -1; }
        if (pid == 0) {
            /* Keep 0/1/2 open: if the executor was started with a
               standard fd closed, pipe()/open() below could return
               0/1/2 and a later close() would kill the child's stdin,
               stdout or stderr (this broke every pipe/redirection in
               process mode, where the tab child had lost fd 0). */
            for (int sfd = 0; sfd <= 2; sfd++)
                if (fcntl(sfd, F_GETFD) < 0) { int nfd = open("/dev/null", O_RDWR); (void)nfd; }
            (void)setpgid(0, pgid);
            signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
            { int chrc = chdir(t->cwd); (void)chrc; }
            if (prev_read >= 0) dup2(prev_read, STDIN_FILENO);
            if (cmds[i].has_heredoc) dup2(hpipe[0], STDIN_FILENO);
            if (i < nc-1) dup2(next_pipe[1], STDOUT_FILENO);
            else dup2(outpipe[1], STDOUT_FILENO);
            /* |& sends stderr down the pipeline too; otherwise stderr is
               captured to the screen like stdout. */
            if (i < nc-1 && cmds[i].pipe_stderr) dup2(next_pipe[1], STDERR_FILENO);
            else dup2(outpipe[1], STDERR_FILENO);
            if (prev_read >= 0) close(prev_read);
            if (hpipe[0] >= 0) close(hpipe[0]);
            if (next_pipe[0] >= 0) close(next_pipe[0]);
            if (next_pipe[1] >= 0) close(next_pipe[1]);
            close(outpipe[0]); close(outpipe[1]);
            /* Redirections apply in the order written, so `>f 2>&1` and
               `2>&1 >f` send stderr to different places, as in bash. A
               failed redirection is status 1 (bash), not 126. */
            for (int r = 0; r < cmds[i].nredirs; r++) {
                Redir *rd = &cmds[i].redirs[r];
                if (rd->close_fd) { close(rd->target_fd); continue; }
                if (rd->dup_fd >= 0) { dup2(rd->dup_fd, rd->target_fd); continue; }
                int flags = (rd->target_fd == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | (rd->append ? O_APPEND : O_TRUNC));
                int fd = open(rd->file, flags, 0644);
                if (fd < 0) { perror(rd->file); _exit(1); }
                dup2(fd, rd->target_fd); close(fd);
                if (rd->both) dup2(rd->target_fd, 2);
            }
            execvp(cmds[i].argv[0],cmds[i].argv); { int e=errno; if (e==ENOENT) dprintf(STDERR_FILENO, "terminator: %s: command not found\n", cmds[i].argv[0]); else { errno=e; perror(cmds[i].argv[0]); } _exit(e==ENOENT?127:126); }
        }
        if (pgid == 0) pgid = pid;
        (void)setpgid(pid, pgid);
        pids[np++] = pid;
        if (prev_read >= 0) close(prev_read);
        if (hpipe[0] >= 0) close(hpipe[0]);
        if (next_pipe[1] >= 0) close(next_pipe[1]);
        prev_read = next_pipe[0];
    }
    if (prev_read >= 0) close(prev_read);
    close(outpipe[1]);
    *fd_out = outpipe[0];
    if (pids_out) for (int i = 0; i < np; i++) pids_out[i] = pids[i];
    if (pgid_out) *pgid_out = pgid;
    return np;
}

static int launch_pipeline(Tab *t, Command *cmds, int nc) {
    int np = launch_pipeline_fds(t, cmds, nc, &t->out_fd, t->pids, &t->pgid);
    if (np < 0) return -1;
    t->pid_count = np;
    t->running = 1;
    return 0;
}

/* Command lists: &&, ||, ; and & join single commands. Operators inside
   quotes, $(...) and backticks are data, not separators; an & that is
   part of a redirection (2>&1, |&, &>) is not a separator either. */

int split_list(const char *line, ListSeg *segs, int maxsegs) {
    int nsegs = 0;
    const char *start = line;
    char q = 0; int esc = 0, depth = 0, bt = 0, last_sig = 0;
    for (const char *p = line; ; p++) {
        char c = *p;
        if (!c || ((c == ';' || c == '\n' || c == '&' || (c == '|' && p[1] == '|')) && !q && !depth && !bt && !esc)) {
            int op = LIST_END;
            if (c == ';' || c == '\n') op = LIST_SEQ;
            else if (c == '|') op = LIST_OR;
            else if (c == '&') {
                if (p[1] == '&') { op = LIST_AND; }
                else if (p[1] == '>' || last_sig == '>' || last_sig == '<' || last_sig == '|') {
                    /* &> redirect, 2>&1 dup, or |& pipe: not a separator. */
                    last_sig = '&';
                    continue;
                } else op = LIST_BG;
            }
            /* Emit the segment before this separator, trimmed. */
            const char *a = start, *b = p;
            while (a < b && (*a == ' ' || *a == '\t')) a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
            if (b > a && nsegs < maxsegs) {
                segs[nsegs].ptr = a; segs[nsegs].len = (size_t)(b - a); segs[nsegs].op = op;
                nsegs++;
            } else if (b > a && nsegs > 0) {
                /* Too many segments: fold the rest into the last one. */
                segs[nsegs-1].len = (size_t)(b - segs[nsegs-1].ptr);
                segs[nsegs-1].op = LIST_END;
            }
            if (!c) break;
            if (op == LIST_AND || op == LIST_OR) p++;
            start = p + 1;
            last_sig = 0;
            continue;
        }
        if (!c) break;
        if (esc) { esc = 0; last_sig = (unsigned char)c; continue; }
        if (c == '\\' && q != '\'') { esc = 1; last_sig = '\\'; continue; }
        if (c == '\'' || c == '"') { if (!q) q = c; else if (q == c) q = 0; last_sig = (unsigned char)c; continue; }
        if (q) { last_sig = (unsigned char)c; continue; }
        if (c == '$' && p[1] == '(') { depth++; p++; last_sig = '('; continue; }
        if (c == ')' && depth > 0) { depth--; last_sig = ')'; continue; }
        if (c == '`') { bt = !bt; last_sig = '`'; continue; }
        if (c != ' ' && c != '\t') last_sig = (unsigned char)c;
    }
    return nsegs;
}

/* Leading NAME=value assignments on a command (VAR=val cmd): set them in
   the environment for the command. Pipelines restore the previous values
   after the fork (the child already inherited them); built-ins keep them,
   matching the documented process-wide variables. */
typedef struct { char name[64]; char *old; int had; } PrefixSave;

static int is_assign_word(const char *w) {
    if (!w[0] || !(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0;
    for (const char *p = w + 1; *p && *p != '='; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return strchr(w, '=') != NULL;
}

static int strip_assignments(char **tokens, int ntok, int *qflags, PrefixSave *saves, int *nsaves) {
    int n = 0;
    while (n < ntok && is_assign_word(tokens[n]) && !(qflags && (qflags[n] & 2))) {
        if (n >= 8) break;
        char *eq = strchr(tokens[n], '=');
        *eq = '\0';
        snprintf(saves[n].name, sizeof(saves[n].name), "%s", tokens[n]);
        const char *old = getenv(tokens[n]);
        saves[n].had = old != NULL;
        saves[n].old = old ? strdup(old) : NULL;
        setenv(tokens[n], eq + 1, 1);
        n++;
    }
    *nsaves = n;
    if (n > 0) {
        for (int i = 0; i < n; i++) free(tokens[i]);
        memmove(tokens, tokens + n, (size_t)(ntok - n) * sizeof(char *));
        ntok -= n;
        tokens[ntok] = NULL;
    }
    return ntok;
}

static void restore_assignments(PrefixSave *saves, int n) {
    for (int i = 0; i < n; i++) {
        if (saves[i].had) setenv(saves[i].name, saves[i].old ? saves[i].old : "", 1);
        else unsetenv(saves[i].name);
        free(saves[i].old);
    }
}

static void discard_saves(PrefixSave *saves, int n) {
    for (int i = 0; i < n; i++) free(saves[i].old);
}

int expand_globs(Tab *t, char **tokens, int *ntok, int *qflags);

/* Run one command line through Terminator's own parser/pipeline machinery
   and append its combined stdout/stderr to out. Used for $(...), backticks
   and multiWatch, so no /bin/sh process is ever involved. */
static int run_capture_one(Tab *t, const char *seg, char *out, size_t outcap, size_t *used_io, int *status_out) {
    char *tokens[MAX_TOKENS]; int ntok = 0; int qflags[MAX_TOKENS];
    if (tokenize(seg, tokens, &ntok, qflags) < 0) return -1;
    if (ntok == 0) { free_tokens(tokens, ntok); return 0; }
    PrefixSave saves[8]; int nsaves = 0;
    ntok = strip_assignments(tokens, ntok, qflags, saves, &nsaves);
    if (ntok == 0) { discard_saves(saves, nsaves); free_tokens(tokens, ntok); return 0; }
    if (expand_globs(t, tokens, &ntok, qflags) < 0) { discard_saves(saves, nsaves); free_tokens(tokens, ntok); return -1; }
    Command cmds[MAX_CMDS]; int nc = 0;
    if (parse_commands(tokens, ntok, cmds, &nc) < 0) { discard_saves(saves, nsaves); free_tokens(tokens, ntok); return -1; }
    int fd = -1; pid_t pids[MAX_CMDS]; pid_t pgid = 0;
    int np = launch_pipeline_fds(t, cmds, nc, &fd, pids, &pgid);
    restore_assignments(saves, nsaves);
    free_tokens(tokens, ntok);
    if (np < 0) return -1;
    size_t used = used_io ? *used_io : 0;
    int open_fd = 1, alive = np, status = 0;
    while (open_fd || alive > 0) {
        if (open_fd) {
            struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN | POLLHUP; pfd.revents = 0;
            int pr = poll(&pfd, 1, 50);
            if (pr > 0) {
                char buf[1024]; ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    if (outcap && used < outcap - 1) {
                        size_t take = (size_t)n;
                        if (take > outcap - 1 - used) take = outcap - 1 - used;
                        memcpy(out + used, buf, take); used += take; out[used] = '\0';
                    }
                } else if (n == 0) { close(fd); open_fd = 0; }
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { close(fd); open_fd = 0; }
            }
        } else sleep_ms(20);
        for (int i = 0; i < np; i++) if (pids[i] > 0) {
            int st; pid_t r = waitpid(pids[i], &st, WNOHANG);
            if (r == pids[i]) { pids[i] = 0; alive--; if (i == np - 1) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); }
            else if (r < 0 && errno == ECHILD) { pids[i] = 0; alive--; }
        }
    }
    if (used_io) *used_io = used;
    if (status_out) *status_out = status;
    return 0;
}

static int run_capture_sync(Tab *t, const char *cmdline, char *out, size_t outcap, int *status_out) {
    if (outcap) out[0] = '\0';
    if (status_out) *status_out = 0;
    char content[INPUT_CAP]; snprintf(content, sizeof(content), "%s", cmdline);
    strip_comment(content);
    ListSeg segs[32]; int nsegs = split_list(content, segs, 32);
    size_t used = 0; int status = 0, prev_op = LIST_END, prev_status = 0;
    for (int si = 0; si < nsegs; si++) {
        if (prev_op == LIST_AND && prev_status != 0) { prev_op = segs[si].op; continue; }
        if (prev_op == LIST_OR && prev_status == 0) { prev_op = segs[si].op; continue; }
        char seg[INPUT_CAP]; size_t sl = segs[si].len < sizeof(seg) - 1 ? segs[si].len : sizeof(seg) - 1;
        memcpy(seg, segs[si].ptr, sl); seg[sl] = '\0';
        int st = 0;
        if (run_capture_one(t, seg, out, outcap, &used, &st) < 0) return -1;
        prev_status = st; status = st; prev_op = segs[si].op;
        if (t->exit_requested) break;
    }
    if (status_out) *status_out = status;
    return 0;
}

/* Heredoc scanning: find a `<<DELIM` operator on one line, outside quotes
   and outside $(...) / backticks, and read its delimiter word.
   Returns 1 when found (delim filled, *quoted set), 0 when no heredoc
   operator is present, -1 when `<<` is present but has no usable
   delimiter word after it. `<<-` is not special: the '-' becomes part
   of the delimiter word, exactly as written. */
int heredoc_find(const char *line, char *delim, size_t cap, int *quoted) {
    if (quoted) *quoted = 0;
    char q = 0; int esc = 0, depth = 0, bt = 0;
    for (size_t i = 0; line[i] && line[i] != '\n'; i++) {
        char c = line[i];
        if (esc) { esc = 0; continue; }
        if (c == '\\' && q != '\'') { esc = 1; continue; }
        if (c == '\'' || c == '"') { if (!q) q = c; else if (q == c) q = 0; continue; }
        if (q) continue; /* `<<` inside any quotes is literal text, not an operator */
        if (c == '$' && line[i + 1] == '(') { depth++; i++; continue; }
        if (c == ')' && depth > 0) { depth--; continue; }
        if (c == '`') { bt = !bt; continue; }
        if (depth > 0 || bt) continue;
        if (c == '<' && line[i + 1] == '<') {
            size_t j = i + 2;
            while (line[j] == ' ' || line[j] == '\t') j++;
            int qq = 0; char qc = 0;
            if (line[j] == '\'' || line[j] == '"') { qq = 1; qc = line[j]; j++; }
            size_t k = 0; int overflow = 0;
            if (qq) {
                while (line[j] && line[j] != qc && line[j] != '\n') {
                    if (k + 1 < cap) delim[k++] = line[j]; else overflow = 1;
                    j++;
                }
                if (line[j] != qc || k == 0 || overflow) return -1;
            } else {
                while (line[j] && line[j] != ' ' && line[j] != '\t' && line[j] != '\n' &&
                       line[j] != '<' && line[j] != '>' && line[j] != '|' && line[j] != '&') {
                    if (k + 1 < cap) delim[k++] = line[j]; else overflow = 1;
                    j++;
                }
                if (k == 0 || overflow) return -1;
            }
            delim[k] = '\0';
            if (quoted) *quoted = qq;
            return 1;
        }
    }
    return 0;
}

/* 1 when the first line has a heredoc operator followed by other words
   on the same line (a body pasted on one line, which can never work:
   the body must start on the next line). Redirections, pipes and a
   trailing & after the delimiter are fine. */
static int heredoc_same_line_body(const char *s) {
    char first[INPUT_CAP];
    size_t fl = strcspn(s, "\n");
    if (fl >= sizeof(first)) return 0;
    memcpy(first, s, fl); first[fl] = '\0';
    char *tokens[MAX_TOKENS]; int ntok = 0;
    if (tokenize(first, tokens, &ntok, NULL) < 0) return 0;
    int bad = 0;
    for (int i = 0; i < ntok && !bad; i++) {
        if (!strcmp(tokens[i], "<<")) {
            i++; /* delimiter word */
            if (i >= ntok) break;
            for (i++; i < ntok; i++) {
                if (!strcmp(tokens[i], "|") || !strcmp(tokens[i], "|&")) break;
                {
                    int target = 0, is_input = 0, append = 0;
                    if (redir_op(tokens[i], &target, &is_input, &append) ||
                        !strcmp(tokens[i], "&>") || !strcmp(tokens[i], "&>>")) { i++; continue; }
                }
                if (!strcmp(tokens[i], "&")) continue;
                bad = 1; break;
            }
            break;
        }
    }
    free_tokens(tokens, ntok);
    return bad;
}

/* 1 when text starts a heredoc whose delimiter line has not arrived yet,
   0 when there is no heredoc or its delimiter line is already present. */
int heredoc_open(const char *text, char *delim, size_t cap, int *quoted) {
    if (heredoc_find(text, delim, cap, quoted) <= 0) return 0;
    const char *p = strchr(text, '\n');
    if (!p) return 1;
    size_t dlen = strlen(delim);
    while (p) {
        p++;
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == dlen && strncmp(p, delim, len) == 0) return 0;
        if (!e) break;
        p = e;
    }
    return 1;
}

int line_needs_more(const char *s) {
    char delim[64]; int quoted = 0;
    if (heredoc_find(s, delim, sizeof(delim), &quoted) > 0) {
        /* A body on the same line can never be a heredoc: submit at once
           so the engine reports it instead of waiting for a delimiter
           line that will never come. */
        if (heredoc_same_line_body(s)) return 0;
        return heredoc_open(s, delim, sizeof(delim), &quoted);
    }
    char q = 0; int esc = 0; int depth = 0; int bt = 0;
    for (size_t i = 0; s[i]; i++) {
        if (esc) { esc = 0; continue; }
        if (s[i] == '\\' && q != '\'') { esc = 1; continue; }
        if ((s[i] == '\'' || s[i] == '"')) { if (!q) q = s[i]; else if (q == s[i]) q = 0; continue; }
        if (q == '\'') continue;
        if (s[i] == '$' && s[i + 1] == '(') { depth++; i++; continue; }
        if (s[i] == ')' && depth > 0) { depth--; continue; }
        if (s[i] == '`') bt = !bt;
    }
    size_t n = strlen(s); int trailing_slash = 0;
    if (n > 0 && s[n - 1] == '\\') { size_t k = n; while (k > 0 && s[k - 1] == '\\') k--; trailing_slash = ((n - k) % 2) == 1; }
    return q != 0 || trailing_slash || depth > 0 || bt;
}

/* Command substitution: evaluate $(...) and `...` and replace them with
   captured output. Substitutions are executed by Terminator's own parser
   and pipeline launcher (run_capture_sync), never by an external shell. */

static int expand_substitutions(Tab *t, char *line, size_t cap) {
    for (int iter = 0; iter < 32; iter++) {
        /* Locate an innermost $(...) or a `...` pair, skipping single-quoted
           regions (bash: no substitution inside single quotes). */
        int sq = 0, dq = 0, esc = 0, is_backtick = 0;
        char *open = NULL, *close = NULL, *last_open = NULL;
        for (char *p = line; *p; p++) {
            if (esc) { esc = 0; continue; }
            if (*p == '\\' && !sq) { esc = 1; continue; }
            if (*p == '\'' && !dq) { sq = !sq; continue; }
            if (*p == '"' && !sq) { dq = !dq; continue; }
            if (sq) continue;
            if (*p == '$' && p[1] == '(') { last_open = p; p++; continue; }
            if (*p == ')' && last_open) { open = last_open; close = p; is_backtick = 0; break; }
            if (*p == '`') {
                char *q = p + 1, *cq = NULL;
                for (; *q; q++) { if (*q == '\\' && q[1] == '`') { q++; continue; } if (*q == '`') { cq = q; break; } }
                if (cq) { open = p; close = cq; is_backtick = 1; }
                break;
            }
        }
        if (!open) return 0;
        size_t content_off = is_backtick ? 1 : 2, content_len = (size_t)(close - (open + content_off));
        char *content = strndup(open + content_off, content_len);
        if (!content) return -1;
        /* The content may itself contain the other substitution form;
           expand it recursively before running it (innermost first). */
        if (expand_substitutions(t, content, INPUT_CAP) < 0) { free(content); return -1; }
        char output[INPUT_CAP];
        int rc = run_capture_sync(t, content, output, sizeof(output), NULL);
        free(content);
        if (rc < 0) return -1;
        size_t used = strlen(output);
        while (used > 0 && output[used - 1] == '\n') output[--used] = '\0';
        size_t prefix = (size_t)(open - line), suffix = strlen(close + 1);
        size_t total = prefix + used + suffix;
        if (total + 1 > cap) return -1;
        memmove(open + used, close + 1, suffix + 1);
        memcpy(open, output, used);
        line[total] = '\0';
    }
    return -1;
}

/* $(( ... )) integer arithmetic: + - * / % ( ) unary minus, variable
   names read from the environment, 0x hex. Runs before command
   substitution, whose $( prefix would otherwise swallow $((. */
static long arith_expr(const char **pp, int *err);
static void arith_skip(const char **pp) { while (**pp == ' ' || **pp == '\t') (*pp)++; }
static long arith_number(const char **pp, int *err) {
    arith_skip(pp);
    const char *p = *pp;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        char *end = NULL; long v = strtol(p + 2, &end, 16);
        if (end == p + 2) { *err = 1; return 0; }
        *pp = end; return v;
    }
    if (!isdigit((unsigned char)*p)) { *err = 1; return 0; }
    char *end = NULL; long v = strtol(p, &end, 10);
    *pp = end; return v;
}
static long arith_factor(const char **pp, int *err) {
    arith_skip(pp);
    const char *p = *pp;
    if (*p == '(') { (*pp)++; long v = arith_expr(pp, err); arith_skip(pp); if (**pp == ')') (*pp)++; else *err = 1; return v; }
    if (*p == '-') { (*pp)++; return -arith_factor(pp, err); }
    if (*p == '+') { (*pp)++; return arith_factor(pp, err); }
    if (*p == '$') (*pp)++;
    if (isalpha((unsigned char)**pp) || **pp == '_') {
        char name[128]; size_t n = 0;
        while ((isalnum((unsigned char)**pp) || **pp == '_') && n + 1 < sizeof(name)) name[n++] = *(*pp)++;
        name[n] = 0;
        const char *val = getenv(name);
        return val ? strtol(val, NULL, 0) : 0;
    }
    return arith_number(pp, err);
}
static long arith_term(const char **pp, int *err) {
    long v = arith_factor(pp, err);
    for (;;) {
        arith_skip(pp);
        char op = **pp;
        if (op != '*' && op != '/' && op != '%') break;
        (*pp)++;
        long rhs = arith_factor(pp, err);
        if (*err) return 0;
        if (op == '*') v *= rhs;
        else if (op == '/') { if (rhs == 0) { *err = 1; return 0; } v /= rhs; }
        else { if (rhs == 0) { *err = 1; return 0; } v %= rhs; }
    }
    return v;
}
static long arith_expr(const char **pp, int *err) {
    long v = arith_term(pp, err);
    for (;;) {
        arith_skip(pp);
        char op = **pp;
        if (op != '+' && op != '-') break;
        (*pp)++;
        long rhs = arith_term(pp, err);
        if (*err) return 0;
        v = (op == '+') ? v + rhs : v - rhs;
    }
    return v;
}
int expand_arithmetic(char *s, size_t cap) {
    for (int iter = 0; iter < 32; iter++) {
        int sq = 0, dq = 0, esc = 0; char *open = NULL;
        for (char *p = s; *p; p++) {
            if (esc) { esc = 0; continue; }
            if (*p == '\\' && !sq) { esc = 1; continue; }
            if (*p == '\'' && !dq) { sq = !sq; continue; }
            if (*p == '"' && !sq) { dq = !dq; continue; }
            if (sq) continue;
            if (*p == '$' && p[1] == '(' && p[2] == '(') { open = p; break; }
        }
        if (!open) return 0;
        /* Find the matching close: the content is balanced inside one
           paren level, and the construct ends with a second ')'. */
        int depth = 1; char *inner = NULL;
        for (char *p = open + 3; *p; p++) {
            if (*p == '(') depth++;
            else if (*p == ')') { depth--; if (depth == 0) { inner = p; break; } }
        }
        if (!inner || inner[1] != ')') return 0; /* not an arithmetic expansion; leave it */
        char *close = inner + 1;
        char expr[INPUT_CAP];
        size_t elen = (size_t)(inner - (open + 3));
        if (elen >= sizeof(expr)) return -1;
        memcpy(expr, open + 3, elen); expr[elen] = 0;
        const char *pp = expr; int err = 0;
        long v = arith_expr(&pp, &err);
        arith_skip(&pp);
        if (err || *pp) return -1;
        char num[32]; snprintf(num, sizeof(num), "%ld", v);
        size_t prefix = (size_t)(open - s), suffix = strlen(close + 1), total = prefix + strlen(num) + suffix;
        if (total + 1 > cap) return -1;
        memmove(open + strlen(num), close + 1, suffix + 1);
        memcpy(open, num, strlen(num));
        s[total] = 0;
    }
    return -1;
}

/* Expand unquoted wildcard patterns after tokenization, relative to the
   tab's cwd (the executor never chdirs, so plain glob() would use the
   launch directory). Quoted words and the word after << are left alone.
   Bash never returns . or .. from a glob, so those two are filtered. */
int expand_globs(Tab *t, char **tokens, int *ntok, int *qflags) {
    char *expanded[MAX_TOKENS]; int nout = 0;
    for (int i = 0; i < *ntok; i++) {
        /* The word after `<<` is a heredoc delimiter, never a pattern. */
        if (!strcmp(tokens[i], "<<") && i + 1 < *ntok) {
            if (nout + 2 > MAX_TOKENS - 1) return -1;
            expanded[nout++] = tokens[i++];
            expanded[nout++] = tokens[i];
            continue;
        }
        if (strpbrk(tokens[i], "*?[") && !(qflags && (qflags[i] & 1))) {
            const char *pat = tokens[i];
            int made = (pat[0] != '/');
            char *full = NULL;
            if (made) {
                size_t pl = strlen(t->cwd) + 1 + strlen(pat) + 1;
                full = malloc(pl);
                if (!full) return -1;
                snprintf(full, pl, "%s/%s", t->cwd, pat);
            }
            glob_t g; memset(&g, 0, sizeof(g));
            int gr = glob(made ? full : pat, 0, NULL, &g);
            int added = 0;
            if (gr == 0) {
                size_t cl = strlen(t->cwd);
                for (size_t j = 0; j < g.gl_pathc && nout < MAX_TOKENS - 1; j++) {
                    const char *res = g.gl_pathv[j];
                    if (made && strncmp(res, t->cwd, cl) == 0 && res[cl] == '/') res += cl + 1;
                    if (!strcmp(res, ".") || !strcmp(res, "..")) continue;
                    expanded[nout] = strdup(res);
                    if (!expanded[nout]) { globfree(&g); if (made) free(full); return -1; }
                    nout++; added++;
                }
            }
            globfree(&g);
            if (made) free(full);
            if (added) { free(tokens[i]); continue; }
        }
        if (nout < MAX_TOKENS - 1) expanded[nout++] = tokens[i];
        else return -1;
    }
    for (int i = 0; i < nout; i++) tokens[i] = expanded[i];
    tokens[nout] = NULL; *ntok = nout; return 0;
}


int execute_command_sync(Tab *t, const char *line) {
    t->suppress_history++;
    execute_command(t, line);
    t->suppress_history--;
    wait_for_idle(t);
    return t->last_status;
}


/* Recursion guard for ${VAR-word} word expansion: a word may itself
   contain ${...}, but pathological nesting must not grow the stack. */
static _Thread_local int brace_expand_depth;

int expand_variables(Tab *t, char *s, size_t cap) {
    char out[INPUT_CAP]; size_t oi=0; int single=0, dbl=0, esc=0;
    char argbuf[INPUT_CAP];
    for(size_t i=0;s[i];) {
        if (esc) {
            /* The previous backslash escapes this character: copy it
               literally, with no $ expansion and no quote toggling, so
               "\$" stays a literal $ and \" stays a literal quote. The
               backslash itself was already copied; the tokenizer drops
               it when it strips the quotes. */
            if (oi + 1 >= sizeof(out)) return -1;
            out[oi++] = s[i++]; esc = 0; continue;
        }
        if(s[i]=='\\' && !single){esc=1;if(oi+1>=sizeof(out))return -1;out[oi++]=s[i++];continue;}
        if(s[i]=='\'' && !dbl){single=!single;if(oi+1>=sizeof(out))return -1;out[oi++]=s[i++];continue;}
        if(s[i]=='"' && !single){dbl=!dbl;if(oi+1>=sizeof(out))return -1;out[oi++]=s[i++];continue;}
        if(s[i]=='~' && !single && !dbl &&
           (i==0 || s[i-1]==' ' || s[i-1]=='\t' || s[i-1]=='=' || s[i-1]==':' ||
            s[i-1]=='|' || s[i-1]==';' || s[i-1]=='&')) {
            /* Tilde expansion: ~, ~/..., ~user, ~user/... at a word start. */
            size_t j=i+1; char uname[128]; size_t un=0;
            while(s[j] && s[j]!='/' && s[j]!=' ' && s[j]!='\t' && s[j]!=':' && s[j]!='|' &&
                  s[j]!=';' && s[j]!='&' && s[j]!='"' && s[j]!='\'' && un+1<sizeof(uname)) uname[un++]=s[j++];
            uname[un]=0;
            const char *home = NULL;
            if(un==0) home=getenv("HOME");
            else { struct passwd *pw=getpwnam(uname); home=pw?pw->pw_dir:NULL; }
            if(home && (s[j]=='/' || s[j]=='\0' || s[j]==' ' || s[j]=='\t' || s[j]==':' ||
                        s[j]=='|' || s[j]==';' || s[j]=='&' || s[j]=='"' || s[j]=='\'')) {
                size_t hn=strlen(home);if(oi+hn>=sizeof(out))return -1;memcpy(out+oi,home,hn);oi+=hn;
                i=j;continue;
            }
        }
        if(s[i]=='$' && !single) {
            size_t j=i+1; char name[256]={0}; size_t nn=0;
            char brace_word[INPUT_CAP]; int brace_op = 0, brace_colon = 0;
            brace_word[0] = 0;
            if(s[j]=='$'){ /* $$: this process's pid */
                char pb[32];snprintf(pb,sizeof(pb),"%d",(int)getpid());
                if (oi + strlen(pb) >= sizeof(out)) return -1;
                memcpy(out+oi,pb,strlen(pb));oi+=strlen(pb);
                i=j+1;continue;
            }
            if(s[j]=='@' || s[j]=='*'){ /* $@ / $*: all script arguments, space-joined */
                argbuf[0]=0; size_t al=0;
                const char *ac=getenv("TERMINATOR_ARGC"); int n=ac?atoi(ac):0;
                for(int k=1;k<=n && k<=9;k++){char key[32];snprintf(key,sizeof(key),"TERMINATOR_ARG_%d",k);const char *av=getenv(key);if(!av)av="";if(al) { if(al+1>=sizeof(argbuf))break; argbuf[al++]=' '; } size_t vl=strlen(av);if(al+vl>=sizeof(argbuf))vl=sizeof(argbuf)-1-al;memcpy(argbuf+al,av,vl);al+=vl;}
                argbuf[al]=0;
                if (oi + al >= sizeof(out)) return -1;
                memcpy(out+oi,argbuf,al);oi+=al;
                i=j+1;continue;
            }
            if(s[j]=='{' ){
                /* Braced form: ${VAR}, ${VAR-word}, ${VAR:-word},
                   ${VAR+word}, ${VAR:+word}, ${VAR=word}, ${VAR:=word}.
                   The content scan counts nested ${...} so a default
                   word may itself hold a braced expansion. */
                j++;
                size_t cstart = j; int depth = 1;
                while (s[j] && depth > 0) {
                    if (s[j]=='$' && s[j+1]=='{') { depth++; j += 2; continue; }
                    if (s[j]=='}') { depth--; if (depth == 0) break; }
                    j++;
                }
                size_t cend = j;
                if (s[j]=='}') j++;
                size_t p = cstart;
                if (p < cend && ((s[p]>='A'&&s[p]<='Z')||(s[p]>='a'&&s[p]<='z')||s[p]=='_')) {
                    while (p < cend && ((s[p]>='A'&&s[p]<='Z')||(s[p]>='a'&&s[p]<='z')||(s[p]>='0'&&s[p]<='9')||s[p]=='_')) p++;
                } else if (p < cend && (s[p]=='?'||s[p]=='#'||s[p]=='@'||s[p]=='*'||s[p]=='$'||(s[p]>='0'&&s[p]<='9'))) {
                    if (s[p]>='0'&&s[p]<='9') { while (p < cend && s[p]>='0'&&s[p]<='9') p++; }
                    else p++;
                }
                for (size_t k = cstart; k < p && nn+1<sizeof(name); k++) name[nn++] = s[k];
                size_t q = p;
                if (q < cend && s[q]==':' && q+1 < cend && (s[q+1]=='-'||s[q+1]=='+'||s[q+1]=='=')) { brace_op = s[q+1]; brace_colon = 1; q += 2; }
                else if (q < cend && (s[q]=='-'||s[q]=='+'||s[q]=='=')) { brace_op = s[q]; q++; }
                if (brace_op) {
                    size_t wn = 0;
                    for (size_t k = q; k < cend && wn+1<sizeof(brace_word); k++) brace_word[wn++] = s[k];
                    brace_word[wn] = 0;
                }
            }
            else if(s[j]=='#'){name[nn++]='#';j++;}
            else if(s[j]=='?' ){name[nn++]='?';j++;}
            else if(s[j]>='0'&&s[j]<='9'){name[nn++]=s[j++];}
            else if((s[j]>='A'&&s[j]<='Z')||(s[j]>='a'&&s[j]<='z')||s[j]=='_'){while(((s[j]>='A'&&s[j]<='Z')||(s[j]>='a'&&s[j]<='z')||(s[j]>='0'&&s[j]<='9')||s[j]=='_')&&nn+1<sizeof(name))name[nn++]=s[j++];}
            else {if(oi+1>=sizeof(out))return -1;out[oi++]=s[i++];continue;}
            name[nn]=0; const char *val=getenv(name); char status[32];
            if(!strcmp(name,"?")){snprintf(status,sizeof(status),"%d",t->last_status);val=status;}
            if(!strcmp(name,"#")){val=getenv("TERMINATOR_ARGC");if(!val)val="0";}
            if(!strcmp(name,"0")){val=getenv("TERMINATOR_ARG_0");if(!val)val="terminator";}
            if(name[0]>='1'&&name[0]<='9'){char key[32];snprintf(key,sizeof(key),"TERMINATOR_ARG_%.1s",name);val=getenv(key);}
            if (brace_op) {
                /* ${VAR-word} etc: expand the word (it may hold its own
                   $ expansions), then pick word or value by the
                   operator: '-' when unset, ':-' when unset or empty,
                   '+' when set, ':+' when set and non-empty, '='
                   like '-' but also assigns the word to the variable. */
                char wbuf[INPUT_CAP];
                snprintf(wbuf, sizeof(wbuf), "%s", brace_word);
                if (brace_expand_depth < 16) {
                    brace_expand_depth++;
                    int rc = expand_variables(t, wbuf, sizeof(wbuf));
                    brace_expand_depth--;
                    if (rc < 0) return -1;
                }
                int is_set = (val != NULL);
                int is_null = !is_set || val[0] == '\0';
                int use_word = 0;
                if (brace_op=='-') use_word = brace_colon ? is_null : !is_set;
                else if (brace_op=='+') use_word = brace_colon ? !is_null : is_set;
                else if (brace_op=='=') {
                    use_word = brace_colon ? is_null : !is_set;
                    if (use_word && ((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')||name[0]=='_')) setenv(name, wbuf, 1);
                }
                const char *pick = use_word ? wbuf : val;
                if (pick) { size_t vn=strlen(pick);if(oi+vn>=sizeof(out))return -1;memcpy(out+oi,pick,vn);oi+=vn; }
            } else if(val){size_t vn=strlen(val);if(oi+vn>=sizeof(out))return -1;memcpy(out+oi,val,vn);oi+=vn;}
            i=j;continue;
        }
        if (oi+1>=sizeof(out)) return -1;
        out[oi++]=s[i++];
    }
    out[oi]=0;if(oi>=cap)return -1;memcpy(s,out,oi+1);return 0;
}

/* A line of only redirections (`> file`, `< missing`) creates or checks
   the files and runs no command, as in bash. Returns 1 when handled. */
static int try_redirect_only(Tab *t, char **tokens, int ntok, int has_hd) {
    int end = ntok;
    if (end > 0 && !strcmp(tokens[end - 1], "&")) end--;
    if (end == 0) return 0;
    for (int i = 0; i < end; i++) {
        const char *op = tokens[i];
        int is_heredoc = !strcmp(op, "<<");
        int is_both = !strcmp(op, "&>") || !strcmp(op, "&>>");
        int target = 0, is_input = 0, append = 0;
        if (!is_heredoc && !is_both && !redir_op(op, &target, &is_input, &append)) return 0;
        if (i + 1 >= end) return 0; /* dangling: let the parser report it */
        if (is_heredoc) { if (!has_hd) return 0; i++; continue; } /* body discarded */
        if (!strcmp(tokens[i + 1], "&")) { /* N>&M dup form: nothing to create */
            if (i + 2 >= end) return 0;
            i += 2; continue;
        }
        const char *file = tokens[i + 1];
        if (is_both && ((file[0] >= '0' && file[0] <= '2' && file[1] == 0) || !strcmp(file, "-"))) { i++; continue; }
        char path[PATH_MAX];
        if (file[0] == '/') snprintf(path, sizeof(path), "%s", file);
        else join_path(path, sizeof(path), t->cwd, file);
        int fd;
        if (is_input) fd = open(path, O_RDONLY);
        else if (is_both) fd = open(path, O_WRONLY | O_CREAT | (!strcmp(op, "&>>") ? O_APPEND : O_TRUNC), 0644);
        else fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { print_error(t, file); t->last_status = 1; return 1; }
        close(fd);
        i++;
    }
    t->last_status = 0;
    return 1;
}

/* Execute one single command (one list segment): expand it, run a
   built-in in-process, or launch it as a pipeline. heredoc_body is
   non-NULL only for the segment carrying the << operator. Saves records
   leading NAME=value assignments; the caller discards them (built-ins
   keep the assignments; pipelines restore them after the fork). */
static void execute_single_inner(Tab *t, const char *segtext, const char *heredoc_body,
                                 size_t heredoc_len, int heredoc_quoted, int force_bg,
                                 const char *cmd_text, PrefixSave *saves, int *nsaves_out) {
    (void)heredoc_quoted;
    char work[INPUT_CAP];
    snprintf(work, sizeof(work), "%s", segtext);
    int has_hd = (heredoc_body != NULL);
    if (expand_variables(t, work, sizeof(work)) < 0) {
        append_text(t, "Variable expansion failed or command is too long.\n");
        t->last_status = 1; return;
    }
    if (expand_arithmetic(work, sizeof(work)) < 0) {
        append_text(t, "Arithmetic expansion failed.\n");
        t->last_status = 1; return;
    }
    if (expand_substitutions(t, work, sizeof(work)) < 0) {
        append_text(t, "Command substitution failed or is too long.\n");
        t->last_status = 1; return;
    }

    /* multiWatch in the spec's bracket form is parsed from the raw line,
       because tokenising would mangle the brackets, commas and quotes. */
    {
        char mwcmds[MAX_MULTIWATCH_COMMANDS][INPUT_CAP];
        int mwn = parse_multiwatch_brackets(work, mwcmds, MAX_MULTIWATCH_COMMANDS);
        if (mwn >= 0) {
            if (mwn == 0) { append_text(t, "Usage: multiWatch [\"command 1\", \"command 2\", ...]\n"); t->last_status = 2; return; }
            run_multiwatch_list(t, mwcmds, mwn);
            return;
        }
    }
    char *tokens[MAX_TOKENS]; int ntok = 0; int qflags[MAX_TOKENS];
    int tr = tokenize(work, tokens, &ntok, qflags);
    if (tr == -2) { append_text(t, "Syntax error: unmatched quote.\n"); t->last_status = 2; return; }
    if (tr < 0) { append_text(t, "Could not parse command.\n"); t->last_status = 2; return; }
    ntok = strip_assignments(tokens, ntok, qflags, saves, nsaves_out);
    if (!ntok) { free_tokens(tokens, ntok); t->last_status = 0; return; }
    if (expand_globs(t, tokens, &ntok, qflags) < 0) { free_tokens(tokens, ntok); append_text(t, "Too many expanded arguments.\n"); t->last_status = 2; return; }
    if (!ntok) { free_tokens(tokens, ntok); return; }
    if (try_redirect_only(t, tokens, ntok, has_hd)) { free_tokens(tokens, ntok); return; }

    /* Output redirection for built-ins: `history > f`, `jobs > f`, ...
       (bash redirects built-in output too; external commands get their
       redirection from the pipeline machinery instead.) */
    {
        static const char *builtins[] = { "cd", "exit", "clear", "export", "unset", "history",
                                          "multiWatch", "jobs", "bg", "fg", "source", "bash", "pwd", NULL };
        int is_builtin = 0;
        for (int i = 0; builtins[i]; i++) if (!strcmp(tokens[0], builtins[i])) { is_builtin = 1; break; }
        if (tokens[0][0] == '.' && tokens[0][1] == '/') is_builtin = 1;
        int has_pipe = 0;
        for (int i = 0; i < ntok; i++) if (!strcmp(tokens[i], "|") || !strcmp(tokens[i], "|&")) has_pipe = 1;
        if (is_builtin && !has_pipe) {
            int i = 0;
            while (i + 1 < ntok) {
                const char *op = tokens[i];
                int is_out = !strcmp(op, ">") || !strcmp(op, ">>") || !strcmp(op, "&>") || !strcmp(op, "&>>");
                int is_err = !strcmp(op, "2>") || !strcmp(op, "2>>");
                if (!is_out && !is_err) { i++; continue; }
                /* &>2 / &>- duplicates stdout instead of naming a file. */
                if ((is_out && (!strcmp(op, "&>") || !strcmp(op, "&>>"))) &&
                    ((tokens[i + 1][0] >= '0' && tokens[i + 1][0] <= '2' && tokens[i + 1][1] == 0) ||
                     !strcmp(tokens[i + 1], "-"))) { i++; continue; }
                /* Built-ins write through one stream: the first redirect
                   captures it; a second redirect still creates its file. */
                char path[PATH_MAX];
                if (tokens[i + 1][0] == '/') snprintf(path, sizeof(path), "%s", tokens[i + 1]);
                else join_path(path, sizeof(path), t->cwd, tokens[i + 1]);
                int fd = open(path, O_WRONLY | O_CREAT | (!strcmp(op, ">>") || !strcmp(op, "2>>") || !strcmp(op, "&>>") ? O_APPEND : O_TRUNC), 0644);
                if (fd < 0) {
                    if (t->capture_fd < 0) { print_error(t, tokens[i + 1]); t->last_status = 1; free_tokens(tokens, ntok); return; }
                } else if (t->capture_fd < 0) {
                    t->capture_fd = fd;
                } else close(fd);
                free(tokens[i]); free(tokens[i + 1]);
                memmove(tokens + i, tokens + i + 2, (size_t)(ntok - i - 2) * sizeof(char *));
                ntok -= 2; tokens[ntok] = NULL;
            }
        }
    }

    /* Built-ins run in the tab's executor so each tab keeps its own
       cwd, variables and job table. */
    if (!strcmp(tokens[0], "exit")) {
        int code = ntok > 1 ? atoi(tokens[1]) : t->last_status;
        free_tokens(tokens, ntok);
        t->last_status = code;
        t->exit_requested = 1;
        if (batch_mode || proc_child) running = 0;
        return;
    }
    if (!strcmp(tokens[0], "cd")) {
        if (ntok > 2) {
            append_text(t, "cd: too many arguments\n");
            t->last_status = 1; free_tokens(tokens, ntok); return;
        }
        int print_dir = 0;
        const char *dest = ntok > 1 ? tokens[1] : getenv("HOME");
        char dashbuf[PATH_MAX];
        if (dest && !strcmp(dest, "-")) {
            if (!t->prev_cwd[0]) {
                append_text(t, "cd: OLDPWD not set\n");
                t->last_status = 1; free_tokens(tokens, ntok); return;
            }
            snprintf(dashbuf, sizeof(dashbuf), "%s", t->prev_cwd);
            dest = dashbuf; print_dir = 1;
        }
        char resolved[PATH_MAX];
        char canonical[PATH_MAX];
        if (!dest) dest = "/";
        int path_len;
        if (dest[0] == '/')
            path_len = snprintf(resolved, sizeof(resolved), "%s", dest);
        else if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
            const char *home = getenv("HOME");
            path_len = snprintf(resolved, sizeof(resolved), "%s%s", home ? home : "/", dest + 1);
        }
        else
            path_len = snprintf(resolved, sizeof(resolved), "%s/%s", t->cwd, dest);
        if (path_len < 0 || (size_t)path_len >= sizeof(resolved)) {
            append_text(t, "cd: path is too long\n");
            t->last_status = 1;
        } else if (!realpath(resolved, canonical)) {
            char cb[PATH_MAX + 64];
            snprintf(cb, sizeof(cb), "cd: %s: %s\n", dest, strerror(errno));
            append_text(t, cb);
            t->last_status = 1;
        } else {
            struct stat st;
            if (stat(canonical, &st) < 0) {
                char cb[PATH_MAX + 64];
                snprintf(cb, sizeof(cb), "cd: %s: %s\n", dest, strerror(errno));
                append_text(t, cb);
                t->last_status = 1;
            } else if (!S_ISDIR(st.st_mode)) {
                append_text(t, "cd: not a directory\n");
                t->last_status = 1;
            } else {
                snprintf(t->prev_cwd, sizeof(t->prev_cwd), "%s", t->cwd);
                snprintf(t->cwd, sizeof(t->cwd), "%s", canonical);
                t->last_status = 0;
                if (print_dir) { append_text(t, t->cwd); append_text(t, "\n"); }
            }
        }
        free_tokens(tokens, ntok); return;
    }
    if (!strcmp(tokens[0], "clear") && ntok == 1) {
        clear_tab_screen(t);
        free_tokens(tokens, ntok);
        return;
    }
    if (!strcmp(tokens[0], "export")) {
        if (ntok == 1) {
            extern char **environ;
            for (char **e = environ; *e; e++) {
                char *eq = strchr(*e, '=');
                char eb[INPUT_CAP];
                if (eq) snprintf(eb, sizeof(eb), "declare -x %.*s=\"%s\"\n", (int)(eq - *e), *e, eq + 1);
                else snprintf(eb, sizeof(eb), "declare -x %s\n", *e);
                append_text(t, eb);
            }
            t->last_status = 0;
            free_tokens(tokens, ntok);
            return;
        }
        int ok = 1;
        for (int i = 1; i < ntok; i++) {
            char *eq = strchr(tokens[i], '=');
            if (eq) {
                *eq = '\0';
                if (tokens[i][0] == '\0' || setenv(tokens[i], eq + 1, 1) < 0) {
                    append_text(t, "export: invalid variable assignment\n");
                    ok = 0;
                }
            } else if (tokens[i][0] && !getenv(tokens[i])) {
                if (setenv(tokens[i], "", 1) < 0) {
                    print_error(t, "export");
                    ok = 0;
                }
            }
        }
        t->last_status = ok ? 0 : 1;
        free_tokens(tokens, ntok);
        return;
    }
    if (!strcmp(tokens[0], "unset")) {
        for (int i = 1; i < ntok; i++) { if (unsetenv(tokens[i]) < 0) print_error(t, "unset"); }
        t->last_status = 0;
        free_tokens(tokens, ntok);
        return;
    }
    if (!strcmp(tokens[0], "history")) {
        int count = hist_count();
        int begin = count > 1000 ? count - 1000 : 0;
        for (int i = begin; i < count; i++) {
            char entry[INPUT_CAP];
            if (hist_get(i, entry, sizeof(entry))) {
                char hb[INPUT_CAP + 32];
                snprintf(hb, sizeof(hb), "%5d  %s\n", i + 1, entry);
                append_text(t, hb);
            }
        }
        t->last_status = 0;
        free_tokens(tokens, ntok); return;
    }
    if (!strcmp(tokens[0], "multiWatch")) { run_multiwatch(t, tokens, ntok); free_tokens(tokens, ntok); return; }
    if (!strcmp(tokens[0], "jobs")) { show_jobs(t); t->last_status = 0; free_tokens(tokens, ntok); return; }
    if (!strcmp(tokens[0], "bg") || !strcmp(tokens[0], "fg")) {
        int id = job_id_arg(tokens, ntok);
        if (id < 0 && ntok < 2) {
            /* No argument: the most recent job, as in bash. */
            id = -1;
            for (int i = 0; i < MAX_JOBS; i++)
                if (t->jobs[i].active && t->jobs[i].id > id) id = t->jobs[i].id;
        }
        pthread_mutex_lock(&state_lock);
        Job *j = find_job(t, id);
        if (!j) {
            pthread_mutex_unlock(&state_lock);
            append_text(t, "job: specify a valid job number, e.g. %1\n");
            t->last_status = 1;
        } else if (!strcmp(tokens[0], "fg") && t->running) {
            pthread_mutex_unlock(&state_lock);
            append_text(t, "fg: this tab already has a foreground command.\n");
            t->last_status = 1;
        } else if (kill(-j->pgid, SIGCONT) < 0) {
            pthread_mutex_unlock(&state_lock);
            print_error(t, tokens[0]);
            t->last_status = 1;
        } else {
            for (int k = 0; k < j->npids; k++) j->pid_stopped[k] = 0;
            j->stopped = 0;
            if (!strcmp(tokens[0], "fg")) {
                t->pgid = j->pgid; t->pid_count = j->npids; t->out_fd = j->out_fd;
                for (int k = 0; k < j->npids; k++) t->pids[k] = j->pids[k];
                char resumed_command[INPUT_CAP]; snprintf(resumed_command, sizeof(resumed_command), "%s", j->command);
                memset(j, 0, sizeof(*j)); j->out_fd = -1;
                t->running = 1;
                t->last_status = 0;
                pthread_mutex_unlock(&state_lock);
                append_text(t, resumed_command); append_text(t, "\n");
            } else {
                t->last_status = 0;
                pthread_mutex_unlock(&state_lock);
                append_text(t, "[job continued in background]\n");
            }
        }
        free_tokens(tokens, ntok); return;
    }
    if (!strcmp(tokens[0], "source")) {
        if (ntok < 2) { append_text(t, "Usage: source script.sh\n"); t->last_status = 2; free_tokens(tokens, ntok); return; }
        t->last_status = run_script_file(t, tokens[1], ntok - 2, ntok > 2 ? tokens + 2 : NULL, 0);
        free_tokens(tokens, ntok); return;
    }
    if (tokens[0][0] == '.' && tokens[0][1] == '/') {
        /* ./script.sh [args...] is interpreted by Terminator itself. */
        t->last_status = run_script_file(t, tokens[0], ntok - 1, ntok > 1 ? tokens + 1 : NULL, 0);
        free_tokens(tokens, ntok); return;
    }
    if (!strcmp(tokens[0], "bash")) {
        if (ntok < 2) { append_text(t, "Usage: bash script.sh [args...]\n"); t->last_status = 2; }
        else t->last_status = run_script_file(t, tokens[1], ntok - 2, ntok > 2 ? tokens + 2 : NULL, 0);
        free_tokens(tokens, ntok); return;
    }
    if (!strcmp(tokens[0], "pwd") && ntok == 1) { append_text(t, t->cwd); append_text(t, "\n"); t->last_status = 0; free_tokens(tokens, ntok); return; }
    int background = force_bg;
    if (!background && ntok > 0 && !strcmp(tokens[ntok - 1], "&")) {
        background = 1;
        free(tokens[--ntok]);
        if (ntok == 0) { append_text(t, "Syntax error: missing command before &.\n"); t->last_status = 2; free_tokens(tokens, ntok); return; }
    }
    Command cmds[MAX_CMDS]; int nc = 0;
    int pr = parse_commands(tokens, ntok, cmds, &nc);
    if (pr < 0) {
        if (pr == -2) append_text(t, "Syntax error: heredoc body must be on the following lines, not on the same line as <<DELIM.\n");
        else if (pr == -3) append_text(t, "Syntax error: only one heredoc per command.\n");
        else if (pr == -4) append_text(t, "Syntax error: '<<' needs a delimiter word, e.g. <<EOF.\n");
        else if (pr == -5) append_text(t, "Syntax error: redirection needs a file name.\n");
        else append_text(t, "Syntax error in command, redirection or pipe.\n");
        t->last_status = 2; free_tokens(tokens, ntok); return;
    }
    int hd_count = 0, hd_idx = -1;
    for (int i = 0; i < nc; i++) if (cmds[i].has_heredoc) { hd_count++; hd_idx = i; }
    if (hd_count > 1) {
        append_text(t, "Syntax error: only one heredoc per command line.\n");
        t->last_status = 2; free_tokens(tokens, ntok); return;
    }
    if (has_hd && hd_count == 0) {
        append_text(t, "Syntax error: heredoc was changed by expansion; use a literal delimiter.\n");
        t->last_status = 2; free_tokens(tokens, ntok); return;
    }
    if (!has_hd && hd_count == 1) {
        append_text(t, "Syntax error: heredoc has no body; put the content on the following lines ending with the delimiter.\n");
        t->last_status = 2; free_tokens(tokens, ntok); return;
    }
    if (has_hd) {
        for (int r = 0; r < cmds[hd_idx].nredirs; r++)
            if (cmds[hd_idx].redirs[r].target_fd == 0 && cmds[hd_idx].redirs[r].file) {
                append_text(t, "Syntax error: heredoc cannot be combined with input redirection.\n");
                t->last_status = 2; free_tokens(tokens, ntok); return;
            }
        cmds[hd_idx].heredoc = strdup(heredoc_body);
        if (!cmds[hd_idx].heredoc) { append_text(t, "Out of memory.\n"); t->last_status = 1; free_tokens(tokens, ntok); return; }
        cmds[hd_idx].heredoc_len = heredoc_len;
    }
    snprintf(t->fg_command, sizeof(t->fg_command), "%s", cmd_text);
    if (launch_pipeline(t, cmds, nc) < 0) {
        restore_assignments(saves, *nsaves_out);
        for (int i = 0; i < nc; i++) free(cmds[i].heredoc);
        free_tokens(tokens, ntok); t->last_status = 1; return;
    }
    restore_assignments(saves, *nsaves_out);
    for (int i = 0; i < nc; i++) free(cmds[i].heredoc);
    if (background) {
        pthread_mutex_lock(&state_lock);
        Job *job = add_job(t, cmd_text, 0);
        pthread_mutex_unlock(&state_lock);
        if (job) {
            t->running = 0;
            t->last_status = 0;
            if (!batch_mode) { char jb[64]; snprintf(jb, sizeof(jb), "[%d] started in background\n", job->id); append_text(t, jb); }
        }
    }
    free_tokens(tokens, ntok);
}

static void execute_single(Tab *t, const char *segtext, const char *heredoc_body,
                           size_t heredoc_len, int heredoc_quoted, int force_bg,
                           const char *cmd_text) {
    PrefixSave saves[8]; int nsaves = 0;
    execute_single_inner(t, segtext, heredoc_body, heredoc_len, heredoc_quoted,
                         force_bg, cmd_text, saves, &nsaves);
    discard_saves(saves, nsaves);
}

/* Execute one complete command line (already assembled, with any
   continuation lines joined by '\n'). This is the single shell engine used
   by GUI tabs (worker threads / tab processes), scripts and --batch mode.
   The caller is responsible for prompt echo and for waiting (wait_for_idle)
   when synchronous execution is required. */
void execute_command(Tab *t, const char *line) {
    char work[INPUT_CAP];
    snprintf(work, sizeof(work), "%s", line);
    /* Heredoc: split the assembled text into its first line and the body
       that follows it, ending at a line that is exactly the delimiter.
       The body stays raw here; it is expanded below, and only when the
       delimiter was unquoted. */
    char first_line[INPUT_CAP]; first_line[0] = '\0';
    char heredoc_body[INPUT_CAP]; size_t heredoc_len = 0;
    int has_hd = 0, heredoc_quoted = 0;
    char heredoc_delim[64] = "";
    const char *nl = strchr(work, '\n');
    if (nl) {
        size_t flen = (size_t)(nl - work);
        if (flen >= sizeof(first_line)) flen = sizeof(first_line) - 1;
        memcpy(first_line, work, flen); first_line[flen] = '\0';
        int hf = heredoc_find(first_line, heredoc_delim, sizeof(heredoc_delim), &heredoc_quoted);
        if (hf < 0) {
            append_text(t, "Syntax error: '<<' needs a delimiter word, e.g. <<EOF.\n");
            t->last_status = 2; return;
        }
        if (hf > 0) {
            const char *body_start = nl + 1;
            const char *p = body_start; int found = 0;
            size_t dlen = strlen(heredoc_delim);
            while (1) {
                const char *e = strchr(p, '\n');
                size_t llen = e ? (size_t)(e - p) : strlen(p);
                if (llen == dlen && strncmp(p, heredoc_delim, llen) == 0) {
                    heredoc_len = (size_t)(p - body_start);
                    const char *rest = e ? e + 1 : p + llen;
                    while (*rest == '\n' || *rest == '\r' || *rest == ' ' || *rest == '\t') rest++;
                    if (*rest) {
                        append_text(t, "Syntax error: unexpected input after the heredoc delimiter line.\n");
                        t->last_status = 2; return;
                    }
                    found = 1; break;
                }
                if (!e) break;
                p = e + 1;
            }
            if (!found) {
                char eb[128];
                snprintf(eb, sizeof(eb), "Syntax error: heredoc delimiter '%s' not found.\n", heredoc_delim);
                append_text(t, eb);
                t->last_status = 2; return;
            }
            if (heredoc_len >= sizeof(heredoc_body)) {
                append_text(t, "Syntax error: heredoc body is too long.\n");
                t->last_status = 2; return;
            }
            memcpy(heredoc_body, body_start, heredoc_len);
            heredoc_body[heredoc_len] = '\0';
            has_hd = 1;
            snprintf(work, sizeof(work), "%s", first_line);
        }
    }
    /* History records the command as typed; for a heredoc that is the
       first line only, so body lines never become history entries. The
       same holds for any other multi-line submission (a pasted block,
       a quoted continuation): only its first line is recorded, so the
       one-entry-per-line history file stays well formed. */
    if (!t->suppress_history) {
        if (has_hd) hist_add(first_line);
        else {
            const char *nl = strchr(work, '\n');
            if (nl) {
                char hb[INPUT_CAP]; size_t hn = (size_t)(nl - work);
                if (hn >= sizeof(hb)) hn = sizeof(hb) - 1;
                memcpy(hb, work, hn); hb[hn] = '\0'; hist_add(hb);
            } else hist_add(work);
        }
    }
    t->hist_pos = -1;
    strip_comment(work);
    if (has_hd && !heredoc_quoted) {
        if (expand_variables(t, heredoc_body, sizeof(heredoc_body)) < 0 ||
            expand_arithmetic(heredoc_body, sizeof(heredoc_body)) < 0 ||
            expand_substitutions(t, heredoc_body, sizeof(heredoc_body)) < 0) {
            append_text(t, "Heredoc expansion failed or body is too long.\n");
            t->last_status = 1; return;
        }
        heredoc_len = strlen(heredoc_body);
    }
    /* Command lists: split on &&, ||, ; and &, then run each segment in
       order, honouring short-circuiting. Expansion happens per segment,
       so `X=1 && echo $X` sees the new value, as in bash. */
    ListSeg segs[32]; int nsegs = split_list(work, segs, 32);
    const char *cmd_text = has_hd ? first_line : line;
    int prev_op = LIST_END, prev_status = 0;
    for (int si = 0; si < nsegs; si++) {
        if (prev_op == LIST_AND && prev_status != 0) { prev_op = segs[si].op; continue; }
        if (prev_op == LIST_OR && prev_status == 0) { prev_op = segs[si].op; continue; }
        char seg[INPUT_CAP]; size_t sl = segs[si].len < sizeof(seg) - 1 ? segs[si].len : sizeof(seg) - 1;
        memcpy(seg, segs[si].ptr, sl); seg[sl] = '\0';
        char hd_delim2[64]; int hd_q2 = 0;
        int seg_hd = has_hd && heredoc_find(seg, hd_delim2, sizeof(hd_delim2), &hd_q2) > 0;
        execute_single(t, seg, seg_hd ? heredoc_body : NULL, seg_hd ? heredoc_len : 0,
                       heredoc_quoted, segs[si].op == LIST_BG, cmd_text);
        if (t->exit_requested) break;
        /* A foreground pipeline must finish before the next segment can
           be judged (short-circuiting needs its status). */
        if (t->running) wait_for_idle(t);
        prev_status = t->last_status;
        prev_op = segs[si].op;
    }
}
