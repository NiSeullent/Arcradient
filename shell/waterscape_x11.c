/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * shell/waterscape_x11.c - Linux X11 + MIT-SHM Zero-Copy Desktop Window Presenter
 *
 * Uses MIT-SHM (`XShmCreateImage` + `shmget`/`shmat` + `XShmPutImage`) so the
 * multi-core 64x64 tile + SIMD framebuffer is presented directly from shared RAM
 * to the X11 server with zero socket copies (automatically falling back to
 * `XPutImage` over remote/networked X11 connections).
 * Includes 60 FPS non-blocking `select()` tick loop, X11 `CLIPBOARD` (`UTF8_STRING` /
 * `XA_STRING`) sync, full keyboard/mouse modifiers, and `_NET_WM_STATE_FULLSCREEN`.
 */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#endif

#include "waterscape_presenter.h"

#if !defined(_WIN32)

#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *waterscape_present_backend_name(WaterscapePresentBackend backend) {
    switch (backend) {
    case WS_PRESENT_WIN32_D3D11:  return "Win32 Direct3D 11 + DXGI SwapChain";
    case WS_PRESENT_WIN32_D3D9:   return "Win32 Direct3D 9 Hardware HAL";
    case WS_PRESENT_WIN32_OPENGL: return "Win32 OpenGL Hardware ICD";
    case WS_PRESENT_WIN32_GDI:    return "Win32 GDI DIBSection";
    case WS_PRESENT_X11_MITSHM:   return "X11 MIT-SHM Zero-Copy (XShmPutImage)";
    case WS_PRESENT_X11_XIMAGE:   return "X11 Network-Safe XPutImage";
    default:                      return "None";
    }
}

bool waterscape_show_file_dialog(bool is_save, const char *default_filename_utf8,
                                 char *out_path_utf8, size_t out_cap) {
    (void)is_save;
    (void)default_filename_utf8;
    if (!out_path_utf8 || out_cap == 0u) return false;
    out_path_utf8[0] = '\0';

    const char *env_path = getenv("WATERSCAPE_FILE_DIALOG_PATH");
    if (env_path && env_path[0] != '\0') {
        arcr_str_copy(out_path_utf8, out_cap, env_path);
        return true;
    }
    return false;
}

#if defined(WATERSCAPE_HAVE_X11)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/shm.h>

typedef struct X11PresenterSurface {
    Display                 *dpy;
    int                      screen;
    Visual                  *visual;
    int                      depth;
    Window                   win;
    GC                       gc;
    WaterscapePresentBackend backend;
    bool                     allow_shm;
    bool                     os_fullscreen;
    WaterscapeFrameMetrics   fm;

    XImage                  *ximg;
    XShmSegmentInfo          shminfo;
    bool                     shm_attached;
    uint32_t                *pixels;
    int32_t                  width;
    int32_t                  height;
    int32_t                  stride_px;

    Cursor                   cur_arrow;
    Cursor                   cur_pointer;
    Cursor                   cur_text;
    ArcrCursorKind           active_cursor;

    /* X11 Selection / Clipboard & EWMH Fullscreen Atoms */
    Atom                     atom_clipboard;
    Atom                     atom_targets;
    Atom                     atom_utf8;
    Atom                     atom_text;
    Atom                     atom_incr;
    Atom                     atom_clip_prop;
    Atom                     atom_wm_state;
    Atom                     atom_wm_fullscreen;

    char                    *owned_clip;
    size_t                   owned_clip_len;
    bool                     owns_clipboard;
    char                     last_synced_clip[8192];
} X11PresenterSurface;

static int32_t x11_detect_dpi(Display *dpy, int screen) {
    const char *env_dpi = getenv("WATERSCAPE_FORCE_DPI");
    if (env_dpi && env_dpi[0] != '\0') {
        int v = atoi(env_dpi);
        if (v >= 72 && v <= 384) return (int32_t)v;
    }

    const char *rms = XResourceManagerString(dpy);
    if (rms) {
        const char *p = strstr(rms, "Xft.dpi:");
        if (p) {
            p += 8;
            while (*p == ' ' || *p == '\t') ++p;
            double xft_dpi = atof(p);
            if (xft_dpi >= 72.0 && xft_dpi <= 384.0) {
                return (int32_t)(xft_dpi + 0.5);
            }
        }
    }

    int px_w = DisplayWidth(dpy, screen);
    int mm_w = DisplayWidthMM(dpy, screen);
    if (px_w > 0 && mm_w > 20) {
        double raw = ((double)px_w * 25.4) / (double)mm_w;
        if (raw >= 84.0 && raw <= 108.0) return 96;
        if (raw >= 72.0 && raw <= 384.0) return (int32_t)(raw + 0.5);
    }
    return 96;
}

