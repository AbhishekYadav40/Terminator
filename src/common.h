/* Terminator — common.h
 * Shared declarations for the staged source files. Every file
 * includes this header; it carries the includes, constants, the
 * Tab/Job/MultiWatch types, the shared globals, and the prototypes
 * of every function used in more than one file. No function is
 * renamed: the split only moves code between files.
 */
#ifndef TERMINATOR_COMMON_H
#define TERMINATOR_COMMON_H

#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <ctype.h>
#include <semaphore.h>
#include <pwd.h>
#define MAX_TABS 20
#define MAX_SCREEN 16000
#define INPUT_CAP 4096
#define MAX_TOKENS 256
#define MAX_CMDS 32
#define TAB_WIDTH 120
#define TAB_HEIGHT 34
#define LINE_HEIGHT 18
#define OUTPUT_CHUNK 2048
#define MAX_HISTORY 100
#define MAX_JOBS 64
typedef struct { int id; pid_t pgid; pid_t pids[MAX_CMDS]; unsigned char pid_stopped[MAX_CMDS]; int npids; int active; int stopped; int out_fd; int tab_index; char command[INPUT_CAP]; } Job;
typedef struct {
    int active;
    char screen[MAX_SCREEN];
    size_t screen_len;
    char input[INPUT_CAP];
    size_t input_len;
    size_t cursor_pos;
    int scroll_offset;  /* display rows scrolled up from the tail (screen.c) */
    int screen_gen;     /* bumped on every screen append/clear (screen.c) */
    /* Mouse text selection (screen.c): a drag in the text area selects
       wrapped display rows/columns; Ctrl+Shift+C copies the selection.
       sel_gen/sel_wrap are the screen generation and wrap width the
       selection was made against, so stale selections are discarded. */
    int sel_active, sel_dragging, sel_gen;
    size_t sel_wrap;
    size_t sel_a_row, sel_a_col, sel_b_row, sel_b_col;
    char cwd[PATH_MAX];
    char prev_cwd[PATH_MAX]; /* previous cwd, for `cd -` (OLDPWD) */
    int out_fd;
    pid_t pids[MAX_CMDS];
    int pid_count;
    pid_t pgid;
    int running;
    int last_status;
    int multiline;  /* quote or trailing backslash continuation */
    char pending_cmd[INPUT_CAP];
    char fg_command[INPUT_CAP];
    int hist_pos;   /* index into the shared history; -1 = not browsing */
    Job jobs[MAX_JOBS];
    int next_job_id;
    /* Executor state (thread worker / tab process / batch). */
    int suppress_history;
    int capture_fd;       /* >=0 while a built-in's output is redirected to a file */
    volatile int busy;
    volatile int exit_requested;
    volatile int has_submit;
    volatile int dead;
    pthread_t thr;
    int thr_started;
    volatile int child_gone;
    char queue_line[INPUT_CAP];
    pid_t child_pid;    /* process mode, GUI side: the tab process */
    int cmd_fd;         /* process mode, GUI side: GUI -> tab process */
    int resp_fd;        /* process mode, GUI side: tab process -> GUI */
    unsigned char resp_buf[16384];
    size_t resp_len;
} Tab;
#define MAX_MULTIWATCH_COMMANDS 32
#define MW_MAX_SEGS 16
#define MULTIWATCH_INTERVAL_MS 2000
#define MULTIWATCH_OUTPUT_LIMIT 8192

