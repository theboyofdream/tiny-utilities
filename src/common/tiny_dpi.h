#ifndef TINY_DPI_H
#define TINY_DPI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * TinyDPI_EnablePerMonitorAwareness:
 * Enables Per-Monitor v2 DPI awareness with clean fallbacks for older Windows builds.
 */
static inline void TinyDPI_EnablePerMonitorAwareness(void) {
    typedef BOOL (WINAPI *pfnSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        pfnSetProcessDpiAwarenessContext setCtx =
            (pfnSetProcessDpiAwarenessContext)(void*)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    SetProcessDPIAware();
}

/*
 * TinyDPI_GetDpiForMonitor:
 * Queries the active DPI for a given HMONITOR (using shcore!GetDpiForMonitor with user32 fallback).
 */
static inline UINT TinyDPI_GetDpiForMonitor(HMONITOR hMon) {
    if (!hMon) return 96;

    typedef HRESULT (WINAPI *pfnGetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        pfnGetDpiForMonitor getDpi = (pfnGetDpiForMonitor)(void*)GetProcAddress(hShcore, "GetDpiForMonitor");
        if (getDpi) {
            UINT dpiX = 96, dpiY = 96;
            if (SUCCEEDED(getDpi(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiY > 0) {
                FreeLibrary(hShcore);
                return dpiY;
            }
        }
        FreeLibrary(hShcore);
    }

    HDC hdc = GetDC(NULL);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    return dpi > 0 ? dpi : 96;
}

/*
 * TinyDPI_GetDpiForPoint:
 * Queries the DPI of the display monitor containing screen coordinates pt.
 */
static inline UINT TinyDPI_GetDpiForPoint(POINT pt) {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    return TinyDPI_GetDpiForMonitor(hMon);
}

/*
 * TinyDPI_GetDpiForWindow:
 * Queries the DPI for a specific window handle (using user32!GetDpiForWindow or MonitorFromWindow fallback).
 */
static inline UINT TinyDPI_GetDpiForWindow(HWND hWnd) {
    if (!hWnd) return 96;

    typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        pfnGetDpiForWindow getDpi = (pfnGetDpiForWindow)(void*)GetProcAddress(hUser32, "GetDpiForWindow");
        if (getDpi) {
            UINT dpi = getDpi(hWnd);
            if (dpi > 0) return dpi;
        }
    }

    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    return TinyDPI_GetDpiForMonitor(hMon);
}

#endif /* TINY_DPI_H */
