/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/waterscape_x11.c - Linux X11 + MIT-SHM Zero-Copy Desktop Window Presenter
 *
 * Uses MIT-SHM (`XShmCreateImage` + `shmget`/`shmat` + `XShmPutImage`) so the
 * multi-core 64x64 tile + SIMD framebuffer is presented directly from shared RAM
 * to the X11 server with zero socket copies (automatically falling back to
 * `XPutImage` over remote/networked X11 connections).
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _XOPEN_SOURCE   700
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
} X11PresenterSurface;

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

    int32_t init_w = (opts && opts->initial_width > 0)  ? opts->initial_width  : 1080;
    int32_t init_h = (opts && opts->initial_height > 0) ? opts->initial_height : 720;

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
                 ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    surf.gc            = DefaultGC(dpy, surf.screen);
    surf.cur_arrow     = XCreateFontCursor(dpy, XC_left_ptr);
    surf.cur_pointer   = XCreateFontCursor(dpy, XC_hand2);
    surf.cur_text      = XCreateFontCursor(dpy, XC_xterm);
    surf.active_cursor = ARCR_CURSOR_ARROW;
    XDefineCursor(dpy, surf.win, surf.cur_arrow);

    if (!x11_create_surface(&surf, init_w, init_h)) {
        XDestroyWindow(dpy, surf.win);
        XCloseDisplay(dpy);
        return 1;
    }

    printf("  X11 Presenter  : %s (%dx%d, depth %d)\n",
           waterscape_present_backend_name(surf.backend),
           (int)init_w, (int)init_h, surf.depth);

    XMapWindow(dpy, surf.win);
    XFlush(dpy);

    uint64_t start_usec    = arcr_platform_monotonic_usec();
    uint64_t deadline_usec = (opts && opts->auto_close_ms > 0u)
                                 ? (start_usec + (uint64_t)opts->auto_close_ms * 1000ULL)
                                 : 0ULL;

    bool running     = true;
    bool needs_paint = true;
    int  x11_fd      = ConnectionNumber(dpy);

    while (running) {
        if (deadline_usec > 0ULL) {
            uint64_t now = arcr_platform_monotonic_usec();
            if (now >= deadline_usec) {
                break;
            }
            if (XPending(dpy) == 0 && !needs_paint) {
                uint64_t rem_usec = deadline_usec - now;
                struct timeval tv;
                tv.tv_sec  = (long)(rem_usec / 1000000ULL);
                tv.tv_usec = (long)(rem_usec % 1000000ULL);
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(x11_fd, &rfds);
                (void)select(x11_fd + 1, &rfds, NULL, NULL, &tv);
            }
        }

        while (XPending(dpy) > 0 || (deadline_usec == 0ULL && !needs_paint)) {
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
                needs_paint = true;
                break;
            }
            case ButtonPress: {
                bool ctrl = (ev.xbutton.state & ControlMask) != 0;
                if (ev.xbutton.button == Button4) {
                    if (arcr_shell_on_mouse_wheel(shell, 1, ctrl)) needs_paint = true;
                } else if (ev.xbutton.button == Button5) {
                    if (arcr_shell_on_mouse_wheel(shell, -1, ctrl)) needs_paint = true;
                } else {
                    int btn = 0;
                    if (ev.xbutton.button == Button2) btn = 1;
                    else if (ev.xbutton.button == Button3) btn = 2;
                    arcr_shell_on_mouse_down(shell, ev.xbutton.x, ev.xbutton.y, btn);
                    needs_paint = true;
                }
                break;
            }
            case ButtonRelease:
                if (ev.xbutton.button == Button1) {
                    if (arcr_shell_on_mouse_up(shell, ev.xbutton.x, ev.xbutton.y, 0)) {
                        needs_paint = true;
                    }
                }
                break;
            case KeyPress: {
                bool ctrl = (ev.xkey.state & ControlMask) != 0;
                bool alt  = (ev.xkey.state & Mod1Mask) != 0;
                char buf[32];
                KeySym ks = NoSymbol;
                int n = XLookupString(&ev.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);
                if (n < 0) n = 0;
                buf[n] = '\0';

                if (ks >= XK_F1 && ks <= XK_F12) {
                    int fn_num = (int)(ks - XK_F1) + 1;
                    if (arcr_shell_on_key(shell, NULL, ARCR_KEY_NONE, ctrl, alt, fn_num)) {
                        needs_paint = true;
                    }
                    break;
                }

                ArcrSpecialKey sk = ARCR_KEY_NONE;
                if (ks == XK_BackSpace)                        sk = ARCR_KEY_BACKSPACE;
                else if (ks == XK_Delete || ks == XK_KP_Delete) sk = ARCR_KEY_DELETE;
                else if (ks == XK_Return || ks == XK_KP_Enter)  sk = ARCR_KEY_ENTER;
                else if (ks == XK_Tab)                         sk = ARCR_KEY_TAB;
                else if (ks == XK_Left)                        sk = ARCR_KEY_LEFT;
                else if (ks == XK_Right)                       sk = ARCR_KEY_RIGHT;
                else if (ks == XK_Home)                        sk = ARCR_KEY_HOME;
                else if (ks == XK_End)                         sk = ARCR_KEY_END;
                else if (ks == XK_Escape)                      sk = ARCR_KEY_ESCAPE;

                if (sk != ARCR_KEY_NONE) {
                    if (arcr_shell_on_key(shell, NULL, sk, ctrl, alt, 0)) {
                        needs_paint = true;
                    }
                    break;
                }

                if (!ctrl && !alt &&
                    (ks == XK_Up || ks == XK_Down || ks == XK_Page_Up || ks == XK_Page_Down)) {
                    int32_t steps = (ks == XK_Page_Up || ks == XK_Page_Down) ? 4 : 1;
                    if (ks == XK_Down || ks == XK_Page_Down) steps = -steps;
                    if (arcr_shell_on_mouse_wheel(shell, steps, false)) {
                        needs_paint = true;
                    }
                    break;
                }

                if (ctrl) {
                    KeySym base_ks = XLookupKeysym(&ev.xkey, 0);
                    if (base_ks >= 0x20u && base_ks <= 0x7Eu) {
                        char kstr[2];
                        kstr[0] = (char)base_ks;
                        kstr[1] = '\0';
                        if (arcr_shell_on_key(shell, kstr, ARCR_KEY_NONE, true, alt, 0)) {
                            needs_paint = true;
                        }
                    }
                    break;
                }

                if (n > 0 && (unsigned char)buf[0] >= 0x20u) {
                    if (arcr_shell_on_key(shell, buf, ARCR_KEY_NONE, false, alt, 0)) {
                        needs_paint = true;
                    }
                }
                break;
            }
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    running = false;
                }
                break;
            default:
                break;
            }

            if (!running) break;
            if (XPending(dpy) == 0) break;
        }

        if (running && needs_paint) {
            x11_present(&surf, shell);
            needs_paint = false;
        }
    }

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
