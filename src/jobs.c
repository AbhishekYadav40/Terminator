/* Terminator — jobs.c
 * Jobs and foreground waiting (Design section 5): the per-tab job
 *  * table, reaping, Ctrl+C / Ctrl+Z handling, and wait_for_idle.
 */
#include "common.h"

static void reap_children(Tab *t);


/* Discard the typed-but-unexecuted line (and any half-typed
   continuation/heredoc): Ctrl+C abandons the line in a real shell,
   in every mode, whether a command is running or not. */
void discard_input_line(Tab *t) {
    t->input_len = 0; t->cursor_pos = 0; t->input[0] = '\0';
    t->multiline = 0; t->pending_cmd[0] = '\0';
}

void interrupt_current_command(Tab *t) {
    if (t->running) {
        int sent = 0;
        if (t->pgid > 0 && kill(-t->pgid, SIGINT) == 0) sent = 1;
        for (int i = 0; i < t->pid_count; i++) { if (t->pids[i] > 0 && kill(t->pids[i], SIGINT) == 0) sent = 1; }
        if (!sent) append_text(t, "Ctrl+C: no running child process to interrupt.\n");
        discard_input_line(t); /* typed-ahead input is flushed with the interrupt */
    } else {
        int tab_index = (int)(t - tabs);
        if (tab_index >= 0 && tab_index < MAX_TABS && multiwatches[tab_index].active) {
            stop_multiwatch(tab_index, 1);
            discard_input_line(t);
            append_text(t, "^C\n");
            return;
        }
        discard_input_line(t);
        append_text(t, "^C\n");
    }
}

Job *find_job(Tab *t, int id) { for (int i = 0; i < MAX_JOBS; i++) if (t->jobs[i].active && t->jobs[i].id == id) return &t->jobs[i]; return NULL; }

/* Transfer a running tab's process group, children, and output stream to the
   tab's own job table. From this point only refresh_jobs() calls waitpid()
   for these children. */
Job *add_job(Tab *t, const char *command, int stopped) {
    if (t->pgid <= 0) return NULL;
    for (int i = 0; i < MAX_JOBS; i++) if (!t->jobs[i].active) {
        Job *j = &t->jobs[i]; memset(j, 0, sizeof(*j));
        j->id = t->next_job_id++; j->pgid = t->pgid; j->npids = t->pid_count;
        if (j->npids > MAX_CMDS) j->npids = MAX_CMDS;
        for (int k = 0; k < j->npids; k++) j->pids[k] = t->pids[k];
        j->active = 1; j->stopped = stopped; j->out_fd = t->out_fd; j->tab_index = (int)(t - tabs);
        snprintf(j->command, sizeof(j->command), "%s", command ? command : "");
        t->out_fd = -1; t->pgid = 0; t->pid_count = 0;
        memset(t->pids, 0, sizeof(t->pids));
        return j;
    }
    append_text(t, "job: job table is full; command remains attached to this tab.\n");
    return NULL;
}

void refresh_jobs(Tab *t) {
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &t->jobs[i];
        if (!j->active) continue;
        /* Keep background output flowing without blocking the event loop. */
        if (j->out_fd >= 0) {
            char buf[OUTPUT_CHUNK];
            for (;;) {
                ssize_t n = read(j->out_fd, buf, sizeof(buf));
                if (n > 0) append_screen(t, buf, (size_t)n);
                else if (n == 0) { close(j->out_fd); j->out_fd = -1; break; }
                else { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) break; close(j->out_fd); j->out_fd = -1; break; }
            }
        }
        int live = 0, all_stopped = 1;
        for (int k = 0; k < j->npids; k++) {
            pid_t pid = j->pids[k]; if (pid <= 0) continue;
            int st = 0; pid_t r = waitpid(pid, &st, WNOHANG | WUNTRACED | WCONTINUED);
            if (r == pid) {
                if (WIFEXITED(st) || WIFSIGNALED(st)) { j->pids[k] = 0; j->pid_stopped[k] = 0; continue; }
                if (WIFSTOPPED(st)) j->pid_stopped[k] = 1;
                else if (WIFCONTINUED(st)) j->pid_stopped[k] = 0;
            } else if (r < 0 && errno == ECHILD) { j->pids[k] = 0; j->pid_stopped[k] = 0; continue; }
            if (j->pids[k] > 0) { live++; if (!j->pid_stopped[k]) all_stopped = 0; }
        }
        if (live == 0) {
            if (j->out_fd >= 0) { close(j->out_fd); j->out_fd = -1; }
            j->active = 0;
        } else j->stopped = all_stopped;
    }
    pthread_mutex_unlock(&state_lock);
}

