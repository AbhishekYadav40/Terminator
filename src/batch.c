/* Terminator — batch.c
 * Headless --batch mode (Design section 10): the same engine with
 *  * no X connection, for tests and scripted demos.
 */
#include "common.h"

volatile sig_atomic_t batch_sigint = 0;


/* ---------------- --batch headless mode ---------------- */
static void batch_on_sigint(int sig) { (void)sig; batch_sigint = 1; }

int batch_main(void) {
    load_persistent_history();
    install_handler(SIGINT, batch_on_sigint);
    Tab *t = &tabs[0];
    init_tab(t);
    if (!getcwd(t->cwd, sizeof(t->cwd))) strcpy(t->cwd, "/");
    tab_count = 1; current_tab = 0;
    char *line = NULL; size_t cap = 0; ssize_t nr;
    while (running && (nr = getline(&line, &cap, stdin)) >= 0) {
        while (nr > 0 && (line[nr - 1] == '\n' || line[nr - 1] == '\r')) line[--nr] = 0;
        if (pending_append(t, line)) {
            if (isatty(STDIN_FILENO)) { fputs("> ", stdout); fflush(stdout); }
            continue;
        }
        char full[INPUT_CAP]; snprintf(full, sizeof(full), "%s", t->pending_cmd);
        t->pending_cmd[0] = '\0';
        if (!full[0]) continue;
        execute_command(t, full);
        wait_for_idle(t);
        close_capture(t);
        /* multiWatch keeps running until interrupted (Ctrl+C / SIGINT). */
        while (multiwatches[0].active && running) {
            if (batch_sigint) { batch_sigint = 0; stop_multiwatch(0, 0); }
            service_multiwatch(0);
            sleep_ms(25);
        }
        fflush(stdout);
    }
    if (t->pending_cmd[0])
        append_text(t, "Syntax error: incomplete input (unclosed quote or missing heredoc delimiter).\n");
    free(line);
    return 0;
}