static volatile int g_x11_shm_err_trapped = 0;

static int x11_shm_error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    g_x11_shm_err_trapped = 1;
    return 0;
}

static void x11_destroy_surface(X11PresenterSurface *s) {
    if (!s || !s->dpy) return;
    if (s->shm_attached) {
        XShmDetach(s->dpy, &s->shminfo);
        XSync(s->dpy, False);
        s->shm_attached = false;
    }
    if (s->ximg) {
        if (s->backend == WS_PRESENT_X11_MITSHM) {
            s->ximg->data = NULL;
            XDestroyImage(s->ximg);
            if (s->shminfo.shmaddr && s->shminfo.shmaddr != (char *)-1) {
                shmdt(s->shminfo.shmaddr);
            }
        } else {
            /* XDestroyImage frees malloc'd `s->ximg->data` automatically */
            XDestroyImage(s->ximg);
        }
        s->ximg   = NULL;
        s->pixels = NULL;
    }
    s->shminfo.shmaddr = NULL;
    s->shminfo.shmid   = -1;
}

static bool x11_create_surface(X11PresenterSurface *s, int32_t w, int32_t h) {
    if (!s || !s->dpy || w <= 0 || h <= 0) return false;
    x11_destroy_surface(s);

    if (s->allow_shm && XShmQueryExtension(s->dpy)) {
        memset(&s->shminfo, 0, sizeof(s->shminfo));
        s->shminfo.shmid = -1;
        XImage *img = XShmCreateImage(s->dpy, s->visual, (unsigned int)s->depth,
                                      ZPixmap, NULL, &s->shminfo,
                                      (unsigned int)w, (unsigned int)h);
        if (img && img->bits_per_pixel == 32) {
            size_t total_bytes = (size_t)img->bytes_per_line * (size_t)img->height;
            s->shminfo.shmid = shmget(IPC_PRIVATE, total_bytes, IPC_CREAT | 0600);
            if (s->shminfo.shmid >= 0) {
                s->shminfo.shmaddr = (char *)shmat(s->shminfo.shmid, NULL, 0);
                if (s->shminfo.shmaddr != (char *)-1) {
                    img->data           = s->shminfo.shmaddr;
                    s->shminfo.readOnly = False;

                    g_x11_shm_err_trapped = 0;
                    XErrorHandler prev_h = XSetErrorHandler(x11_shm_error_handler);
                    Status attached = XShmAttach(s->dpy, &s->shminfo);
                    XSync(s->dpy, False);
                    XSetErrorHandler(prev_h);

                    /* Mark segment for automatic kernel cleanup once detached */
                    shmctl(s->shminfo.shmid, IPC_RMID, NULL);

                    if (attached && !g_x11_shm_err_trapped) {
                        s->ximg         = img;
                        s->shm_attached = true;
                        s->pixels       = (uint32_t *)(void *)s->shminfo.shmaddr;
                        s->width        = w;
                        s->height       = h;
                        s->stride_px    = img->bytes_per_line / 4;
                        s->backend      = WS_PRESENT_X11_MITSHM;
                        return true;
                    }
                    shmdt(s->shminfo.shmaddr);
                } else {
                    shmctl(s->shminfo.shmid, IPC_RMID, NULL);
                }
            }
        }
        if (img) {
            img->data = NULL;
            XDestroyImage(img);
        }
    }

    /* Fallback: Standard XImage + XPutImage */
    char *raw = (char *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!raw) return false;
    XImage *img = XCreateImage(s->dpy, s->visual, (unsigned int)s->depth,
                               ZPixmap, 0, raw,
                               (unsigned int)w, (unsigned int)h, 32, w * 4);
    if (!img) {
        free(raw);
        return false;
    }
    s->ximg      = img;
    s->pixels    = (uint32_t *)(void *)raw;
    s->width     = w;
    s->height    = h;
    s->stride_px = img->bytes_per_line / 4;
    s->backend   = WS_PRESENT_X11_XIMAGE;
    return true;
}

