/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/waterscape_win32.c - Win32 Desktop Window + Multi-Generation GPU Presenter
 *
 * Presentation Chain (Zero Static GPU .lib Imports — Launches on Vanilla Win2000 SP4!):
 *   1. Direct3D 11 (`d3d11.dll`) : DXGI Flip-Discard (Win10/11) -> Discard (Win7/Vista)
 *   2. Direct3D 9  (`d3d9.dll`)  : Hardware HAL X8R8G8B8 SwapChain (Win2000 DX9c / XP / Vista)
 *   3. OpenGL ICD  (`opengl32.dll`): Verified Hardware ICD (rejects "GDI Generic")
 *   4. Win32 GDI   (`gdi32.dll`) : Universal Zero-Crash `CreateDIBSection` + `BitBlt`
 *
 * Also integrates full Korean `Imm32` (`WM_IME_COMPOSITION`) Hangul IME composition,
 * dynamic `IDC_HAND` / `IDC_IBEAM` / `IDC_ARROW` cursors, mouse wheel zoom/scroll,
 * and browser keyboard shortcuts.
 */
#include "waterscape_presenter.h"

#if defined(_WIN32)

#ifndef WINVER
#  define WINVER 0x0500
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0500
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_MOUSEWHEEL
#  define WM_MOUSEWHEEL 0x020A
#endif
#ifndef IDC_HAND
#  define IDC_HAND MAKEINTRESOURCEW(32649)
#endif
#ifndef GWLP_USERDATA
#  define GWLP_USERDATA (-21)
#endif

const char *waterscape_present_backend_name(WaterscapePresentBackend backend) {
    switch (backend) {
    case WS_PRESENT_WIN32_D3D11:  return "Win32 Direct3D 11 + DXGI SwapChain";
    case WS_PRESENT_WIN32_D3D9:   return "Win32 Direct3D 9 Hardware HAL";
    case WS_PRESENT_WIN32_OPENGL: return "Win32 OpenGL Hardware ICD";
    case WS_PRESENT_WIN32_GDI:    return "Win32 GDI DIBSection (Universal)";
    case WS_PRESENT_X11_MITSHM:   return "X11 MIT-SHM Zero-Copy";
    case WS_PRESENT_X11_XIMAGE:   return "X11 XPutImage";
    default:                      return "None";
    }
}

/* Helper to copy FARPROC into typed function pointer without -Wpedantic warnings */
#define WS_LOAD_PROC(hmod, fn_var, sym_name)                    \
    do {                                                        \
        FARPROC raw_sym_ = GetProcAddress((hmod), (sym_name));  \
        memcpy(&(fn_var), &raw_sym_, sizeof(fn_var));           \
    } while (0)

/* -------------------------------------------------------------------------
 * 1. Minimal Pure C COM ABI Definitions for Dynamic D3D11 / DXGI
 * ------------------------------------------------------------------------- */
typedef struct WsGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} WsGuid;

static const WsGuid kIID_ID3D11Texture2D = {
    0x6F15AAF2u, 0xD208u, 0x4E89u, {0x9Au, 0xB4u, 0x48u, 0x95u, 0x35u, 0xD3u, 0x4Fu, 0x9Cu}
};

typedef struct WsDxgiRational {
    UINT Numerator;
    UINT Denominator;
} WsDxgiRational;

typedef struct WsDxgiModeDesc {
    UINT           Width;
    UINT           Height;
    WsDxgiRational RefreshRate;
    UINT           Format;
    UINT           ScanlineOrdering;
    UINT           Scaling;
} WsDxgiModeDesc;

typedef struct WsDxgiSampleDesc {
    UINT Count;
    UINT Quality;
} WsDxgiSampleDesc;

typedef struct WsDxgiSwapChainDesc {
    WsDxgiModeDesc   BufferDesc;
    WsDxgiSampleDesc SampleDesc;
    UINT             BufferUsage;
    UINT             BufferCount;
    HWND             OutputWindow;
    BOOL             Windowed;
    UINT             SwapEffect;
    UINT             Flags;
} WsDxgiSwapChainDesc;

typedef struct WsComUnknown WsComUnknown;
typedef struct WsComUnknownVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsComUnknown *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsComUnknown *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsComUnknown *This);
} WsComUnknownVtbl;
struct WsComUnknown {
    const WsComUnknownVtbl *lpVtbl;
};

