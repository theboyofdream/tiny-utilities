/*
 * Pin2Top — toggle topmost state + border overlay for active or interactively selected windows.
 * Source: pin-to-top.c
 *
 * Spec: PRD.md (sections 3, 4, 5, 6, 8). Agent guide: AGENTS.md / CHECKLIST.md.
 *
 * Behavior:
 *   Interactive (double click / default): shows window selection overlay (like capture --window).
 *   CLI (--no-picker / --active / --foreground): directly pins foreground window.
 *   Second launch: mutex already exists -> signal TOGGLE event -> exit immediately.
 *   Toggle: if foreground window is pinned -> unpin + destroy overlay (exits if 0 pinned).
 *           if foreground window not pinned -> launches interactive window picker.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdbool.h>
#include <wchar.h>
#include "common/color-thief-algorithm.h"
#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"
#include "common/tiny_dpi.h"
#include "common/tiny_gui.h"
#include "common/font.h"

#define APPMUTEX_NAME L"Global\\TinyPin2TopMutex"
#define APPEVENT_NAME L"Global\\TinyPin2TopEvent"

#define TRACK_INTERVAL_MS 16 /* ~60 FPS */

#define KEY_COLOR RGB(1, 1, 2) /* transparent-to-mouse color key */

/* Tunable defaults */
#define DEFAULT_BORDER_WIDTH 2            /* border weight in px, 1..20 */
#define DEFAULT_BORDER_COLOR RGB(0,120,215) /* Windows accent blue (fallback) */
#define DEFAULT_MAX_WINDOWS 1             /* max pinned windows, 1..10 */
#define BORDER_MIN    1
#define BORDER_MAX    20
#define MAXWINDOWS_MIN 1
#define MAXWINDOWS_MAX 10

#define MAX_PIN_SLOTS 10

typedef struct {
    int borderWidth;
    int maxWindows;
    bool windowSelection;
} Config;

typedef struct {
    HWND target;
    HWND overlay;
    RECT lastRect;
} Pinned;

static Pinned g_pinned[MAX_PIN_SLOTS];
static int g_pinnedCount = 0;
static Config g_cfg = {DEFAULT_BORDER_WIDTH, DEFAULT_MAX_WINDOWS, true};
static COLORREF g_borderColor = DEFAULT_BORDER_COLOR;
static HANDLE g_event = NULL;
static bool g_running = true;

static int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;
static HBITMAP g_hbmDesktop = NULL;
static HWND g_pickerHoveredHwnd = NULL;
static RECT g_pickerHoveredRect = {0, 0, 0, 0};
static bool g_pickerActive = false;

/* ------------------------------------------------------------------ */
/* CLI parsing                                                         */
/* ------------------------------------------------------------------ */

