/* Synthetic X11 key sender: finds the Terminator window and sends it
   KeyPress events with XSendEvent. Usage:
     keys <spec> [spec...]
   spec = type:<text>            type a string
        | key:<keysym>           single key, e.g. key:Return key:Tab key:Left
        | ctrl:<keysym>          with ControlMask      (e.g. ctrl:c)
        | ctrl+shift:<keysym>    with Control+Shift    (e.g. ctrl+shift:v)
        | shift:<keysym>
        | sleep:<ms>
        | click:<x>,<y>          mouse click, e.g. click:60,10 (tab bar)
        | drag:<x1>,<y1>,<x2>,<y2> mouse drag (press, motion, release)
        | wheel:up|down          mouse wheel scroll in the screen area
*/
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *dpy;
static Window target;

static Window find_window(Window root) {
    Window r, p, *kids = NULL; unsigned int nk = 0;
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk) && kids) {
        for (unsigned int i = 0; i < nk; i++) {
            char *name = NULL;
            if (XFetchName(dpy, kids[i], &name) && name) {
                int hit = strstr(name, "Terminator") != NULL;
                XFree(name);
                if (hit) { Window w = kids[i]; XFree(kids); return w; }
            }
            Window w = find_window(kids[i]);
            if (w) { XFree(kids); return w; }
        }
        XFree(kids);
    }
    return 0;
}

static KeySym char_keysym(char c) {
    switch (c) {
        case ' ': return XK_space; case '>': return XK_greater; case '<': return XK_less;
        case '+': return XK_plus; case '%': return XK_percent; case '"': return XK_quotedbl;
        case '$': return XK_dollar; case '*': return XK_asterisk; case '|': return XK_bar;
        case '(': return XK_parenleft; case ')': return XK_parenright;
        case '[': return XK_bracketleft; case ']': return XK_bracketright;
        case '=': return XK_equal; case ';': return XK_semicolon; case '\'': return XK_apostrophe;
        case '`': return XK_grave; case '~': return XK_asciitilde; case '#': return XK_numbersign;
        case '&': return XK_ampersand; case '!': return XK_exclam; case '?': return XK_question;
        case ':': return XK_colon; case '\\': return XK_backslash; case ',': return XK_comma;
        case '@': return XK_at; case '^': return XK_asciicircum;
        case '{': return XK_braceleft; case '}': return XK_braceright;
        case '_': return XK_underscore; case '/': return XK_slash; case '.': return XK_period;
        case '-': return XK_minus;
        default: {
            char s[2] = {c, 0};
            return XStringToKeysym(s);
        }
    }
}
static void send_click(int x, int y) {
    XButtonEvent ev; memset(&ev, 0, sizeof(ev));
    ev.display = dpy; ev.window = target; ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None; ev.time = CurrentTime; ev.x = x; ev.y = y; ev.x_root = x; ev.y_root = y;
    ev.state = 0; ev.button = Button1; ev.same_screen = True;
    ev.type = ButtonPress;
    XSendEvent(dpy, target, True, ButtonPressMask, (XEvent *)&ev);
    ev.type = ButtonRelease;
    XSendEvent(dpy, target, True, ButtonReleaseMask, (XEvent *)&ev);
    XFlush(dpy);
}

/* Drag with Button1 held: press at the start, motion events along the
   path (state Button1Mask, as a real drag delivers), release at the end. */
static void send_drag(int x1, int y1, int x2, int y2) {
    XButtonEvent bev; memset(&bev, 0, sizeof(bev));
    bev.display = dpy; bev.window = target; bev.root = DefaultRootWindow(dpy);
    bev.subwindow = None; bev.time = CurrentTime; bev.x_root = x1; bev.y_root = y1;
    bev.state = 0; bev.button = Button1; bev.same_screen = True;
    bev.type = ButtonPress; bev.x = x1; bev.y = y1;
    XSendEvent(dpy, target, True, ButtonPressMask, (XEvent *)&bev);
    for (int step = 1; step <= 6; step++) {
        XMotionEvent mev; memset(&mev, 0, sizeof(mev));
        mev.display = dpy; mev.window = target; mev.root = DefaultRootWindow(dpy);
        mev.subwindow = None; mev.time = CurrentTime; mev.same_screen = True;
        mev.state = Button1Mask; mev.is_hint = NotifyNormal;
        mev.x = x1 + (x2 - x1) * step / 6; mev.y = y1 + (y2 - y1) * step / 6;
        mev.x_root = mev.x; mev.y_root = mev.y; mev.type = MotionNotify;
        XSendEvent(dpy, target, True, ButtonMotionMask, (XEvent *)&mev);
        usleep(20 * 1000);
    }
    bev.type = ButtonRelease; bev.x = x2; bev.y = y2; bev.x_root = x2; bev.y_root = y2;
    bev.state = Button1Mask;
    XSendEvent(dpy, target, True, ButtonReleaseMask, (XEvent *)&bev);
    XFlush(dpy);
}

