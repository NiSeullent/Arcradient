/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * shell/waterscape_win32.c - Single-Frame Win32 Window, Per-Monitor V2 DPI,
 *                            DWM Extended Titlebar, Imm32 IME, Common Dialog,
 *                            Server-Side UI Automation & Multi-Gen GPU Presenter
 *
 * Presentation Chain (Zero Modern Static DLL Imports — Runs on Win2000 SP4 PE 5.0!):
 *   1. Direct3D 11 (`d3d11.dll`)   : DXGI Flip-Discard (Win10/11) -> Discard (Win7/Vista)
 *   2. Direct3D 9  (`d3d9.dll`)    : Hardware HAL X8R8G8B8 SwapChain + DEVICENOTRESET Reset
 *   3. OpenGL ICD  (`opengl32.dll`): CS_OWNDC + Textured `glTexSubImage2D` Quad (`GL_BGRA_EXT`)
 *   4. Win32 GDI   (`gdi32.dll`)   : Universal Zero-Crash `CreateDIBSection` + `BitBlt`
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
#include <ole2.h>
#include <commdlg.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_MOUSEWHEEL
#  define WM_MOUSEWHEEL 0x020A
#endif
#ifndef WM_NCMOUSELEAVE
#  define WM_NCMOUSELEAVE 0x02A2
#endif
#ifndef WM_DPICHANGED
#  define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_DWMCOMPOSITIONCHANGED
#  define WM_DWMCOMPOSITIONCHANGED 0x031E
#endif
#ifndef WM_GETOBJECT
#  define WM_GETOBJECT 0x003D
#endif
#ifndef IDC_HAND
#  define IDC_HAND MAKEINTRESOURCEW(32649)
#endif
#ifndef IDC_SIZEWE
#  define IDC_SIZEWE MAKEINTRESOURCEW(32644)
#endif
#ifndef IDC_SIZENS
#  define IDC_SIZENS MAKEINTRESOURCEW(32645)
#endif
#ifndef GWLP_USERDATA
#  define GWLP_USERDATA (-21)
#endif
#ifndef MONITOR_DEFAULTTONEAREST
#  define MONITOR_DEFAULTTONEAREST 0x00000002u
#endif
#ifndef SM_CXPADDEDBORDER
#  define SM_CXPADDEDBORDER 92
#endif
#ifndef SPI_GETHIGHCONTRAST
#  define SPI_GETHIGHCONTRAST 0x0042u
#endif
#ifndef HCF_HIGHCONTRASTON
#  define HCF_HIGHCONTRASTON 0x00000001u
#endif
#ifndef OFN_DONTADDTORECENT
#  define OFN_DONTADDTORECENT 0x02000000u
#endif

#define WS_DWMWA_CAPTION_BUTTON_BOUNDS 5u
#define WS_UIA_ROOT_OBJECT_ID          (-25L)
#define WS_UIA_APPEND_RUNTIME_ID       3
#define WS_UIA_E_ELEMENTNOTAVAILABLE   ((HRESULT)0x80040201L)
#define WS_UIA_E_INVALIDOPERATION      ((HRESULT)0x80040200L)

#define WS_D3DERR_DEVICELOST           ((HRESULT)0x88760868L)
#define WS_D3DERR_DEVICENOTRESET       ((HRESULT)0x88760869L)

#define WS_TIMER_TICK_60FPS            1u
#define WS_TIMER_AUTOCLOSE             2u

/* Signed multi-monitor coordinate extraction (critical for negative coords!) */
#define WS_GET_X_LPARAM(lp) ((int32_t)(int16_t)LOWORD(lp))
#define WS_GET_Y_LPARAM(lp) ((int32_t)(int16_t)HIWORD(lp))

const char *waterscape_present_backend_name(WaterscapePresentBackend backend) {
    switch (backend) {
    case WS_PRESENT_WIN32_D3D11:  return "Win32 Direct3D 11 + DXGI SwapChain";
    case WS_PRESENT_WIN32_D3D9:   return "Win32 Direct3D 9 Hardware HAL";
    case WS_PRESENT_WIN32_OPENGL: return "Win32 OpenGL Hardware ICD (Textured Quad)";
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

static uint32_t ws_colorref_to_argb(COLORREF cr) {
    uint32_t r = (uint32_t)(cr & 0xFFu);
    uint32_t g = (uint32_t)((cr >> 8) & 0xFFu);
    uint32_t b = (uint32_t)((cr >> 16) & 0xFFu);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int32_t ws_dip_to_px(int32_t dip, int32_t dpi) {
    int32_t eff = (dpi >= 72 && dpi <= 480) ? dpi : 96;
    if (dip >= 0) return (dip * eff + 48) / 96;
    return -((-dip * eff + 48) / 96);
}

/* -------------------------------------------------------------------------
 * 0. Dynamic Post-Win2000 & Optional System DLL API Table
 * ------------------------------------------------------------------------- */
typedef struct WsDwmMargins {
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
} WsDwmMargins;

typedef struct WsHighContrastW {
    UINT   cbSize;
    DWORD  dwFlags;
    LPWSTR lpszDefaultScheme;
} WsHighContrastW;

typedef HRESULT (WINAPI *PFN_SetCurrentProcessExplicitAppUserModelID)(PCWSTR AppID);
typedef void    (WINAPI *PFN_SHAddToRecentDocs)(UINT uFlags, LPCVOID pv);
typedef UINT    (WINAPI *PFN_GetDpiForWindow)(HWND hwnd);
typedef UINT    (WINAPI *PFN_GetDpiForSystem)(void);
typedef int     (WINAPI *PFN_GetSystemMetricsForDpi)(int nIndex, UINT dpi);
typedef BOOL    (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT lpRect, DWORD dwStyle,
                                                       BOOL bMenu, DWORD dwExStyle, UINT dpi);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR hmonitor, int dpiType,
                                               UINT *dpiX, UINT *dpiY);
typedef HRESULT (WINAPI *PFN_DwmIsCompositionEnabled)(BOOL *pfEnabled);
typedef HRESULT (WINAPI *PFN_DwmExtendFrameIntoClientArea)(HWND hWnd, const WsDwmMargins *pMarInset);
typedef BOOL    (WINAPI *PFN_DwmDefWindowProc)(HWND hWnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, LRESULT *plResult);
typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND hwnd, DWORD dwAttribute,
                                                    PVOID pvAttribute, DWORD cbAttribute);
typedef BOOL    (WINAPI *PFN_GetOpenFileNameW)(LPOPENFILENAMEW lpofn);
typedef BOOL    (WINAPI *PFN_GetSaveFileNameW)(LPOPENFILENAMEW lpofn);
typedef BSTR    (WINAPI *PFN_SysAllocString)(const OLECHAR *psz);
typedef void    (WINAPI *PFN_SysFreeString)(BSTR bstrString);
typedef SAFEARRAY *(WINAPI *PFN_SafeArrayCreateVector)(VARTYPE vt, LONG lLbound, ULONG cElements);
typedef HRESULT (WINAPI *PFN_SafeArrayPutElement)(SAFEARRAY *psa, LONG *rgIndices, void *pv);
typedef HRESULT (WINAPI *PFN_SafeArrayDestroy)(SAFEARRAY *psa);
typedef LRESULT (WINAPI *PFN_UiaReturnRawElementProvider)(HWND hwnd, WPARAM wParam,
                                                          LPARAM lParam, void *el);
typedef HRESULT (WINAPI *PFN_UiaHostProviderFromHwnd)(HWND hwnd, void **ppProvider);

typedef struct WsWin32DynApis {
    HMODULE                                     shcore_dll;
    HMODULE                                     dwmapi_dll;
    HMODULE                                     oleaut32_dll;
    HMODULE                                     uia_dll;

    PFN_SetCurrentProcessExplicitAppUserModelID SetCurrentProcessExplicitAppUserModelID;
    PFN_SHAddToRecentDocs                       SHAddToRecentDocs;
    PFN_GetDpiForWindow                         GetDpiForWindow;
    PFN_GetDpiForSystem                         GetDpiForSystem;
    PFN_GetSystemMetricsForDpi                  GetSystemMetricsForDpi;
    PFN_AdjustWindowRectExForDpi                AdjustWindowRectExForDpi;
    PFN_GetDpiForMonitor                        GetDpiForMonitor;

    PFN_DwmIsCompositionEnabled                 DwmIsCompositionEnabled;
    PFN_DwmExtendFrameIntoClientArea            DwmExtendFrameIntoClientArea;
    PFN_DwmDefWindowProc                        DwmDefWindowProc;
    PFN_DwmGetWindowAttribute                   DwmGetWindowAttribute;

    PFN_SysAllocString                          SysAllocString;
    PFN_SysFreeString                           SysFreeString;
    PFN_SafeArrayCreateVector                   SafeArrayCreateVector;
    PFN_SafeArrayPutElement                     SafeArrayPutElement;
    PFN_SafeArrayDestroy                        SafeArrayDestroy;

    PFN_UiaReturnRawElementProvider             UiaReturnRawElementProvider;
    PFN_UiaHostProviderFromHwnd                 UiaHostProviderFromHwnd;
} WsWin32DynApis;

static void ws_win32_load_dyn_apis(WsWin32DynApis *api) {
    ZeroMemory(api, sizeof(*api));

    HMODULE hShell32 = GetModuleHandleA("shell32.dll");
    if (hShell32) {
        WS_LOAD_PROC(hShell32, api->SetCurrentProcessExplicitAppUserModelID,
                     "SetCurrentProcessExplicitAppUserModelID");
        WS_LOAD_PROC(hShell32, api->SHAddToRecentDocs, "SHAddToRecentDocs");
    }

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        WS_LOAD_PROC(hUser32, api->GetDpiForWindow,          "GetDpiForWindow");
        WS_LOAD_PROC(hUser32, api->GetDpiForSystem,          "GetDpiForSystem");
        WS_LOAD_PROC(hUser32, api->GetSystemMetricsForDpi,   "GetSystemMetricsForDpi");
        WS_LOAD_PROC(hUser32, api->AdjustWindowRectExForDpi, "AdjustWindowRectExForDpi");
    }

    api->shcore_dll = LoadLibraryA("shcore.dll");
    if (api->shcore_dll) {
        WS_LOAD_PROC(api->shcore_dll, api->GetDpiForMonitor, "GetDpiForMonitor");
    }

    api->dwmapi_dll = LoadLibraryA("dwmapi.dll");
    if (api->dwmapi_dll) {
        WS_LOAD_PROC(api->dwmapi_dll, api->DwmIsCompositionEnabled,      "DwmIsCompositionEnabled");
        WS_LOAD_PROC(api->dwmapi_dll, api->DwmExtendFrameIntoClientArea, "DwmExtendFrameIntoClientArea");
        WS_LOAD_PROC(api->dwmapi_dll, api->DwmDefWindowProc,             "DwmDefWindowProc");
        WS_LOAD_PROC(api->dwmapi_dll, api->DwmGetWindowAttribute,        "DwmGetWindowAttribute");
    }

    api->oleaut32_dll = LoadLibraryA("oleaut32.dll");
    if (api->oleaut32_dll) {
        WS_LOAD_PROC(api->oleaut32_dll, api->SysAllocString,        "SysAllocString");
        WS_LOAD_PROC(api->oleaut32_dll, api->SysFreeString,         "SysFreeString");
        WS_LOAD_PROC(api->oleaut32_dll, api->SafeArrayCreateVector, "SafeArrayCreateVector");
        WS_LOAD_PROC(api->oleaut32_dll, api->SafeArrayPutElement,   "SafeArrayPutElement");
        WS_LOAD_PROC(api->oleaut32_dll, api->SafeArrayDestroy,      "SafeArrayDestroy");
    }

    api->uia_dll = LoadLibraryA("uiautomationcore.dll");
    if (api->uia_dll) {
        WS_LOAD_PROC(api->uia_dll, api->UiaReturnRawElementProvider, "UiaReturnRawElementProvider");
        WS_LOAD_PROC(api->uia_dll, api->UiaHostProviderFromHwnd,     "UiaHostProviderFromHwnd");
    }
}

static void ws_win32_free_dyn_apis(WsWin32DynApis *api) {
    if (api->uia_dll)      { FreeLibrary(api->uia_dll);      api->uia_dll      = NULL; }
    if (api->oleaut32_dll) { FreeLibrary(api->oleaut32_dll); api->oleaut32_dll = NULL; }
    if (api->dwmapi_dll)   { FreeLibrary(api->dwmapi_dll);   api->dwmapi_dll   = NULL; }
    if (api->shcore_dll)   { FreeLibrary(api->shcore_dll);   api->shcore_dll   = NULL; }
}

/* -------------------------------------------------------------------------
 * 1. Minimal Pure C COM ABI Definitions for Dynamic D3D11 / DXGI
 * ------------------------------------------------------------------------- */
typedef struct WsGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} WsGuid;

static bool ws_guid_eq(const void *a, const WsGuid *b) {
    return a && b && (memcmp(a, b, sizeof(WsGuid)) == 0);
}

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
    void    *unk_methods[8];
    HRESULT (STDMETHODCALLTYPE *Present)(WsDxgiSwapChain *This, UINT SyncInterval, UINT Flags);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(WsDxgiSwapChain *This, UINT Buffer,
                                           const WsGuid *riid, void **ppSurface);
    void    *fs_methods[3];
    HRESULT (STDMETHODCALLTYPE *ResizeBuffers)(WsDxgiSwapChain *This, UINT BufferCount,
                                               UINT Width, UINT Height,
                                               UINT NewFormat, UINT SwapChainFlags);
} WsDxgiSwapChainVtbl;
struct WsDxgiSwapChain {
    const WsDxgiSwapChainVtbl *lpVtbl;
};

