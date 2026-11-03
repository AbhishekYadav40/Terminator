/* Terminator — executor.c
 * Execution modes (Design section 3): the per-tab worker thread,
 *  * the forked tab process, framed packets, and starting/servicing them.
 */
#include "common.h"


/* ---------------- Per-tab executors ----------------
   Thread mode: one worker thread per tab runs the shell engine; only the
   GUI thread touches X11. Process mode: each tab is a forked process
   running the same engine loop, exchanging framed packets with the GUI;
   Ctrl+C/Ctrl+Z arrive as SIGUSR1/SIGUSR2 and become kill() on the
   foreground process group. */
static void on_child_sigint(int sig) { (void)sig; child_sigint = 1; }
static void on_child_sigz(int sig) { (void)sig; child_sigz = 1; }
void install_handler(int sig, void (*fn)(int)) { struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = fn; sigaction(sig, &sa, NULL); }
void close_capture(Tab *t) { if (t->capture_fd >= 0) { close(t->capture_fd); t->capture_fd = -1; } }

static void *tab_worker_main(void *arg) {
    Tab *t = (Tab *)arg;
    int idx = (int)(t - tabs);
    while (!t->dead) {
        int did = 0;
        if (t->has_submit) {
            char line[INPUT_CAP];
            __sync_synchronize();
            snprintf(line, sizeof(line), "%s", t->queue_line);
            t->has_submit = 0;
            execute_command(t, line);
            wait_for_idle(t);
            close_capture(t);
            t->busy = 0;
            did = 1;
        }
        if (idx >= 0 && idx < MAX_TABS && multiwatches[idx].active) { service_multiwatch(idx); did = 1; }
        refresh_jobs(t);
        if (!did) { sleep_ms(25); }
    }
    return NULL;
}

static void tab_child_main(Tab *ct, int idx, int cmd_fd, int resp_fd) {
    (void)idx;
    proc_child = 1;
    child_resp_fd = resp_fd;
    running = 1;
    install_handler(SIGUSR1, on_child_sigint);
    install_handler(SIGUSR2, on_child_sigz);
    char last_cwd[PATH_MAX]; snprintf(last_cwd, sizeof(last_cwd), "%s", ct->cwd);
    unsigned char hdr[5];
    while (running) {
        if (ct->running) wait_for_idle(ct);
        if (child_sigint) { child_sigint = 0; interrupt_current_command(ct); }
        if (child_sigz) { child_sigz = 0; suspend_foreground(ct); }
        service_multiwatch(idx);
        refresh_jobs(ct);
        struct pollfd pfd; pfd.fd = cmd_fd; pfd.events = POLLIN | POLLHUP; pfd.revents = 0;
        int pr = poll(&pfd, 1, 30);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (read_all_fd(cmd_fd, hdr, 5) < 0) break;
            uint32_t len; memcpy(&len, hdr + 1, 4);
            if (len > INPUT_CAP) break;
            char payload[INPUT_CAP + 1];
            if (len && read_all_fd(cmd_fd, payload, len) < 0) break;
            payload[len] = '\0';
            if (hdr[0] == 'L') {
                execute_command(ct, payload);
                if (strcmp(last_cwd, ct->cwd) != 0) {
                    snprintf(last_cwd, sizeof(last_cwd), "%s", ct->cwd);
                    write_packet(resp_fd, 'W', ct->cwd, (uint32_t)strlen(ct->cwd));
                }
                wait_for_idle(ct);
                close_capture(ct);
                write_packet(resp_fd, 'D', NULL, 0);
            } else if (hdr[0] == 'S') break;
        } else if (pfd.revents & POLLHUP) break;
    }
    _exit(0);
}

void start_tab_executor(int idx) {
    Tab *t = &tabs[idx];
    if (batch_mode) return;
    if (process_mode) {
        int cmdpipe[2], resppipe[2];
        if (pipe(cmdpipe) < 0 || pipe(resppipe) < 0) { perror("pipe"); exit(1); }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            char cwd[PATH_MAX]; snprintf(cwd, sizeof(cwd), "%s", t->cwd);
            if (dpy) close(ConnectionNumber(dpy));
            for (int i2 = 0; i2 < MAX_TABS; i2++) {
                /* Only active slots own fds: never-created slots are
                   zero-filled, so their cmd_fd/resp_fd read as 0 and
                   an unchecked close() would close the child's stdin. */
                if (!tabs[i2].active) continue;
                if (tabs[i2].cmd_fd >= 0) close(tabs[i2].cmd_fd);
                if (tabs[i2].resp_fd >= 0) close(tabs[i2].resp_fd);
            }
            close(cmdpipe[1]); close(resppipe[0]);
            init_tab(t);
            snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
            memset(multiwatches, 0, sizeof(multiwatches));
            tab_count = 1;
            current_tab = idx;
            tab_child_main(t, idx, cmdpipe[0], resppipe[1]);
            _exit(0);
        }
        close(cmdpipe[0]); close(resppipe[1]);
        t->child_pid = pid; t->cmd_fd = cmdpipe[1]; t->resp_fd = resppipe[0];
        int fl = fcntl(t->resp_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(t->resp_fd, F_SETFL, fl | O_NONBLOCK);
    } else {
        if (pthread_create(&t->thr, NULL, tab_worker_main, t) == 0) {
            t->thr_started = 1;
            pthread_detach(t->thr);
        }
    }
}

/* Process-mode GUI side: consume framed packets from one tab process. */
void service_tab_packets(Tab *t) {
    if (t->resp_fd < 0) return;
    ssize_t n = read(t->resp_fd, t->resp_buf + t->resp_len, sizeof(t->resp_buf) - t->resp_len);
    if (n > 0) t->resp_len += (size_t)n;
    else if (n == 0) { close(t->resp_fd); t->resp_fd = -1; t->child_gone = 1; return; }
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { close(t->resp_fd); t->resp_fd = -1; t->child_gone = 1; return; }
    size_t off = 0;
    while (t->resp_len - off >= 5) {
        char type = (char)t->resp_buf[off];
        uint32_t len; memcpy(&len, t->resp_buf + off + 1, 4);
        if (len > INPUT_CAP + PATH_MAX) { off = t->resp_len; break; }
        if (t->resp_len - off < 5 + (size_t)len) break;
        const char *payload = (const char *)(t->resp_buf + off + 5);
        if (type == 'T') append_screen(t, payload, len);
        else if (type == 'W') { if (len) { size_t wn = len < sizeof(t->cwd) - 1 ? len : sizeof(t->cwd) - 1; memcpy(t->cwd, payload, wn); t->cwd[wn] = '\0'; } }
        else if (type == 'D') t->busy = 0;
        else if (type == 'L') clear_tab_screen(t);
        off += 5 + len;
    }
    if (off) { memmove(t->resp_buf, t->resp_buf + off, t->resp_len - off); t->resp_len -= off; }
}
