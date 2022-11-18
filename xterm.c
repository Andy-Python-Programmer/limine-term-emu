#define _XOPEN_SOURCE 600

#include <stddef.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <config.h>
#include <string.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "terminal/backends/framebuffer.h"
#include "terminal/term.h"

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

#define DEFAULT_COLS 120
#define DEFAULT_ROWS 35

#define WINDOW_WIDTH (DEFAULT_COLS * (FONT_WIDTH + 1))
#define WINDOW_HEIGHT (DEFAULT_ROWS * FONT_HEIGHT)

#define WINDOW_TITLE "Limine Terminal"

static int pty_master;
bool is_running = true;

struct term_context *context = NULL;
uint8_t *framebuffer = NULL;

static void *read_from_pty(void *arg) {
    (void)arg;

    int read_bytes;
    char buffer[512];

    while (is_running) {
        read_bytes = read(pty_master, buffer, sizeof(buffer));
        if (read_bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }

            printf("read_from_pty: read() failed (errno=%d)\n", errno);
            break;
        }

        term_write(context, buffer, read_bytes);
    }

    is_running = false;
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Window Creation
    struct winsize win_size = {
        .ws_col = DEFAULT_COLS,
        .ws_row = DEFAULT_ROWS,
        .ws_xpixel = WINDOW_WIDTH,
        .ws_ypixel = WINDOW_HEIGHT,
    };

    // PTY setup
    pty_master = posix_openpt(O_RDWR);

    unlockpt(pty_master);
    grantpt(pty_master);

    int pid = fork();

    if (pid == 0) {
        int pty_slave = open(ptsname(pty_master), O_RDWR | O_NOCTTY);

        close(pty_master);
        ioctl(pty_slave, TIOCSCTTY, 0);
        ioctl(pty_slave, TIOCSWINSZ, &win_size);

        dup2(pty_slave, 0);
        dup2(pty_slave, 1);
        dup2(pty_slave, 2);
        close(pty_slave);

        execlp("/usr/bin/bash", "/usr/bin/bash", NULL);
    }

    Display *display = XOpenDisplay(NULL);
    int screen = XDefaultScreen(display);

    int blackColor = BlackPixel(display, screen);
    int whiteColor = WhitePixel(display, screen);

    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(XSetWindowAttributes));

    attr.event_mask = ExposureMask | KeyPressMask;
    attr.background_pixel = BlackPixel(display, screen);

    int depth = DefaultDepth(display, screen);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, win_size.ws_xpixel, win_size.ws_ypixel, 0, blackColor, blackColor);

    XSelectInput(display, window, StructureNotifyMask | KeyPressMask | KeyReleaseMask);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, whiteColor);

    XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, (unsigned char *)WINDOW_TITLE, strlen(WINDOW_TITLE));

    if (pty_master < 0) {
        printf("Could not open a PTY\n");
        return 1;
    }

    // Framebuffer initialization.
    size_t framebuffer_len = win_size.ws_xpixel * win_size.ws_ypixel * 4;

    framebuffer = malloc(framebuffer_len);
    memset(framebuffer, 0, framebuffer_len);

    context = fbterm_init(
        malloc,
        (uint32_t *)framebuffer, WINDOW_WIDTH, WINDOW_HEIGHT, WINDOW_WIDTH * 4,
        NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 1, 1, 0
    );

    pthread_t pty_thread;

    if (pthread_create(&pty_thread, NULL, read_from_pty, NULL) != 0) {
        goto cleanup;
    }

    while (true) {
        XEvent e;
        XNextEvent(display, &e);

        // The X server generates this event type whenever a client application 
        // changes the window's state from unmapped to mapped 
        if (e.type == MapNotify) {
            break;
        }
    }

    XImage *image = XCreateImage(display, DefaultVisual(display, screen), depth, ZPixmap, 0, (char *)framebuffer, win_size.ws_xpixel, win_size.ws_ypixel, 32, 0);

    for (;;) {
        XPutImage(display, window, gc, image, 0, 0, 0, 0, win_size.ws_xpixel, win_size.ws_ypixel);
    }

cleanup:
    XFreeGC(display, gc);
	XDestroyWindow(display, window);
	XCloseDisplay(display);	
    return 0;
}
