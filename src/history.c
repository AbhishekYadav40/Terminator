/* Terminator — history.c
 * Shared history (Design section 3): heap list in thread/batch
 *  * mode, mmap + process-shared semaphore ring in process mode, and the
 *  * ~/.terminator_history file. Only hist_add / hist_count / hist_get
 *  * and shm_history_init are visible outside this file.
 */
#include "common.h"

#define MAX_PERSISTENT_HISTORY 10000
#define HIST_ENTRY_MAX 512
/* Shared history for process mode: lives in an mmap(MAP_SHARED) segment
   created before any tab process is forked; guarded by a process-shared
   semaphore stored in the same segment. In thread/batch mode the same
   accessors use a heap list guarded by a pthread mutex. */
typedef struct {
    sem_t sem;
    int count;
    char entries[MAX_PERSISTENT_HISTORY][HIST_ENTRY_MAX];
} SharedHistory;
static SharedHistory *shm_hist = NULL;
static char **persistent_history;
static size_t persistent_history_count;
static int persistent_history_loaded;
static char history_file_path[PATH_MAX];
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

static void hist_lock(void) { if (shm_hist) sem_wait(&shm_hist->sem); else pthread_mutex_lock(&history_mutex); }
static void hist_unlock(void) { if (shm_hist) sem_post(&shm_hist->sem); else pthread_mutex_unlock(&history_mutex); }
static void hist_path_init(void) {
    if (!history_file_path[0]) {
        const char *home = getenv("HOME"); if (!home) home = ".";
        snprintf(history_file_path, sizeof(history_file_path), "%s/.terminator_history", home);
    }
}
/* Read the next non-empty history-file line, trimmed of its newline.
   Returns 1 with *line holding the line, 0 at end of file. */
static int next_history_line(FILE *f, char **line, size_t *cap) {
    ssize_t n;
    while ((n = getline(line, cap, f)) >= 0) {
        while (n > 0 && ((*line)[n - 1] == '\n' || (*line)[n - 1] == '\r')) (*line)[--n] = 0;
        if (n) return 1;
    }
    return 0;
}
/* Caller must hold the history lock. Heap-list (non-shared) mode only. */
static void load_persistent_history_locked(void) {
    if (persistent_history_loaded) return;
    persistent_history_loaded = 1;
    hist_path_init();
    FILE *f = fopen(history_file_path, "r"); if (!f) return;
    char *line = NULL; size_t cap = 0;
    while (next_history_line(f, &line, &cap)) {
        if (persistent_history_count == MAX_PERSISTENT_HISTORY) {
            free(persistent_history[0]);
            memmove(persistent_history, persistent_history + 1, (MAX_PERSISTENT_HISTORY - 1) * sizeof(*persistent_history));
            persistent_history_count--;
        }
        char **q = realloc(persistent_history, (persistent_history_count + 1) * sizeof(*q));
        if (!q) break;
        persistent_history = q;
        persistent_history[persistent_history_count] = strdup(line);
        if (!persistent_history[persistent_history_count]) break;
        persistent_history_count++;
    }
    free(line); fclose(f);
}
void load_persistent_history(void) {
    if (shm_hist) return;
    hist_lock(); load_persistent_history_locked(); hist_unlock();
}
int hist_count(void) {
    int n; hist_lock();
    if (shm_hist) n = shm_hist->count;
    else { load_persistent_history_locked(); n = (int)persistent_history_count; }
    hist_unlock(); return n;
}
int hist_get(int idx, char *buf, size_t cap) {
    int ok = 0; hist_lock();
    if (shm_hist) {
        if (idx >= 0 && idx < shm_hist->count) { snprintf(buf, cap, "%s", shm_hist->entries[idx]); ok = 1; }
    } else {
        load_persistent_history_locked();
        if (idx >= 0 && (size_t)idx < persistent_history_count) { snprintf(buf, cap, "%s", persistent_history[idx]); ok = 1; }
    }
    hist_unlock(); return ok;
}
/* Rewrite the history file from the in-memory entries. Caller must hold
   hist_lock. The file then holds exactly the last MAX_PERSISTENT_HISTORY
   commands, as Task 9 requires; appending alone would let it grow forever.
   Only used when an old entry was shifted out, so the cost is rare. */