static void x11_present(X11PresenterSurface *s, ArcrBrowserShell *shell) {
    if (!s || !s->ximg || !s->pixels || s->width <= 0 || s->height <= 0) return;
    arcr_shell_render_frame(shell, s->pixels, s->width, s->height, s->stride_px);

    if (s->backend == WS_PRESENT_X11_MITSHM && s->shm_attached) {
        XShmPutImage(s->dpy, s->win, s->gc, s->ximg,
                     0, 0, 0, 0, (unsigned int)s->width, (unsigned int)s->height, False);
    } else {
        XPutImage(s->dpy, s->win, s->gc, s->ximg,
                  0, 0, 0, 0, (unsigned int)s->width, (unsigned int)s->height);
    }
    XFlush(s->dpy);
}

static void x11_apply_cursor(X11PresenterSurface *s, ArcrCursorKind ck) {
    if (ck == s->active_cursor) return;
    s->active_cursor = ck;
    Cursor c = s->cur_arrow;
    if (ck == ARCR_CURSOR_POINTER) c = s->cur_pointer;
    else if (ck == ARCR_CURSOR_TEXT) c = s->cur_text;
    XDefineCursor(s->dpy, s->win, c);
}

static void x11_set_utf8_title(Display *dpy, Window win, const char *utf8_title) {
    XStoreName(dpy, win, utf8_title);
    Atom utf8_str = XInternAtom(dpy, "UTF8_STRING", False);
    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom net_icon = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    if (utf8_str != None && net_name != None) {
        XChangeProperty(dpy, win, net_name, utf8_str, 8, PropModeReplace,
                        (const unsigned char *)utf8_title, (int)strlen(utf8_title));
    }
    if (utf8_str != None && net_icon != None) {
        XChangeProperty(dpy, win, net_icon, utf8_str, 8, PropModeReplace,
                        (const unsigned char *)utf8_title, (int)strlen(utf8_title));
    }
}