static Config BuildConfig(void)
{
    Config cfg = {DEFAULT_BORDER_WIDTH, DEFAULT_MAX_WINDOWS, true};

    bool noPicker = false;
    bool noWinSelFlag = false;
    bool activeFlag = false;
    bool foregroundFlag = false;
    const wchar_t *winSelStr = NULL;

    const CliOption opts[] = {
        { L"--border-width",        CLI_OPT_INT,    &cfg.borderWidth, BORDER_MIN, BORDER_MAX },
        { L"-b",                    CLI_OPT_INT,    &cfg.borderWidth, BORDER_MIN, BORDER_MAX },
        { L"--max-windows",         CLI_OPT_INT,    &cfg.maxWindows,  MAXWINDOWS_MIN, MAXWINDOWS_MAX },
        { L"-m",                    CLI_OPT_INT,    &cfg.maxWindows,  MAXWINDOWS_MIN, MAXWINDOWS_MAX },
        { L"--window-selection",    CLI_OPT_STRING, &winSelStr,       0, 0 },
        { L"--no-picker",           CLI_OPT_BOOL,   &noPicker,        0, 0 },
        { L"--no-window-selection", CLI_OPT_BOOL,   &noWinSelFlag,    0, 0 },
        { L"--active",              CLI_OPT_BOOL,   &activeFlag,      0, 0 },
        { L"--foreground",          CLI_OPT_BOOL,   &foregroundFlag,  0, 0 },
    };

    TinyCLI_ParseCommandLine(opts, sizeof(opts) / sizeof(opts[0]));

    if (winSelStr != NULL) {
        if (_wcsicmp(winSelStr, L"false") == 0 || _wcsicmp(winSelStr, L"0") == 0) {
            cfg.windowSelection = false;
        } else if (_wcsicmp(winSelStr, L"true") == 0 || _wcsicmp(winSelStr, L"1") == 0) {
            cfg.windowSelection = true;
        }
    }

    if (noPicker || noWinSelFlag || activeFlag || foregroundFlag) {
        cfg.windowSelection = false;
    }

    /* Support legacy --key=val format if passed */
    const wchar_t *rawCmd = GetCommandLineW();
    if (rawCmd) {
        wchar_t buf[2048];
        wcsncpy_s(buf, sizeof(buf) / sizeof(buf[0]), rawCmd, _TRUNCATE);
        wchar_t *argv[TINY_CLI_MAX_ARGS];
        int argc = TinyCLI_Tokenize(buf, argv, TINY_CLI_MAX_ARGS);
        for (int i = 1; i < argc; i++) {
            if (_wcsnicmp(argv[i], L"--border-width=", 15) == 0) {
                int val = _wtoi(argv[i] + 15);
                if (val < BORDER_MIN) val = BORDER_MIN;
                if (val > BORDER_MAX) val = BORDER_MAX;
                cfg.borderWidth = val;
            } else if (_wcsnicmp(argv[i], L"--max-windows=", 14) == 0) {
                int val = _wtoi(argv[i] + 14);
                if (val < MAXWINDOWS_MIN) val = MAXWINDOWS_MIN;
                if (val > MAXWINDOWS_MAX) val = MAXWINDOWS_MAX;
                cfg.maxWindows = val;
            } else if (_wcsnicmp(argv[i], L"--window-selection=", 19) == 0) {
                const wchar_t *val = argv[i] + 19;
                if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) {
                    cfg.windowSelection = false;
                } else {
                    cfg.windowSelection = true;
                }
            }
        }
    }

    return cfg;
}

/* ------------------------------------------------------------------ */
/* Window frame, cloaking and target validation helpers               */
/* ------------------------------------------------------------------ */

typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND, DWORD, PVOID, DWORD);

static BOOL GetVisibleFrame(HWND hwnd, RECT *out)
{
    static DwmGetWindowAttributeFn fn = (DwmGetWindowAttributeFn)-1;
    if (fn == (DwmGetWindowAttributeFn)-1) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        fn = (dwm != NULL)
                 ? (DwmGetWindowAttributeFn)GetProcAddress(dwm, "DwmGetWindowAttribute")
                 : NULL;
    }
    if (fn == NULL)
        return FALSE;
    return SUCCEEDED(fn(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out, sizeof(RECT)));
}

static BOOL IsWindowCloaked(HWND hwnd)
{
    static DwmGetWindowAttributeFn fn = (DwmGetWindowAttributeFn)-1;
    if (fn == (DwmGetWindowAttributeFn)-1) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        fn = (dwm != NULL)
                 ? (DwmGetWindowAttributeFn)GetProcAddress(dwm, "DwmGetWindowAttribute")
                 : NULL;
    }
    if (fn == NULL)
        return FALSE;
    DWORD cloaked = 0;
    if (SUCCEEDED(fn(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;
    return FALSE;
}

static void GetFrameRect(HWND hwnd, RECT *out)
{
    if (!GetVisibleFrame(hwnd, out) || (out->right <= out->left) || (out->bottom <= out->top))
        GetWindowRect(hwnd, out);
}

static bool IsValidTargetWindow(HWND hwnd)
{
    if (hwnd == NULL || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow())
        return false;
    if (IsWindowCloaked(hwnd))
        return false;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW))
        return false;

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 63);
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        _wcsicmp(cls, L"TinyPinPickerOverlayClass") == 0 || _wcsicmp(cls, L"TinyPin2TopOverlay") == 0)
        return false;

    RECT r;
    GetFrameRect(hwnd, &r);
    if ((r.right - r.left) <= 10 || (r.bottom - r.top) <= 10)
        return false;

    return true;
}