typedef struct WsDxgiSwapChain WsDxgiSwapChain;
typedef struct WsDxgiSwapChainVtbl {
    void    *unk_methods[8]; /* 0..2 IUnknown, 3..6 IDXGIObject, 7 IDXGIDeviceSubObject */
    HRESULT (STDMETHODCALLTYPE *Present)(WsDxgiSwapChain *This, UINT SyncInterval, UINT Flags);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(WsDxgiSwapChain *This, UINT Buffer,
                                           const WsGuid *riid, void **ppSurface);
    void    *fs_methods[3];  /* 10..12 SetFullscreenState, GetFullscreenState, GetDesc */
    HRESULT (STDMETHODCALLTYPE *ResizeBuffers)(WsDxgiSwapChain *This, UINT BufferCount,
                                               UINT Width, UINT Height,
                                               UINT NewFormat, UINT SwapChainFlags);
} WsDxgiSwapChainVtbl;
struct WsDxgiSwapChain {
    const WsDxgiSwapChainVtbl *lpVtbl;
};

typedef struct WsD3D11DeviceContext WsD3D11DeviceContext;
typedef struct WsD3D11DeviceContextVtbl {
    void *prior_methods[48]; /* 0..2 IUnknown, 3..6 DeviceChild, 7..47 Pipeline & CopyResource */
    void (STDMETHODCALLTYPE *UpdateSubresource)(WsD3D11DeviceContext *This,
                                                void *pDstResource,
                                                UINT DstSubresource,
                                                const void *pDstBox,
                                                const void *pSrcData,
                                                UINT SrcRowPitch,
                                                UINT SrcDepthPitch);
} WsD3D11DeviceContextVtbl;
struct WsD3D11DeviceContext {
    const WsD3D11DeviceContextVtbl *lpVtbl;
};

typedef HRESULT (WINAPI *PFN_WsD3D11CreateDeviceAndSwapChain)(
    void *pAdapter,
    UINT DriverType,
    HMODULE Software,
    UINT Flags,
    const UINT *pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const WsDxgiSwapChainDesc *pSwapChainDesc,
    WsDxgiSwapChain **ppSwapChain,
    WsComUnknown **ppDevice,
    UINT *pFeatureLevel,
    WsD3D11DeviceContext **ppImmediateContext
);

/* -------------------------------------------------------------------------
 * 2. Minimal Pure C COM ABI Definitions for Dynamic Direct3D 9
 * ------------------------------------------------------------------------- */
typedef struct WsD3DPresentParams {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    UINT BackBufferFormat;
    UINT BackBufferCount;
    UINT MultiSampleType;
    DWORD MultiSampleQuality;
    UINT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} WsD3DPresentParams;

typedef struct WsD3DLockedRect {
    INT   Pitch;
    void *pBits;
} WsD3DLockedRect;

typedef struct WsD3DSurface9 WsD3DSurface9;
typedef struct WsD3DSurface9Vtbl {
    void    *unk_and_res_methods[13]; /* 0..2 IUnknown, 3..10 Resource9, 11 GetContainer, 12 GetDesc */
    HRESULT (STDMETHODCALLTYPE *LockRect)(WsD3DSurface9 *This, WsD3DLockedRect *pLockedRect,
                                          const RECT *pRect, DWORD Flags);
    HRESULT (STDMETHODCALLTYPE *UnlockRect)(WsD3DSurface9 *This);
} WsD3DSurface9Vtbl;
struct WsD3DSurface9 {
    const WsD3DSurface9Vtbl *lpVtbl;
};

typedef struct WsD3DDevice9 WsD3DDevice9;
typedef struct WsD3DDevice9Vtbl {
    void    *unk_methods[3];
    HRESULT (STDMETHODCALLTYPE *TestCooperativeLevel)(WsD3DDevice9 *This);
    void    *mid_methods[12]; /* 4..15 */
    HRESULT (STDMETHODCALLTYPE *Reset)(WsD3DDevice9 *This, WsD3DPresentParams *pPresentationParameters);
    HRESULT (STDMETHODCALLTYPE *Present)(WsD3DDevice9 *This, const RECT *pSourceRect,
                                         const RECT *pDestRect, HWND hDestWindowOverride,
                                         const void *pDirtyRegion);
    HRESULT (STDMETHODCALLTYPE *GetBackBuffer)(WsD3DDevice9 *This, UINT iSwapChain,
                                               UINT iBackBuffer, UINT Type,
                                               WsD3DSurface9 **ppBackBuffer);
} WsD3DDevice9Vtbl;
struct WsD3DDevice9 {
    const WsD3DDevice9Vtbl *lpVtbl;
};

typedef struct WsDirect3D9 WsDirect3D9;
typedef struct WsDirect3D9Vtbl {
    void    *prior_methods[16]; /* 0..2 IUnknown, 3..15 Adapter enumeration */
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(WsDirect3D9 *This, UINT Adapter, UINT DeviceType,
                                              HWND hFocusWindow, DWORD BehaviorFlags,
                                              WsD3DPresentParams *pPresentationParameters,
                                              WsD3DDevice9 **ppReturnedDeviceInterface);
} WsDirect3D9Vtbl;
struct WsDirect3D9 {
    const WsDirect3D9Vtbl *lpVtbl;
};