void show_jobs(Tab *t) {
    refresh_jobs(t);
    int any = 0;
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < MAX_JOBS; i++) if (t->jobs[i].active) {
        char b[INPUT_CAP + 128];
        snprintf(b, sizeof(b), "[%d] %-8s %s\n", t->jobs[i].id,
                 t->jobs[i].stopped ? "Stopped" : "Running", t->jobs[i].command);
        append_text(t, b); any = 1;
    }
    pthread_mutex_unlock(&state_lock);
    if (!any) append_text(t, "No active jobs.\n");
}
int job_id_arg(char **tok,int ntok) { if(ntok<2)return -1; const char *p=tok[1]; if(*p=='%')p++; char *end=NULL; long n=strtol(p,&end,10); return (!*p||!end||*end||n<1||n>INT_MAX)?-1:(int)n; }


/* Drain a tab's foreground pipeline until it finishes, forwarding output.
   Used wherever execution is synchronous (scripts, --batch), and by the
   per-tab executor loops. multiWatch and background jobs are serviced on
   the same pass so they keep flowing while a foreground command runs. */
void wait_for_idle(Tab *t) {
    int idx = (int)(t - tabs);
    while (t->running) {
        if (batch_mode && batch_sigint) { batch_sigint = 0; interrupt_current_command(t); }
        if (proc_child && child_sigint) { child_sigint = 0; interrupt_current_command(t); }
        if (proc_child && child_sigz) { child_sigz = 0; suspend_foreground(t); }
        if (t->out_fd >= 0) {
            struct pollfd q; q.fd = t->out_fd; q.events = POLLIN | POLLHUP; q.revents = 0;
            int pr = poll(&q, 1, 50);
            if (pr > 0) {
                char ob[OUTPUT_CHUNK]; ssize_t z;
                while ((z = read(t->out_fd, ob, sizeof(ob))) > 0) append_screen(t, ob, (size_t)z);
                if (z == 0) { close(t->out_fd); t->out_fd = -1; }
            }
        } else sleep_ms(20);
        reap_children(t);
        if (idx >= 0 && idx < MAX_TABS) service_multiwatch(idx);
        refresh_jobs(t);
    }
    if (t->out_fd >= 0) {
        char ob[OUTPUT_CHUNK]; ssize_t z;
        while ((z = read(t->out_fd, ob, sizeof(ob))) > 0) append_screen(t, ob, (size_t)z);
        if (z == 0 || (z < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { close(t->out_fd); t->out_fd = -1; }
    }
}


/* Ctrl+Z: stop the tab's foreground pipeline and move it to the tab's
   job table as a stopped job. Runs on the GUI thread in thread mode and
   inside the tab process in process mode. */
void suspend_foreground(Tab *t) {
    if (t->running && t->pgid > 0) {
        if (kill(-t->pgid, SIGTSTP) == 0) {
            pthread_mutex_lock(&state_lock);
            Job *job = add_job(t, t->fg_command[0] ? t->fg_command : "(foreground command)", 1);
            pthread_mutex_unlock(&state_lock);
            if (job) {
                char b[128];
                snprintf(b, sizeof(b), "^Z\n[%d]+ Stopped %.100s\n", job->id, job->command);
                append_text(t, b);
                t->running = 0;
            }
        } else print_error(t, "Ctrl+Z");
    } else append_text(t, "Ctrl+Z: no foreground job.\n");
}


static void reap_children(Tab *t) {
    if (!t->running) return;
    int alive=0;
    for(int i=0;i<t->pid_count;i++) {
        if(t->pids[i]<=0) continue;
        int st; pid_t r=waitpid(t->pids[i],&st,WNOHANG);
        if(r==0) alive++;
        else if(r==t->pids[i]) { t->pids[i]=0; if(i==t->pid_count-1) t->last_status=WIFEXITED(st)?WEXITSTATUS(st):128+(WIFSIGNALED(st)?WTERMSIG(st):0); }
    }
    if (!alive && t->out_fd<0) { t->running=0; t->pid_count=0; t->pgid=0; if (!batch_mode) append_text(t,"\n"); }
}