/* EWMH `_NET_WM_STATE_FULLSCREEN` window state synchronization */
static void x11_sync_os_fullscreen(X11PresenterSurface *s, bool want_fs) {
    if (!s || !s->dpy || s->os_fullscreen == want_fs) return;
    s->os_fullscreen = want_fs;
    if (s->atom_wm_state == None || s->atom_wm_fullscreen == None) return;

    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type                 = ClientMessage;
    xev.xclient.window       = s->win;
    xev.xclient.message_type = s->atom_wm_state;
    xev.xclient.format       = 32;
    xev.xclient.data.l[0]    = want_fs ? 1L : 0L; /* 1 = _NET_WM_STATE_ADD, 0 = REMOVE */
    xev.xclient.data.l[1]    = (long)s->atom_wm_fullscreen;
    xev.xclient.data.l[2]    = 0L;
    xev.xclient.data.l[3]    = 1L; /* Normal application source indication */
    XSendEvent(s->dpy, RootWindow(s->dpy, s->screen), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(s->dpy);
}

/* Claim X11 `CLIPBOARD` + `PRIMARY` ownership when `arcr_platform_clipboard_*` changes */
static void x11_push_clipboard_if_changed(X11PresenterSurface *s) {
    if (!s || !s->dpy || s->atom_clipboard == None) return;
    char buf[8192];
    buf[0] = '\0';
    if (!arcr_platform_clipboard_get_utf8(buf, sizeof(buf)) || buf[0] == '\0') {
        return;
    }
    if (strcmp(buf, s->last_synced_clip) == 0 && s->owns_clipboard) {
        return;
    }

    size_t len = strlen(buf);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return;
    memcpy(copy, buf, len + 1u);

    free(s->owned_clip);
    s->owned_clip     = copy;
    s->owned_clip_len = len;
    snprintf(s->last_synced_clip, sizeof(s->last_synced_clip), "%s", buf);

    XSetSelectionOwner(s->dpy, s->atom_clipboard, s->win, CurrentTime);
    XSetSelectionOwner(s->dpy, XA_PRIMARY, s->win, CurrentTime);
    s->owns_clipboard = (XGetSelectionOwner(s->dpy, s->atom_clipboard) == s->win);
    XFlush(s->dpy);
}

static bool x11_request_selection_target(X11PresenterSurface *s,
                                         Atom selection_atom,
                                         Atom target_atom,
                                         char *out_buf,
                                         size_t out_cap) {
    if (!s || !s->dpy || selection_atom == None || target_atom == None ||
        !out_buf || out_cap == 0u) {
        return false;
    }

    XDeleteProperty(s->dpy, s->win, s->atom_clip_prop);
    XConvertSelection(s->dpy, selection_atom, target_atom,
                      s->atom_clip_prop, s->win, CurrentTime);
    XFlush(s->dpy);

    int x11_fd = ConnectionNumber(s->dpy);
    uint64_t deadline = arcr_platform_monotonic_usec() + 40000ULL; /* Max 40 ms wait */

    while (arcr_platform_monotonic_usec() < deadline) {
        XEvent sev;
        if (XCheckTypedWindowEvent(s->dpy, s->win, SelectionNotify, &sev)) {
            if (sev.xselection.property == None) {
                return false;
            }
            Atom actual_type = None;
            int actual_fmt = 0;
            unsigned long nitems = 0UL;
            unsigned long bytes_after = 0UL;
            unsigned char *prop_data = NULL;
            int rc = XGetWindowProperty(s->dpy, s->win, s->atom_clip_prop,
                                        0L, 65536L, True, AnyPropertyType,
                                        &actual_type, &actual_fmt,
                                        &nitems, &bytes_after, &prop_data);
            if (rc != Success || !prop_data) {
                if (prop_data) XFree(prop_data);
                return false;
            }
            if ((s->atom_incr != None && actual_type == s->atom_incr) ||
                actual_fmt != 8 || nitems == 0UL) {
                XFree(prop_data);
                return false;
            }
            size_t copy_len = (size_t)nitems;
            if (copy_len >= out_cap) copy_len = out_cap - 1u;
            memcpy(out_buf, prop_data, copy_len);
            out_buf[copy_len] = '\0';
            XFree(prop_data);
            return copy_len > 0u;
        }

        uint64_t now = arcr_platform_monotonic_usec();
        if (now >= deadline) break;
        uint64_t rem = deadline - now;
        if (rem > 5000ULL) rem = 5000ULL;
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = (long)rem;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds);
        (void)select(x11_fd + 1, &rfds, NULL, NULL, &tv);
    }
    return false;
}

/* Pull external X11 `CLIPBOARD` (`UTF8_STRING` -> `XA_STRING`) into platform clipboard */
static void x11_pull_clipboard(X11PresenterSurface *s) {
    if (!s || !s->dpy || s->atom_clipboard == None) return;

    Atom sel_atom = s->atom_clipboard;
    Window owner = XGetSelectionOwner(s->dpy, sel_atom);
    if (owner == s->win) return; /* Already synced in-process */
    if (owner == None) {
        sel_atom = XA_PRIMARY;
        owner = XGetSelectionOwner(s->dpy, sel_atom);
        if (owner == None || owner == s->win) return;
    }

    char buf[8192];
    buf[0] = '\0';
    bool ok = false;
    if (s->atom_utf8 != None) {
        ok = x11_request_selection_target(s, sel_atom, s->atom_utf8, buf, sizeof(buf));
    }
    if (!ok) {
        ok = x11_request_selection_target(s, sel_atom, XA_STRING, buf, sizeof(buf));
    }
    if (ok && buf[0] != '\0') {
        snprintf(s->last_synced_clip, sizeof(s->last_synced_clip), "%s", buf);
        (void)arcr_platform_clipboard_set_utf8(buf);
    }
}

