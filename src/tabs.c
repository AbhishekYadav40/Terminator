/* Terminator — tabs.c
 * Tab slots (Design section 2): creating, closing and looking up
 *  * the fixed MAX_TABS slots whose identity is stable for the tab's life.
 */
#include "common.h"


/* Zero a tab slot and set the fields every fresh tab starts with. */
void init_tab(Tab *t) {
    memset(t, 0, sizeof(*t));
    t->active = 1; t->out_fd = -1; t->cmd_fd = -1; t->resp_fd = -1; t->capture_fd = -1;
    t->hist_pos = -1; t->next_job_id = 1;
}
int create_tab(void) {
    int idx = -1; for (int i = 0; i < MAX_TABS; i++) if (!tabs[i].active) { idx = i; break; }
    if (idx < 0) return -1;
    Tab *t = &tabs[idx];
    init_tab(t);
    if (!getcwd(t->cwd, sizeof(t->cwd))) strcpy(t->cwd, "/");
    tab_count++; current_tab = idx;
    if (!batch_mode) start_tab_executor(idx);
    return idx;
}

int tab_slot_at(int pos) {
    int c = 0; for (int i = 0; i < MAX_TABS; i++) if (tabs[i].active) { if (c == pos) return i; c++; }
    return -1;
}

void terminate_tab(Tab *t) {
    if (!proc_child && !batch_mode) {
        if (process_mode) {
            if (t->child_pid > 0) {
                kill(t->child_pid, SIGTERM);
                int st; while (waitpid(t->child_pid, &st, 0) < 0 && errno == EINTR) {}
                t->child_pid = 0;
            }
            if (t->cmd_fd >= 0) { close(t->cmd_fd); t->cmd_fd = -1; }
            if (t->resp_fd >= 0) { close(t->resp_fd); t->resp_fd = -1; }
            t->busy = 0;
        } else {
            t->dead = 1;
            sleep_ms(60);
        }
    }
    int tab_index = (int)(t - tabs);
    if (tab_index >= 0 && tab_index < MAX_TABS && multiwatches[tab_index].active) {
        MultiWatch *mw = &multiwatches[tab_index];
        stop_multiwatch(tab_index, 0);
        /* A tab is about to disappear, so finish cleanup here instead of losing
           the child PID/fd table when the tab slot is released. */
        for (int i = 0; i < mw->command_count; i++) {
            if (mw->pgids[i] > 0) (void)kill(-mw->pgids[i], SIGKILL);
            for (int k = 0; k < mw->npids[i]; k++) if (mw->pids[i][k] > 0) {
                (void)kill(mw->pids[i][k], SIGKILL);
                while (waitpid(mw->pids[i][k], NULL, 0) < 0 && errno == EINTR) {}
            }
            if (mw->fds[i] >= 0) close(mw->fds[i]);
        }
        memset(mw, 0, sizeof(*mw));
    }
    for (int ji = 0; ji < MAX_JOBS; ji++) if (t->jobs[ji].active) {
        Job *j = &t->jobs[ji];
        if (j->pgid > 0) (void)kill(-j->pgid, SIGKILL);
        for (int k = 0; k < j->npids; k++) if (j->pids[k] > 0) (void)kill(j->pids[k], SIGKILL);
        if (j->out_fd >= 0) close(j->out_fd);
        j->active = 0;
    }
    if (t->out_fd >= 0) { close(t->out_fd); t->out_fd = -1; }
    if (t->running) {
        for (int i = 0; i < t->pid_count; i++) if (t->pids[i] > 0) kill(t->pids[i], SIGTERM);
        for (int i = 0; i < t->pid_count; i++) if (t->pids[i] > 0) waitpid(t->pids[i], NULL, WNOHANG);
    }
    t->running = 0; t->pid_count = 0;
}

void close_tab(int idx) {
    if (tab_count <= 1 || idx < 0 || idx >= MAX_TABS || !tabs[idx].active) return;
    terminate_tab(&tabs[idx]);
    tabs[idx].active = 0; tab_count--;
    if (current_tab == idx) {
        current_tab = 0;
        for (int k = 0; k < MAX_TABS; k++) if (tabs[k].active) { current_tab = k; break; }
    }
}