typedef struct {
    int active, stopping, tab_index, command_count, round_number, round_started;
    long long next_run_ms;
    char commands[MAX_MULTIWATCH_COMMANDS][INPUT_CAP];
    pid_t pids[MAX_MULTIWATCH_COMMANDS][MAX_CMDS];
    int npids[MAX_MULTIWATCH_COMMANDS];
    pid_t pgids[MAX_MULTIWATCH_COMMANDS];
    int fds[MAX_MULTIWATCH_COMMANDS];
    int process_done[MAX_MULTIWATCH_COMMANDS], header_written[MAX_MULTIWATCH_COMMANDS];
    int footer_written[MAX_MULTIWATCH_COMMANDS], out_started[MAX_MULTIWATCH_COMMANDS];
    int out_last_nl[MAX_MULTIWATCH_COMMANDS];
    /* A watched command may be a list (sleep 1; echo A): its segments
       run one after another inside the same round, under the same
       header/footer, with && / || short-circuiting on each segment's
       exit status. Slices of commands[i]: off/len per segment, op is
       the separator AFTER the segment. */
    int seg_count[MAX_MULTIWATCH_COMMANDS];
    int seg_index[MAX_MULTIWATCH_COMMANDS];
    int seg_status[MAX_MULTIWATCH_COMMANDS];
    int seg_off[MAX_MULTIWATCH_COMMANDS][MW_MAX_SEGS];
    int seg_len[MAX_MULTIWATCH_COMMANDS][MW_MAX_SEGS];
    int seg_op[MAX_MULTIWATCH_COMMANDS][MW_MAX_SEGS];
} MultiWatch;

/* ---- Configurable key bindings (Task 7) ----
   Bindings change in ~/.terminatorrc without recompiling (read at every
   startup, so they persist). Formats: clear=Ctrl+L, or
   bind Ctrl+L clear-screen. Invalid/unknown/conflicting bindings are
   reported and the default is kept. */
enum {
    BIND_NEW_TAB, BIND_CLOSE_TAB, BIND_CLEAR, BIND_SEARCH, BIND_LINE_START,
    BIND_LINE_END, BIND_KILL_START, BIND_KILL_END, BIND_DELETE_WORD, BIND_COPY,
    BIND_PASTE, BIND_INTERRUPT, BIND_SUSPEND, BIND_EOF, BIND_HIST_PREV,
    BIND_HIST_NEXT, BIND_CURSOR_LEFT, BIND_CURSOR_RIGHT, BIND_TAB_NEXT, BIND_TAB_PREV,
    BIND_SCROLL_UP, BIND_SCROLL_DOWN, BIND_COUNT
};
/* One redirection, in the order written (bash applies them left to right,
   so `>f 2>&1` and `2>&1 >f` differ). file is NULL for dup/close forms. */
typedef struct { int target_fd; char *file; int append; int dup_fd; int close_fd; int both; } Redir;
typedef struct { char *argv[MAX_TOKENS]; int argc; Redir redirs[8]; int nredirs; int pipe_stderr; int has_heredoc; char *heredoc; size_t heredoc_len; } Command;

/* Command lists: &&, ||, ; and & join single commands. Operators inside
   quotes, $(...) and backticks are data, not separators; an & that is
   part of a redirection (2>&1, |&, &>) is not a separator either. */
#define LIST_END 0
#define LIST_SEQ 1  /* ; : always run the next command */
#define LIST_BG  2  /* & : run the previous command in the background */
#define LIST_AND 3  /* && : run next only if the previous status is 0 */
#define LIST_OR  4  /* || : run next only if the previous status is not 0 */
typedef struct { const char *ptr; size_t len; int op; } ListSeg;
int split_list(const char *line, ListSeg *segs, int maxsegs);

/* Shared globals: defined in main.c (display, tabs, mode flags),
 * multiwatch.c (multiwatches), config.c (config_warnings) and
 * batch.c (batch_sigint); declared here for all files. */
extern Display *dpy;
extern Window win;
extern GC gc;
extern XFontStruct *font_info;
extern Tab tabs[MAX_TABS];
extern int tab_count;
extern int current_tab;
extern int running;
extern int process_mode;
extern int batch_mode;
extern int proc_child;
extern int child_resp_fd;
extern volatile int screen_dirty;
extern pthread_mutex_t state_lock;
extern int win_width, win_height;
extern MultiWatch multiwatches[MAX_TABS];
extern char config_warnings[4096];
extern volatile sig_atomic_t child_sigint, child_sigz;
extern volatile sig_atomic_t batch_sigint;
extern Atom clipboard_atom, utf8_atom, paste_property;
extern char clipboard_text[INPUT_CAP];