typedef WsDirect3D9 *(WINAPI *PFN_WsDirect3DCreate9)(UINT SDKVersion);

/* -------------------------------------------------------------------------
 * 3. Dynamic OpenGL ICD Function Pointers
 * ------------------------------------------------------------------------- */
typedef HGLRC         (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL          (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL          (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef const uint8_t *(APIENTRY *PFN_glGetString)(uint32_t);
typedef void          (APIENTRY *PFN_glViewport)(int, int, int, int);
typedef void          (APIENTRY *PFN_glRasterPos2f)(float, float);
typedef void          (APIENTRY *PFN_glPixelZoom)(float, float);
typedef void          (APIENTRY *PFN_glDrawPixels)(int, int, uint32_t, uint32_t, const void *);
typedef void          (APIENTRY *PFN_glFlush)(void);

typedef struct Win32PresenterHost {
    ArcrEngine              *engine;
    ArcrBrowserShell        *shell;
    WaterscapePresentBackend backend;
    bool                     force_cpu;

    /* Universal CPU Framebuffer + GDI Fallback */
    HDC                      mem_dc;
    HBITMAP                  dib;
    HGDIOBJ                  old_bmp;
    uint32_t                *pixels;
    int32_t                  fb_w;
    int32_t                  fb_h;

    /* Dynamic D3D11 State */
    HMODULE                  d3d11_dll;
    WsComUnknown            *d3d11_dev;
    WsD3D11DeviceContext    *d3d11_ctx;
    WsDxgiSwapChain         *d3d11_swap;

    /* Dynamic D3D9 State */
    HMODULE                  d3d9_dll;
    WsDirect3D9             *d3d9_obj;
    WsD3DDevice9            *d3d9_dev;

    /* Dynamic OpenGL ICD State */
    HMODULE                  gl_dll;
    HDC                      gl_hdc;
    HGLRC                    gl_rc;
    PFN_wglMakeCurrent       fn_wglMakeCurrent;
    PFN_wglDeleteContext     fn_wglDeleteContext;
    PFN_glViewport           fn_glViewport;
    PFN_glRasterPos2f        fn_glRasterPos2f;
    PFN_glPixelZoom          fn_glPixelZoom;
    PFN_glDrawPixels         fn_glDrawPixels;
    PFN_glFlush              fn_glFlush;

    /* Cursors & UTF-16 High Surrogate Buffer */
    HCURSOR                  cur_arrow;
    HCURSOR                  cur_pointer;
    HCURSOR                  cur_text;
    ArcrCursorKind           active_cursor;
    WCHAR                    pending_high_surrogate;
} Win32PresenterHost;

static void com_safe_release(void *iface_ptr) {
    if (!iface_ptr) return;
    WsComUnknown *u = (WsComUnknown *)iface_ptr;
    if (u->lpVtbl && u->lpVtbl->Release) {
        u->lpVtbl->Release(u);
    }
}

static void teardown_gpu_backends(Win32PresenterHost *h, HWND hwnd) {
    com_safe_release(h->d3d11_swap); h->d3d11_swap = NULL;
    com_safe_release(h->d3d11_ctx);  h->d3d11_ctx  = NULL;
    com_safe_release(h->d3d11_dev);  h->d3d11_dev  = NULL;
    if (h->d3d11_dll) {
        FreeLibrary(h->d3d11_dll);
        h->d3d11_dll = NULL;
    }

    com_safe_release(h->d3d9_dev); h->d3d9_dev = NULL;
    com_safe_release(h->d3d9_obj); h->d3d9_obj = NULL;
    if (h->d3d9_dll) {
        FreeLibrary(h->d3d9_dll);
        h->d3d9_dll = NULL;
    }

    if (h->gl_rc && h->fn_wglMakeCurrent && h->fn_wglDeleteContext) {
        h->fn_wglMakeCurrent(NULL, NULL);
        h->fn_wglDeleteContext(h->gl_rc);
        h->gl_rc = NULL;
    }
    if (h->gl_hdc && hwnd) {
        ReleaseDC(hwnd, h->gl_hdc);
        h->gl_hdc = NULL;
    }
    if (h->gl_dll) {
        FreeLibrary(h->gl_dll);
        h->gl_dll = NULL;
    }
    h->backend = WS_PRESENT_WIN32_GDI;
}

static bool try_init_d3d11(Win32PresenterHost *h, HWND hwnd, int32_t w, int32_t ht) {
    h->d3d11_dll = LoadLibraryA("d3d11.dll");
    if (!h->d3d11_dll) return false;

    PFN_WsD3D11CreateDeviceAndSwapChain fn_create = NULL;
    WS_LOAD_PROC(h->d3d11_dll, fn_create, "D3D11CreateDeviceAndSwapChain");
    if (!fn_create) {
        FreeLibrary(h->d3d11_dll);
        h->d3d11_dll = NULL;
        return false;
    }

    /* DXGI_FORMAT_B8G8R8A8_UNORM = 87, DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20 */
    const UINT feature_levels[3] = {0xB000u, 0xA100u, 0xA000u}; /* 11.0, 10.1, 10.0 */
    UINT chosen_fl = 0u;

    /* Try Win10+ DXGI_SWAP_EFFECT_FLIP_DISCARD (4) first, then Win7/Vista DISCARD (0) */
    const UINT swap_effects[2] = {4u, 0u};
    const UINT buf_counts[2]   = {2u, 1u};
    for (int attempt = 0; attempt < 2; ++attempt) {
        WsDxgiSwapChainDesc sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferDesc.Width                   = (UINT)w;
        sd.BufferDesc.Height                  = (UINT)ht;
        sd.BufferDesc.RefreshRate.Numerator   = 60u;
        sd.BufferDesc.RefreshRate.Denominator = 1u;
        sd.BufferDesc.Format                  = 87u; /* DXGI_FORMAT_B8G8R8A8_UNORM */
        sd.SampleDesc.Count                   = 1u;
        sd.BufferUsage                        = 0x20u;
        sd.BufferCount                        = buf_counts[attempt];
        sd.OutputWindow                       = hwnd;
        sd.Windowed                           = TRUE;
        sd.SwapEffect                         = swap_effects[attempt];

        HRESULT hr = fn_create(NULL, 1u /* D3D_DRIVER_TYPE_HARDWARE */, NULL,
                               0x20u /* D3D11_CREATE_DEVICE_BGRA_SUPPORT */,
                               feature_levels, 3u, 7u /* D3D11_SDK_VERSION */,
                               &sd, &h->d3d11_swap, &h->d3d11_dev, &chosen_fl, &h->d3d11_ctx);
        if (hr >= 0 && h->d3d11_swap && h->d3d11_dev && h->d3d11_ctx) {
            h->backend = WS_PRESENT_WIN32_D3D11;
            return true;
        }
        com_safe_release(h->d3d11_swap); h->d3d11_swap = NULL;
        com_safe_release(h->d3d11_ctx);  h->d3d11_ctx  = NULL;
        com_safe_release(h->d3d11_dev);  h->d3d11_dev  = NULL;
    }

    teardown_gpu_backends(h, hwnd);
    return false;
}

static bool try_init_d3d9(Win32PresenterHost *h, HWND hwnd, int32_t w, int32_t ht) {
    h->d3d9_dll = LoadLibraryA("d3d9.dll");
    if (!h->d3d9_dll) return false;

    PFN_WsDirect3DCreate9 fn_create9 = NULL;
    WS_LOAD_PROC(h->d3d9_dll, fn_create9, "Direct3DCreate9");
    if (!fn_create9) {
        FreeLibrary(h->d3d9_dll);
        h->d3d9_dll = NULL;
        return false;
    }

    h->d3d9_obj = fn_create9(32u /* D3D_SDK_VERSION */);
    if (!h->d3d9_obj) {
        FreeLibrary(h->d3d9_dll);
        h->d3d9_dll = NULL;
        return false;
    }

    WsD3DPresentParams pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth      = (UINT)w;
    pp.BackBufferHeight     = (UINT)ht;
    pp.BackBufferFormat     = 22u; /* D3DFMT_X8R8G8B8 */
    pp.BackBufferCount      = 1u;
    pp.SwapEffect           = 1u;  /* D3DSWAPEFFECT_DISCARD */
    pp.hDeviceWindow        = hwnd;
    pp.Windowed             = TRUE;
    pp.Flags                = 0x00000001u; /* D3DPRESENTFLAG_LOCKABLE_BACKBUFFER */
    pp.PresentationInterval = 0x00000001u; /* D3DPRESENT_INTERVAL_ONE */

    /* D3DDEVTYPE_HAL = 1; try HARDWARE_VERTEXPROCESSING (0x40), then SOFTWARE (0x20) */
    HRESULT hr = h->d3d9_obj->lpVtbl->CreateDevice(
        h->d3d9_obj, 0u, 1u, hwnd, 0x40u, &pp, &h->d3d9_dev);
    if (hr < 0 || !h->d3d9_dev) {
        hr = h->d3d9_obj->lpVtbl->CreateDevice(
            h->d3d9_obj, 0u, 1u, hwnd, 0x20u, &pp, &h->d3d9_dev);
    }
    if (hr >= 0 && h->d3d9_dev) {
        h->backend = WS_PRESENT_WIN32_D3D9;
        return true;
    }

    teardown_gpu_backends(h, hwnd);
    return false;
}

static bool try_init_opengl_icd(Win32PresenterHost *h, HWND hwnd) {
    h->gl_dll = LoadLibraryA("opengl32.dll");
    if (!h->gl_dll) return false;

    PFN_wglCreateContext fn_create_ctx = NULL;
    PFN_glGetString      fn_get_str    = NULL;
    WS_LOAD_PROC(h->gl_dll, fn_create_ctx,       "wglCreateContext");
    WS_LOAD_PROC(h->gl_dll, h->fn_wglMakeCurrent, "wglMakeCurrent");
    WS_LOAD_PROC(h->gl_dll, h->fn_wglDeleteContext, "wglDeleteContext");
    WS_LOAD_PROC(h->gl_dll, fn_get_str,          "glGetString");
    WS_LOAD_PROC(h->gl_dll, h->fn_glViewport,    "glViewport");
    WS_LOAD_PROC(h->gl_dll, h->fn_glRasterPos2f, "glRasterPos2f");
    WS_LOAD_PROC(h->gl_dll, h->fn_glPixelZoom,   "glPixelZoom");
    WS_LOAD_PROC(h->gl_dll, h->fn_glDrawPixels,  "glDrawPixels");
    WS_LOAD_PROC(h->gl_dll, h->fn_glFlush,       "glFlush");

    if (!fn_create_ctx || !h->fn_wglMakeCurrent || !h->fn_wglDeleteContext ||
        !fn_get_str || !h->fn_glViewport || !h->fn_glDrawPixels) {
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    h->gl_hdc = GetDC(hwnd);
    if (!h->gl_hdc) {
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(h->gl_hdc, &pfd);
    if (pf == 0 || !SetPixelFormat(h->gl_hdc, pf, &pfd)) {
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    h->gl_rc = fn_create_ctx(h->gl_hdc);
    if (!h->gl_rc || !h->fn_wglMakeCurrent(h->gl_hdc, h->gl_rc)) {
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    const char *renderer = (const char *)fn_get_str(0x1F01u /* GL_RENDERER */);
    if (!renderer || strstr(renderer, "GDI Generic") != NULL) {
        /* Reject unaccelerated Win2000 GDI Generic software OpenGL */
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    h->backend = WS_PRESENT_WIN32_OPENGL;
    return true;
}

static void win32_resize_surfaces(Win32PresenterHost *h, HWND hwnd, int32_t w, int32_t ht) {
    if (!h || w <= 0 || ht <= 0) return;
    if (w == h->fb_w && ht == h->fb_h && h->dib) return;

    if (!h->mem_dc) {
        h->mem_dc = CreateCompatibleDC(NULL);
    }
    if (h->dib) {
        SelectObject(h->mem_dc, h->old_bmp);
        DeleteObject(h->dib);
        h->dib    = NULL;
        h->pixels = NULL;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -ht; /* Top-down ARGB8888 / BGRA8888 */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HBITMAP hbm  = CreateDIBSection(h->mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm && bits) {
        h->old_bmp = SelectObject(h->mem_dc, hbm);
        h->dib     = hbm;
        h->pixels  = (uint32_t *)bits;
        h->fb_w    = w;
        h->fb_h    = ht;
    }

    if (h->backend == WS_PRESENT_WIN32_D3D11 && h->d3d11_swap) {
        HRESULT hr = h->d3d11_swap->lpVtbl->ResizeBuffers(
            h->d3d11_swap, 0u, (UINT)w, (UINT)ht, 87u, 0u);
        if (hr < 0) {
            teardown_gpu_backends(h, hwnd);
        }
    } else if (h->backend == WS_PRESENT_WIN32_D3D9 && h->d3d9_dev) {
        WsD3DPresentParams pp;
        ZeroMemory(&pp, sizeof(pp));
        pp.BackBufferWidth      = (UINT)w;
        pp.BackBufferHeight     = (UINT)ht;
        pp.BackBufferFormat     = 22u;
        pp.BackBufferCount      = 1u;
        pp.SwapEffect           = 1u;
        pp.hDeviceWindow        = hwnd;
        pp.Windowed             = TRUE;
        pp.Flags                = 0x00000001u;
        pp.PresentationInterval = 0x00000001u;
        if (h->d3d9_dev->lpVtbl->Reset(h->d3d9_dev, &pp) < 0) {
            teardown_gpu_backends(h, hwnd);
        }
    }
}

static void win32_present_frame(Win32PresenterHost *h, HWND hwnd, HDC paint_hdc) {
    if (!h || !h->shell || !h->pixels || h->fb_w <= 0 || h->fb_h <= 0) return;

    arcr_shell_render_frame(h->shell, h->pixels, h->fb_w, h->fb_h, h->fb_w);

    /* 1. Direct3D 11 Hardware SwapChain Presentation */
    if (h->backend == WS_PRESENT_WIN32_D3D11 && h->d3d11_swap && h->d3d11_ctx) {
        void *backbuf = NULL;
        HRESULT hr = h->d3d11_swap->lpVtbl->GetBuffer(
            h->d3d11_swap, 0u, &kIID_ID3D11Texture2D, &backbuf);
        if (hr >= 0 && backbuf) {
            h->d3d11_ctx->lpVtbl->UpdateSubresource(
                h->d3d11_ctx, backbuf, 0u, NULL, h->pixels,
                (UINT)(h->fb_w * 4), (UINT)(h->fb_w * h->fb_h * 4));
            com_safe_release(backbuf);
            hr = h->d3d11_swap->lpVtbl->Present(h->d3d11_swap, 1u, 0u);
            if (hr >= 0) return;
        }
        /* Seamless zero-crash fallback on DXGI_ERROR_DEVICE_REMOVED */
        teardown_gpu_backends(h, hwnd);
        arcr_shell_set_status_overlay(h->shell, "D3D11 장치 손실 감지 -> GDI 무중단 복귀 완료");
    }

    /* 2. Direct3D 9 Hardware SwapChain Presentation */
    if (h->backend == WS_PRESENT_WIN32_D3D9 && h->d3d9_dev) {
        if (h->d3d9_dev->lpVtbl->TestCooperativeLevel(h->d3d9_dev) >= 0) {
            WsD3DSurface9 *surf = NULL;
            if (h->d3d9_dev->lpVtbl->GetBackBuffer(h->d3d9_dev, 0u, 0u, 0u, &surf) >= 0 && surf) {
                WsD3DLockedRect lr;
                if (surf->lpVtbl->LockRect(surf, &lr, NULL, 0u) >= 0 && lr.pBits) {
                    size_t row_bytes = (size_t)h->fb_w * 4u;
                    for (int32_t ry = 0; ry < h->fb_h; ++ry) {
                        memcpy((uint8_t *)lr.pBits + (intptr_t)ry * (intptr_t)lr.Pitch,
                               h->pixels + (size_t)ry * (size_t)h->fb_w,
                               row_bytes);
                    }
                    surf->lpVtbl->UnlockRect(surf);
                    com_safe_release(surf);
                    if (h->d3d9_dev->lpVtbl->Present(h->d3d9_dev, NULL, NULL, NULL, NULL) >= 0) {
                        return;
                    }
                } else {
                    com_safe_release(surf);
                }
            }
        }
        /* Seamless zero-crash fallback on D3DERR_DEVICELOST */
        teardown_gpu_backends(h, hwnd);
        arcr_shell_set_status_overlay(h->shell, "D3D9 장치 손실 감지 -> GDI 무중단 복귀 완료");
    }

    /* 3. Verified OpenGL Hardware ICD Presentation */
    if (h->backend == WS_PRESENT_WIN32_OPENGL && h->gl_rc && h->gl_hdc) {
        h->fn_wglMakeCurrent(h->gl_hdc, h->gl_rc);
        h->fn_glViewport(0, 0, h->fb_w, h->fb_h);
        if (h->fn_glRasterPos2f) h->fn_glRasterPos2f(-1.0f, 1.0f);
        if (h->fn_glPixelZoom)   h->fn_glPixelZoom(1.0f, -1.0f);
        h->fn_glDrawPixels(h->fb_w, h->fb_h, 0x80E1u /* GL_BGRA_EXT */,
                           0x1401u /* GL_UNSIGNED_BYTE */, h->pixels);
        if (h->fn_glFlush) h->fn_glFlush();
        SwapBuffers(h->gl_hdc);
        return;
    }

    /* 4. Universal Win2000+ GDI DIBSection BitBlt */
    HDC dst = paint_hdc ? paint_hdc : GetDC(hwnd);
    if (dst) {
        BitBlt(dst, 0, 0, h->fb_w, h->fb_h, h->mem_dc, 0, 0, SRCCOPY);
        if (!paint_hdc) ReleaseDC(hwnd, dst);
    }
}

static void apply_win32_cursor(Win32PresenterHost *h, ArcrCursorKind kind) {
    h->active_cursor = kind;
    HCURSOR c = h->cur_arrow;
    if (kind == ARCR_CURSOR_POINTER && h->cur_pointer) c = h->cur_pointer;
    else if (kind == ARCR_CURSOR_TEXT && h->cur_text)   c = h->cur_text;
    if (c) SetCursor(c);
}

static LRESULT CALLBACK waterscape_win32_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32PresenterHost *h = (Win32PresenterHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == 1u) {
            KillTimer(hwnd, 1u);
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SIZE:
        if (h) {
            int32_t w  = (int32_t)LOWORD(lParam);
            int32_t ht = (int32_t)HIWORD(lParam);
            if (w > 0 && ht > 0) {
                win32_resize_surfaces(h, hwnd, w, ht);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (h && LOWORD(lParam) == HTCLIENT) {
            apply_win32_cursor(h, h->active_cursor);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (h && h->shell) {
            int32_t x = (int32_t)(int16_t)LOWORD(lParam);
            int32_t y = (int32_t)(int16_t)HIWORD(lParam);
            ArcrCursorKind ck = arcr_shell_on_mouse_move(h->shell, x, y);
            apply_win32_cursor(h, ck);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (h && h->shell) {
            int btn = (msg == WM_MBUTTONDOWN) ? 1 : ((msg == WM_RBUTTONDOWN) ? 2 : 0);
            int32_t x = (int32_t)(int16_t)LOWORD(lParam);
            int32_t y = (int32_t)(int16_t)HIWORD(lParam);
            SetCapture(hwnd);
            arcr_shell_on_mouse_down(h->shell, x, y, btn);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (h && h->shell) {
            ReleaseCapture();
            int32_t x = (int32_t)(int16_t)LOWORD(lParam);
            int32_t y = (int32_t)(int16_t)HIWORD(lParam);
            if (arcr_shell_on_mouse_up(h->shell, x, y, 0)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (h && h->shell) {
            int16_t delta = (int16_t)HIWORD(wParam);
            bool    ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int32_t steps = delta / 120;
            if (steps == 0) steps = (delta > 0) ? 1 : -1;
            if (arcr_shell_on_mouse_wheel(h->shell, steps, ctrl)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (h && h->shell) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (wParam >= VK_F1 && wParam <= VK_F12) {
                int fn_num = (int)(wParam - VK_F1) + 1;
                if (arcr_shell_on_key(h->shell, NULL, ARCR_KEY_NONE, ctrl, alt, fn_num)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            ArcrSpecialKey sk = ARCR_KEY_NONE;
            if (wParam == VK_BACK)        sk = ARCR_KEY_BACKSPACE;
            else if (wParam == VK_DELETE) sk = ARCR_KEY_DELETE;
            else if (wParam == VK_RETURN) sk = ARCR_KEY_ENTER;
            else if (wParam == VK_TAB)    sk = ARCR_KEY_TAB;
            else if (wParam == VK_LEFT)   sk = ARCR_KEY_LEFT;
            else if (wParam == VK_RIGHT)  sk = ARCR_KEY_RIGHT;
            else if (wParam == VK_HOME)   sk = ARCR_KEY_HOME;
            else if (wParam == VK_END)    sk = ARCR_KEY_END;
            else if (wParam == VK_ESCAPE) sk = ARCR_KEY_ESCAPE;

            if (sk != ARCR_KEY_NONE) {
                if (arcr_shell_on_key(h->shell, NULL, sk, ctrl, alt, 0)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            /* Ctrl+<Key> accelerators */
            if (ctrl && ((wParam >= 'A' && wParam <= 'Z') ||
                         (wParam >= '0' && wParam <= '9') ||
                         wParam == 0xBB /* VK_OEM_PLUS */ ||
                         wParam == 0xBD /* VK_OEM_MINUS */)) {
                char key_ch[2];
                key_ch[0] = (wParam == 0xBB) ? '=' : ((wParam == 0xBD) ? '-' : (char)wParam);
                key_ch[1] = '\0';
                if (arcr_shell_on_key(h->shell, key_ch, ARCR_KEY_NONE, true, alt, 0)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            /* Up/Down/PgUp/PgDn document scrolling */
            if (!ctrl && !alt &&
                (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_PRIOR || wParam == VK_NEXT)) {
                int32_t steps = (wParam == VK_PRIOR || wParam == VK_NEXT) ? 4 : 1;
                if (wParam == VK_DOWN || wParam == VK_NEXT) steps = -steps;
                if (arcr_shell_on_mouse_wheel(h->shell, steps, false)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        break;
    case WM_CHAR:
        if (h && h->shell) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
            WCHAR wc  = (WCHAR)wParam;
            if (ctrl || wc < 0x20u) return 0;

            WCHAR wseq[3];
            int   wlen = 0;
            if (wc >= 0xD800u && wc <= 0xDBFFu) {
                h->pending_high_surrogate = wc;
                return 0;
            } else if (wc >= 0xDC00u && wc <= 0xDFFFu && h->pending_high_surrogate != 0) {
                wseq[0] = h->pending_high_surrogate;
                wseq[1] = wc;
                wlen    = 2;
                h->pending_high_surrogate = 0;
            } else {
                h->pending_high_surrogate = 0;
                wseq[0] = wc;
                wlen    = 1;
            }
            char utf8[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, wseq, wlen, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
            if (n > 0) {
                utf8[n] = '\0';
                if (arcr_shell_on_key(h->shell, utf8, ARCR_KEY_NONE, false, alt, 0)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    case WM_IME_COMPOSITION:
        if (h && h->shell) {
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                bool committed = (lParam & GCS_RESULTSTR) != 0;
                DWORD flag     = committed ? GCS_RESULTSTR : GCS_COMPSTR;
                LONG bytes     = ImmGetCompositionStringW(himc, flag, NULL, 0);
                if (bytes > 0 && bytes < 512) {
                    WCHAR wbuf[256];
                    ZeroMemory(wbuf, sizeof(wbuf));
                    ImmGetCompositionStringW(himc, flag, wbuf, (DWORD)bytes);
                    char utf8[768];
                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, bytes / (LONG)sizeof(WCHAR),
                                                utf8, (int)sizeof(utf8) - 1, NULL, NULL);
                    if (n > 0) {
                        utf8[n] = '\0';
                        arcr_shell_on_ime_composition(h->shell, utf8, n, committed);
                        char status[832];
                        snprintf(status, sizeof(status),
                                 committed ? "한글 입력 완료: %s" : "한글 조합 중: %s", utf8);
                        arcr_shell_set_status_overlay(h->shell, status);
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                ImmReleaseContext(hwnd, himc);
                if (committed) return 0;
            }
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (h) {
            win32_present_frame(h, hwnd, hdc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (h) teardown_gpu_backends(h, hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int waterscape_run_desktop_window(ArcrEngine *engine,
                                  ArcrBrowserShell *shell,
                                  const WaterscapeWindowOptions *opts) {
    if (!engine || !shell) return 1;

    Win32PresenterHost host;
    ZeroMemory(&host, sizeof(host));
    host.engine      = engine;
    host.shell       = shell;
    host.backend     = WS_PRESENT_WIN32_GDI;
    host.force_cpu   = opts ? opts->force_cpu_presenter : false;
    host.cur_arrow   = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    host.cur_text    = LoadCursorW(NULL, MAKEINTRESOURCEW(32513)); /* IDC_IBEAM */
    host.cur_pointer = LoadCursorW(NULL, IDC_HAND);
    if (!host.cur_pointer) host.cur_pointer = host.cur_arrow;

    int32_t init_w = (opts && opts->initial_width > 0)  ? opts->initial_width  : 1080;
    int32_t init_h = (opts && opts->initial_height > 0) ? opts->initial_height : 720;

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = waterscape_win32_wndproc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor       = host.cur_arrow;
    wc.lpszClassName = L"WaterscapeBrowserWnd10";
    RegisterClassExW(&wc);

    WCHAR wtitle[256];
    const char *utf8_title = (opts && opts->window_title && opts->window_title[0])
                                 ? opts->window_title
                                 : "Waterscape Web Browser 1.0";
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8_title, -1, wtitle, 255);
    if (wn <= 0) {
        lstrcpyW(wtitle, L"Waterscape Web Browser 1.0");
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, init_w, init_h,
        NULL, NULL, hInst, &host);
    if (!hwnd) return 1;

    RECT cr;
    GetClientRect(hwnd, &cr);
    int32_t cw = ARCR_MAX(64, (int32_t)(cr.right - cr.left));
    int32_t ch = ARCR_MAX(64, (int32_t)(cr.bottom - cr.top));
    win32_resize_surfaces(&host, hwnd, cw, ch);

    if (!host.force_cpu) {
        if (!try_init_d3d11(&host, hwnd, cw, ch)) {
            if (!try_init_d3d9(&host, hwnd, cw, ch)) {
                (void)try_init_opengl_icd(&host, hwnd);
            }
        }
    }

    printf("  Win32 Presenter: %s\n", waterscape_present_backend_name(host.backend));

    if (opts && opts->auto_close_ms > 0u) {
        SetTimer(hwnd, 1u, (UINT)opts->auto_close_ms, NULL);
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (host.dib) {
        SelectObject(host.mem_dc, host.old_bmp);
        DeleteObject(host.dib);
    }
    if (host.mem_dc) DeleteDC(host.mem_dc);
    return 0;
}

#else
/* Non-Win32 ISO C11 compliant translation unit placeholder */
typedef int waterscape_win32_non_windows_tu_t;
#endif /* _WIN32 */
