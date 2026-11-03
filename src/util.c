/* Terminator — util.c
 * Small helpers shared by several stages: safe path joining,
 *  * millisecond sleep, and the framed-packet read/write primitives used
 *  * on the GUI <-> tab-process pipes.
 */
#include "common.h"

/* Join dir + "/" + name into dst, truncating safely (no snprintf
   truncation warnings, and over-long paths degrade gracefully). */
void join_path(char *dst, size_t dstsz, const char *dir, const char *name) {
    size_t pos = 0;
    while (dir[pos] && pos + 1 < dstsz) { dst[pos] = dir[pos]; pos++; }
    if (pos + 1 < dstsz) dst[pos++] = '/';
    size_t i = 0;
    while (name[i] && pos + 1 < dstsz) { dst[pos++] = name[i++]; }
    dst[pos] = '\0';
}
void sleep_ms(int ms) { struct timespec ts = {0, (long)ms * 1000000}; nanosleep(&ts, NULL); }


/* Framed packets used on the GUI <-> tab-process pipes in process mode.
   Header: 1 type byte + uint32 length (native order, same machine). */
int write_all_fd(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return -1; } p += w; n -= (size_t)w; }
    return 0;
}
int write_packet(int fd, char type, const void *payload, uint32_t len) {
    unsigned char hdr[5]; hdr[0] = (unsigned char)type; memcpy(hdr + 1, &len, 4);
    if (write_all_fd(fd, hdr, 5) < 0) return -1;
    if (len && write_all_fd(fd, payload, len) < 0) return -1;
    return 0;
}
int read_all_fd(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    while (n) { ssize_t r = read(fd, p, n); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