typedef struct WsD3D11DeviceContext WsD3D11DeviceContext;
typedef struct WsD3D11DeviceContextVtbl {
    void *prior_methods[48];
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
    void    *unk_and_res_methods[13];
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
    void    *mid_methods[12];
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
    void    *prior_methods[16];
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
 * 3. Dynamic OpenGL ICD Textured-Quad Function Pointers
 * ------------------------------------------------------------------------- */
typedef HGLRC          (WINAPI   *PFN_wglCreateContext)(HDC);
typedef BOOL           (WINAPI   *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL           (WINAPI   *PFN_wglDeleteContext)(HGLRC);
typedef const uint8_t *(APIENTRY *PFN_glGetString)(uint32_t);
typedef void           (APIENTRY *PFN_glViewport)(int, int, int, int);
typedef void           (APIENTRY *PFN_glGenTextures)(int, uint32_t *);
typedef void           (APIENTRY *PFN_glDeleteTextures)(int, const uint32_t *);
typedef void           (APIENTRY *PFN_glBindTexture)(uint32_t, uint32_t);
typedef void           (APIENTRY *PFN_glTexParameteri)(uint32_t, uint32_t, int);
typedef void           (APIENTRY *PFN_glTexImage2D)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void *);
typedef void           (APIENTRY *PFN_glTexSubImage2D)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void *);
typedef void           (APIENTRY *PFN_glEnable)(uint32_t);
typedef void           (APIENTRY *PFN_glDisable)(uint32_t);
typedef void           (APIENTRY *PFN_glMatrixMode)(uint32_t);
typedef void           (APIENTRY *PFN_glLoadIdentity)(void);
typedef void           (APIENTRY *PFN_glPixelStorei)(uint32_t, int);
typedef void           (APIENTRY *PFN_glColor4f)(float, float, float, float);
typedef void           (APIENTRY *PFN_glBegin)(uint32_t);
typedef void           (APIENTRY *PFN_glTexCoord2f)(float, float);
typedef void           (APIENTRY *PFN_glVertex2f)(float, float);
typedef void           (APIENTRY *PFN_glEnd)(void);
typedef void           (APIENTRY *PFN_glFlush)(void);

typedef struct WsUiaNode WsUiaNode;

typedef struct Win32PresenterHost {
    HWND                     hwnd;
    ArcrEngine              *engine;
    ArcrBrowserShell        *shell;
    WaterscapePresentBackend backend;
    bool                     force_cpu;
    bool                     force_classic_frame;
    bool                     dwm_extended;
    bool                     uia_used;
    int32_t                  ime_target_zone; /* 0 = omnibar, 1 = findbar, 2 = viewport */
    int32_t                  last_vp_click_x;
    int32_t                  last_vp_click_y;
    WaterscapeFrameMetrics   fm;
    WsWin32DynApis           api;
    WsUiaNode               *uia_root;

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

    /* Dynamic OpenGL ICD Textured Quad State (`CS_OWNDC`) */
    HMODULE                  gl_dll;
    HDC                      gl_hdc;
    HGLRC                    gl_rc;
    uint32_t                 gl_tex;
    int32_t                  gl_tex_w;
    int32_t                  gl_tex_h;
    PFN_wglMakeCurrent       fn_wglMakeCurrent;
    PFN_wglDeleteContext     fn_wglDeleteContext;
    PFN_glViewport           fn_glViewport;
    PFN_glGenTextures        fn_glGenTextures;
    PFN_glDeleteTextures     fn_glDeleteTextures;
    PFN_glBindTexture        fn_glBindTexture;
    PFN_glTexParameteri      fn_glTexParameteri;
    PFN_glTexImage2D         fn_glTexImage2D;
    PFN_glTexSubImage2D      fn_glTexSubImage2D;
    PFN_glEnable             fn_glEnable;
    PFN_glDisable            fn_glDisable;
    PFN_glMatrixMode         fn_glMatrixMode;
    PFN_glLoadIdentity       fn_glLoadIdentity;
    PFN_glPixelStorei        fn_glPixelStorei;
    PFN_glColor4f            fn_glColor4f;
    PFN_glBegin              fn_glBegin;
    PFN_glTexCoord2f         fn_glTexCoord2f;
    PFN_glVertex2f           fn_glVertex2f;
    PFN_glEnd                fn_glEnd;
    PFN_glFlush              fn_glFlush;

    /* OS Borderless Fullscreen State (F11) */
    bool                     os_fullscreen;
    DWORD                    saved_style;
    DWORD                    saved_ex_style;
    WINDOWPLACEMENT          saved_placement;

    /* Cursors & UTF-16 High Surrogate Buffer */
    HCURSOR                  cur_arrow;
    HCURSOR                  cur_pointer;
    HCURSOR                  cur_text;
    HCURSOR                  cur_size_we;
    HCURSOR                  cur_size_ns;
    ArcrCursorKind           active_cursor;
    int                      splitter_cursor_dir; /* 0 = none, 1 = WE, 2 = NS */
    int32_t                  dpi_from_msg;        /* Authoritative DPI from WM_DPICHANGED when > 0 */
    WCHAR                    pending_high_surrogate;
} Win32PresenterHost;

static Win32PresenterHost *g_ws_active_host = NULL;

/* -------------------------------------------------------------------------
 * 4. Owned Win32 Common File Dialog (`waterscape_show_file_dialog`)
 * ------------------------------------------------------------------------- */
bool waterscape_show_file_dialog(bool is_save, const char *default_filename_utf8,
                                 char *out_path_utf8, size_t out_cap) {
    if (!out_path_utf8 || out_cap == 0u) return false;
    out_path_utf8[0] = '\0';

    const char *env_override = getenv("WATERSCAPE_FILE_DIALOG_PATH");
    if (env_override && env_override[0] != '\0') {
        arcr_str_copy(out_path_utf8, out_cap, env_override);
        return true;
    }

    HMODULE hComdlg = LoadLibraryA("comdlg32.dll");
    if (!hComdlg) return false;

    PFN_GetOpenFileNameW fn_open = NULL;
    PFN_GetSaveFileNameW fn_save = NULL;
    WS_LOAD_PROC(hComdlg, fn_open, "GetOpenFileNameW");
    WS_LOAD_PROC(hComdlg, fn_save, "GetSaveFileNameW");

    if ((is_save && !fn_save) || (!is_save && !fn_open)) {
        FreeLibrary(hComdlg);
        return false;
    }

    HWND  owner_hwnd = (g_ws_active_host && IsWindow(g_ws_active_host->hwnd))
                           ? g_ws_active_host->hwnd
                           : NULL;
    bool  is_private = (g_ws_active_host && g_ws_active_host->shell)
                           ? arcr_shell_is_private_browsing(g_ws_active_host->shell)
                           : false;

    WCHAR file_buf[MAX_PATH * 2];
    ZeroMemory(file_buf, sizeof(file_buf));
    if (default_filename_utf8 && default_filename_utf8[0] != '\0') {
        MultiByteToWideChar(CP_UTF8, 0, default_filename_utf8, -1,
                            file_buf, (int)(sizeof(file_buf) / sizeof(file_buf[0])) - 1);
    }

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner_hwnd;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0HTML Documents (*.html;*.htm)\0*.html;*.htm\0\0";
    ofn.nFilterIndex = 1u;
    ofn.lpstrFile   = file_buf;
    ofn.nMaxFile    = (DWORD)(sizeof(file_buf) / sizeof(file_buf[0]));
    ofn.Flags       = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (is_save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    }
    if (is_private) {
        /* Never record Recent Docs entries during Private Browsing! */
        ofn.Flags |= OFN_DONTADDTORECENT;
    }

    BOOL accepted = is_save ? fn_save(&ofn) : fn_open(&ofn);
    FreeLibrary(hComdlg);

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        SetActiveWindow(owner_hwnd);
        SetFocus(owner_hwnd);
    }

    if (!accepted || file_buf[0] == L'\0') {
        return false;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, out_path_utf8, (int)out_cap, NULL, NULL);
    if (n <= 0) {
        out_path_utf8[0] = '\0';
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * 5. Pure C11 Server-Side UI Automation (`WM_GETOBJECT` / `IRawElementProvider*`)
 * ------------------------------------------------------------------------- */
static const WsGuid kIID_WsIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}
};
static const WsGuid kIID_IRawElementProviderSimple = {
    0xD6DD68D1u, 0x86FDu, 0x4332u, {0x86u, 0x66u, 0x9Au, 0xBEu, 0xDEu, 0xA2u, 0xD2u, 0x4Cu}
};
static const WsGuid kIID_IRawElementProviderFragment = {
    0xF7063DA8u, 0x8359u, 0x439Cu, {0x92u, 0x97u, 0xBBu, 0xC5u, 0x29u, 0x9Au, 0x7Du, 0x87u}
};
static const WsGuid kIID_IRawElementProviderFragmentRoot = {
    0x620CE2A5u, 0xAB8Fu, 0x40A9u, {0x86u, 0xCBu, 0xDEu, 0x3Cu, 0x75u, 0x59u, 0x9Bu, 0x58u}
};
static const WsGuid kIID_IInvokeProvider = {
    0x54FCB24Bu, 0xE18Eu, 0x47A2u, {0xB4u, 0xD3u, 0xECu, 0xCBu, 0xE7u, 0x75u, 0x99u, 0xA2u}
};
static const WsGuid kIID_IValueProvider = {
    0xC7935180u, 0x6FB3u, 0x4201u, {0xB1u, 0x74u, 0x7Du, 0xF7u, 0x3Au, 0xDBu, 0xF6u, 0x4Au}
};
static const WsGuid kIID_ISelectionItemProvider = {
    0x2ACAD808u, 0xB2D4u, 0x452Du, {0xA4u, 0x07u, 0x91u, 0xFFu, 0x1Au, 0xD1u, 0x67u, 0xB2u}
};

enum {
    WS_UIA_InvokePatternId               = 10000,
    WS_UIA_ValuePatternId                = 10002,
    WS_UIA_SelectionItemPatternId        = 10010,

    WS_UIA_ProcessIdPropertyId           = 30002,
    WS_UIA_ControlTypePropertyId         = 30003,
    WS_UIA_LocalizedControlTypePropertyId= 30004,
    WS_UIA_NamePropertyId                = 30005,
    WS_UIA_HasKeyboardFocusPropertyId    = 30008,
    WS_UIA_IsKeyboardFocusablePropertyId = 30009,
    WS_UIA_IsEnabledPropertyId           = 30010,
    WS_UIA_AutomationIdPropertyId        = 30011,
    WS_UIA_IsControlElementPropertyId    = 30016,
    WS_UIA_IsContentElementPropertyId    = 30017,
    WS_UIA_NativeWindowHandlePropertyId  = 30020,
    WS_UIA_IsOffscreenPropertyId         = 30022,
    WS_UIA_ValueValuePropertyId          = 30045,

    WS_UIA_ButtonControlTypeId           = 50000,
    WS_UIA_EditControlTypeId             = 50004,
    WS_UIA_TabItemControlTypeId          = 50019,
    WS_UIA_DocumentControlTypeId         = 50030,
    WS_UIA_WindowControlTypeId           = 50032,
    WS_UIA_PaneControlTypeId             = 50033,

    WS_UIA_Nav_Parent                    = 0,
    WS_UIA_Nav_NextSibling               = 1,
    WS_UIA_Nav_PreviousSibling           = 2,
    WS_UIA_Nav_FirstChild                = 3,
    WS_UIA_Nav_LastChild                 = 4
};

typedef enum WsUiaNodeKind {
    WS_UIA_NODE_ROOT          = 0,
    WS_UIA_NODE_TAB_ITEM      = 1,
    WS_UIA_NODE_NEW_TAB_BTN   = 2,
    WS_UIA_NODE_BACK_BTN      = 3,
    WS_UIA_NODE_FORWARD_BTN   = 4,
    WS_UIA_NODE_RELOAD_BTN    = 5,
    WS_UIA_NODE_OMNIBAR_EDIT  = 6,
    WS_UIA_NODE_MENU_BTN      = 7,
    WS_UIA_NODE_WEB_DOCUMENT  = 8,
    WS_UIA_NODE_DEVTOOLS_DOCK = 9
} WsUiaNodeKind;

typedef struct WsUiaRect {
    double left;
    double top;
    double width;
    double height;
} WsUiaRect;

typedef struct WsUiaSimpleObj   WsUiaSimpleObj;
typedef struct WsUiaFragmentObj WsUiaFragmentObj;
typedef struct WsUiaFragRootObj WsUiaFragRootObj;
typedef struct WsUiaInvokeObj   WsUiaInvokeObj;
typedef struct WsUiaValueObj    WsUiaValueObj;
typedef struct WsUiaSelItemObj  WsUiaSelItemObj;