static void rewrite_history_file_locked(void) {
    int fd = open(history_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    flock(fd, LOCK_EX);
    if (shm_hist) {
        for (int i = 0; i < shm_hist->count; i++) dprintf(fd, "%s\n", shm_hist->entries[i]);
    } else {
        for (size_t i = 0; i < persistent_history_count; i++)
            if (persistent_history[i]) dprintf(fd, "%s\n", persistent_history[i]);
    }
    flock(fd, LOCK_UN); close(fd);
}

void hist_add(const char *line) {
    if (!line || !*line) return;
    hist_path_init();
    hist_lock();
    int shifted = 0;
    if (shm_hist) {
        int c = shm_hist->count;
        if (c && strcmp(shm_hist->entries[c - 1], line) == 0) { hist_unlock(); return; }
        if (c >= MAX_PERSISTENT_HISTORY) {
            memmove(shm_hist->entries, shm_hist->entries + 1, sizeof(shm_hist->entries[0]) * (MAX_PERSISTENT_HISTORY - 1));
            c = MAX_PERSISTENT_HISTORY - 1;
            shifted = 1;
        }
        { size_t hl = strlen(line); if (hl >= HIST_ENTRY_MAX) hl = HIST_ENTRY_MAX - 1; memcpy(shm_hist->entries[c], line, hl); shm_hist->entries[c][hl] = '\0'; }
        shm_hist->count = c + 1;
    } else {
        load_persistent_history_locked();
        if (persistent_history_count && strcmp(persistent_history[persistent_history_count - 1], line) == 0) { hist_unlock(); return; }
        if (persistent_history_count == MAX_PERSISTENT_HISTORY) {
            free(persistent_history[0]);
            memmove(persistent_history, persistent_history + 1, (MAX_PERSISTENT_HISTORY - 1) * sizeof(*persistent_history));
            persistent_history_count--;
            shifted = 1;
        }
        char **q = realloc(persistent_history, (persistent_history_count + 1) * sizeof(*q));
        if (q) {
            persistent_history = q;
            persistent_history[persistent_history_count] = strdup(line);
            if (persistent_history[persistent_history_count]) persistent_history_count++;
        }
    }
    /* File writes stay under hist_lock so a rewrite (shift case) cannot
       drop a line another tab appended in between. */
    if (shifted) rewrite_history_file_locked();
    else {
        int fd = open(history_file_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) { flock(fd, LOCK_EX); dprintf(fd, "%s\n", line); flock(fd, LOCK_UN); close(fd); }
    }
    hist_unlock();
}

/* Create the shared-history segment and preload it from the history file.
   Called by the GUI parent before forking any tab process. */
void shm_history_init(void) {
    shm_hist = mmap(NULL, sizeof(SharedHistory), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm_hist == MAP_FAILED) { shm_hist = NULL; return; }
    memset(shm_hist, 0, sizeof(*shm_hist)); sem_init(&shm_hist->sem, 1, 1);
    hist_path_init();
    FILE *f = fopen(history_file_path, "r"); if (!f) return;
    char *line = NULL; size_t cap = 0;
    while (next_history_line(f, &line, &cap)) {
        if (shm_hist->count >= MAX_PERSISTENT_HISTORY) {
            memmove(shm_hist->entries, shm_hist->entries + 1, sizeof(shm_hist->entries[0]) * (MAX_PERSISTENT_HISTORY - 1));
            shm_hist->count = MAX_PERSISTENT_HISTORY - 1;
        }
        /* skip consecutive duplicates, like hist_add */
        if (shm_hist->count && strcmp(shm_hist->entries[shm_hist->count - 1], line) == 0) continue;
        snprintf(shm_hist->entries[shm_hist->count], HIST_ENTRY_MAX, "%s", line);
        shm_hist->count++;
    }
    free(line); fclose(f);
}
