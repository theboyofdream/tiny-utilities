#ifndef TINY_TRAY_H
#define TINY_TRAY_H

/*
 * TinyTray — notification-area icon host for the console server tools
 * (kyper, whisper-cast). Zero assets and zero heap: the 16x16 icon is
 * generated at runtime from raw 32bpp pixels (flat colored disc with a
 * darker rim and a 1px transparent margin), so tools stay single-.c files
 * with no .rc resources.
 *
 * Host design: a hidden (never visible) WS_EX_TOOLWINDOW message window is
 * created on the calling thread. The tool's main wait loop must call
 * TinyTray_PumpMessages() whenever MsgWaitForMultipleObjects reports
 * pending input — otherwise the right-click menu cannot appear. Right-click
 * opens a minimal "Exit" menu and double-click exits as well; both invoke
 * the onExit callback supplied at creation. The icon is re-added
 * automatically after an Explorer restart (TaskbarCreated broadcast).
 *
 * Hard-rule note (user-approved 2026-10-26): the host window is a GUI window
 * by API necessity, but it is never shown on screen (zero size, tool window,
 * no activate) — only the notification icon itself is visible.
 */

#include <shellapi.h>

/* Send this message to the tray host window to run the exact menu-exit code
 * path (used by automated smoke tests and any programmatic shutdown). */
#define TINYTRAY_MSG_EXIT (WM_APP + 0x7A21)

#define TINYTRAY_TIP_CAP  128
#define TINYTRAY_ID       1

static HWND    g_trayHwnd = NULL;
static HICON   g_trayIcon = NULL;
static UINT    g_trayCbMsg = 0;
static UINT    g_trayTaskbarMsg = 0;
static void  (*g_trayOnExit)(void) = NULL;
static wchar_t g_trayTip[TINYTRAY_TIP_CAP];

/* Runtime icon: flat disc with a ~45% darker rim for contrast on light taskbars. */
static HICON TinyTray_MakeIcon(COLORREF rgb) {
    /* xorBits: 16x16 BGRA pixels; andBits: 1bpp mask, all zero = opaque + alpha. */
    static unsigned char xorBits[16 * 16 * 4];
    static unsigned char andBits[16 * 2];
    const float cx = 7.5f, cy = 7.5f;
    const float rCore = 5.9f, rEdge = 7.0f;
    const BYTE br = GetRValue(rgb), bg = GetGValue(rgb), bb = GetBValue(rgb);
    ZeroMemory(xorBits, sizeof(xorBits));
    ZeroMemory(andBits, sizeof(andBits));
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float d = dx * dx + dy * dy;
            unsigned char *px = xorBits + (size_t)y * 64 + x * 4;
            if (d <= rCore * rCore) {
                px[0] = bb; px[1] = bg; px[2] = br; px[3] = 0xFF;
            } else if (d <= rEdge * rEdge) {
                px[0] = (BYTE)(bb * 0.5f); px[1] = (BYTE)(bg * 0.5f);
                px[2] = (BYTE)(br * 0.5f); px[3] = 0xFF;
            }
        }
    }
    return CreateIcon(GetModuleHandleW(NULL), 16, 16, 1, 32, andBits, xorBits);
}

static bool TinyTray_EnsureAdded(void) {
    if (!g_trayHwnd || !g_trayIcon) return false;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayHwnd;
    nid.uID = TINYTRAY_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = g_trayCbMsg;
    nid.hIcon = g_trayIcon;
    wcscpy_s(nid.szTip, _countof(nid.szTip), g_trayTip);
    return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

static void TinyTray_HandleExitRequest(HWND hwnd) {
    (void)hwnd;
    if (g_trayOnExit) g_trayOnExit();
}

static LRESULT CALLBACK TinyTray_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_trayTaskbarMsg && msg == g_trayTaskbarMsg) {
        TinyTray_EnsureAdded();                     /* Explorer restarted: re-add icon */
        return 0;
    }
    if (g_trayCbMsg && msg == g_trayCbMsg) {
        if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);              /* required so the menu dismisses */
            HMENU menu = CreatePopupMenu();
            if (menu) {
                AppendMenuW(menu, MF_STRING, 1, L"Exit");
                UINT on = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                                         pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                if (on == 1) TinyTray_HandleExitRequest(hwnd);
            }
        } else if (lp == WM_LBUTTONDBLCLK) {
            TinyTray_HandleExitRequest(hwnd);
        }
        return 0;
    }
    if (msg == TINYTRAY_MSG_EXIT) {
        TinyTray_HandleExitRequest(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/*
 * Create the hidden host window + notification icon.
 * className: unique per tool (e.g. L"TinyKyperTrayHost").
 * tooltip:   szTip text (same string as the auto re-add uses).
 * balloonTitle/balloonText: optional startup balloon (NULL title = skip).
 * iconRgb:   runtime disc icon color.
 * onExit:    invoked on menu Exit / double-click; tool flips its running flag.
 * Fails cleanly (returns false) when the taskbar refuses the icon.
 */
static bool TinyTray_Create(const wchar_t *className, const wchar_t *tooltip,
                            const wchar_t *balloonTitle, const wchar_t *balloonText,
                            COLORREF iconRgb, void (*onExit)(void)) {
    if (g_trayHwnd) return true;
    g_trayOnExit = onExit;
    g_trayCbMsg = WM_APP + 0x7A20;
    g_trayTaskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (tooltip) {
        wcsncpy_s(g_trayTip, TINYTRAY_TIP_CAP, tooltip, _TRUNCATE);
    } else {
        g_trayTip[0] = L'\0';
    }
    g_trayIcon = TinyTray_MakeIcon(iconRgb);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TinyTray_WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc)) return false;

    g_trayHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className, L"",
                                 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!g_trayHwnd) return false;

    if (!TinyTray_EnsureAdded()) {
        DestroyWindow(g_trayHwnd);
        g_trayHwnd = NULL;
        return false;
    }

    if (balloonTitle && balloonText && balloonText[0]) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_trayHwnd;
        nid.uID = TINYTRAY_ID;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, _countof(nid.szInfoTitle), balloonTitle, _TRUNCATE);
        wcsncpy_s(nid.szInfo, _countof(nid.szInfo), balloonText, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    return true;
}

/* Remove the icon and destroy the host window (no-op when not created). */
static void TinyTray_Destroy(void) {
    if (!g_trayHwnd) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayHwnd;
    nid.uID = TINYTRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(g_trayHwnd);
    g_trayHwnd = NULL;
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

/* Pump pending window messages; call whenever the main loop sees input. */
static void TinyTray_PumpMessages(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

#endif /* TINY_TRAY_H */
