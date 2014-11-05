/* Copyright (c) 2014 The crouton Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * WebSocket server that acts as a X11 framebuffer server. It communicates
 * with the extension in Chromium OS. It sends framebuffer and cursor data,
 * and receives keyboard/mouse events.
 *
 */

#define _GNU_SOURCE /* for ppoll */
#include "websocket.h"
#include "fbserver-proto.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/tcp.h>
#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

static Display *dpy;

int damageEvent;
int fixesEvent;

/* shm entry cache */
struct cache_entry {
    uint64_t paddr; /* Address from PNaCl side */
    int fd;
    void *map; /* mmap-ed memory */
    size_t length; /* mmap length */
};

struct cache_entry cache[2];
int next_entry;

/* Remember which keys/buttons are currently pressed */
typedef enum { INVALID=0, MOUSE=1, KEYBOARD=2 } keybuttontype;
struct keybutton {
    keybuttontype type;
    uint32_t code; /* KeyCode or mouse button */
};

/* Store currently pressed keys/buttons in an array.
 * No valid entry on or after curmax. */
struct keybutton pressed[256];
int curmax = 0;

void kb_add(keybuttontype type, uint32_t code) {
    trueorabort(curmax < sizeof(pressed)/sizeof(struct keybutton),
                "Too many keys pressed");
    int firstfree = curmax;
    int i;
    for (i = 0; i < curmax; i++) {
        if (pressed[i].type == type && pressed[i].code == code) {
            /* Key already recorded */
            return;
        } else if (pressed[i].type == INVALID && firstfree == curmax) {
            firstfree = i;
        }
    }
    pressed[firstfree].type = type;
    pressed[firstfree].code = code;
    if (firstfree == curmax)
        curmax++;
}

void kb_remove(keybuttontype type, uint32_t code) {
    int lastvalid = -1;
    int i;
    for (i = 0; i < curmax; i++) {
        if (pressed[i].type == type && pressed[i].code == code) {
            pressed[i].type = INVALID;
        } else if (pressed[i].type != INVALID) {
            lastvalid = i;
        }
    }
    curmax = lastvalid+1;
}

void kb_release_all() {
    int i;
    log(2, "Releasing all keys...");
    for (i = 0; i < curmax; i++) {
        if (pressed[i].type == MOUSE) {
            log(2, "Mouse %d", pressed[i].code);
            XTestFakeButtonEvent(dpy, pressed[i].code, 0, CurrentTime);
        } else if (pressed[i].type == KEYBOARD) {
            log(2, "Keyboard %d", pressed[i].code);
            XTestFakeKeyEvent(dpy, pressed[i].code, 0, CurrentTime);
        }
    }
    curmax = 0;
}

/* X11-related functions */

static int xerror_handler(Display *dpy, XErrorEvent *e) {
    return 0;
}

/* Register XDamage events for a given Window. */
static void register_damage(Display *dpy, Window win) {
    XWindowAttributes attrib;
    if (XGetWindowAttributes(dpy, win, &attrib) &&
            !attrib.override_redirect) {
        XDamageCreate(dpy, win, XDamageReportRawRectangles);
    }
}

static int init_display(char* name) {
    dpy = XOpenDisplay(name);

    if (!dpy) {
        error("Cannot open display.");
        return -1;
    }

    /* We need XTest, XDamage and XFixes */
    int event, error, major, minor;
    if (!XTestQueryExtension(dpy, &event, &error, &major, &minor)) {
        error("XTest not available!");
        return -1;
    }

    if (!XDamageQueryExtension(dpy, &damageEvent, &error)) {
        error("XDamage not available!");
        return -1;
    }

    if (!XFixesQueryExtension(dpy, &fixesEvent, &error)) {
        error("XFixes not available!");
        return -1;
    }

    /* Get notified when new windows are created. */
    Window root = DefaultRootWindow(dpy);
    XSelectInput(dpy, root, SubstructureNotifyMask);

    /* Register damage events for existing windows */
    Window rootp, parent;
    Window *children;
    unsigned int i, nchildren;
    XQueryTree(dpy, root, &rootp, &parent, &children, &nchildren);

    /* FIXME: We never reset the handler, is that a good thing? */
    XSetErrorHandler(xerror_handler);

    register_damage(dpy, root);
    for (i = 0; i < nchildren; i++) {
        register_damage(dpy, children[i]);
    }

    /* Register for cursor events */
    XFixesSelectCursorInput(dpy, root, XFixesDisplayCursorNotifyMask);

    return 0;
}

/* Change resolution using external handler.
 * Reply must be a resolution in "canonical" form: <w>x<h>[_<rate>] */