static void send_wheel(int up) {
    XButtonEvent ev; memset(&ev, 0, sizeof(ev));
    ev.display = dpy; ev.window = target; ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None; ev.time = CurrentTime; ev.x = 512; ev.y = 400; ev.x_root = 512; ev.y_root = 400;
    ev.state = 0; ev.button = up ? Button4 : Button5; ev.same_screen = True;
    ev.type = ButtonPress;
    XSendEvent(dpy, target, True, ButtonPressMask, (XEvent *)&ev);
    XFlush(dpy);
}

static void send_key_ev(KeyCode kc, unsigned int mods) {
    XKeyEvent ev; memset(&ev, 0, sizeof(ev));
    ev.type = KeyPress; ev.display = dpy; ev.window = target; ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None; ev.time = CurrentTime; ev.x = 1; ev.y = 1; ev.x_root = 1; ev.y_root = 1;
    ev.state = mods; ev.keycode = kc; ev.same_screen = True;
    if (ev.keycode) XSendEvent(dpy, target, True, KeyPressMask, (XEvent *)&ev);
    XFlush(dpy);
}

/* Type one character: find a (keycode, level) pair in the server's
   keymap whose keysym is the character, and set Shift exactly when the
   pair is a shifted level. XKeysymToKeycode alone is wrong here: on
   Xvfb's fallback keymap the comma key has only `comma`, and `less`
   lives unshifted on another key, so sending XK_less with ShiftMask
   arrives as `>` instead of `<`. Real desktops have the full layout,
   but the harness must type correctly on this keymap too. */
static void send_char(char c) {
    KeySym ks = char_keysym(c);
    KeyCode kc = 0; unsigned int mods = 0;
    int min_kc = 0, max_kc = 0, per = 0;
    XDisplayKeycodes(dpy, &min_kc, &max_kc);
    KeySym *map = XGetKeyboardMapping(dpy, min_kc, max_kc - min_kc + 1, &per);
    if (map) {
        for (int k = min_kc; k <= max_kc && !kc; k++)
            for (int l = 0; l < per && !kc; l++)
                if (map[(k - min_kc) * per + l] == ks) {
                    kc = (KeyCode)k; mods = (l == 1 || l == 3) ? ShiftMask : 0;
                }
        XFree(map);
    }
    if (!kc) kc = XKeysymToKeycode(dpy, ks);
    send_key_ev(kc, mods);
}

static void send_key(KeySym ks, unsigned int mods) {
    send_key_ev(XKeysymToKeycode(dpy, ks), mods);
}

int main(int argc, char **argv) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "keys: no display\n"); return 1; }
    for (int tries = 0; tries < 50; tries++) {
        target = find_window(DefaultRootWindow(dpy));
        if (target) break;
        usleep(200 * 1000);
    }
    if (!target) { fprintf(stderr, "keys: Terminator window not found\n"); return 1; }
    XSetInputFocus(dpy, target, RevertToParent, CurrentTime);
    XFlush(dpy);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "type:", 5)) {
            for (const char *p = a + 5; *p; p++) {
                send_char(*p);
                usleep(30 * 1000);
            }
        } else if (!strncmp(a, "sleep:", 6)) {
            usleep(atoi(a + 6) * 1000);
        } else if (!strncmp(a, "click:", 6)) {
            int cx = 0, cy = 0;
            if (sscanf(a + 6, "%d,%d", &cx, &cy) == 2) send_click(cx, cy);
        } else if (!strncmp(a, "drag:", 5)) {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (sscanf(a + 5, "%d,%d,%d,%d", &x1, &y1, &x2, &y2) == 4) send_drag(x1, y1, x2, y2);
        } else if (!strncmp(a, "wheel:", 6)) {
            send_wheel(!strcmp(a + 6, "up"));
        } else {
            unsigned int mods = 0; const char *name = a;
            if (!strncmp(a, "ctrl+shift:", 11)) { mods = ControlMask | ShiftMask; name = a + 11; }
            else if (!strncmp(a, "ctrl:", 5)) { mods = ControlMask; name = a + 5; }
            else if (!strncmp(a, "shift:", 6)) { mods = ShiftMask; name = a + 6; }
            else if (!strncmp(a, "key:", 4)) { name = a + 4; }
            KeySym ks = XStringToKeysym(name);
            if (ks == NoSymbol && strlen(name) == 1) ks = char_keysym(name[0]);
            if (ks == NoSymbol) { fprintf(stderr, "keys: bad keysym %s\n", name); return 1; }
            send_key(ks, mods);
        }
        usleep(60 * 1000);
    }
    return 0;
}