static void x11_handle_selection_request(X11PresenterSurface *s, const XSelectionRequestEvent *req) {
    if (!s || !s->dpy || !req) return;

    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type      = SelectionNotify;
    reply.display   = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target    = req->target;
    reply.time      = req->time;
    reply.property  = None;

    Atom dst_prop = (req->property != None) ? req->property : req->target;

    if (s->owned_clip && s->owned_clip_len > 0u) {
        if (s->atom_targets != None && req->target == s->atom_targets) {
            Atom supported[4];
            int count = 0;
            supported[count++] = s->atom_targets;
            if (s->atom_utf8 != None) supported[count++] = s->atom_utf8;
            supported[count++] = XA_STRING;
            if (s->atom_text != None) supported[count++] = s->atom_text;
            XChangeProperty(s->dpy, req->requestor, dst_prop, XA_ATOM, 32,
                            PropModeReplace, (const unsigned char *)supported, count);
            reply.property = dst_prop;
        } else if (req->target == XA_STRING ||
                   (s->atom_utf8 != None && req->target == s->atom_utf8) ||
                   (s->atom_text != None && req->target == s->atom_text)) {
            Atom prop_type = (req->target == s->atom_text && s->atom_utf8 != None)
                                 ? s->atom_utf8
                                 : req->target;
            XChangeProperty(s->dpy, req->requestor, dst_prop, prop_type, 8,
                            PropModeReplace,
                            (const unsigned char *)s->owned_clip,
                            (int)s->owned_clip_len);
            reply.property = dst_prop;
        }
    }

    XSendEvent(s->dpy, req->requestor, False, 0L, (XEvent *)&reply);
    XFlush(s->dpy);
}

