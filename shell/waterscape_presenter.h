/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/waterscape_presenter.h - Cross-Platform Desktop Window & Hardware GPU Presenter
 *
 * Bridges `ArcrBrowserShell` to native OS windowing and multi-generation GPU presentation:
 * - Windows (`waterscape_win32.c`):
 *     Direct3D 11 DXGI Flip -> Direct3D 9.0c/9Ex -> OpenGL ICD -> Win2000 GDI DIBSection
 *     + Korean Imm32 IME composition (`WM_IME_COMPOSITION`) + Mouse/Wheel/Clipboard
 * - Linux / POSIX (`waterscape_x11.c`):
 *     X11 + MIT-SHM (`XShmPutImage`) Zero-Copy Shared-Memory Window Presenter
 *     (with remote-display `XPutImage` fallback) + Cursors + Keyboard/Mouse/Wheel
 */
#ifndef WATERSCAPE_PRESENTER_H
#define WATERSCAPE_PRESENTER_H

#include "arcr_shell.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum WaterscapePresentBackend {
    WS_PRESENT_NONE         = 0,
    WS_PRESENT_WIN32_D3D11  = 1, /* Modern Windows Direct3D 11 + DXGI Flip/Discard SwapChain */
    WS_PRESENT_WIN32_D3D9   = 2, /* Legacy Win2000/XP/Vista Direct3D 9 Hardware SwapChain */
    WS_PRESENT_WIN32_OPENGL = 3, /* Verified Hardware OpenGL ICD (rejects GDI Generic) */
    WS_PRESENT_WIN32_GDI    = 4, /* Universal Zero-Crash GDI CreateDIBSection + BitBlt */
    WS_PRESENT_X11_MITSHM   = 5, /* Linux X11 + MIT-SHM Zero-Copy XShmPutImage */
    WS_PRESENT_X11_XIMAGE   = 6  /* Linux X11 Standard XPutImage Fallback */
} WaterscapePresentBackend;

typedef struct WaterscapeWindowOptions {
    const char *window_title;
    int32_t     initial_width;
    int32_t     initial_height;
    bool        force_cpu_presenter; /* Force GDI / XShm CPU presentation */
    uint32_t    auto_close_ms;       /* > 0: automatically close window after N ms (CI/Xvfb) */
} WaterscapeWindowOptions;

const char *waterscape_present_backend_name(WaterscapePresentBackend backend);

/*
 * Returns true if browser chrome hover/focus/overlay state or the active tab's
 * viewport framebuffer has been dirtied since the last `arcr_shell_render_frame`.
 */
bool waterscape_shell_needs_repaint(const ArcrBrowserShell *shell);

/*
 * Opens a native OS desktop window for `shell` and runs the interactive event +
 * hardware presentation loop until the user closes the window (or `auto_close_ms` elapses).
 * Returns 0 on clean exit, non-zero if a display/window cannot be opened.
 */
int waterscape_run_desktop_window(ArcrEngine *engine,
                                  ArcrBrowserShell *shell,
                                  const WaterscapeWindowOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* WATERSCAPE_PRESENTER_H */
