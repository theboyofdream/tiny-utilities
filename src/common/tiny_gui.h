#ifndef TINY_GUI_H
#define TINY_GUI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

typedef BOOL (WINAPI *pfnTinyAlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);

typedef struct {
    RECT bounds;
    bool hasBounds;
} TinyMonitorBoundsCtx;

static inline BOOL CALLBACK TinyGUI_EnumMonitorsProc(HMONITOR hMon, HDC hdcMon, LPRECT lprcMon, LPARAM dwData) {
    TinyMonitorBoundsCtx *ctx = (TinyMonitorBoundsCtx *)dwData;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (!ctx->hasBounds) {
            ctx->bounds = mi.rcMonitor;
            ctx->hasBounds = true;
        } else {
            if (mi.rcMonitor.left < ctx->bounds.left) ctx->bounds.left = mi.rcMonitor.left;
            if (mi.rcMonitor.top < ctx->bounds.top) ctx->bounds.top = mi.rcMonitor.top;
            if (mi.rcMonitor.right > ctx->bounds.right) ctx->bounds.right = mi.rcMonitor.right;
            if (mi.rcMonitor.bottom > ctx->bounds.bottom) ctx->bounds.bottom = mi.rcMonitor.bottom;
        }
    }
    return TRUE;
}

/*
 * TinyGUI_GetVirtualScreenBounds:
 * Computes exact multi-monitor desktop union bounding box.
 */
static inline void TinyGUI_GetVirtualScreenBounds(int *outX, int *outY, int *outW, int *outH) {
    TinyMonitorBoundsCtx ctx = { {0, 0, 0, 0}, false };
    EnumDisplayMonitors(NULL, NULL, TinyGUI_EnumMonitorsProc, (LPARAM)&ctx);

    if (ctx.hasBounds && (ctx.bounds.right - ctx.bounds.left) > 0 && (ctx.bounds.bottom - ctx.bounds.top) > 0) {
        if (outX) *outX = ctx.bounds.left;
        if (outY) *outY = ctx.bounds.top;
        if (outW) *outW = ctx.bounds.right - ctx.bounds.left;
        if (outH) *outH = ctx.bounds.bottom - ctx.bounds.top;
    } else {
        if (outX) *outX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        if (outY) *outY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (outW) *outW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        if (outH) *outH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
}

/*
 * TinyGUI_ApplyDimOverlay:
 * Copies desktop background and blends a dark translucent alpha layer over it.
 */
static inline void TinyGUI_ApplyDimOverlay(HDC hdcDest, HDC hdcDesktop, int vw, int vh, BYTE alpha) {
    BitBlt(hdcDest, 0, 0, vw, vh, hdcDesktop, 0, 0, SRCCOPY);

    HDC hdcDim = CreateCompatibleDC(hdcDest);
    HBITMAP hbmDim = CreateCompatibleBitmap(hdcDest, vw, vh);
    HBITMAP hOldDim = (HBITMAP)SelectObject(hdcDim, hbmDim);
    HBRUSH hDimBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT fullR = {0, 0, vw, vh};
    FillRect(hdcDim, &fullR, hDimBrush);
    DeleteObject(hDimBrush);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    HMODULE hMsimg = LoadLibraryW(L"msimg32.dll");
    if (hMsimg) {
        pfnTinyAlphaBlend pAlphaBlend = (pfnTinyAlphaBlend)(void*)GetProcAddress(hMsimg, "AlphaBlend");
        if (pAlphaBlend) {
            pAlphaBlend(hdcDest, 0, 0, vw, vh, hdcDim, 0, 0, vw, vh, bf);
        }
        FreeLibrary(hMsimg);
    }

    SelectObject(hdcDim, hOldDim);
    DeleteObject(hbmDim);
    DeleteDC(hdcDim);
}

/*
 * TinyGUI_HitTestResizeBorders:
 * Performs 8-direction border resize hit-testing for frameless windows on WM_NCHITTEST.
 */
static inline LRESULT TinyGUI_HitTestResizeBorders(HWND hwnd, LPARAM lParam, int borderThickness) {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    RECT rc;
    GetWindowRect(hwnd, &rc);

    int x = pt.x - rc.left;
    int y = pt.y - rc.top;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    bool left   = (x >= 0 && x < borderThickness);
    bool right  = (x >= w - borderThickness && x < w);
    bool top    = (y >= 0 && y < borderThickness);
    bool bottom = (y >= h - borderThickness && y < h);

    if (top && left)     return HTTOPLEFT;
    if (top && right)    return HTTOPRIGHT;
    if (bottom && left)  return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left)            return HTLEFT;
    if (right)           return HTRIGHT;
    if (top)             return HTTOP;
    if (bottom)          return HTBOTTOM;

    return HTCLIENT;
}

#include "font.h"

/*
 * TinyGUI_CreateScaledFont:
 * Creates a clean Cleartype GDI font with specified font size and weight.
 */
static inline HFONT TinyGUI_CreateScaledFont(int fontSize, int weight, const wchar_t *faceName) {
    return TinyFont_Create(faceName, fontSize, weight, false, 0);
}

#endif /* TINY_GUI_H */