int waterscape_run_desktop_window(ArcrEngine *engine,
                                  ArcrBrowserShell *shell,
                                  const WaterscapeWindowOptions *opts) {
    (void)engine;
    if (!shell) return 1;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr,
                "waterscape: unable to open X11 DISPLAY (use --headless or --snapshot-dir <dir>).\n");
        return 1;
    }

    X11PresenterSurface surf;
    memset(&surf, 0, sizeof(surf));
    surf.dpy       = dpy;
    surf.screen    = DefaultScreen(dpy);
    surf.visual    = DefaultVisual(dpy, surf.screen);
    surf.depth     = DefaultDepth(dpy, surf.screen);
    surf.allow_shm = !(opts && opts->force_cpu_presenter);

    surf.atom_clipboard     = XInternAtom(dpy, "CLIPBOARD", False);
    surf.atom_targets       = XInternAtom(dpy, "TARGETS", False);
    surf.atom_utf8          = XInternAtom(dpy, "UTF8_STRING", False);
    surf.atom_text          = XInternAtom(dpy, "TEXT", False);
    surf.atom_incr          = XInternAtom(dpy, "INCR", False);
    surf.atom_clip_prop     = XInternAtom(dpy, "WATERSCAPE_CLIP_PROP", False);
    surf.atom_wm_state      = XInternAtom(dpy, "_NET_WM_STATE", False);
    surf.atom_wm_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    (void)arcr_platform_clipboard_get_utf8(surf.last_synced_clip, sizeof(surf.last_synced_clip));

    int32_t init_w = (opts && opts->initial_width > 0)  ? opts->initial_width  : 1080;
    int32_t init_h = (opts && opts->initial_height > 0) ? opts->initial_height : 720;

    /* X11 WM owns the single outer window frame & caption; shell draws zero caption glyphs */
    arcr_shell_default_frame_metrics(&surf.fm);
    surf.fm.mode                   = ARCR_FRAME_OS_SINGLE;
    surf.fm.dpi                    = x11_detect_dpi(dpy, surf.screen);
    surf.fm.os_caption_height_px   = 0;
    surf.fm.caption_inset_left_px  = 0;
    surf.fm.caption_inset_right_px = 0;
    surf.fm.window_active          = true;
    surf.fm.is_maximized           = false;
    arcr_shell_set_frame_metrics(shell, &surf.fm);
    arcr_shell_resize(shell, init_w, init_h);

    surf.win = XCreateSimpleWindow(
        dpy, RootWindow(dpy, surf.screen),
        40, 40, (unsigned int)init_w, (unsigned int)init_h, 0,
        BlackPixel(dpy, surf.screen), WhitePixel(dpy, surf.screen));

    const char *title = (opts && opts->window_title && opts->window_title[0])
                            ? opts->window_title
                            : "Waterscape Browser";
    x11_set_utf8_title(dpy, surf.win, title);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, surf.win, &wm_delete, 1);

    XSelectInput(dpy, surf.win,
                 ExposureMask | StructureNotifyMask | FocusChangeMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    surf.gc            = DefaultGC(dpy, surf.screen);
    surf.cur_arrow     = XCreateFontCursor(dpy, XC_left_ptr);
    surf.cur_pointer   = XCreateFontCursor(dpy, XC_hand2);
    surf.cur_text      = XCreateFontCursor(dpy, XC_xterm);
    surf.active_cursor = ARCR_CURSOR_ARROW;
    XDefineCursor(dpy, surf.win, surf.cur_arrow);

    if (!x11_create_surface(&surf, init_w, init_h)) {
        XFreeCursor(dpy, surf.cur_arrow);
        XFreeCursor(dpy, surf.cur_pointer);
        XFreeCursor(dpy, surf.cur_text);
        XDestroyWindow(dpy, surf.win);
        XCloseDisplay(dpy);
        return 1;
    }

    printf("  X11 Presenter  : %s (%dx%d, depth %d, %d DPI, Single-Frame OS)\n",
           waterscape_present_backend_name(surf.backend),
           (int)init_w, (int)init_h, surf.depth, (int)surf.fm.dpi);

    XMapWindow(dpy, surf.win);
    if (arcr_shell_is_fullscreen(shell)) {
        x11_sync_os_fullscreen(&surf, true);
    }
    XFlush(dpy);

    uint64_t start_usec    = arcr_platform_monotonic_usec();
    uint64_t deadline_usec = (opts && opts->auto_close_ms > 0u)
                                 ? (start_usec + (uint64_t)opts->auto_close_ms * 1000ULL)
                                 : 0ULL;

    bool running     = true;
    bool needs_paint = true;
    int  x11_fd      = ConnectionNumber(dpy);

    while (running) {
        uint64_t now = arcr_platform_monotonic_usec();
        if (deadline_usec > 0ULL && now >= deadline_usec) {
            break;
        }

        /* 16 ms (~60 FPS) non-blocking select() timeout when idle between frames */
        if (XPending(dpy) == 0 && !needs_paint) {
            uint64_t wait_usec = 16000ULL;
            if (deadline_usec > 0ULL) {
                uint64_t rem_usec = deadline_usec - now;
                if (rem_usec < wait_usec) {
                    wait_usec = rem_usec;
                }
            }
            struct timeval tv;
            tv.tv_sec  = (long)(wait_usec / 1000000ULL);
            tv.tv_usec = (long)(wait_usec % 1000000ULL);
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(x11_fd, &rfds);
            (void)select(x11_fd + 1, &rfds, NULL, NULL, &tv);
        }

        while (XPending(dpy) > 0 && running) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    needs_paint = true;
                }
                break;

            case ConfigureNotify: {
                int32_t nw = (int32_t)ev.xconfigure.width;
                int32_t nh = (int32_t)ev.xconfigure.height;
                /* Drain queued ConfigureNotify events */
                while (XCheckTypedWindowEvent(dpy, surf.win, ConfigureNotify, &ev)) {
                    nw = (int32_t)ev.xconfigure.width;
                    nh = (int32_t)ev.xconfigure.height;
                }
                if (nw > 0 && nh > 0 && (nw != surf.width || nh != surf.height)) {
                    arcr_shell_resize(shell, nw, nh);
                    x11_create_surface(&surf, nw, nh);
                    needs_paint = true;
                }
                break;
            }

            case MotionNotify: {
                int32_t mx = ev.xmotion.x;
                int32_t my = ev.xmotion.y;
                while (XCheckTypedWindowEvent(dpy, surf.win, MotionNotify, &ev)) {
                    mx = ev.xmotion.x;
                    my = ev.xmotion.y;
                }
                ArcrCursorKind ck = arcr_shell_on_mouse_move(shell, mx, my);
                x11_apply_cursor(&surf, ck);
                if (waterscape_shell_needs_repaint(shell)) {
                    needs_paint = true;
                }
                break;
            }

            case ButtonPress: {
                bool ctrl  = (ev.xbutton.state & ControlMask) != 0;
                bool shift = (ev.xbutton.state & ShiftMask) != 0;
                if (ev.xbutton.button == Button4) {
                    if (arcr_shell_on_mouse_wheel(shell, 1, ctrl)) needs_paint = true;
                } else if (ev.xbutton.button == Button5) {
                    if (arcr_shell_on_mouse_wheel(shell, -1, ctrl)) needs_paint = true;
                } else if (ev.xbutton.button == Button1 ||
                           ev.xbutton.button == Button2 ||
                           ev.xbutton.button == Button3) {
                    int btn = 0;
                    if (ev.xbutton.button == Button2) btn = 1;
                    else if (ev.xbutton.button == Button3) {
                        btn = 2;
                        /* Sync external clipboard before opening Context Menu (Paste & Go) */
                        x11_pull_clipboard(&surf);
                    }
                    if (arcr_shell_on_mouse_down_ex(shell, ev.xbutton.x, ev.xbutton.y,
                                                    btn, ctrl, shift) ||
                        waterscape_shell_needs_repaint(shell)) {
                        needs_paint = true;
                    }
                    x11_push_clipboard_if_changed(&surf);
                    x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));
                }
                break;
            }

            case ButtonRelease: {
                int btn = -1;
                if (ev.xbutton.button == Button1) btn = 0;
                else if (ev.xbutton.button == Button2) btn = 1;
                else if (ev.xbutton.button == Button3) btn = 2;
                if (btn >= 0) {
                    if (arcr_shell_on_mouse_up(shell, ev.xbutton.x, ev.xbutton.y, btn) ||
                        waterscape_shell_needs_repaint(shell)) {
                        needs_paint = true;
                    }
                    x11_push_clipboard_if_changed(&surf);
                    x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));
                }
                break;
            }

            case KeyPress: {
                bool ctrl  = (ev.xkey.state & ControlMask) != 0;
                bool alt   = (ev.xkey.state & Mod1Mask) != 0;
                bool shift = (ev.xkey.state & ShiftMask) != 0;
                char buf[32];
                KeySym ks = NoSymbol;
                int n = XLookupString(&ev.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);
                if (n < 0) n = 0;
                buf[n] = '\0';

                if (ks >= XK_F1 && ks <= XK_F12) {
                    int fn_num = (int)(ks - XK_F1) + 1;
                    if (arcr_shell_on_key_ex(shell, NULL, ARCR_KEY_NONE,
                                             ctrl, alt, shift, fn_num)) {
                        needs_paint = true;
                    }
                    x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));
                    break;
                }

                ArcrSpecialKey sk = ARCR_KEY_NONE;
                if (ks == XK_BackSpace)                               sk = ARCR_KEY_BACKSPACE;
                else if (ks == XK_Delete || ks == XK_KP_Delete)       sk = ARCR_KEY_DELETE;
                else if (ks == XK_Return || ks == XK_KP_Enter)        sk = ARCR_KEY_ENTER;
                else if (ks == XK_Tab)                                sk = ARCR_KEY_TAB;
                else if (ks == XK_ISO_Left_Tab) {
                    sk    = ARCR_KEY_TAB;
                    shift = true;
                }
                else if (ks == XK_Left || ks == XK_KP_Left)           sk = ARCR_KEY_LEFT;
                else if (ks == XK_Right || ks == XK_KP_Right)         sk = ARCR_KEY_RIGHT;
                else if (ks == XK_Home || ks == XK_KP_Home)           sk = ARCR_KEY_HOME;
                else if (ks == XK_End || ks == XK_KP_End)             sk = ARCR_KEY_END;
                else if (ks == XK_Escape)                             sk = ARCR_KEY_ESCAPE;
                else if (ks == XK_Up || ks == XK_KP_Up)               sk = ARCR_KEY_UP;
                else if (ks == XK_Down || ks == XK_KP_Down)           sk = ARCR_KEY_DOWN;
                else if (ks == XK_Page_Up || ks == XK_KP_Page_Up)     sk = ARCR_KEY_PAGE_UP;
                else if (ks == XK_Page_Down || ks == XK_KP_Page_Down) sk = ARCR_KEY_PAGE_DOWN;

                if (sk != ARCR_KEY_NONE) {
                    if (arcr_shell_on_key_ex(shell, NULL, sk, ctrl, alt, shift, 0)) {
                        needs_paint = true;
                    }
                    x11_push_clipboard_if_changed(&surf);
                    x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));
                    break;
                }

                if (ctrl || alt) {
                    KeySym base_ks = XLookupKeysym(&ev.xkey, 0);
                    if (ctrl && !alt && !shift) {
                        ArcrSpecialKey clip_sk = ARCR_KEY_NONE;
                        if (base_ks == XK_a || base_ks == XK_A)      clip_sk = ARCR_KEY_SELECT_ALL;
                        else if (base_ks == XK_c || base_ks == XK_C) clip_sk = ARCR_KEY_COPY;
                        else if (base_ks == XK_x || base_ks == XK_X) clip_sk = ARCR_KEY_CUT;
                        else if (base_ks == XK_v || base_ks == XK_V) {
                            x11_pull_clipboard(&surf);
                            clip_sk = ARCR_KEY_PASTE;
                        }
                        if (clip_sk != ARCR_KEY_NONE) {
                            if (arcr_shell_on_key_ex(shell, NULL, clip_sk, true, false, false, 0)) {
                                needs_paint = true;
                            }
                            x11_push_clipboard_if_changed(&surf);
                            break;
                        }
                    }

                    if (base_ks >= 0x20u && base_ks <= 0x7Eu) {
                        char kstr[2];
                        kstr[0] = (char)base_ks;
                        kstr[1] = '\0';
                        if (arcr_shell_on_key_ex(shell, kstr, ARCR_KEY_NONE,
                                                 ctrl, alt, shift, 0)) {
                            needs_paint = true;
                        }
                        x11_push_clipboard_if_changed(&surf);
                        x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));
                    }
                    break;
                }

                if (n > 0 && (unsigned char)buf[0] >= 0x20u && (unsigned char)buf[0] != 0x7Fu) {
                    if (arcr_shell_on_key_ex(shell, buf, ARCR_KEY_NONE,
                                             false, false, shift, 0)) {
                        needs_paint = true;
                    }
                }
                break;
            }

            case FocusIn:
                if (!surf.fm.window_active) {
                    surf.fm.window_active = true;
                    arcr_shell_set_frame_metrics(shell, &surf.fm);
                    needs_paint = true;
                }
                break;

            case FocusOut:
                if (surf.fm.window_active) {
                    surf.fm.window_active = false;
                    arcr_shell_set_frame_metrics(shell, &surf.fm);
                    needs_paint = true;
                }
                break;

            case SelectionRequest:
                x11_handle_selection_request(&surf, &ev.xselectionrequest);
                break;

            case SelectionClear:
                if (ev.xselectionclear.selection == surf.atom_clipboard) {
                    surf.owns_clipboard = false;
                }
                break;

            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    running = false;
                }
                break;

            default:
                break;
            }
        }

        if (!running) break;

        /* Drive 60 FPS async network load progress, CSS @keyframes, JS timers, GIFs & <video> */
        if (arcr_shell_tick(shell, arcr_platform_monotonic_usec()) ||
            waterscape_shell_needs_repaint(shell)) {
            needs_paint = true;
        }
        x11_sync_os_fullscreen(&surf, arcr_shell_is_fullscreen(shell));

        if (needs_paint) {
            x11_present(&surf, shell);
            needs_paint = false;
        }
    }

    free(surf.owned_clip);
    surf.owned_clip = NULL;
    x11_destroy_surface(&surf);
    XFreeCursor(dpy, surf.cur_arrow);
    XFreeCursor(dpy, surf.cur_pointer);
    XFreeCursor(dpy, surf.cur_text);
    XDestroyWindow(dpy, surf.win);
    XCloseDisplay(dpy);
    return 0;
}

#else /* !WATERSCAPE_HAVE_X11 */

int waterscape_run_desktop_window(ArcrEngine *engine,
                                  ArcrBrowserShell *shell,
                                  const WaterscapeWindowOptions *opts) {
    (void)engine;
    (void)shell;
    (void)opts;
    fprintf(stderr,
            "waterscape: built without WATERSCAPE_HAVE_X11; pass --headless, --benchmark, "
            "or --snapshot-dir <dir>.\n");
    return 1;
}

#endif /* WATERSCAPE_HAVE_X11 */

#else
/* Win32 ISO C11 compliant translation unit placeholder */
typedef int waterscape_x11_windows_tu_t;
#endif /* !_WIN32 */