/* FIXME: Maybe errors here should not be fatal... */
void change_resolution(const struct resolution* rin) {
    /* Setup parameters and run command */
    char arg1[32], arg2[32];
    int c;
    c = snprintf(arg1, sizeof(arg1), "%d", rin->width);
    trueorabort(c > 0, "snprintf");
    c = snprintf(arg2, sizeof(arg2), "%d", rin->height);
    trueorabort(c > 0, "snprintf");

    char* cmd = "setres";
    char* args[] = {cmd, arg1, arg2, NULL};
    char buffer[256];
    log(2, "Running %s %s %s", cmd, arg1, arg2);
    c = popen2(cmd, args, NULL, 0, buffer, sizeof(buffer));
    trueorabort(c > 0, "popen2");

    /* Parse output */
    buffer[c < sizeof(buffer) ? c : (sizeof(buffer)-1)] = 0;
    log(2, "Result: %s", buffer);
    char* cut = strchr(buffer, '_');
    if (cut) *cut = 0;
    cut = strchr(buffer, 'x');
    trueorabort(cut, "Invalid answer: %s", buffer);
    *cut = 0;

    char* endptr;
    long nwidth = strtol(buffer, &endptr, 10);
    trueorabort(buffer != endptr && *endptr == '\0',
                    "Invalid width: '%s'", buffer);
    long nheight = strtol(cut+1, &endptr, 10);
    trueorabort(cut+1 != endptr && (*endptr == '\0' || *endptr == '\n'),
                    "Invalid height: '%s'", cut+1);
    log(1, "New resolution %ld x %ld", nwidth, nheight);

    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct resolution)];
    struct resolution* r = (struct resolution*)(reply_raw+FRAMEMAXHEADERSIZE);
    r->type = 'R';
    r->width = nwidth;
    r->height = nheight;
    socket_client_write_frame(reply_raw, sizeof(*r), WS_OPCODE_BINARY, 1);
}

void close_mmap(struct cache_entry* entry) {
    if (!entry->map)
        return;

    log(2, "Closing mmap %p %zu %d", entry->map, entry->length, entry->fd);
    munmap(entry->map, entry->length);
    close(entry->fd);
    entry->map = NULL;
}

/* Find NaCl/Chromium shm memory using external handler.
 * Reply must be in the form PID:file */
struct cache_entry* find_shm(uint64_t paddr, uint64_t sig, size_t length) {
    struct cache_entry* entry = NULL;

    /* Find entry in cache */
    if (cache[0].paddr == paddr)
        entry = &cache[0];
    else if (cache[1].paddr == paddr)
        entry = &cache[1];
    else {
        /* Not found: erase an existing entry. */
        entry = &cache[next_entry];
        next_entry = (next_entry + 1) % 2;
        close_mmap(entry);
    }

    int try;
    for (try = 0; try < 2; try++) {
        /* Check signature */
        if (entry->map) {
            if (*((uint64_t*)entry->map) == sig) {
                return entry;
            }
            error("Invalid signature, fetching new shm!");
            close_mmap(entry);
        }

        /* Setup parameters and run command */
        char arg1[32], arg2[32];
        int c;

        c = snprintf(arg1, sizeof(arg1), "%08lx", (long)paddr & 0xffffffff);
        trueorabort(c > 0, "snprintf");
        int i, p = 0;
        for (i = 0; i < 8; i++) {
            c = snprintf(arg2+p, sizeof(arg2)-p, "%02x",
                         ((uint8_t*)&sig)[i]);
            trueorabort(c > 0, "snprintf");
            p += c;
        }

        char* cmd = "croutonfindnacl";
        char* args[] = {cmd, arg1, arg2, NULL};
        char buffer[256];
        log(2, "Running %s %s %s", cmd, arg1, arg2);
        c = popen2(cmd, args, NULL, 0, buffer, sizeof(buffer));
        if (c <= 0) {
            error("Error running helper.");
            return NULL;
        }
        buffer[c < sizeof(buffer) ? c : (sizeof(buffer)-1)] = 0;
        log(2, "Result: %s", buffer);

        /* Parse PID:file output */
        char* cut = strchr(buffer, ':');
        if (!cut) {
            error("No ':' in helper reply: %s.", cut);
            return NULL;
        }
        *cut = 0;

        char* endptr;
        long pid = strtol(buffer, &endptr, 10);
        if(buffer == endptr || *endptr != '\0') {
            error("Invalid pid: %s", buffer);
            return NULL;
        }
        char* file = cut+1;
        log(2, "PID:%ld, FILE:%s", pid, file);

        entry->paddr = paddr;
        entry->fd = open(file, O_RDWR);
        if (entry->fd < 0) {
            error("Cannot open file %s\n", file);
            return NULL;
        }

        entry->length = length;
        entry->map = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED,
                          entry->fd, 0);
        if (!entry->map) {
            error("Cannot mmap %s\n", file);
            close(entry->fd);
            return NULL;
        }