typedef struct WsUiaSimpleVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaSimpleObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaSimpleObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaSimpleObj *This);
    HRESULT (STDMETHODCALLTYPE *get_ProviderOptions)(WsUiaSimpleObj *This, int *pRetVal);
    HRESULT (STDMETHODCALLTYPE *GetPatternProvider)(WsUiaSimpleObj *This, int patternId, void **pRetVal);
    HRESULT (STDMETHODCALLTYPE *GetPropertyValue)(WsUiaSimpleObj *This, int propertyId, VARIANT *pRetVal);
    HRESULT (STDMETHODCALLTYPE *get_HostRawElementProvider)(WsUiaSimpleObj *This, void **pRetVal);
} WsUiaSimpleVtbl;

typedef struct WsUiaFragmentVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaFragmentObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaFragmentObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaFragmentObj *This);
    HRESULT (STDMETHODCALLTYPE *Navigate)(WsUiaFragmentObj *This, int direction, void **pRetVal);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeId)(WsUiaFragmentObj *This, SAFEARRAY **pRetVal);
    HRESULT (STDMETHODCALLTYPE *get_BoundingRectangle)(WsUiaFragmentObj *This, WsUiaRect *pRetVal);
    HRESULT (STDMETHODCALLTYPE *GetEmbeddedFragmentRoots)(WsUiaFragmentObj *This, SAFEARRAY **pRetVal);
    HRESULT (STDMETHODCALLTYPE *SetFocus)(WsUiaFragmentObj *This);
    HRESULT (STDMETHODCALLTYPE *get_FragmentRoot)(WsUiaFragmentObj *This, void **pRetVal);
} WsUiaFragmentVtbl;

typedef struct WsUiaFragRootVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaFragRootObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaFragRootObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaFragRootObj *This);
    HRESULT (STDMETHODCALLTYPE *ElementProviderFromPoint)(WsUiaFragRootObj *This, double x, double y, void **pRetVal);
    HRESULT (STDMETHODCALLTYPE *GetFocus)(WsUiaFragRootObj *This, void **pRetVal);
} WsUiaFragRootVtbl;

typedef struct WsUiaInvokeVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaInvokeObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaInvokeObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaInvokeObj *This);
    HRESULT (STDMETHODCALLTYPE *Invoke)(WsUiaInvokeObj *This);
} WsUiaInvokeVtbl;

typedef struct WsUiaValueVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaValueObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaValueObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaValueObj *This);
    HRESULT (STDMETHODCALLTYPE *SetValue)(WsUiaValueObj *This, LPCWSTR val);
    HRESULT (STDMETHODCALLTYPE *get_Value)(WsUiaValueObj *This, BSTR *pRetVal);
    HRESULT (STDMETHODCALLTYPE *get_IsReadOnly)(WsUiaValueObj *This, BOOL *pRetVal);
} WsUiaValueVtbl;

typedef struct WsUiaSelItemVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaSelItemObj *This, const WsGuid *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaSelItemObj *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaSelItemObj *This);
    HRESULT (STDMETHODCALLTYPE *Select)(WsUiaSelItemObj *This);
    HRESULT (STDMETHODCALLTYPE *AddToSelection)(WsUiaSelItemObj *This);
    HRESULT (STDMETHODCALLTYPE *RemoveFromSelection)(WsUiaSelItemObj *This);
    HRESULT (STDMETHODCALLTYPE *get_IsSelected)(WsUiaSelItemObj *This, BOOL *pRetVal);
    HRESULT (STDMETHODCALLTYPE *get_SelectionContainer)(WsUiaSelItemObj *This, void **pRetVal);
} WsUiaSelItemVtbl;

struct WsUiaSimpleObj   { const WsUiaSimpleVtbl   *lpVtbl; WsUiaNode *owner; };
struct WsUiaFragmentObj { const WsUiaFragmentVtbl *lpVtbl; WsUiaNode *owner; };
struct WsUiaFragRootObj { const WsUiaFragRootVtbl *lpVtbl; WsUiaNode *owner; };
struct WsUiaInvokeObj   { const WsUiaInvokeVtbl   *lpVtbl; WsUiaNode *owner; };
struct WsUiaValueObj    { const WsUiaValueVtbl    *lpVtbl; WsUiaNode *owner; };
struct WsUiaSelItemObj  { const WsUiaSelItemVtbl  *lpVtbl; WsUiaNode *owner; };

struct WsUiaNode {
    WsUiaSimpleObj      simple;
    WsUiaFragmentObj    frag;
    WsUiaFragRootObj    frag_root;
    WsUiaInvokeObj      invoke;
    WsUiaValueObj       value;
    WsUiaSelItemObj     sel_item;
    LONG                ref_count;
    Win32PresenterHost *host;
    WsUiaNodeKind       kind;
    size_t              tab_index;
};

typedef struct WsUiaChildSpec {
    WsUiaNodeKind kind;
    size_t        tab_index;
} WsUiaChildSpec;

static WsUiaNode *ws_uia_create_node(Win32PresenterHost *host, WsUiaNodeKind kind, size_t tab_index);

static bool ws_uia_host_alive(const WsUiaNode *n) {
    return n && n->host && (n->host == g_ws_active_host) &&
           n->host->shell && n->host->hwnd && IsWindow(n->host->hwnd);
}

static BSTR ws_uia_alloc_bstr_utf8(Win32PresenterHost *h, const char *utf8) {
    if (!h || !h->api.SysAllocString) return NULL;
    if (!utf8) utf8 = "";
    WCHAR wbuf[512];
    wbuf[0] = L'\0';
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])) - 1);
    if (wn <= 0) wbuf[0] = L'\0';
    return h->api.SysAllocString(wbuf);
}

static size_t ws_uia_build_children(Win32PresenterHost *h, WsUiaChildSpec *out_list, size_t cap) {
    if (!h || !h->shell || !out_list || cap == 0u) return 0u;
    size_t cnt = 0u;
    size_t tabs = arcr_shell_tab_count(h->shell);
    for (size_t i = 0u; i < tabs && cnt < cap; ++i) {
        out_list[cnt++] = (WsUiaChildSpec){WS_UIA_NODE_TAB_ITEM, i};
    }
    if (!arcr_shell_is_fullscreen(h->shell)) {
        const WsUiaNodeKind kChromeControls[6] = {
            WS_UIA_NODE_NEW_TAB_BTN,
            WS_UIA_NODE_BACK_BTN,
            WS_UIA_NODE_FORWARD_BTN,
            WS_UIA_NODE_RELOAD_BTN,
            WS_UIA_NODE_OMNIBAR_EDIT,
            WS_UIA_NODE_MENU_BTN
        };
        for (size_t k = 0u; k < 6u && cnt < cap; ++k) {
            out_list[cnt++] = (WsUiaChildSpec){kChromeControls[k], 0u};
        }
    }
    if (cnt < cap) {
        out_list[cnt++] = (WsUiaChildSpec){WS_UIA_NODE_WEB_DOCUMENT, 0u};
    }
    if (arcr_shell_is_devtools_visible(h->shell) && !arcr_shell_is_fullscreen(h->shell) && cnt < cap) {
        out_list[cnt++] = (WsUiaChildSpec){WS_UIA_NODE_DEVTOOLS_DOCK, 0u};
    }
    return cnt;
}

static bool ws_uia_compute_client_rect(Win32PresenterHost *h, WsUiaNodeKind kind,
                                       size_t tab_index, ArcrRect *out_rc) {
    if (!h || !h->shell || !out_rc) return false;
    ZeroMemory(out_rc, sizeof(*out_rc));

    if (kind == WS_UIA_NODE_ROOT) {
        RECT cr;
        if (!GetClientRect(h->hwnd, &cr)) return false;
        *out_rc = (ArcrRect){0, 0, cr.right - cr.left, cr.bottom - cr.top};
        return true;
    }

    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(h->shell, &m);
    int32_t dpi = h->fm.dpi >= 72 ? h->fm.dpi : 96;

    if (kind == WS_UIA_NODE_TAB_ITEM) {
        return arcr_shell_get_tab_rect(h->shell, tab_index, out_rc);
    }
    if (kind == WS_UIA_NODE_NEW_TAB_BTN) {
        if (m.tab_strip_rect.height <= 0) return false;
        int32_t plus_w = ws_dip_to_px(28, dpi);
        int32_t plus_h = ws_dip_to_px(24, dpi);
        int32_t plus_y = m.tab_strip_rect.y + (m.tab_strip_rect.height - plus_h) / 2;
        int32_t right_x = m.tab_strip_rect.x + ws_dip_to_px(6, dpi);
        size_t  tc = arcr_shell_tab_count(h->shell);
        for (size_t i = 0u; i < tc; ++i) {
            ArcrRect tr;
            if (arcr_shell_get_tab_rect(h->shell, i, &tr)) {
                if (tr.x + tr.width > right_x) right_x = tr.x + tr.width;
            }
        }
        if (arcr_shell_get_tab_overflow_count(h->shell) > 0) {
            right_x += ws_dip_to_px(4 + 22, dpi);
        }
        *out_rc = (ArcrRect){right_x + ws_dip_to_px(3, dpi), plus_y, plus_w, plus_h};
        return true;
    }
    if (kind == WS_UIA_NODE_BACK_BTN || kind == WS_UIA_NODE_FORWARD_BTN ||
        kind == WS_UIA_NODE_RELOAD_BTN || kind == WS_UIA_NODE_MENU_BTN) {
        if (m.nav_toolbar_rect.height <= 0) return false;
        int32_t pad_x   = ws_dip_to_px(8, dpi);
        int32_t btn_sz  = ws_dip_to_px(28, dpi);
        int32_t btn_gap = ws_dip_to_px(4, dpi);
        int32_t by      = m.nav_toolbar_rect.y + (m.nav_toolbar_rect.height - btn_sz) / 2;
        if (kind == WS_UIA_NODE_BACK_BTN) {
            *out_rc = (ArcrRect){m.nav_toolbar_rect.x + pad_x, by, btn_sz, btn_sz};
        } else if (kind == WS_UIA_NODE_FORWARD_BTN) {
            *out_rc = (ArcrRect){m.nav_toolbar_rect.x + pad_x + (btn_sz + btn_gap), by, btn_sz, btn_sz};
        } else if (kind == WS_UIA_NODE_RELOAD_BTN) {
            *out_rc = (ArcrRect){m.nav_toolbar_rect.x + pad_x + 2 * (btn_sz + btn_gap), by, btn_sz, btn_sz};
        } else {
            int32_t mw = ws_dip_to_px(30, dpi);
            *out_rc = (ArcrRect){m.nav_toolbar_rect.x + m.nav_toolbar_rect.width - pad_x - mw,
                                 by, mw, btn_sz};
        }
        return true;
    }
    if (kind == WS_UIA_NODE_OMNIBAR_EDIT) {
        if (m.address_bar_rect.width <= 0) return false;
        *out_rc = m.address_bar_rect;
        return true;
    }
    if (kind == WS_UIA_NODE_WEB_DOCUMENT) {
        if (m.content_viewport.width <= 0 || m.content_viewport.height <= 0) return false;
        *out_rc = m.content_viewport;
        return true;
    }
    if (kind == WS_UIA_NODE_DEVTOOLS_DOCK) {
        if (m.devtools_dock_rect.width <= 0 || m.devtools_dock_rect.height <= 0) return false;
        *out_rc = m.devtools_dock_rect;
        return true;
    }
    return false;
}

static bool ws_uia_has_invoke(WsUiaNodeKind k) {
    return k == WS_UIA_NODE_TAB_ITEM || k == WS_UIA_NODE_NEW_TAB_BTN ||
           k == WS_UIA_NODE_BACK_BTN || k == WS_UIA_NODE_FORWARD_BTN ||
           k == WS_UIA_NODE_RELOAD_BTN || k == WS_UIA_NODE_MENU_BTN;
}

static bool ws_uia_has_value(WsUiaNodeKind k) {
    return k == WS_UIA_NODE_OMNIBAR_EDIT || k == WS_UIA_NODE_WEB_DOCUMENT;
}

static HRESULT ws_uia_query_node(WsUiaNode *n, const WsGuid *riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!n || !riid) return E_INVALIDARG;

    if (ws_guid_eq(riid, &kIID_WsIUnknown) || ws_guid_eq(riid, &kIID_IRawElementProviderSimple)) {
        *ppv = &n->simple;
    } else if (ws_guid_eq(riid, &kIID_IRawElementProviderFragment)) {
        *ppv = &n->frag;
    } else if (n->kind == WS_UIA_NODE_ROOT && ws_guid_eq(riid, &kIID_IRawElementProviderFragmentRoot)) {
        *ppv = &n->frag_root;
    } else if (ws_uia_has_invoke(n->kind) && ws_guid_eq(riid, &kIID_IInvokeProvider)) {
        *ppv = &n->invoke;
    } else if (ws_uia_has_value(n->kind) && ws_guid_eq(riid, &kIID_IValueProvider)) {
        *ppv = &n->value;
    } else if (n->kind == WS_UIA_NODE_TAB_ITEM && ws_guid_eq(riid, &kIID_ISelectionItemProvider)) {
        *ppv = &n->sel_item;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&n->ref_count);
    return S_OK;
}

static ULONG ws_uia_addref_node(WsUiaNode *n) {
    return (ULONG)InterlockedIncrement(&n->ref_count);
}

static ULONG ws_uia_release_node(WsUiaNode *n) {
    LONG rc = InterlockedDecrement(&n->ref_count);
    if (rc <= 0) {
        free(n);
        return 0u;
    }
    return (ULONG)rc;
}

