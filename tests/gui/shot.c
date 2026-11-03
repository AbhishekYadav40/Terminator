/* Dump the Terminator window to a PPM file. Usage: shot <out.ppm> */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *dpy;
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
int main(int argc, char **argv) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    Window w = find_window(DefaultRootWindow(dpy));
    if (!w) { fprintf(stderr, "shot: window not found\n"); return 1; }
    XWindowAttributes at; XGetWindowAttributes(dpy, w, &at);
    XImage *im = XGetImage(dpy, w, 0, 0, at.width, at.height, AllPlanes, ZPixmap);
    if (!im) return 1;
    FILE *f = fopen(argv[1], "wb");
    fprintf(f, "P6\n%d %d\n255\n", at.width, at.height);
    for (int y = 0; y < at.height; y++) for (int x = 0; x < at.width; x++) {
        unsigned long px = XGetPixel(im, x, y);
        unsigned char rgb[3] = { (unsigned char)((px >> 16) & 0xff), (unsigned char)((px >> 8) & 0xff), (unsigned char)(px & 0xff) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