        log(2, "mmap ok %p %zu %d", entry->map, entry->length, entry->fd);
    }

    error("Cannot find shm.");
    return NULL;
}

/* WebSocket functions */

XImage* img = NULL;
XShmSegmentInfo shminfo;

/* Write framebuffer image to websocket/shm */
int write_image(const struct screen* screen) {
    char reply_raw[FRAMEMAXHEADERSIZE+sizeof(struct screen_reply)];
    struct screen_reply* reply =
                           (struct screen_reply*)(reply_raw+FRAMEMAXHEADERSIZE);
    int refresh = 0;

    memset(reply_raw, 0, sizeof(*reply_raw));

    reply->type = 'S';
    reply->width = screen->width;
    reply->height = screen->height;

    /* Allocate XShmImage */
    if (!img || img->width != screen->width || img->height != screen->height) {
        if (img) {
            XDestroyImage(img);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, 0);
        }

        /* FIXME: Some error checking should happen here... */
        img = XShmCreateImage(dpy, DefaultVisual(dpy, 0), 24,
                              ZPixmap, NULL, &shminfo,
                              screen->width, screen->height);
        trueorabort(img, "XShmCreateImage");
        shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line*img->height,
                               IPC_CREAT|0777);
        trueorabort(shminfo.shmid != -1, "shmget");
        shminfo.shmaddr = img->data = shmat(shminfo.shmid, 0, 0);
        trueorabort(shminfo.shmaddr != (void*)-1, "shmat");
        shminfo.readOnly = False;
        int ret = XShmAttach(dpy, &shminfo);
        trueorabort(ret, "XShmAttach");
        /* Force refresh */
        refresh = 1;
    }

    if (screen->refresh) {
        log(1, "Force refresh from client.");
        /* refresh forced by the client */
        refresh = 1;
    }

    XEvent ev;
    /* Register damage on new windows */
    while (XCheckTypedEvent(dpy, MapNotify, &ev)) {
        register_damage(dpy, ev.xcreatewindow.window);
        refresh = 1;
    }

    /* Check for damage */
    while (XCheckTypedEvent(dpy, damageEvent+XDamageNotify, &ev)) {
        refresh = 1;
    }

    /* Check for cursor events */
    reply->cursor_updated = 0;
    while (XCheckTypedEvent(dpy, fixesEvent+XFixesCursorNotify, &ev)) {
        XFixesCursorNotifyEvent* curev = (XFixesCursorNotifyEvent*)&ev;
        char* name = XGetAtomName(dpy, curev->cursor_name);
        log(2, "cursor! %ld %s", curev->cursor_serial, name);
        XFree(name);
        reply->cursor_updated = 1;
        reply->cursor_serial = curev->cursor_serial;
    }

    /* No update */
    if (!refresh) {
        reply->shm = 0;
        reply->updated = 0;
        socket_client_write_frame(reply_raw, sizeof(*reply),
                                  WS_OPCODE_BINARY, 1);
        return 0;
    }

    /* Get new image from framebuffer */
    XShmGetImage(dpy, DefaultRootWindow(dpy), img, 0, 0, AllPlanes);

    int size = img->bytes_per_line * img->height;

    trueorabort(size == screen->width*screen->height*4,
                    "Invalid screen byte count");

    if (screen->shm) {
        struct cache_entry* entry = find_shm(screen->paddr, screen->sig, size);

        reply->shm = 1;
        reply->updated = 1;
        reply->shmfailed = 0;

        if (entry && entry->map) {
            if (size == entry->length) {
                memcpy(entry->map, img->data, size);
                msync(entry->map, size, MS_SYNC);
            } else {
                /* This should never happen (it means the client passed an
                 * outdated buffer to us). */
                error("Invalid shm entry length (client bug!).");
                reply->shmfailed = 1;
            }
        } else {
            /* Keep the flow going, even if we cannot find the shm. Next time
             * the NaCl client reallocates the buffer, we are likely to be able
             * to find it. */
            error("Cannot find shm, moving on...");
            reply->shmfailed = 1;
        }

        /* Confirm write is done */
        socket_client_write_frame(reply_raw, sizeof(*reply),
                                  WS_OPCODE_BINARY, 1);
    } else {
        trueorabort(0, "Non-SHM path is currently broken!");
        /* Confirm write is done */
        reply->shm = 0;
        reply->updated = 1;
        socket_client_write_frame(reply_raw, sizeof(*reply),
                                  WS_OPCODE_BINARY, 0);
        /* FIXME: This is broken with current API... */
        socket_client_write_frame(img->data, size, WS_OPCODE_BINARY, 1);
    }

    return 0;
}

