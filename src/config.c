/* Terminator — config.c
 * Hotkeys and ~/.terminatorrc (Design section 7): the binding
 *  * table, its defaults, validation, and matching key events to actions.
 */
#include "common.h"

static char hotkey_file[PATH_MAX];

static const char *bind_names[BIND_COUNT] = {
    "new_tab", "close_tab", "clear", "history_search", "line_start", "line_end",
    "delete_to_start", "delete_to_end", "delete_word", "copy", "paste", "interrupt",
    "suspend", "eof", "history_prev", "history_next", "cursor_left", "cursor_right",
    "tab_next", "tab_prev", "scroll_up", "scroll_down"
};
static const char *bind_alias[BIND_COUNT] = {
    "new-tab", "close-tab", "clear-screen", "history-search", "line-start", "line-end",
    "delete-to-start", "delete-to-end", "delete-word", "copy", "paste", "interrupt",
    "suspend", "eof", "history-prev", "history-next", "cursor-left", "cursor-right",
    "tab-next", "tab-prev", "scroll-up", "scroll-down"
};
static unsigned int bind_mods[BIND_COUNT];
static KeySym bind_keys[BIND_COUNT];
char config_warnings[4096];

static void config_warn(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fprintf(stderr, "%s", buf);
    size_t cur = strlen(config_warnings), bl = strlen(buf);
    if (cur + bl < sizeof(config_warnings) - 1) memcpy(config_warnings + cur, buf, bl + 1);
}
static KeySym canon_key(KeySym k) { if (k >= XK_A && k <= XK_Z) return (KeySym)(k + 32); return k; }
void bindings_set_defaults(void) {
    static const struct { unsigned int m; KeySym k; } d[BIND_COUNT] = {
        {ControlMask, XK_t}, {ControlMask | ShiftMask, XK_w}, {ControlMask, XK_l}, {ControlMask, XK_r},
        {ControlMask, XK_a}, {ControlMask, XK_e}, {ControlMask, XK_u}, {ControlMask, XK_k},
        {ControlMask, XK_w}, {ControlMask | ShiftMask, XK_c}, {ControlMask | ShiftMask, XK_v},
        {ControlMask, XK_c}, {ControlMask, XK_z}, {ControlMask, XK_d},
        {0, XK_Up}, {0, XK_Down}, {0, XK_Left}, {0, XK_Right},
        {ControlMask, XK_Tab}, {ControlMask | ShiftMask, XK_ISO_Left_Tab},
        {ShiftMask, XK_Prior}, {ShiftMask, XK_Next}
    };
    for (int i = 0; i < BIND_COUNT; i++) { bind_mods[i] = d[i].m; bind_keys[i] = canon_key(d[i].k); }
}
int key_matches(XKeyEvent *ev, KeySym ks, int action) {
    unsigned int m = ev->state & (ControlMask | ShiftMask | Mod1Mask);
    return m == bind_mods[action] && canon_key(ks) == bind_keys[action];
}
static int find_binding(const char *name) {
    for (int i = 0; i < BIND_COUNT; i++)
        if (!strcmp(name, bind_names[i]) || !strcmp(name, bind_alias[i])) return i;
    return -1;
}


static int parse_hotkey(const char *value, unsigned int *mods, KeySym *key) {
    if (!value || !*value) return -1;
    char tmp[96]; if (snprintf(tmp, sizeof(tmp), "%s", value) >= (int)sizeof(tmp)) return -1;
    unsigned int m = 0; char *save = NULL; char *part = strtok_r(tmp, "+", &save); KeySym k = NoSymbol;
    while (part) {
        if (!strcasecmp(part, "ctrl") || !strcasecmp(part, "control")) m |= ControlMask;
        else if (!strcasecmp(part, "shift")) m |= ShiftMask;
        else if (!strcasecmp(part, "alt")) m |= Mod1Mask;
        else { k = XStringToKeysym(part); if (k == NoSymbol) return -1; }
        part = strtok_r(NULL, "+", &save);
    }
    if (k == NoSymbol) { return -1; } *mods = m; *key = k; return 0;
}

void load_hotkeys(void) {
    bindings_set_defaults();
    const char *home = getenv("HOME"); if (!home) home = "."; snprintf(hotkey_file, sizeof(hotkey_file), "%s/.terminatorrc", home);
    FILE *f = fopen(hotkey_file, "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        char name[64] = "", value[96] = "";
        int ok = 0;
        if (!strncmp(p, "bind ", 5)) {
            char combo[96] = "", action[64] = "";
            if (sscanf(p + 5, "%95s %63s", combo, action) == 2) {
                snprintf(name, sizeof(name), "%s", action);
                snprintf(value, sizeof(value), "%s", combo);
                ok = 1;
            }
        } else if (sscanf(p, "%63[^=]=%95s", name, value) == 2) ok = 1;
        if (!ok) { config_warn("Terminator: ignoring malformed line in ~/.terminatorrc: %s", line); continue; }
        size_t nl = strlen(name); while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) name[--nl] = '\0';
        char *ns = name; while (*ns == ' ' || *ns == '\t') ns++;
        int bi = find_binding(ns);
        if (bi < 0) { config_warn("Terminator: unknown action '%s' in ~/.terminatorrc; ignoring.\n", ns); continue; }
        unsigned int mods = 0; KeySym sym = NoSymbol;
        if (parse_hotkey(value, &mods, &sym) < 0) {
            config_warn("Terminator: invalid hotkey '%s' for %s; keeping default.\n", value, bind_names[bi]);
            continue;
        }
        sym = canon_key(sym);
        int conflict = -1;
        for (int i = 0; i < BIND_COUNT; i++) if (i != bi && bind_mods[i] == mods && bind_keys[i] == sym) { conflict = i; break; }
        if (conflict >= 0) {
            config_warn("Terminator: hotkey for %s conflicts with %s; keeping default for %s.\n",
                        bind_names[bi], bind_names[conflict], bind_names[bi]);
            continue;
        }
        bind_mods[bi] = mods; bind_keys[bi] = sym;
    }
    fclose(f);
}