/* ------------------------------------------------------------------ */
/* Overlay window                                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(KEY_COLOR);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        if (g_cfg.borderWidth > 0 && rc.right > 0 && rc.bottom > 0) {
            HBRUSH br = CreateSolidBrush(g_borderColor);
            RECT r;
            int w = g_cfg.borderWidth;

            r = rc; r.bottom = rc.top + w; FillRect(dc, &r, br);
            r = rc; r.top = rc.bottom - w; FillRect(dc, &r, br);
            r = rc; r.right = rc.left + w; r.top += w; r.bottom -= w; FillRect(dc, &r, br);
            r = rc; r.left = rc.right - w; r.top += w; r.bottom -= w; FillRect(dc, &r, br);

            DeleteObject(br);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void GetOverlayRect(HWND target, RECT *out)
{
    GetFrameRect(target, out);
    int w = g_cfg.borderWidth;
    out->left -= w;
    out->top -= w;
    out->right += w;
    out->bottom += w;
}

static HWND CreateOverlayFor(HWND target)
{
    static const wchar_t *cls = L"TinyPin2TopOverlay";
    static bool registered = false;

    if (!registered) {
        WNDCLASSW wc = {0};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT r;
    GetOverlayRect(target, &r);

    HWND ov = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls, L"", WS_POPUP,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (ov == NULL) return NULL;

    SetLayeredWindowAttributes(ov, KEY_COLOR, 0, LWA_COLORKEY);
    ShowWindow(ov, SW_SHOWNOACTIVATE);
    SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return ov;
}

/* ------------------------------------------------------------------ */
/* Pin / unpin helpers                                                */
/* ------------------------------------------------------------------ */

