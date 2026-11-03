/* Terminator — main.c
 * Program entry, X11 window setup and the event loop (Design
 *  * sections 1-2), plus the CLIPBOARD selection handlers. Shared
 *  * globals (tabs, display, mode flags) are defined here.
 */
#include "common.h"

Display *dpy;
Window win;
GC gc;
XFontStruct *font_info;
Tab tabs[MAX_TABS];
int tab_count = 0;
int current_tab = 0;
int running = 1;
int process_mode = 0;
int batch_mode = 0;
int proc_child = 0;       /* nonzero in a forked tab process */
int child_resp_fd = -1;   /* tab process -> GUI pipe (child side) */

volatile int screen_dirty = 0;
volatile sig_atomic_t child_sigint = 0, child_sigz = 0;
pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

int win_width = 900, win_height = 600;

/* X11 clipboard atoms used by Ctrl+V. */
Atom clipboard_atom, utf8_atom, paste_property;
char clipboard_text[INPUT_CAP];

void request_clipboard_paste(void) {
    XConvertSelection(dpy, clipboard_atom, utf8_atom, paste_property, win, CurrentTime); XFlush(dpy);
}

static void handle_selection_request(XSelectionRequestEvent *req) {
    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = SelectionNotify; reply.display = req->display; reply.requestor = req->requestor;
    reply.selection = req->selection; reply.target = req->target; reply.time = req->time; reply.property = None;
    Atom targets = XInternAtom(dpy, "TARGETS", False), utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom property = req->property == None ? req->target : req->property;
    if (req->target == targets) {
        Atom supported[3] = {targets, utf8, XA_STRING};
        XChangeProperty(dpy, req->requestor, property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported, 3);
        reply.property = property;
    } else if (req->target == utf8 || req->target == XA_STRING) {
        XChangeProperty(dpy, req->requestor, property, req->target, 8, PropModeReplace,
                        (unsigned char *)clipboard_text, (int)strlen(clipboard_text));
        reply.property = property;
    }
    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
    XFlush(dpy);
}

/* Insert one pasted character at the cursor (the input cap applies
   exactly as when typing). Returns 1 when it was inserted. */
static int paste_insert_char(Tab *t, char c) {
    if (t->input_len + 1 >= INPUT_CAP) return 0;
    memmove(t->input + t->cursor_pos + 1, t->input + t->cursor_pos, t->input_len - t->cursor_pos + 1);
    t->input[t->cursor_pos++] = c;
    t->input_len++;
    t->input[t->input_len] = '\0';
    return 1;
}

static void handle_paste_result(XSelectionEvent *ev) {
    if (ev->property == None) {
        XConvertSelection(dpy, clipboard_atom, XA_STRING, paste_property, win, CurrentTime);
        XFlush(dpy);
        return;
    }
    Atom actual_type; int actual_format; unsigned long nitems = 0, bytes_after = 0; unsigned char *data = NULL;
    int rc = XGetWindowProperty(dpy, win, ev->property, 0, INPUT_CAP,
                                True, AnyPropertyType, &actual_type, &actual_format,
                                &nitems, &bytes_after, &data);
    if (rc == Success && data && actual_format == 8) {
        Tab *t = &tabs[current_tab];
        /* Pasted newlines stay newlines, as in a real terminal: pasting
           several lines and pressing Return runs each one (the engine
           splits a submitted line on newlines), and a paste ending in
           a newline submits at once. While a history search is active
           a newline is just a space — a search term is one line. */
        int searching = history_search_active();
        int ended_newline = 0;
        for (unsigned long i = 0; i < nitems; i++) {
            unsigned char c = data[i];
            if (c == '\r' || c == '\n') {
                if (i + 1 < nitems && c == '\r' && data[i + 1] == '\n') i++; /* CRLF: one newline */
                if (searching) paste_insert_char(t, ' ');
                else ended_newline = paste_insert_char(t, '\n');
                continue;
            }
            ended_newline = 0;
            if (c >= 32 || c == '\t') paste_insert_char(t, (char)c);
        }
        if (ended_newline && !searching) submit_current_line(t);
    }
    if (data) XFree(data);
}