/* Write cursor image to websocket */
int write_cursor() {
    XFixesCursorImage *img = XFixesGetCursorImage(dpy);
    int size = img->width*img->height;
    const int replylength = sizeof(struct cursor_reply)+sizeof(uint32_t)*size;
    char reply_raw[FRAMEMAXHEADERSIZE+replylength];
    struct cursor_reply* reply =
                           (struct cursor_reply*)(reply_raw+FRAMEMAXHEADERSIZE);

    memset(reply_raw, 0, sizeof(*reply_raw));

    reply->type = 'P';
    reply->width = img->width;
    reply->height = img->height;
    reply->xhot = img->xhot;
    reply->yhot = img->yhot;
    reply->cursor_serial = img->cursor_serial;
    /* This casts long[] to uint32_t[] */
    int i;
    for (i = 0; i < size; i++)
        reply->pixels[i] = img->pixels[i];

    socket_client_write_frame(reply_raw, replylength, WS_OPCODE_BINARY, 1);
    XFree(img);

    return 0;
}

/* Check if a packet size is correct */
int check_size(int length, int target, char* error) {
    if (length != target) {
        error("Invalid %s packet (%d != %d)", error, length, target);
        socket_client_close(0);
        return 0;
    }
    return 1;
}

void usage(char* argv0) {
    fprintf(stderr, "%s [-v 0-3] display\n", argv0);
    exit(1);
}

int main(int argc, char** argv) {
    int c;
    while ((c = getopt(argc, argv, "v:")) != -1) {
        switch (c) {
        case 'v':
            verbose = atoi(optarg);
            break;
        default:
            usage(argv[0]);
        }
    }

    if (optind != argc-1)
        usage(argv[0]);

    char* display = argv[optind];

    trueorabort(display[0] == ':', "Invalid display: '%s'", display);

    char* endptr;
    int displaynum = (int)strtol(display+1, &endptr, 10);
    trueorabort(display+1 != endptr && (*endptr == '\0' || *endptr == '.'),
                    "Invalid display number: '%s'", display);

    init_display(display);
    socket_server_init(PORT_BASE+displaynum);

    unsigned char buffer[BUFFERSIZE];
    int length;

    while (1) {
        socket_server_accept(VERSION);
        while (1) {
            length = socket_client_read_frame((char*)buffer, sizeof(buffer));
            if (length < 0) {
                socket_client_close(1);
                break;
            }

            if (length < 1) {
                error("Invalid packet from client (size <1).");
                socket_client_close(0);
                break;
            }

            switch (buffer[0]) {
            case 'S': /* Screen */
                if (!check_size(length, sizeof(struct screen), "screen"))
                    break;
                write_image((struct screen*)buffer);
                break;
            case 'P': /* Cursor */
                if (!check_size(length, sizeof(struct cursor), "cursor"))
                    break;
                write_cursor();
                break;
            case 'R': /* Resolution */
                if (!check_size(length, sizeof(struct resolution),
                                "resolution"))
                    break;
                change_resolution((struct resolution*)buffer);
                break;
            case 'K': { /* Key */
                if (!check_size(length, sizeof(struct key), "key"))
                    break;
                struct key* k = (struct key*)buffer;
                KeyCode kc = XKeysymToKeycode(dpy, k->keysym);
                log(2, "Key: ks=%04x kc=%04x\n", k->keysym, kc);
                if (kc != 0) {
                    XTestFakeKeyEvent(dpy, kc, k->down, CurrentTime);
                    if (k->down) kb_add(KEYBOARD, kc);
                    else kb_remove(KEYBOARD, kc);
                } else {
                    error("Invalid keysym %04x.", k->keysym);
                }
                break;
            }
            case 'C': { /* Click */
                if (!check_size(length, sizeof(struct mouseclick),
                                "mouseclick"))
                    break;
                struct mouseclick* mc = (struct mouseclick*)buffer;
                XTestFakeButtonEvent(dpy, mc->button, mc->down, CurrentTime);
                if (mc->down) kb_add(MOUSE, mc->button);
                else kb_remove(MOUSE, mc->button);
                break;
            }
            case 'M': { /* Mouse move */
                if (!check_size(length, sizeof(struct mousemove), "mousemove"))
                    break;
                struct mousemove* mm = (struct mousemove*)buffer;
                XTestFakeMotionEvent(dpy, 0, mm->x, mm->y, CurrentTime);
                break;
            }
            case 'Q': /* "Quit": release all keys */
                kb_release_all();
                break;
            default:
                error("Invalid packet from client (%d).", buffer[0]);
                socket_client_close(0);
            }
        }
        socket_client_close(0);
        kb_release_all();
        close_mmap(&cache[0]);
        close_mmap(&cache[1]);
    }

    return 0;
}