static HRESULT STDMETHODCALLTYPE ws_uia_simple_qi(WsUiaSimpleObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_simple_addref(WsUiaSimpleObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_simple_release(WsUiaSimpleObj *This) {
    return ws_uia_release_node(This->owner);
}

static HRESULT STDMETHODCALLTYPE ws_uia_simple_get_options(WsUiaSimpleObj *This, int *pRetVal) {
    (void)This;
    if (!pRetVal) return E_POINTER;
    /* ProviderOptions_ServerSideProvider (0x2) | ProviderOptions_UseComThreading (0x20) */
    *pRetVal = 0x22;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_simple_get_pattern(WsUiaSimpleObj *This, int patternId, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;

    if (patternId == WS_UIA_InvokePatternId && ws_uia_has_invoke(n->kind)) {
        *pRetVal = &n->invoke;
        ws_uia_addref_node(n);
    } else if (patternId == WS_UIA_ValuePatternId && ws_uia_has_value(n->kind)) {
        *pRetVal = &n->value;
        ws_uia_addref_node(n);
    } else if (patternId == WS_UIA_SelectionItemPatternId && n->kind == WS_UIA_NODE_TAB_ITEM) {
        *pRetVal = &n->sel_item;
        ws_uia_addref_node(n);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_simple_get_prop(WsUiaSimpleObj *This, int propertyId, VARIANT *pRetVal) {
    if (!pRetVal) return E_POINTER;
    ZeroMemory(pRetVal, sizeof(*pRetVal));
    V_VT(pRetVal) = VT_EMPTY;

    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;
    ArcrBrowserShell *sh  = h->shell;

    if (propertyId == WS_UIA_ProcessIdPropertyId) {
        V_VT(pRetVal) = VT_I4;
        V_I4(pRetVal) = (LONG)GetCurrentProcessId();
        return S_OK;
    }
    if (propertyId == WS_UIA_ControlTypePropertyId) {
        LONG ct = WS_UIA_PaneControlTypeId;
        switch (n->kind) {
        case WS_UIA_NODE_ROOT:          ct = WS_UIA_WindowControlTypeId;   break;
        case WS_UIA_NODE_TAB_ITEM:      ct = WS_UIA_TabItemControlTypeId;  break;
        case WS_UIA_NODE_OMNIBAR_EDIT:  ct = WS_UIA_EditControlTypeId;     break;
        case WS_UIA_NODE_WEB_DOCUMENT:  ct = WS_UIA_DocumentControlTypeId; break;
        case WS_UIA_NODE_DEVTOOLS_DOCK: ct = WS_UIA_PaneControlTypeId;     break;
        default:                        ct = WS_UIA_ButtonControlTypeId;   break;
        }
        V_VT(pRetVal) = VT_I4;
        V_I4(pRetVal) = ct;
        return S_OK;
    }
    if (propertyId == WS_UIA_NamePropertyId) {
        char name_buf[256];
        const char *name = "Waterscape";
        switch (n->kind) {
        case WS_UIA_NODE_ROOT:          name = "Waterscape Web Browser 2.0"; break;
        case WS_UIA_NODE_TAB_ITEM:      name = arcr_shell_get_tab_title(sh, n->tab_index); break;
        case WS_UIA_NODE_NEW_TAB_BTN:   name = "New Tab";                    break;
        case WS_UIA_NODE_BACK_BTN:      name = "Back";                       break;
        case WS_UIA_NODE_FORWARD_BTN:   name = "Forward";                    break;
        case WS_UIA_NODE_RELOAD_BTN:    name = "Reload";                     break;
        case WS_UIA_NODE_OMNIBAR_EDIT:  name = "Address and Search Bar";     break;
        case WS_UIA_NODE_MENU_BTN:      name = "Main Menu";                  break;
        case WS_UIA_NODE_DEVTOOLS_DOCK: name = "Waterscape DevTools";        break;
        case WS_UIA_NODE_WEB_DOCUMENT: {
            ArcrWebView *av = arcr_shell_active_webview(sh);
            const char *t   = av ? arcr_webview_get_title(av) : NULL;
            if (t && t[0]) {
                snprintf(name_buf, sizeof(name_buf), "%s", t);
                name = name_buf;
            } else {
                name = "Web Content";
            }
            break;
        }
        }
        BSTR b = ws_uia_alloc_bstr_utf8(h, name);
        if (b) {
            V_VT(pRetVal)   = VT_BSTR;
            V_BSTR(pRetVal) = b;
        }
        return S_OK;
    }
    if (propertyId == WS_UIA_AutomationIdPropertyId) {
        char aid[64];
        switch (n->kind) {
        case WS_UIA_NODE_ROOT:          strcpy(aid, "WaterscapeWindow"); break;
        case WS_UIA_NODE_TAB_ITEM:      snprintf(aid, sizeof(aid), "TabItem_%lu", (unsigned long)n->tab_index); break;
        case WS_UIA_NODE_NEW_TAB_BTN:   strcpy(aid, "NewTabButton");     break;
        case WS_UIA_NODE_BACK_BTN:      strcpy(aid, "BackButton");       break;
        case WS_UIA_NODE_FORWARD_BTN:   strcpy(aid, "ForwardButton");    break;
        case WS_UIA_NODE_RELOAD_BTN:    strcpy(aid, "ReloadButton");     break;
        case WS_UIA_NODE_OMNIBAR_EDIT:  strcpy(aid, "OmnibarEdit");      break;
        case WS_UIA_NODE_MENU_BTN:      strcpy(aid, "MainMenuButton");   break;
        case WS_UIA_NODE_WEB_DOCUMENT:  strcpy(aid, "WebDocument");      break;
        case WS_UIA_NODE_DEVTOOLS_DOCK: strcpy(aid, "DevToolsDock");     break;
        }
        BSTR b = ws_uia_alloc_bstr_utf8(h, aid);
        if (b) {
            V_VT(pRetVal)   = VT_BSTR;
            V_BSTR(pRetVal) = b;
        }
        return S_OK;
    }
    if (propertyId == WS_UIA_IsEnabledPropertyId) {
        bool en = true;
        if (n->kind == WS_UIA_NODE_BACK_BTN)    en = arcr_shell_can_go_back(sh);
        if (n->kind == WS_UIA_NODE_FORWARD_BTN) en = arcr_shell_can_go_forward(sh);
        V_VT(pRetVal)   = VT_BOOL;
        V_BOOL(pRetVal) = en ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    if (propertyId == WS_UIA_IsControlElementPropertyId ||
        propertyId == WS_UIA_IsContentElementPropertyId ||
        propertyId == WS_UIA_IsKeyboardFocusablePropertyId) {
        V_VT(pRetVal)   = VT_BOOL;
        V_BOOL(pRetVal) = VARIANT_TRUE;
        return S_OK;
    }
    if (propertyId == WS_UIA_HasKeyboardFocusPropertyId) {
        bool focused = false;
        if (h->fm.window_active) {
            if (n->kind == WS_UIA_NODE_OMNIBAR_EDIT && h->ime_target_zone == 0) focused = true;
            else if (n->kind == WS_UIA_NODE_WEB_DOCUMENT && h->ime_target_zone == 2) focused = true;
        }
        V_VT(pRetVal)   = VT_BOOL;
        V_BOOL(pRetVal) = focused ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    if (propertyId == WS_UIA_IsOffscreenPropertyId) {
        ArcrRect rc;
        bool vis = ws_uia_compute_client_rect(h, n->kind, n->tab_index, &rc) &&
                   rc.width > 0 && rc.height > 0;
        V_VT(pRetVal)   = VT_BOOL;
        V_BOOL(pRetVal) = vis ? VARIANT_FALSE : VARIANT_TRUE;
        return S_OK;
    }
    if (propertyId == WS_UIA_NativeWindowHandlePropertyId && n->kind == WS_UIA_NODE_ROOT) {
        V_VT(pRetVal) = VT_I4;
        V_I4(pRetVal) = (LONG)(intptr_t)h->hwnd;
        return S_OK;
    }
    if (propertyId == WS_UIA_ValueValuePropertyId && ws_uia_has_value(n->kind)) {
        const char *val = arcr_shell_get_address_bar_text(sh);
        BSTR b = ws_uia_alloc_bstr_utf8(h, val);
        if (b) {
            V_VT(pRetVal)   = VT_BSTR;
            V_BSTR(pRetVal) = b;
        }
        return S_OK;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_simple_get_host(WsUiaSimpleObj *This, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->kind == WS_UIA_NODE_ROOT && n->host->api.UiaHostProviderFromHwnd) {
        return n->host->api.UiaHostProviderFromHwnd(n->host->hwnd, pRetVal);
    }
    return S_OK;
}

/* Fragment Vtbl */
static HRESULT STDMETHODCALLTYPE ws_uia_frag_qi(WsUiaFragmentObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_frag_addref(WsUiaFragmentObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_frag_release(WsUiaFragmentObj *This) {
    return ws_uia_release_node(This->owner);
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_navigate(WsUiaFragmentObj *This, int direction, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;

    WsUiaChildSpec specs[ARCR_SHELL_MAX_TABS + 12u];
    size_t count = ws_uia_build_children(h, specs, sizeof(specs) / sizeof(specs[0]));

    if (n->kind == WS_UIA_NODE_ROOT) {
        if (count == 0u) return S_OK;
        if (direction == WS_UIA_Nav_FirstChild) {
            WsUiaNode *ch = ws_uia_create_node(h, specs[0].kind, specs[0].tab_index);
            if (ch) *pRetVal = &ch->frag;
        } else if (direction == WS_UIA_Nav_LastChild) {
            WsUiaNode *ch = ws_uia_create_node(h, specs[count - 1u].kind, specs[count - 1u].tab_index);
            if (ch) *pRetVal = &ch->frag;
        }
        return S_OK;
    }

    if (direction == WS_UIA_Nav_Parent) {
        if (h->uia_root) {
            ws_uia_addref_node(h->uia_root);
            *pRetVal = &h->uia_root->frag;
        }
        return S_OK;
    }
    if (direction == WS_UIA_Nav_NextSibling || direction == WS_UIA_Nav_PreviousSibling) {
        for (size_t i = 0u; i < count; ++i) {
            if (specs[i].kind == n->kind &&
                (n->kind != WS_UIA_NODE_TAB_ITEM || specs[i].tab_index == n->tab_index)) {
                if (direction == WS_UIA_Nav_NextSibling && i + 1u < count) {
                    WsUiaNode *sib = ws_uia_create_node(h, specs[i + 1u].kind, specs[i + 1u].tab_index);
                    if (sib) *pRetVal = &sib->frag;
                } else if (direction == WS_UIA_Nav_PreviousSibling && i > 0u) {
                    WsUiaNode *sib = ws_uia_create_node(h, specs[i - 1u].kind, specs[i - 1u].tab_index);
                    if (sib) *pRetVal = &sib->frag;
                }
                break;
            }
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_get_runtime_id(WsUiaFragmentObj *This, SAFEARRAY **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->kind == WS_UIA_NODE_ROOT) {
        return S_OK; /* UIA automatically supplies HWND RuntimeId for root */
    }
    Win32PresenterHost *h = n->host;
    if (!h->api.SafeArrayCreateVector || !h->api.SafeArrayPutElement) return S_OK;

    SAFEARRAY *sa = h->api.SafeArrayCreateVector(VT_I4, 0, 2u);
    if (!sa) return E_OUTOFMEMORY;
    LONG idx0 = 0;
    LONG idx1 = 1;
    LONG v0   = WS_UIA_APPEND_RUNTIME_ID;
    LONG v1   = (LONG)n->kind * 100L + (LONG)n->tab_index + 1L;
    h->api.SafeArrayPutElement(sa, &idx0, &v0);
    h->api.SafeArrayPutElement(sa, &idx1, &v1);
    *pRetVal = sa;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_get_bounds(WsUiaFragmentObj *This, WsUiaRect *pRetVal) {
    if (!pRetVal) return E_POINTER;
    ZeroMemory(pRetVal, sizeof(*pRetVal));
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;

    ArcrRect rc;
    if (!ws_uia_compute_client_rect(n->host, n->kind, n->tab_index, &rc) ||
        rc.width <= 0 || rc.height <= 0) {
        return S_OK;
    }
    POINT tl = {rc.x, rc.y};
    ClientToScreen(n->host->hwnd, &tl);
    pRetVal->left   = (double)tl.x;
    pRetVal->top    = (double)tl.y;
    pRetVal->width  = (double)rc.width;
    pRetVal->height = (double)rc.height;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_get_embedded(WsUiaFragmentObj *This, SAFEARRAY **pRetVal) {
    (void)This;
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_set_focus(WsUiaFragmentObj *This) {
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;
    SetFocus(h->hwnd);
    if (n->kind == WS_UIA_NODE_OMNIBAR_EDIT) {
        h->ime_target_zone = 0;
        arcr_shell_on_key_ex(h->shell, NULL, ARCR_KEY_NONE, false, false, false, 6 /* F6 */);
        InvalidateRect(h->hwnd, NULL, FALSE);
    } else if (n->kind == WS_UIA_NODE_TAB_ITEM) {
        arcr_shell_activate_tab(h->shell, n->tab_index);
        InvalidateRect(h->hwnd, NULL, FALSE);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_frag_get_root(WsUiaFragmentObj *This, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->host->uia_root) {
        ws_uia_addref_node(n->host->uia_root);
        *pRetVal = &n->host->uia_root->frag_root;
    }
    return S_OK;
}

/* FragmentRoot Vtbl */
static HRESULT STDMETHODCALLTYPE ws_uia_root_qi(WsUiaFragRootObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_root_addref(WsUiaFragRootObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_root_release(WsUiaFragRootObj *This) {
    return ws_uia_release_node(This->owner);
}

static HRESULT STDMETHODCALLTYPE ws_uia_root_from_point(WsUiaFragRootObj *This,
                                                        double x, double y, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;

    POINT pt = {(LONG)x, (LONG)y};
    ScreenToClient(h->hwnd, &pt);

    WsUiaChildSpec specs[ARCR_SHELL_MAX_TABS + 12u];
    size_t count = ws_uia_build_children(h, specs, sizeof(specs) / sizeof(specs[0]));
    for (size_t i = 0u; i < count; ++i) {
        ArcrRect rc;
        if (ws_uia_compute_client_rect(h, specs[i].kind, specs[i].tab_index, &rc) &&
            pt.x >= rc.x && pt.x < rc.x + rc.width &&
            pt.y >= rc.y && pt.y < rc.y + rc.height) {
            WsUiaNode *hit = ws_uia_create_node(h, specs[i].kind, specs[i].tab_index);
            if (hit) *pRetVal = &hit->frag;
            return S_OK;
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ws_uia_root_get_focus(WsUiaFragRootObj *This, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;

    WsUiaNodeKind fk = (h->ime_target_zone == 0 && !arcr_shell_is_fullscreen(h->shell))
                           ? WS_UIA_NODE_OMNIBAR_EDIT
                           : WS_UIA_NODE_WEB_DOCUMENT;
    WsUiaNode *fn = ws_uia_create_node(h, fk, 0u);
    if (fn) *pRetVal = &fn->frag;
    return S_OK;
}

/* IInvokeProvider Vtbl */
static HRESULT STDMETHODCALLTYPE ws_uia_inv_qi(WsUiaInvokeObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_inv_addref(WsUiaInvokeObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_inv_release(WsUiaInvokeObj *This) {
    return ws_uia_release_node(This->owner);
}
static HRESULT STDMETHODCALLTYPE ws_uia_inv_invoke(WsUiaInvokeObj *This) {
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    Win32PresenterHost *h = n->host;
    ArcrBrowserShell *sh  = h->shell;

    switch (n->kind) {
    case WS_UIA_NODE_TAB_ITEM:
        arcr_shell_activate_tab(sh, n->tab_index);
        break;
    case WS_UIA_NODE_NEW_TAB_BTN:
        arcr_shell_open_tab_url(sh, "waterscape://newtab", true);
        break;
    case WS_UIA_NODE_BACK_BTN:
        arcr_shell_go_back(sh);
        break;
    case WS_UIA_NODE_FORWARD_BTN:
        arcr_shell_go_forward(sh);
        break;
    case WS_UIA_NODE_RELOAD_BTN:
        arcr_shell_reload(sh);
        break;
    case WS_UIA_NODE_MENU_BTN: {
        ArcrRect mr;
        if (ws_uia_compute_client_rect(h, WS_UIA_NODE_MENU_BTN, 0u, &mr)) {
            arcr_shell_on_mouse_down(sh, mr.x + mr.width / 2, mr.y + mr.height / 2, 0);
            arcr_shell_on_mouse_up(sh, mr.x + mr.width / 2, mr.y + mr.height / 2, 0);
        }
        break;
    }
    default:
        return WS_UIA_E_INVALIDOPERATION;
    }
    InvalidateRect(h->hwnd, NULL, FALSE);
    return S_OK;
}

/* IValueProvider Vtbl */
static HRESULT STDMETHODCALLTYPE ws_uia_val_qi(WsUiaValueObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_val_addref(WsUiaValueObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_val_release(WsUiaValueObj *This) {
    return ws_uia_release_node(This->owner);
}
static HRESULT STDMETHODCALLTYPE ws_uia_val_set(WsUiaValueObj *This, LPCWSTR val) {
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->kind != WS_UIA_NODE_OMNIBAR_EDIT) return WS_UIA_E_INVALIDOPERATION;
    if (!val) return E_INVALIDARG;

    char utf8[512];
    int nb = WideCharToMultiByte(CP_UTF8, 0, val, -1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (nb <= 0) utf8[0] = '\0';
    else utf8[nb] = '\0';

    if (utf8[0] != '\0') {
        arcr_shell_navigate_async(n->host->shell, utf8);
        InvalidateRect(n->host->hwnd, NULL, FALSE);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ws_uia_val_get(WsUiaValueObj *This, BSTR *pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    const char *txt = arcr_shell_get_address_bar_text(n->host->shell);
    *pRetVal = ws_uia_alloc_bstr_utf8(n->host, txt);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ws_uia_val_readonly(WsUiaValueObj *This, BOOL *pRetVal) {
    if (!pRetVal) return E_POINTER;
    WsUiaNode *n = This->owner;
    *pRetVal = (n && n->kind == WS_UIA_NODE_OMNIBAR_EDIT) ? FALSE : TRUE;
    return S_OK;
}

/* ISelectionItemProvider Vtbl */
static HRESULT STDMETHODCALLTYPE ws_uia_sel_qi(WsUiaSelItemObj *This, const WsGuid *riid, void **ppv) {
    return ws_uia_query_node(This->owner, riid, ppv);
}
static ULONG STDMETHODCALLTYPE ws_uia_sel_addref(WsUiaSelItemObj *This) {
    return ws_uia_addref_node(This->owner);
}
static ULONG STDMETHODCALLTYPE ws_uia_sel_release(WsUiaSelItemObj *This) {
    return ws_uia_release_node(This->owner);
}
static HRESULT STDMETHODCALLTYPE ws_uia_sel_select(WsUiaSelItemObj *This) {
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->kind != WS_UIA_NODE_TAB_ITEM) return WS_UIA_E_INVALIDOPERATION;
    arcr_shell_activate_tab(n->host->shell, n->tab_index);
    InvalidateRect(n->host->hwnd, NULL, FALSE);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ws_uia_sel_remove(WsUiaSelItemObj *This) {
    (void)This;
    return WS_UIA_E_INVALIDOPERATION;
}
static HRESULT STDMETHODCALLTYPE ws_uia_sel_is_selected(WsUiaSelItemObj *This, BOOL *pRetVal) {
    if (!pRetVal) return E_POINTER;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    *pRetVal = (arcr_shell_active_tab_index(n->host->shell) == n->tab_index) ? TRUE : FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ws_uia_sel_container(WsUiaSelItemObj *This, void **pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = NULL;
    WsUiaNode *n = This->owner;
    if (!ws_uia_host_alive(n)) return WS_UIA_E_ELEMENTNOTAVAILABLE;
    if (n->host->uia_root) {
        ws_uia_addref_node(n->host->uia_root);
        *pRetVal = &n->host->uia_root->simple;
    }
    return S_OK;
}

static const WsUiaSimpleVtbl kWsUiaSimpleVtbl = {
    ws_uia_simple_qi,
    ws_uia_simple_addref,
    ws_uia_simple_release,
    ws_uia_simple_get_options,
    ws_uia_simple_get_pattern,
    ws_uia_simple_get_prop,
    ws_uia_simple_get_host
};

static const WsUiaFragmentVtbl kWsUiaFragmentVtbl = {
    ws_uia_frag_qi,
    ws_uia_frag_addref,
    ws_uia_frag_release,
    ws_uia_frag_navigate,
    ws_uia_frag_get_runtime_id,
    ws_uia_frag_get_bounds,
    ws_uia_frag_get_embedded,
    ws_uia_frag_set_focus,
    ws_uia_frag_get_root
};

static const WsUiaFragRootVtbl kWsUiaFragRootVtbl = {
    ws_uia_root_qi,
    ws_uia_root_addref,
    ws_uia_root_release,
    ws_uia_root_from_point,
    ws_uia_root_get_focus
};

static const WsUiaInvokeVtbl kWsUiaInvokeVtbl = {
    ws_uia_inv_qi,
    ws_uia_inv_addref,
    ws_uia_inv_release,
    ws_uia_inv_invoke
};

static const WsUiaValueVtbl kWsUiaValueVtbl = {
    ws_uia_val_qi,
    ws_uia_val_addref,
    ws_uia_val_release,
    ws_uia_val_set,
    ws_uia_val_get,
    ws_uia_val_readonly
};

static const WsUiaSelItemVtbl kWsUiaSelItemVtbl = {
    ws_uia_sel_qi,
    ws_uia_sel_addref,
    ws_uia_sel_release,
    ws_uia_sel_select,
    ws_uia_sel_select,
    ws_uia_sel_remove,
    ws_uia_sel_is_selected,
    ws_uia_sel_container
};

static WsUiaNode *ws_uia_create_node(Win32PresenterHost *host, WsUiaNodeKind kind, size_t tab_index) {
    WsUiaNode *n = (WsUiaNode *)calloc(1u, sizeof(WsUiaNode));
    if (!n) return NULL;
    n->simple.lpVtbl    = &kWsUiaSimpleVtbl;
    n->simple.owner     = n;
    n->frag.lpVtbl      = &kWsUiaFragmentVtbl;
    n->frag.owner       = n;
    n->frag_root.lpVtbl = &kWsUiaFragRootVtbl;
    n->frag_root.owner  = n;
    n->invoke.lpVtbl    = &kWsUiaInvokeVtbl;
    n->invoke.owner     = n;
    n->value.lpVtbl     = &kWsUiaValueVtbl;
    n->value.owner      = n;
    n->sel_item.lpVtbl  = &kWsUiaSelItemVtbl;
    n->sel_item.owner   = n;
    n->ref_count        = 1L;
    n->host             = host;
    n->kind             = kind;
    n->tab_index        = tab_index;
    return n;
}

/* -------------------------------------------------------------------------
 * 6. DPI, High Contrast, DWM Extended Titlebar & Imm32 IME Helpers
 * ------------------------------------------------------------------------- */
static int32_t win32_query_dpi(Win32PresenterHost *h, HWND hwnd) {
    const char *env_dpi = getenv("WATERSCAPE_FORCE_DPI");
    if (env_dpi && env_dpi[0] != '\0') {
        int v = atoi(env_dpi);
        if (v >= 72 && v <= 480) return (int32_t)v;
    }
    if (h) {
        if (h->dpi_from_msg >= 72 && h->dpi_from_msg <= 480) {
            return h->dpi_from_msg;
        }
        if (hwnd && h->api.GetDpiForWindow) {
            UINT d = h->api.GetDpiForWindow(hwnd);
            if (d >= 72u && d <= 480u) return (int32_t)d;
        }
        if (hwnd && h->api.GetDpiForMonitor) {
            HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            UINT dx = 96u, dy = 96u;
            if (hm && h->api.GetDpiForMonitor(hm, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy) >= 0 &&
                dx >= 72u && dx <= 480u) {
                return (int32_t)dx;
            }
        }
        if (h->api.GetDpiForSystem) {
            UINT d = h->api.GetDpiForSystem();
            if (d >= 72u && d <= 480u) return (int32_t)d;
        }
    }
    HDC hdc = GetDC(hwnd);
    int gd  = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(hwnd, hdc);
    return (gd >= 72 && gd <= 480) ? (int32_t)gd : 96;
}

static int32_t win32_metric_for_dpi(Win32PresenterHost *h, int index, int32_t dpi) {
    if (h && h->api.GetSystemMetricsForDpi && dpi >= 72) {
        return (int32_t)h->api.GetSystemMetricsForDpi(index, (UINT)dpi);
    }
    return (int32_t)GetSystemMetrics(index);
}

static void win32_refresh_high_contrast(Win32PresenterHost *h) {
    if (!h) return;
    WsHighContrastW hc;
    ZeroMemory(&hc, sizeof(hc));
    hc.cbSize = sizeof(hc);
    bool hc_on = false;
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        hc_on = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0u;
    }
    h->fm.high_contrast = hc_on;
    if (hc_on) {
        h->fm.hc_bg_argb     = ws_colorref_to_argb(GetSysColor(COLOR_WINDOW));
        h->fm.hc_fg_argb     = ws_colorref_to_argb(GetSysColor(COLOR_WINDOWTEXT));
        h->fm.hc_accent_argb = ws_colorref_to_argb(GetSysColor(COLOR_HIGHLIGHT));
        h->fm.hc_border_argb = ws_colorref_to_argb(GetSysColor(COLOR_BTNTEXT));
    }
}

static void win32_refresh_dwm_and_metrics(Win32PresenterHost *h, HWND hwnd) {
    if (!h || !h->shell || !hwnd) return;

    int32_t dpi = win32_query_dpi(h, hwnd);
    h->fm.dpi          = dpi;
    h->fm.is_maximized = IsZoomed(hwnd) ? true : false;
    win32_refresh_high_contrast(h);

    BOOL comp_enabled = FALSE;
    bool can_dwm      = !h->force_classic_frame &&
                        h->api.DwmIsCompositionEnabled &&
                        h->api.DwmExtendFrameIntoClientArea &&
                        (h->api.DwmIsCompositionEnabled(&comp_enabled) >= 0) &&
                        (comp_enabled != FALSE);

    h->dwm_extended = can_dwm;

    if (can_dwm && !h->os_fullscreen) {
        int32_t btn_cluster_w = ws_dip_to_px(138, dpi);
        int32_t cap_h         = ARCR_MAX(ws_dip_to_px(32, dpi),
                                         win32_metric_for_dpi(h, SM_CYCAPTION, dpi));

        if (h->api.DwmGetWindowAttribute && !IsIconic(hwnd)) {
            RECT cb;
            ZeroMemory(&cb, sizeof(cb));
            HRESULT hr = h->api.DwmGetWindowAttribute(
                hwnd, WS_DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof(cb));
            int32_t bw = (int32_t)(cb.right - cb.left);
            int32_t bh = (int32_t)(cb.bottom - cb.top);
            if (hr >= 0 && bw >= ws_dip_to_px(64, dpi) && bw <= ws_dip_to_px(320, dpi) &&
                bh >= ws_dip_to_px(16, dpi) && bh <= ws_dip_to_px(96, dpi)) {
                btn_cluster_w = bw;
                if (bh > cap_h) cap_h = bh;
            }
        }

        h->fm.mode                   = ARCR_FRAME_DWM_EXTENDED;
        h->fm.os_caption_height_px   = cap_h;
        h->fm.caption_inset_left_px  = 0;
        h->fm.caption_inset_right_px = btn_cluster_w;
        arcr_shell_set_frame_metrics(h->shell, &h->fm);

        ArcrShellLayoutMetrics m;
        arcr_shell_compute_layout(h->shell, &m);
        WsDwmMargins margins = {0, 0, m.tab_strip_rect.height, 0};
        h->api.DwmExtendFrameIntoClientArea(hwnd, &margins);
    } else {
        h->fm.mode                   = ARCR_FRAME_OS_SINGLE;
        h->fm.os_caption_height_px   = 0;
        h->fm.caption_inset_left_px  = 0;
        h->fm.caption_inset_right_px = 0;
        arcr_shell_set_frame_metrics(h->shell, &h->fm);

        if (h->api.DwmExtendFrameIntoClientArea) {
            WsDwmMargins zero_m = {0, 0, 0, 0};
            h->api.DwmExtendFrameIntoClientArea(hwnd, &zero_m);
        }
    }
}

static void win32_position_ime_window(Win32PresenterHost *h, HWND hwnd) {
    if (!h || !h->shell || !hwnd) return;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;

    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(h->shell, &m);

    ArcrRect anchor = m.address_bar_rect;
    POINT    pt     = {anchor.x + ws_dip_to_px(12, h->fm.dpi),
                       anchor.y + ws_dip_to_px(4, h->fm.dpi)};

    if (h->ime_target_zone == 1 && arcr_shell_is_find_bar_visible(h->shell) &&
        m.find_bar_rect.height > 0) {
        anchor = m.find_bar_rect;
        pt.x   = anchor.x + ws_dip_to_px(64, h->fm.dpi);
        pt.y   = anchor.y + ws_dip_to_px(4, h->fm.dpi);
    } else if (h->ime_target_zone == 2 && m.content_viewport.height > 0) {
        anchor = (ArcrRect){
            ARCR_CLAMP(h->last_vp_click_x, m.content_viewport.x + 8,
                       m.content_viewport.x + ARCR_MAX(16, m.content_viewport.width - 32)),
            ARCR_CLAMP(h->last_vp_click_y, m.content_viewport.y + 8,
                       m.content_viewport.y + ARCR_MAX(16, m.content_viewport.height - 32)),
            ws_dip_to_px(120, h->fm.dpi),
            ws_dip_to_px(26, h->fm.dpi)
        };
        pt.x = anchor.x;
        pt.y = anchor.y;
    }

    COMPOSITIONFORM cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.dwStyle      = CFS_POINT;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(himc, &cf);

    CANDIDATEFORM cand;
    ZeroMemory(&cand, sizeof(cand));
    cand.dwIndex      = 0u;
    cand.dwStyle      = CFS_EXCLUDE;
    cand.ptCurrentPos = (POINT){pt.x, anchor.y + anchor.height};
    cand.rcArea       = (RECT){anchor.x, anchor.y,
                               anchor.x + anchor.width, anchor.y + anchor.height};
    ImmSetCandidateWindow(himc, &cand);

    ImmReleaseContext(hwnd, himc);
}

/* -------------------------------------------------------------------------
 * 7. GPU / GDI Framebuffer Presentation Chain
 * ------------------------------------------------------------------------- */
static int32_t next_pow2_i32(int32_t v) {
    int32_t p = 64;
    while (p < v && p < 16384) p <<= 1;
    return p;
}

static void fill_d3d9_present_params(WsD3DPresentParams *pp, HWND hwnd, int32_t w, int32_t ht) {
    ZeroMemory(pp, sizeof(*pp));
    pp->BackBufferWidth      = (UINT)ARCR_MAX(64, w);
    pp->BackBufferHeight     = (UINT)ARCR_MAX(64, ht);
    pp->BackBufferFormat     = 22u; /* D3DFMT_X8R8G8B8 */
    pp->BackBufferCount      = 1u;
    pp->SwapEffect           = 1u;  /* D3DSWAPEFFECT_DISCARD */
    pp->hDeviceWindow        = hwnd;
    pp->Windowed             = TRUE;
    pp->Flags                = 0x00000001u; /* D3DPRESENTFLAG_LOCKABLE_BACKBUFFER */
    pp->PresentationInterval = 0x00000001u; /* D3DPRESENT_INTERVAL_ONE */
}

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
        if (h->gl_hdc) {
            h->fn_wglMakeCurrent(h->gl_hdc, h->gl_rc);
            if (h->gl_tex && h->fn_glDeleteTextures) {
                h->fn_glDeleteTextures(1, &h->gl_tex);
            }
        }
        h->fn_wglMakeCurrent(NULL, NULL);
        h->fn_wglDeleteContext(h->gl_rc);
        h->gl_rc    = NULL;
        h->gl_tex   = 0u;
        h->gl_tex_w = 0;
        h->gl_tex_h = 0;
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

    const UINT feature_levels[3] = {0xB000u, 0xA100u, 0xA000u};
    UINT chosen_fl = 0u;

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
    fill_d3d9_present_params(&pp, hwnd, w, ht);

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

static void gl_ensure_texture_capacity(Win32PresenterHost *h, int32_t w, int32_t ht) {
    int32_t req_w = next_pow2_i32(w);
    int32_t req_h = next_pow2_i32(ht);
    if (h->gl_tex != 0u && req_w == h->gl_tex_w && req_h == h->gl_tex_h) return;

    if (h->gl_tex == 0u) {
        h->fn_glGenTextures(1, &h->gl_tex);
    }
    h->fn_glBindTexture(0x0DE1u /* GL_TEXTURE_2D */, h->gl_tex);
    h->fn_glTexParameteri(0x0DE1u, 0x2801u /* GL_TEXTURE_MIN_FILTER */, 0x2600 /* GL_NEAREST */);
    h->fn_glTexParameteri(0x0DE1u, 0x2800u /* GL_TEXTURE_MAG_FILTER */, 0x2600 /* GL_NEAREST */);
    h->fn_glTexParameteri(0x0DE1u, 0x2802u /* GL_TEXTURE_WRAP_S */, 0x2900 /* GL_CLAMP */);
    h->fn_glTexParameteri(0x0DE1u, 0x2803u /* GL_TEXTURE_WRAP_T */, 0x2900 /* GL_CLAMP */);
    h->fn_glTexImage2D(0x0DE1u, 0, 0x8058 /* GL_RGBA8 */, req_w, req_h, 0,
                       0x80E1u /* GL_BGRA_EXT */, 0x1401u /* GL_UNSIGNED_BYTE */, NULL);
    h->gl_tex_w = req_w;
    h->gl_tex_h = req_h;
}

static bool try_init_opengl_icd(Win32PresenterHost *h, HWND hwnd, int32_t w, int32_t ht) {
    h->gl_dll = LoadLibraryA("opengl32.dll");
    if (!h->gl_dll) return false;

    PFN_wglCreateContext fn_create_ctx = NULL;
    PFN_glGetString      fn_get_str    = NULL;
    WS_LOAD_PROC(h->gl_dll, fn_create_ctx,           "wglCreateContext");
    WS_LOAD_PROC(h->gl_dll, h->fn_wglMakeCurrent,    "wglMakeCurrent");
    WS_LOAD_PROC(h->gl_dll, h->fn_wglDeleteContext,  "wglDeleteContext");
    WS_LOAD_PROC(h->gl_dll, fn_get_str,              "glGetString");
    WS_LOAD_PROC(h->gl_dll, h->fn_glViewport,        "glViewport");
    WS_LOAD_PROC(h->gl_dll, h->fn_glGenTextures,     "glGenTextures");
    WS_LOAD_PROC(h->gl_dll, h->fn_glDeleteTextures,  "glDeleteTextures");
    WS_LOAD_PROC(h->gl_dll, h->fn_glBindTexture,     "glBindTexture");
    WS_LOAD_PROC(h->gl_dll, h->fn_glTexParameteri,   "glTexParameteri");
    WS_LOAD_PROC(h->gl_dll, h->fn_glTexImage2D,      "glTexImage2D");
    WS_LOAD_PROC(h->gl_dll, h->fn_glTexSubImage2D,   "glTexSubImage2D");
    WS_LOAD_PROC(h->gl_dll, h->fn_glEnable,          "glEnable");
    WS_LOAD_PROC(h->gl_dll, h->fn_glDisable,         "glDisable");
    WS_LOAD_PROC(h->gl_dll, h->fn_glMatrixMode,      "glMatrixMode");
    WS_LOAD_PROC(h->gl_dll, h->fn_glLoadIdentity,    "glLoadIdentity");
    WS_LOAD_PROC(h->gl_dll, h->fn_glPixelStorei,     "glPixelStorei");
    WS_LOAD_PROC(h->gl_dll, h->fn_glColor4f,         "glColor4f");
    WS_LOAD_PROC(h->gl_dll, h->fn_glBegin,           "glBegin");
    WS_LOAD_PROC(h->gl_dll, h->fn_glTexCoord2f,      "glTexCoord2f");
    WS_LOAD_PROC(h->gl_dll, h->fn_glVertex2f,        "glVertex2f");
    WS_LOAD_PROC(h->gl_dll, h->fn_glEnd,             "glEnd");
    WS_LOAD_PROC(h->gl_dll, h->fn_glFlush,           "glFlush");

    if (!fn_create_ctx || !h->fn_wglMakeCurrent || !h->fn_wglDeleteContext ||
        !fn_get_str || !h->fn_glViewport || !h->fn_glGenTextures ||
        !h->fn_glBindTexture || !h->fn_glTexParameteri || !h->fn_glTexImage2D ||
        !h->fn_glTexSubImage2D || !h->fn_glEnable || !h->fn_glBegin ||
        !h->fn_glTexCoord2f || !h->fn_glVertex2f || !h->fn_glEnd) {
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
        teardown_gpu_backends(h, hwnd);
        return false;
    }

    gl_ensure_texture_capacity(h, w, ht);
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
            fprintf(stderr, "waterscape: D3D11 ResizeBuffers failed (0x%08lX); falling back to GDI.\n",
                    (unsigned long)hr);
            teardown_gpu_backends(h, hwnd);
        }
    } else if (h->backend == WS_PRESENT_WIN32_D3D9 && h->d3d9_dev) {
        WsD3DPresentParams pp;
        fill_d3d9_present_params(&pp, hwnd, w, ht);
        HRESULT hr = h->d3d9_dev->lpVtbl->Reset(h->d3d9_dev, &pp);
        if (hr < 0 && hr != WS_D3DERR_DEVICELOST) {
            fprintf(stderr, "waterscape: D3D9 Reset failed (0x%08lX); falling back to GDI.\n",
                    (unsigned long)hr);
            teardown_gpu_backends(h, hwnd);
        }
    } else if (h->backend == WS_PRESENT_WIN32_OPENGL && h->gl_rc && h->gl_hdc) {
        h->fn_wglMakeCurrent(h->gl_hdc, h->gl_rc);
        gl_ensure_texture_capacity(h, w, ht);
    }
}

static void gdi_present_fallback(Win32PresenterHost *h, HWND hwnd, HDC paint_hdc) {
    HDC dst = paint_hdc ? paint_hdc : GetDC(hwnd);
    if (dst) {
        BitBlt(dst, 0, 0, h->fb_w, h->fb_h, h->mem_dc, 0, 0, SRCCOPY);
        if (!paint_hdc) ReleaseDC(hwnd, dst);
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
        fprintf(stderr, "waterscape: D3D11 device lost during Present; falling back to GDI.\n");
        teardown_gpu_backends(h, hwnd);
        arcr_shell_set_status_overlay(h->shell, "D3D11 장치 손실 감지 -> GDI 무중단 복귀 완료");
    }

    /* 2. Direct3D 9 Hardware SwapChain Presentation */
    if (h->backend == WS_PRESENT_WIN32_D3D9 && h->d3d9_dev) {
        HRESULT coop = h->d3d9_dev->lpVtbl->TestCooperativeLevel(h->d3d9_dev);
        if (coop == WS_D3DERR_DEVICELOST) {
            gdi_present_fallback(h, hwnd, paint_hdc);
            return;
        }
        if (coop == WS_D3DERR_DEVICENOTRESET) {
            WsD3DPresentParams pp;
            fill_d3d9_present_params(&pp, hwnd, h->fb_w, h->fb_h);
            coop = h->d3d9_dev->lpVtbl->Reset(h->d3d9_dev, &pp);
        }
        if (coop >= 0) {
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
                    HRESULT phr = h->d3d9_dev->lpVtbl->Present(h->d3d9_dev, NULL, NULL, NULL, NULL);
                    if (phr >= 0) return;
                    if (phr == WS_D3DERR_DEVICELOST || phr == WS_D3DERR_DEVICENOTRESET) {
                        gdi_present_fallback(h, hwnd, paint_hdc);
                        return;
                    }
                } else {
                    com_safe_release(surf);
                }
            }
        }
        fprintf(stderr, "waterscape: D3D9 presentation error; falling back to GDI.\n");
        teardown_gpu_backends(h, hwnd);
        arcr_shell_set_status_overlay(h->shell, "D3D9 장치 오류 감지 -> GDI 무중단 복귀 완료");
    }

    /* 3. Verified OpenGL Hardware ICD Textured Quad */
    if (h->backend == WS_PRESENT_WIN32_OPENGL && h->gl_rc && h->gl_hdc) {
        if (h->fn_wglMakeCurrent(h->gl_hdc, h->gl_rc)) {
            gl_ensure_texture_capacity(h, h->fb_w, h->fb_h);
            h->fn_glViewport(0, 0, h->fb_w, h->fb_h);
            if (h->fn_glMatrixMode && h->fn_glLoadIdentity) {
                h->fn_glMatrixMode(0x1701u /* GL_PROJECTION */);
                h->fn_glLoadIdentity();
                h->fn_glMatrixMode(0x1700u /* GL_MODELVIEW */);
                h->fn_glLoadIdentity();
            }
            if (h->fn_glDisable) {
                h->fn_glDisable(0x0B71u /* GL_DEPTH_TEST */);
                h->fn_glDisable(0x0BE2u /* GL_BLEND */);
            }
            if (h->fn_glPixelStorei) {
                h->fn_glPixelStorei(0x0CF5u /* GL_UNPACK_ALIGNMENT */, 4);
            }
            h->fn_glEnable(0x0DE1u /* GL_TEXTURE_2D */);
            h->fn_glBindTexture(0x0DE1u, h->gl_tex);
            h->fn_glTexSubImage2D(0x0DE1u, 0, 0, 0, h->fb_w, h->fb_h,
                                  0x80E1u /* GL_BGRA_EXT */,
                                  0x1401u /* GL_UNSIGNED_BYTE */,
                                  h->pixels);

            float u_max = (float)h->fb_w / (float)ARCR_MAX(1, h->gl_tex_w);
            float v_max = (float)h->fb_h / (float)ARCR_MAX(1, h->gl_tex_h);
            if (h->fn_glColor4f) h->fn_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            h->fn_glBegin(0x0007u /* GL_QUADS */);
            h->fn_glTexCoord2f(0.0f,  0.0f);  h->fn_glVertex2f(-1.0f,  1.0f);
            h->fn_glTexCoord2f(u_max, 0.0f);  h->fn_glVertex2f( 1.0f,  1.0f);
            h->fn_glTexCoord2f(u_max, v_max); h->fn_glVertex2f( 1.0f, -1.0f);
            h->fn_glTexCoord2f(0.0f,  v_max); h->fn_glVertex2f(-1.0f, -1.0f);
            h->fn_glEnd();

            if (h->fn_glFlush) h->fn_glFlush();
            SwapBuffers(h->gl_hdc);
            return;
        }
        teardown_gpu_backends(h, hwnd);
    }

    /* 4. Universal Win2000+ GDI DIBSection BitBlt */
    gdi_present_fallback(h, hwnd, paint_hdc);
}

/* -------------------------------------------------------------------------
 * 8. Fullscreen Sync & Single-Frame Window Procedure (`WndProc`)
 * ------------------------------------------------------------------------- */
static void win32_sync_os_fullscreen(Win32PresenterHost *h, HWND hwnd) {
    if (!h || !h->shell || !hwnd) return;
    bool want_fs = arcr_shell_is_fullscreen(h->shell);
    if (want_fs == h->os_fullscreen) return;

    if (want_fs) {
        h->saved_style            = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        h->saved_ex_style         = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        h->saved_placement.length = sizeof(h->saved_placement);
        GetWindowPlacement(hwnd, &h->saved_placement);

        RECT mon_r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (hmon) {
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi)) {
                mon_r = mi.rcMonitor;
            }
        }

        h->os_fullscreen = true;
        win32_refresh_dwm_and_metrics(h, hwnd);

        SetWindowLongW(hwnd, GWL_STYLE,
                       (LONG)((h->saved_style & ~(DWORD)WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOP,
                     mon_r.left, mon_r.top,
                     mon_r.right - mon_r.left,
                     mon_r.bottom - mon_r.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        h->os_fullscreen = false;
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)h->saved_style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)h->saved_ex_style);
        if (h->saved_placement.length == sizeof(h->saved_placement)) {
            SetWindowPlacement(hwnd, &h->saved_placement);
        }
        win32_refresh_dwm_and_metrics(h, hwnd);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static void apply_win32_cursor(Win32PresenterHost *h, ArcrCursorKind kind) {
    h->active_cursor = kind;
    HCURSOR c = h->cur_arrow;
    if (h->splitter_cursor_dir == 1 && h->cur_size_we)      c = h->cur_size_we;
    else if (h->splitter_cursor_dir == 2 && h->cur_size_ns) c = h->cur_size_ns;
    else if (kind == ARCR_CURSOR_POINTER && h->cur_pointer) c = h->cur_pointer;
    else if (kind == ARCR_CURSOR_TEXT && h->cur_text)       c = h->cur_text;
    if (c) SetCursor(c);
}

static LRESULT CALLBACK waterscape_win32_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32PresenterHost *h = (Win32PresenterHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    /* Forward DWM caption button messages (including WM_NCMOUSELEAVE) to DwmDefWindowProc first! */
    if (h && h->dwm_extended && !h->os_fullscreen && h->api.DwmDefWindowProc) {
        LRESULT dwm_res = 0;
        if (h->api.DwmDefWindowProc(hwnd, msg, wParam, lParam, &dwm_res)) {
            return dwm_res;
        }
    }

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        Win32PresenterHost *init_h = (Win32PresenterHost *)cs->lpCreateParams;
        if (init_h) init_h->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)init_h);
        return 0;
    }

    case WM_NCCALCSIZE:
        if (wParam == TRUE && h && h->dwm_extended && !h->os_fullscreen) {
            NCCALCSIZE_PARAMS *ncp = (NCCALCSIZE_PARAMS *)lParam;
            if (IsZoomed(hwnd)) {
                /* Clamp maximized client rect strictly to target monitor's work area (multi-monitor safe!) */
                HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                if (hmon) {
                    MONITORINFO mi;
                    ZeroMemory(&mi, sizeof(mi));
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(hmon, &mi)) {
                        ncp->rgrc[0] = mi.rcWork;
                        return 0;
                    }
                }
            }
            /* Preserve standard left, right, and bottom resize borders; remove only standard top titlebar */
            LONG orig_top = ncp->rgrc[0].top;
            LRESULT res   = DefWindowProcW(hwnd, msg, wParam, lParam);
            ncp->rgrc[0].top = orig_top;
            return res;
        }
        break;

    case WM_NCHITTEST:
        if (h && h->dwm_extended && !h->os_fullscreen && h->shell) {
            LRESULT def_ht = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (def_ht != HTCLIENT && def_ht != HTCAPTION && def_ht != HTSYSMENU) {
                return def_ht; /* Native left, right, bottom, and bottom-corner resize borders */
            }

            /* Signed screen coords — essential for negative coordinates on secondary monitors! */
            POINT pt;
            pt.x = WS_GET_X_LPARAM(lParam);
            pt.y = WS_GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd, &pt);

            RECT cr;
            GetClientRect(hwnd, &cr);
            int32_t cw = (int32_t)(cr.right - cr.left);

            if (!IsZoomed(hwnd)) {
                int32_t top_bdr = ARCR_MAX(4, win32_metric_for_dpi(h, SM_CYSIZEFRAME, h->fm.dpi) +
                                              win32_metric_for_dpi(h, SM_CXPADDEDBORDER, h->fm.dpi));
                top_bdr = ARCR_MIN(top_bdr, ws_dip_to_px(6, h->fm.dpi));
                int32_t corner_w = ws_dip_to_px(12, h->fm.dpi);
                if (pt.y >= 0 && pt.y < top_bdr) {
                    if (pt.x < corner_w)       return HTTOPLEFT;
                    if (pt.x >= cw - corner_w) return HTTOPRIGHT;
                    return HTTOP;
                }
            }

            ArcrShellChromeZone zone = arcr_shell_hit_test_chrome_zone(h->shell, pt.x, pt.y);
            if (zone == ARCR_CHROME_CAPTION_DRAG || zone == ARCR_CHROME_OS_CAPTION_INSET) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        break;

    case WM_GETOBJECT:
        if (h && h->shell && (LONG)lParam == WS_UIA_ROOT_OBJECT_ID &&
            h->api.UiaReturnRawElementProvider && h->api.UiaHostProviderFromHwnd) {
            if (!h->uia_root) {
                h->uia_root = ws_uia_create_node(h, WS_UIA_NODE_ROOT, 0u);
            }
            if (h->uia_root) {
                h->uia_used = true;
                return h->api.UiaReturnRawElementProvider(hwnd, wParam, lParam, &h->uia_root->simple);
            }
        }
        break;

    case (WM_APP + 0x2E0):
    case WM_DPICHANGED:
        if (h && h->shell) {
            int32_t new_dpi = (int32_t)HIWORD(wParam);
            if (new_dpi < 72 || new_dpi > 480) {
                new_dpi = (int32_t)LOWORD(wParam);
            }
            if (new_dpi >= 72 && new_dpi <= 480) {
                h->dpi_from_msg = new_dpi;
                h->fm.dpi       = new_dpi;
            }
            const RECT *sug = (msg == WM_DPICHANGED) ? (const RECT *)lParam : NULL;
            if (sug && !IsBadReadPtr(sug, sizeof(RECT)) &&
                sug->right > sug->left && sug->bottom > sug->top) {
                SetWindowPos(hwnd, NULL,
                             sug->left, sug->top,
                             sug->right - sug->left,
                             sug->bottom - sug->top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            win32_refresh_dwm_and_metrics(h, hwnd);
            RECT cr;
            GetClientRect(hwnd, &cr);
            int32_t cw = ARCR_MAX(64, (int32_t)(cr.right - cr.left));
            int32_t ch = ARCR_MAX(64, (int32_t)(cr.bottom - cr.top));
            arcr_shell_resize(h->shell, cw, ch);
            win32_resize_surfaces(h, hwnd, cw, ch);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_ACTIVATE:
        if (h && h->shell) {
            bool active = (LOWORD(wParam) != WA_INACTIVE);
            if (h->fm.window_active != active) {
                h->fm.window_active = active;
                arcr_shell_set_frame_metrics(h->shell, &h->fm);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        break;

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOMPOSITIONCHANGED:
        if (h && h->shell) {
            bool prev_dwm = h->dwm_extended;
            win32_refresh_dwm_and_metrics(h, hwnd);
            if (prev_dwm != h->dwm_extended) {
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == WS_TIMER_TICK_60FPS) {
            if (h && h->shell) {
                if (arcr_shell_tick(h->shell, arcr_platform_monotonic_usec())) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        if (wParam == WS_TIMER_AUTOCLOSE) {
            KillTimer(hwnd, WS_TIMER_AUTOCLOSE);
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_SIZE:
        if (h && wParam != SIZE_MINIMIZED) {
            int32_t w  = (int32_t)LOWORD(lParam);
            int32_t ht = (int32_t)HIWORD(lParam);
            win32_refresh_dwm_and_metrics(h, hwnd);
            if (w > 0 && ht > 0) {
                if (h->shell) arcr_shell_resize(h->shell, w, ht);
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
            int32_t x = WS_GET_X_LPARAM(lParam);
            int32_t y = WS_GET_Y_LPARAM(lParam);
            ArcrShellLayoutMetrics m;
            arcr_shell_compute_layout(h->shell, &m);
            h->splitter_cursor_dir = 0;
            if (m.devtools_splitter_rect.width > 0 &&
                x >= m.devtools_splitter_rect.x &&
                x < m.devtools_splitter_rect.x + m.devtools_splitter_rect.width &&
                y >= m.devtools_splitter_rect.y &&
                y < m.devtools_splitter_rect.y + m.devtools_splitter_rect.height) {
                h->splitter_cursor_dir =
                    (arcr_shell_get_devtools_dock(h->shell) == ARCR_DEVTOOLS_DOCK_RIGHT) ? 1 : 2;
            }
            ArcrCursorKind ck = arcr_shell_on_mouse_move(h->shell, x, y);
            apply_win32_cursor(h, ck);
            if (waterscape_shell_needs_repaint(h->shell)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (h && h->shell) {
            int  btn   = (msg == WM_MBUTTONDOWN) ? 1 : ((msg == WM_RBUTTONDOWN) ? 2 : 0);
            bool ctrl  = GetKeyState(VK_CONTROL) < 0;
            bool shift = GetKeyState(VK_SHIFT) < 0;
            int32_t x  = WS_GET_X_LPARAM(lParam);
            int32_t y  = WS_GET_Y_LPARAM(lParam);

            ArcrShellLayoutMetrics m;
            arcr_shell_compute_layout(h->shell, &m);
            if (x >= m.address_bar_rect.x && x < m.address_bar_rect.x + m.address_bar_rect.width &&
                y >= m.address_bar_rect.y && y < m.address_bar_rect.y + m.address_bar_rect.height) {
                h->ime_target_zone = 0;
            } else if (arcr_shell_is_find_bar_visible(h->shell) &&
                       x >= m.find_bar_rect.x && x < m.find_bar_rect.x + m.find_bar_rect.width &&
                       y >= m.find_bar_rect.y && y < m.find_bar_rect.y + m.find_bar_rect.height) {
                h->ime_target_zone = 1;
            } else if (x >= m.content_viewport.x && x < m.content_viewport.x + m.content_viewport.width &&
                       y >= m.content_viewport.y && y < m.content_viewport.y + m.content_viewport.height) {
                h->ime_target_zone = 2;
                h->last_vp_click_x = x;
                h->last_vp_click_y = y;
            }

            if (msg == WM_LBUTTONDOWN) {
                SetCapture(hwnd);
            }
            arcr_shell_on_mouse_down_ex(h->shell, x, y, btn, ctrl, shift);
            win32_sync_os_fullscreen(h, hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (h && h->shell) {
            int btn = (msg == WM_MBUTTONUP) ? 1 : ((msg == WM_RBUTTONUP) ? 2 : 0);
            if (msg == WM_LBUTTONUP && GetCapture() == hwnd) {
                ReleaseCapture();
            }
            int32_t x = WS_GET_X_LPARAM(lParam);
            int32_t y = WS_GET_Y_LPARAM(lParam);
            if (arcr_shell_on_mouse_up(h->shell, x, y, btn) ||
                waterscape_shell_needs_repaint(h->shell)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (h && h->shell) {
            if (arcr_shell_on_mouse_up(h->shell, 0, 0, 0)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (h && h->shell) {
            int16_t delta = (int16_t)HIWORD(wParam);
            bool    ctrl  = GetKeyState(VK_CONTROL) < 0;
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
            bool ctrl  = GetKeyState(VK_CONTROL) < 0;
            bool alt   = GetKeyState(VK_MENU) < 0;
            bool shift = GetKeyState(VK_SHIFT) < 0;

            if (wParam >= VK_F1 && wParam <= VK_F12) {
                int fn_num = (int)(wParam - VK_F1) + 1;
                if (fn_num == 6) h->ime_target_zone = 0;
                if (arcr_shell_on_key_ex(h->shell, NULL, ARCR_KEY_NONE, ctrl, alt, shift, fn_num)) {
                    win32_sync_os_fullscreen(h, hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            if (alt && !ctrl && wParam == 'D') {
                h->ime_target_zone = 0;
                if (arcr_shell_on_key_ex(h->shell, "d", ARCR_KEY_NONE, false, true, shift, 0)) {
                    win32_sync_os_fullscreen(h, hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            if (ctrl && !alt && !shift) {
                ArcrSpecialKey clip_sk = ARCR_KEY_NONE;
                if (wParam == 'A')      clip_sk = ARCR_KEY_SELECT_ALL;
                else if (wParam == 'C') clip_sk = ARCR_KEY_COPY;
                else if (wParam == 'X') clip_sk = ARCR_KEY_CUT;
                else if (wParam == 'V') clip_sk = ARCR_KEY_PASTE;
                if (clip_sk != ARCR_KEY_NONE) {
                    if (arcr_shell_on_key_ex(h->shell, NULL, clip_sk, true, false, false, 0)) {
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    }
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
            else if (wParam == VK_UP)     sk = ARCR_KEY_UP;
            else if (wParam == VK_DOWN)   sk = ARCR_KEY_DOWN;
            else if (wParam == VK_PRIOR)  sk = ARCR_KEY_PAGE_UP;
            else if (wParam == VK_NEXT)   sk = ARCR_KEY_PAGE_DOWN;

            if (sk != ARCR_KEY_NONE) {
                if (arcr_shell_on_key_ex(h->shell, NULL, sk, ctrl, alt, shift, 0)) {
                    win32_sync_os_fullscreen(h, hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            if (ctrl && ((wParam >= 'A' && wParam <= 'Z') ||
                         (wParam >= '0' && wParam <= '9') ||
                         wParam == 0xBB /* VK_OEM_PLUS */ ||
                         wParam == 0xBD /* VK_OEM_MINUS */)) {
                if (wParam == 'L') h->ime_target_zone = 0;
                if (wParam == 'F') h->ime_target_zone = 1;
                char key_ch[2];
                key_ch[0] = (wParam == 0xBB) ? '=' : ((wParam == 0xBD) ? '-' : (char)wParam);
                key_ch[1] = '\0';
                if (arcr_shell_on_key_ex(h->shell, key_ch, ARCR_KEY_NONE, true, alt, shift, 0)) {
                    win32_sync_os_fullscreen(h, hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        break;

    case WM_CHAR:
        if (h && h->shell) {
            bool ctrl  = GetKeyState(VK_CONTROL) < 0;
            bool alt   = GetKeyState(VK_MENU) < 0;
            bool shift = GetKeyState(VK_SHIFT) < 0;
            WCHAR wc   = (WCHAR)wParam;
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
                if (arcr_shell_on_key_ex(h->shell, utf8, ARCR_KEY_NONE, false, alt, shift, 0)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;

    case WM_IME_STARTCOMPOSITION:
        if (h && h->shell) {
            win32_position_ime_window(h, hwnd);
        }
        break;

    case WM_IME_COMPOSITION:
        if (h && h->shell) {
            win32_position_ime_window(h, hwnd);
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                bool committed = (lParam & GCS_RESULTSTR) != 0;
                DWORD flag     = committed ? GCS_RESULTSTR : GCS_COMPSTR;
                LONG bytes     = ImmGetCompositionStringW(himc, flag, NULL, 0);
                if (bytes >= 0 && bytes < 512) {
                    WCHAR wbuf[256];
                    ZeroMemory(wbuf, sizeof(wbuf));
                    if (bytes > 0) {
                        ImmGetCompositionStringW(himc, flag, wbuf, (DWORD)bytes);
                    }
                    char utf8[768];
                    int n = (bytes > 0)
                                ? WideCharToMultiByte(CP_UTF8, 0, wbuf, bytes / (LONG)sizeof(WCHAR),
                                                      utf8, (int)sizeof(utf8) - 1, NULL, NULL)
                                : 0;
                    if (n >= 0) {
                        utf8[n] = '\0';
                        arcr_shell_on_ime_composition(h->shell, utf8, n, committed);
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                ImmReleaseContext(hwnd, himc);
                if (committed) return 0;
            }
        }
        break;

    case WM_IME_ENDCOMPOSITION:
        if (h && h->shell) {
            arcr_shell_on_ime_composition(h->shell, "", 0, false);
            InvalidateRect(hwnd, NULL, FALSE);
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
        KillTimer(hwnd, WS_TIMER_TICK_60FPS);
        if (h) {
            if (h->uia_used && h->api.UiaReturnRawElementProvider) {
                h->api.UiaReturnRawElementProvider(hwnd, 0, 0, NULL);
            }
            if (h->uia_root) {
                h->uia_root->host = NULL;
                ws_uia_release_node(h->uia_root);
                h->uia_root = NULL;
            }
            teardown_gpu_backends(h, hwnd);
            h->hwnd = NULL;
        }
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

    HRESULT co_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    Win32PresenterHost host;
    ZeroMemory(&host, sizeof(host));
    ws_win32_load_dyn_apis(&host.api);

    /* Stamp explicit AppUserModelID BEFORE creating any window for clean taskbar grouping */
    if (host.api.SetCurrentProcessExplicitAppUserModelID) {
        (void)host.api.SetCurrentProcessExplicitAppUserModelID(WATERSCAPE_APP_USER_MODEL_ID);
    }

    host.engine              = engine;
    host.shell               = shell;
    host.backend             = WS_PRESENT_WIN32_GDI;
    host.force_cpu           = opts ? opts->force_cpu_presenter : false;
    host.force_classic_frame = opts ? opts->force_classic_frame : false;
    host.ime_target_zone     = 2; /* Default webview focus unless F6/Ctrl+L/omnibar click */
    arcr_shell_default_frame_metrics(&host.fm);
    host.fm.window_active    = true;
    host.fm.dpi              = win32_query_dpi(&host, NULL);
    win32_refresh_high_contrast(&host);

    host.cur_arrow   = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    host.cur_text    = LoadCursorW(NULL, MAKEINTRESOURCEW(32513)); /* IDC_IBEAM */
    host.cur_pointer = LoadCursorW(NULL, IDC_HAND);
    host.cur_size_we = LoadCursorW(NULL, IDC_SIZEWE);
    host.cur_size_ns = LoadCursorW(NULL, IDC_SIZENS);
    if (!host.cur_pointer) host.cur_pointer = host.cur_arrow;

    int32_t init_dip_w = (opts && opts->initial_width > 0)  ? opts->initial_width  : 1080;
    int32_t init_dip_h = (opts && opts->initial_height > 0) ? opts->initial_height : 720;
    int32_t init_px_w  = ws_dip_to_px(init_dip_w, host.fm.dpi);
    int32_t init_px_h  = ws_dip_to_px(init_dip_h, host.fm.dpi);

    RECT win_rc = {0, 0, init_px_w, init_px_h};
    if (host.api.AdjustWindowRectExForDpi) {
        host.api.AdjustWindowRectExForDpi(&win_rc, WS_OVERLAPPEDWINDOW, FALSE, 0u, (UINT)host.fm.dpi);
    } else {
        AdjustWindowRectEx(&win_rc, WS_OVERLAPPEDWINDOW, FALSE, 0u);
    }
    int32_t outer_w = ARCR_MAX(240, (int32_t)(win_rc.right - win_rc.left));
    int32_t outer_h = ARCR_MAX(180, (int32_t)(win_rc.bottom - win_rc.top));

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = waterscape_win32_wndproc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                         win32_metric_for_dpi(&host, SM_CXSMICON, host.fm.dpi),
                                         win32_metric_for_dpi(&host, SM_CYSMICON, host.fm.dpi), 0);
    wc.hCursor       = host.cur_arrow;
    wc.lpszClassName = L"WaterscapeBrowserWnd20";
    RegisterClassExW(&wc);

    WCHAR wtitle[256];
    const char *utf8_title = (opts && opts->window_title && opts->window_title[0])
                                 ? opts->window_title
                                 : "Waterscape Web Browser 2.0";
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8_title, -1, wtitle, 255);
    if (wn <= 0) {
        lstrcpyW(wtitle, L"Waterscape Web Browser 2.0");
    }

    g_ws_active_host = &host;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, outer_w, outer_h,
        NULL, NULL, hInst, &host);
    if (!hwnd) {
        g_ws_active_host = NULL;
        ws_win32_free_dyn_apis(&host.api);
        if (SUCCEEDED(co_hr)) CoUninitialize();
        return 1;
    }

    host.hwnd = hwnd;
    win32_refresh_dwm_and_metrics(&host, hwnd);
    if (host.dwm_extended) {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    RECT cr;
    GetClientRect(hwnd, &cr);
    int32_t cw = ARCR_MAX(64, (int32_t)(cr.right - cr.left));
    int32_t ch = ARCR_MAX(64, (int32_t)(cr.bottom - cr.top));
    arcr_shell_resize(shell, cw, ch);
    win32_resize_surfaces(&host, hwnd, cw, ch);

    if (!host.force_cpu) {
        if (!try_init_d3d11(&host, hwnd, cw, ch)) {
            if (!try_init_d3d9(&host, hwnd, cw, ch)) {
                (void)try_init_opengl_icd(&host, hwnd, cw, ch);
            }
        }
    }

    printf("  Win32 Presenter: %s (%d DPI, Frame: %s)\n",
           waterscape_present_backend_name(host.backend),
           (int)host.fm.dpi,
           host.dwm_extended ? "DWM Extended Single-Frame" : "OS Standard Single-Frame");

    SetTimer(hwnd, WS_TIMER_TICK_60FPS, 16u, NULL);
    if (opts && opts->auto_close_ms > 0u) {
        SetTimer(hwnd, WS_TIMER_AUTOCLOSE, (UINT)opts->auto_close_ms, NULL);
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    /* Re-query DWM caption button bounds once window is visible so inset width is exact */
    win32_refresh_dwm_and_metrics(&host, hwnd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_ws_active_host = NULL;
    if (host.dib) {
        SelectObject(host.mem_dc, host.old_bmp);
        DeleteObject(host.dib);
    }
    if (host.mem_dc) DeleteDC(host.mem_dc);
    ws_win32_free_dyn_apis(&host.api);
    if (SUCCEEDED(co_hr)) CoUninitialize();
    return 0;
}

#else
/* Non-Win32 ISO C11 compliant translation unit placeholder */
typedef int waterscape_win32_non_windows_tu_t;
#endif /* _WIN32 */