/* Cross-file functions, grouped by the file that defines them. */
/* history.c */
void load_persistent_history(void);
int hist_count(void);
int hist_get(int idx, char *buf, size_t cap);
void hist_add(const char *line);
void shm_history_init(void);
/* util.c */
void join_path(char *dst, size_t dstsz, const char *dir, const char *name);
void sleep_ms(int ms);
int write_all_fd(int fd, const void *buf, size_t n);
int write_packet(int fd, char type, const void *payload, uint32_t len);
int read_all_fd(int fd, void *buf, size_t n);
/* screen.c */
void append_screen(Tab *t, const char *s, size_t n);
void append_text(Tab *t, const char *s);
void build_prompt(char *dst, size_t cap, const Tab *t);
void draw_screen(void);
void clear_tab_screen(Tab *t);
void scroll_tab(Tab *t, int rows);
int screen_page_rows(void);
void clear_selection(Tab *t);
void sel_begin(Tab *t, int x, int y);
void sel_extend(Tab *t, int x, int y);
void sel_end(Tab *t);
int sel_copy_text(Tab *t, char *dst, size_t cap);
/* tabs.c */
void init_tab(Tab *t);
int create_tab(void);
int tab_slot_at(int pos);
void terminate_tab(Tab *t);
void close_tab(int idx);
/* config.c */
void bindings_set_defaults(void);
int key_matches(XKeyEvent *ev, KeySym ks, int action);
void load_hotkeys(void);
/* jobs.c */
void interrupt_current_command(Tab *t);
void discard_input_line(Tab *t);
Job *find_job(Tab *t, int id);
Job *add_job(Tab *t, const char *command, int stopped);
void refresh_jobs(Tab *t);
void show_jobs(Tab *t);
int job_id_arg(char **tok,int ntok);
void wait_for_idle(Tab *t);
void suspend_foreground(Tab *t);
/* shell.c */
/* qflags[i] (may be NULL) records, per token: bit 1 = the word had a
   quoted portion or an escaped glob character (never glob-expand it),
   bit 2 = the word started inside quotes (never a NAME=value assignment). */
int tokenize(const char *line, char **tokens, int *argc_out, int *qflags);
void free_tokens(char **tokens, int n);
int parse_commands(char **tok, int ntok, Command *cmds, int *ncmd_out);
int heredoc_find(const char *line, char *delim, size_t cap, int *quoted);
int heredoc_open(const char *text, char *delim, size_t cap, int *quoted);
void print_error(Tab *t, const char *what);
int launch_pipeline_fds(Tab *t, Command *cmds, int nc, int *fd_out, pid_t *pids_out, pid_t *pgid_out);
int line_needs_more(const char *s);
int execute_command_sync(Tab *t, const char *line);
int expand_variables(Tab *t, char *s, size_t cap);
int expand_arithmetic(char *s, size_t cap);
int expand_globs(Tab *t, char **tokens, int *ntok, int *qflags);
void strip_comment(char *p);
void execute_command(Tab *t, const char *line);
/* multiwatch.c */
void stop_multiwatch(int tab_index, int announce);
void service_multiwatch(int tab_index);
void run_multiwatch_list(Tab *t, char cmds[][INPUT_CAP], int n);
void run_multiwatch(Tab *t, char **tok, int ntok);
int parse_multiwatch_brackets(const char *line, char cmds[][INPUT_CAP], int maxn);
/* script.c */
int run_script_file(Tab *t, const char *path, int argc, char **argv, int depth);
/* editor.c */
int pending_append(Tab *t, char *seg);
void submit_current_line(Tab *t);
int history_search_active(void);
void handle_key(XKeyEvent *ev);
/* executor.c */
void install_handler(int sig, void (*fn)(int));
void close_capture(Tab *t);
void start_tab_executor(int idx);
void service_tab_packets(Tab *t);
/* batch.c */
int batch_main(void);
/* main.c */
void request_clipboard_paste(void);

#endif /* TERMINATOR_COMMON_H */