static void ApplyPin(HWND hwnd, bool pin)
{
    SetWindowPos(hwnd,
                 pin ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool IsPinned(HWND target)
{
    for (int i = 0; i < g_pinnedCount; i++) {
        if (g_pinned[i].target == target)
            return true;
    }
    return false;
}

static void RemovePinnedAt(int idx, bool unpin)
{
    if (idx < 0 || idx >= g_pinnedCount) return;
    Pinned *p = &g_pinned[idx];
    if (unpin && IsWindow(p->target)) ApplyPin(p->target, false);
    if (p->overlay != NULL && IsWindow(p->overlay)) DestroyWindow(p->overlay);
    for (int i = idx; i + 1 < g_pinnedCount; i++) g_pinned[i] = g_pinned[i + 1];
    g_pinnedCount--;
    g_pinned[g_pinnedCount].target = NULL;
    g_pinned[g_pinnedCount].overlay = NULL;
}

static void UnpinWindow(HWND target)
{
    for (int i = 0; i < g_pinnedCount; i++) {
        if (g_pinned[i].target == target) {
            RemovePinnedAt(i, true);
            return;
        }
    }
}

static void PinWindow(HWND target)
{
    if (target == NULL || !IsWindow(target) || IsPinned(target))
        return;

    RECT frame;
    GetFrameRect(target, &frame);
    if ((frame.right - frame.left) <= 10 || (frame.bottom - frame.top) <= 10)
        return;

    while (g_pinnedCount >= g_cfg.maxWindows && g_pinnedCount > 0) {
        RemovePinnedAt(0, true);
    }
    if (g_pinnedCount >= MAX_PIN_SLOTS)
        return;

    HWND ov = CreateOverlayFor(target);
    if (ov == NULL) return;

    Pinned *p = &g_pinned[g_pinnedCount];
    p->target = target;
    p->overlay = ov;
    p->lastRect = frame;
    ApplyPin(target, true);
    SetWindowPos(ov, HWND_TOPMOST,
                 frame.left - g_cfg.borderWidth,
                 frame.top - g_cfg.borderWidth,
                 frame.right - frame.left + g_cfg.borderWidth * 2,
                 frame.bottom - frame.top + g_cfg.borderWidth * 2,
                 SWP_NOACTIVATE);
    g_pinnedCount++;
}

/* ------------------------------------------------------------------ */
/* Tracking loop                                                      */
/* ------------------------------------------------------------------ */

static void TrackPinnedWindows(void)
{
    HWND fg = GetForegroundWindow();

    for (int i = g_pinnedCount - 1; i >= 0; i--) {
        Pinned *p = &g_pinned[i];
        if (!IsWindow(p->target)) {
            RemovePinnedAt(i, false);
            continue;
        }
        if (IsIconic(p->target)) {
            if (IsWindowVisible(p->overlay)) ShowWindow(p->overlay, SW_HIDE);
            continue;
        }
        if (!IsWindowVisible(p->overlay)) ShowWindow(p->overlay, SW_SHOWNOACTIVATE);

        RECT r;
        GetFrameRect(p->target, &r);
        if (!EqualRect(&r, &p->lastRect)) {
            p->lastRect = r;
            SetWindowPos(p->overlay, HWND_TOPMOST,
                         r.left - g_cfg.borderWidth,
                         r.top - g_cfg.borderWidth,
                         r.right - r.left + g_cfg.borderWidth * 2,
                         r.bottom - r.top + g_cfg.borderWidth * 2,
                         SWP_NOACTIVATE);
            InvalidateRect(p->overlay, NULL, TRUE);
        } else if (p->target == fg) {
            SetWindowPos(p->overlay, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if (g_pinnedCount == 0) {
        g_running = false;
    }
}

/* ------------------------------------------------------------------ */
/* Interactive Window Selection Picker (like capture --window)        */
/* ------------------------------------------------------------------ */

typedef struct {
    POINT pt;
    HWND hIgnore;
    HWND hFound;
    RECT rectFound;
} WindowPickerCtx;

static BOOL CALLBACK EnumWindowsPickerProc(HWND hwnd, LPARAM lParam)
{
    WindowPickerCtx *ctx = (WindowPickerCtx *)lParam;
    if (hwnd == ctx->hIgnore) return TRUE;
    for (int i = 0; i < g_pinnedCount; i++) if (g_pinned[i].overlay == hwnd) return TRUE;
    if (!IsValidTargetWindow(hwnd)) return TRUE;

    RECT r;
    GetFrameRect(hwnd, &r);
    if (PtInRect(&r, ctx->pt)) {
        ctx->hFound = hwnd;
        ctx->rectFound = r;
        return FALSE;
    }
    return TRUE;
}

static bool UpdatePickerHover(HWND hwndOverlay, POINT pt)
{
    WindowPickerCtx ctx;
    ctx.pt = pt;
    ctx.hIgnore = hwndOverlay;
    ctx.hFound = NULL;
    SetRectEmpty(&ctx.rectFound);
    EnumWindows(EnumWindowsPickerProc, (LPARAM)&ctx);
    HWND hTarget = ctx.hFound;
    if (!hTarget) {
        if (g_pickerHoveredHwnd != NULL) {
            g_pickerHoveredHwnd = NULL;
            SetRectEmpty(&g_pickerHoveredRect);
            return true;
        }
        return false;
    }
    if (hTarget != g_pickerHoveredHwnd || !EqualRect(&ctx.rectFound, &g_pickerHoveredRect)) {
        g_pickerHoveredHwnd = hTarget;
        g_pickerHoveredRect = ctx.rectFound;
        return true;
    }
    return false;
}

static LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT pt;
        GetCursorPos(&pt);
        if (UpdatePickerHover(hwnd, pt)) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        HWND chosen = g_pickerHoveredHwnd;
        DestroyWindow(hwnd);
        if (chosen && IsWindow(chosen)) {
            if (IsPinned(chosen)) {
                UnpinWindow(chosen);
            } else {
                if (g_pinnedCount >= g_cfg.maxWindows && g_pinnedCount > 0) {
                    RemovePinnedAt(0, true);
                }
                PinWindow(chosen);
                SetForegroundWindow(chosen);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_KEYDOWN: {
        if (msg == WM_KEYDOWN && wParam != VK_ESCAPE) break;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmBuf = CreateCompatibleBitmap(hdc, g_vw, g_vh);
        HBITMAP hOldBuf = (HBITMAP)SelectObject(hdcMem, hbmBuf);
        POINT ptCursor;
        GetCursorPos(&ptCursor);
        UINT dpi = TinyDPI_GetDpiForPoint(ptCursor);
        HDC hdcDesktop = CreateCompatibleDC(hdc);
        HBITMAP hOldDesk = (HBITMAP)SelectObject(hdcDesktop, g_hbmDesktop);

        TinyGUI_ApplyDimOverlay(hdcMem, hdcDesktop, g_vw, g_vh, 110);
        SetBkMode(hdcMem, TRANSPARENT);

        if (g_pickerHoveredHwnd && IsWindow(g_pickerHoveredHwnd)) {
            RECT sel = {
                g_pickerHoveredRect.left - g_vx,
                g_pickerHoveredRect.top - g_vy,
                g_pickerHoveredRect.right - g_vx,
                g_pickerHoveredRect.bottom - g_vy
            };
            if (sel.left < 0) sel.left = 0;
            if (sel.top < 0) sel.top = 0;
            if (sel.right > g_vw) sel.right = g_vw;
            if (sel.bottom > g_vh) sel.bottom = g_vh;
            int sw = sel.right - sel.left, sh = sel.bottom - sel.top;
            if (sw > 0 && sh > 0) {
                BitBlt(hdcMem, sel.left, sel.top, sw, sh, hdcDesktop, sel.left, sel.top, SRCCOPY);

                COLORREF highlightColor = IsPinned(g_pickerHoveredHwnd)
                    ? RGB(255, 140, 0)
                    : RGB(0, 122, 255);
                HPEN hPen = CreatePen(PS_SOLID, 2, highlightColor);
                HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
                HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hNullBrush);
                Rectangle(hdcMem, sel.left, sel.top, sel.right, sel.bottom);
                SelectObject(hdcMem, hOldBrush);
                SelectObject(hdcMem, hOldPen);
                DeleteObject(hPen);
            }
        }

        HFONT hPromptFont = TinyFont_CreateUI(11, TINY_FONT_WEIGHT_MEDIUM, dpi);
        SelectObject(hdcMem, hPromptFont);
        SetTextColor(hdcMem, RGB(160, 165, 180));

        HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        RECT promptR;
        if (GetMonitorInfoW(hMon, &mi)) {
            promptR.left = mi.rcMonitor.left - g_vx;
            promptR.right = mi.rcMonitor.right - g_vx;
            promptR.bottom = (mi.rcMonitor.bottom - g_vy) - MulDiv(10, (int)dpi, 96);
            promptR.top = (mi.rcMonitor.bottom - g_vy) - MulDiv(40, (int)dpi, 96);
        } else {
            promptR.left = 0;
            promptR.right = g_vw;
            promptR.bottom = g_vh - MulDiv(10, (int)dpi, 96);
            promptR.top = g_vh - MulDiv(40, (int)dpi, 96);
        }

        const wchar_t *promptText = (g_pickerHoveredHwnd && IsPinned(g_pickerHoveredHwnd))
            ? L"[ Click ] Unpin Window        [ Right Click / Esc ] Cancel"
            : L"[ Click ] Pin Window          [ Right Click / Esc ] Cancel";
        DrawTextW(hdcMem, promptText, -1, &promptR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(hPromptFont);

        BitBlt(hdc, 0, 0, g_vw, g_vh, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcDesktop, hOldDesk);
        DeleteDC(hdcDesktop);
        SelectObject(hdcMem, hOldBuf);
        DeleteObject(hbmBuf);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    case WM_DESTROY:
        g_pickerActive = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowInteractiveWindowPicker(void)
{
    if (g_pickerActive) return;
    TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &g_vw, &g_vh);
    if (g_vw <= 0 || g_vh <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    g_hbmDesktop = CreateCompatibleBitmap(hdcScreen, g_vw, g_vh);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hbmDesktop);
    BitBlt(hdcMem, 0, 0, g_vw, g_vh, hdcScreen, g_vx, g_vy, SRCCOPY);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    HINSTANCE hInst = GetModuleHandleW(NULL);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = PickerWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_HAND);
        wc.lpszClassName = L"TinyPinPickerOverlayClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_pickerHoveredHwnd = NULL;
    SetRectEmpty(&g_pickerHoveredRect);

    HWND hwndPicker = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"TinyPinPickerOverlayClass", L"Pin2Top Picker",
        WS_POPUP | WS_VISIBLE,
        g_vx, g_vy, g_vw, g_vh,
        NULL, NULL, hInst, NULL);
    if (!hwndPicker) {
        if (g_hbmDesktop) {
            DeleteObject(g_hbmDesktop);
            g_hbmDesktop = NULL;
        }
        return;
    }

    g_pickerActive = true;
    SetForegroundWindow(hwndPicker);

    POINT ptCursor;
    GetCursorPos(&ptCursor);
    UpdatePickerHover(hwndPicker, ptCursor);
    InvalidateRect(hwndPicker, NULL, FALSE);

    MSG msg;
    while (g_pickerActive && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hbmDesktop) {
        DeleteObject(g_hbmDesktop);
        g_hbmDesktop = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Toggle logic                                                       */
/* ------------------------------------------------------------------ */

static void HandleToggle(void)
{
    HWND fg = GetForegroundWindow();

    /* 1. If foreground window is already pinned, unpin it */
    if (fg != NULL && IsPinned(fg)) {
        UnpinWindow(fg);
        if (g_pinnedCount == 0) g_running = false;
        return;
    }

    /* 2. Interactive window picker mode */
    if (g_cfg.windowSelection) {
        ShowInteractiveWindowPicker();
        if (g_pinnedCount == 0) g_running = false;
        return;
    }

    /* 3. Non-picker / active foreground mode (--no-picker / --active) */
    if (IsValidTargetWindow(fg)) {
        if (g_pinnedCount >= g_cfg.maxWindows && g_pinnedCount > 0) {
            RemovePinnedAt(0, true);
        }
        PinWindow(fg);
    } else {
        /* If toggle invoked on empty area or shell, unpin all and exit */
        g_running = false;
    }
}

/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    TinyDPI_EnablePerMonitorAwareness();
    g_cfg = BuildConfig();

    HANDLE hEvent = NULL;
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &hEvent)) {
        return 0;
    }
    g_event = hEvent;

    g_borderColor = GetWallpaperDominantColorThief();
    if (g_borderColor == KEY_COLOR) {
        g_borderColor = RGB(2, 2, 3);
    }

    if (g_cfg.windowSelection) {
        ShowInteractiveWindowPicker();
    } else {
        HWND target = GetForegroundWindow();
        for (int retry = 0; retry < 10 && (!target || !IsValidTargetWindow(target)); retry++) {
            Sleep(10);
            target = GetForegroundWindow();
        }
        if (IsValidTargetWindow(target)) {
            PinWindow(target);
        }
    }

    if (g_pinnedCount == 0) {
        if (g_event != NULL) CloseHandle(g_event);
        return 0;
    }

    while (g_running) {
        DWORD w = WaitForSingleObject(g_event, TRACK_INTERVAL_MS);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!g_running) break;
        if (w == WAIT_OBJECT_0) HandleToggle();
        if (g_running) TrackPinnedWindows();
    }

    while (g_pinnedCount > 0) RemovePinnedAt(0, true);
    if (g_event != NULL) CloseHandle(g_event);
    return 0;
}