int main(int argc, char **argv) {
    for (int ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "useProcess=True")) process_mode = 1;
        else if (!strcmp(argv[ai], "--batch")) batch_mode = 1;
    }
    if (batch_mode) process_mode = 0;
    bindings_set_defaults();
    load_hotkeys();
    if (batch_mode) return batch_main();

    /* Terminator itself must never be stopped or killed by Ctrl+C/Ctrl+Z:
       those keys are handled as X11 events and forwarded to jobs only. */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    if (process_mode) shm_history_init();
    load_persistent_history();
    dpy = XOpenDisplay(NULL); if (!dpy) { fprintf(stderr, "Cannot open X display.\n"); return 1; }
    clipboard_atom = XInternAtom(dpy, "CLIPBOARD", False); utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
    paste_property = XInternAtom(dpy, "TERMINATOR_CLIPBOARD_PASTE", False);
    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, win_width, win_height, 1, BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, win, "Terminator - Custom Terminal");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | StructureNotifyMask);
    gc = XCreateGC(dpy, win, 0, NULL); font_info = XLoadQueryFont(dpy, "fixed"); if (font_info) XSetFont(dpy, gc, font_info->fid);
    XMapWindow(dpy, win); XFlush(dpy); create_tab();
    if (config_warnings[0]) append_text(&tabs[current_tab], config_warnings);
    draw_screen();
    while (running) {
        struct pollfd fds[1 + MAX_TABS]; int map_tab[1 + MAX_TABS]; nfds_t nfds = 1;
        fds[0].fd = ConnectionNumber(dpy); fds[0].events = POLLIN; map_tab[0] = -1;
        if (process_mode) {
            for (int i = 0; i < MAX_TABS; i++) if (tabs[i].active && tabs[i].resp_fd >= 0) { fds[nfds].fd = tabs[i].resp_fd; fds[nfds].events = POLLIN | POLLHUP | POLLERR; map_tab[nfds] = i; nfds++; }
        }
        int r = poll(fds, nfds, 50); if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (fds[0].revents & POLLIN) {
            while (XPending(dpy)) {
                XEvent ev; XNextEvent(dpy, &ev);
                if (ev.type == Expose) draw_screen();
                else if (ev.type == ConfigureNotify) { win_width = ev.xconfigure.width; win_height = ev.xconfigure.height; clear_selection(&tabs[current_tab]); draw_screen(); }
                else if (ev.type == KeyPress) { handle_key(&ev.xkey); draw_screen(); }
                else if (ev.type == SelectionNotify) { handle_paste_result(&ev.xselection); draw_screen(); }
                else if (ev.type == SelectionRequest) { handle_selection_request(&ev.xselectionrequest); }
                else if (ev.type == MotionNotify) {
                    /* Button1 drag in the text area extends a selection. */
                    if (tabs[current_tab].sel_dragging) { sel_extend(&tabs[current_tab], ev.xmotion.x, ev.xmotion.y); draw_screen(); }
                }
                else if (ev.type == ButtonRelease) {
                    if (ev.xbutton.button == Button1 && tabs[current_tab].sel_dragging) { sel_end(&tabs[current_tab]); draw_screen(); }
                }
                else if (ev.type == ButtonPress) {
                    int x = ev.xbutton.x, y = ev.xbutton.y;
                    if (ev.xbutton.button == Button4) { scroll_tab(&tabs[current_tab], 3); draw_screen(); }
                    else if (ev.xbutton.button == Button5) { scroll_tab(&tabs[current_tab], -3); draw_screen(); }
                    else if (y < TAB_HEIGHT) {
                        int pos = x / TAB_WIDTH;
                        if (pos < tab_count) {
                            int slot = tab_slot_at(pos);
                            if (slot >= 0) {
                                if (x % TAB_WIDTH >= TAB_WIDTH - 28) close_tab(slot);
                                else current_tab = slot;
                            }
                        } else if (pos == tab_count) create_tab();
                        draw_screen();
                    }
                    else if (ev.xbutton.button == Button1) { sel_begin(&tabs[current_tab], x, y); draw_screen(); }
                }
            }
        }
        if (process_mode) {
            for (nfds_t j = 1; j < nfds; j++) if (fds[j].revents & (POLLIN | POLLHUP | POLLERR)) {
                int idx = map_tab[j];
                if (idx >= 0 && idx < MAX_TABS && tabs[idx].active) service_tab_packets(&tabs[idx]);
            }
        }

        /* Tabs that asked to close (exit builtin / Ctrl+D), and tab
           processes that died on their own. */
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].active) continue;
            if (tabs[i].exit_requested || tabs[i].child_gone) {
                tabs[i].exit_requested = 0; tabs[i].child_gone = 0;
                if (tab_count > 1) { close_tab(i); }
                else { terminate_tab(&tabs[i]); tabs[i].active = 0; tab_count = 0; running = 0; }
                draw_screen();
            }
        }
        if (tab_count == 0) running = 0;
        if (screen_dirty) { screen_dirty = 0; draw_screen(); }
    }
    for (int i = 0; i < MAX_TABS; i++) if (tabs[i].active) terminate_tab(&tabs[i]);
    XFreeGC(dpy, gc); if (font_info) XFreeFont(dpy, font_info); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 0;
}
