/*
 * ColorPicker — floating tooltip near cursor with live color sampling & clipboard copy.
 * Source: color-picker.c
 *
 * Multi-Monitor Aware & Configurable Font Size:
 * Supports --font-size N (8..48, default 13) with dynamic card & swatch scaling.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "common/tiny_gui.h"
#include "common/tiny_dpi.h"
#include "common/font.h"
#include "common/clipboard.h"

#define APPMUTEX_NAME L"Global\\TinyColorPickerMutex"
#define APPEVENT_NAME L"Global\\TinyColorPickerEvent"

#define LOOP_INTERVAL_MS 16   /* ~60 FPS */
#define ACTIVATE_GRACE_MS 250 /* ignore click right after activation */

#define DEFAULT_WIDTH 2
#define WIDTH_MIN 1
#define WIDTH_MAX 20
#define DEFAULT_FONT_SIZE 10
#define FONT_MIN 8
#define FONT_MAX 32

#define KEY_COLOR RGB(1, 1, 2) /* key color for transparent overlay */

typedef enum {
    FMT_HEX = 0,
    FMT_RGB = 1,
    FMT_HSL = 2,
    FMT_CMYK = 3,
    FMT_COUNT = 4
} OutputFormat;

typedef struct {
    const wchar_t *label;
    const wchar_t *shortcut;
    wchar_t displayValue[64];
    wchar_t copyValue[64];
} ColorFormatEntry;

static ColorFormatEntry g_formats[FMT_COUNT] = {
    { L"HEX",  L"1", L"", L"" },
    { L"RGB",  L"2", L"", L"" },
    { L"HSL",  L"3", L"", L"" },
    { L"CMYK", L"4", L"", L"" },
};

typedef struct {
    int width;
    OutputFormat format;
    int fontSize;
} Config;

static Config g_cfg = {DEFAULT_WIDTH, FMT_HEX, DEFAULT_FONT_SIZE};
static HWND g_tooltip = NULL;

static int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;
static bool g_running = true;
static COLORREF g_lastColor = RGB(0, 0, 0);

static int g_tooltipW = 220;
static int g_tooltipH = 90;

/* ------------------------------------------------------------------ */
/* CLI parsing                                                         */
/* ------------------------------------------------------------------ */

#define MAX_ARGV 64

static int SplitCommandLine(wchar_t *buf, wchar_t **argv, int max)
{
    int n = 0;
    wchar_t *p = buf;
    for (;;) {
        while (*p == L' ' || *p == L'\t')
            p++;
        if (*p == L'\0')
            break;
        if (n >= max)
            break;
        if (*p == L'"') {
            p++;
            argv[n++] = p;
            while (*p != L'\0' && *p != L'"')
                p++;
            if (*p == L'"')
                *p++ = L'\0';
        } else {
            argv[n++] = p;
            while (*p != L'\0' && *p != L' ' && *p != L'\t')
                p++;
            if (*p != L'\0')
                *p++ = L'\0';
        }
    }
    return n;
}

static void ParseCommandLine(int argc, wchar_t **argv, Config *cfg)
{
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--format") == 0 && i + 1 < argc) {
            i++;
            if (_wcsicmp(argv[i], L"hex") == 0) cfg->format = FMT_HEX;
            else if (_wcsicmp(argv[i], L"rgb") == 0) cfg->format = FMT_RGB;
            else if (_wcsicmp(argv[i], L"hsl") == 0) cfg->format = FMT_HSL;
            else if (_wcsicmp(argv[i], L"cmyk") == 0) cfg->format = FMT_CMYK;
        } else if (_wcsnicmp(argv[i], L"--format=", 9) == 0) {
            const wchar_t *f = argv[i] + 9;
            if (_wcsicmp(f, L"hex") == 0) cfg->format = FMT_HEX;
            else if (_wcsicmp(f, L"rgb") == 0) cfg->format = FMT_RGB;
            else if (_wcsicmp(f, L"hsl") == 0) cfg->format = FMT_HSL;
            else if (_wcsicmp(f, L"cmyk") == 0) cfg->format = FMT_CMYK;
        } else if (_wcsicmp(argv[i], L"--font-size") == 0 && i + 1 < argc) {
            int sz = _wtoi(argv[++i]);
            if (sz >= FONT_MIN && sz <= FONT_MAX) cfg->fontSize = sz;
        } else if (_wcsnicmp(argv[i], L"--font-size=", 12) == 0) {
            int sz = _wtoi(argv[i] + 12);
            if (sz >= FONT_MIN && sz <= FONT_MAX) cfg->fontSize = sz;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Color formatting helpers                                           */
/* ------------------------------------------------------------------ */

static void RGBtoHSL(BYTE r, BYTE g, BYTE b, int *h, int *s, int *l)
{
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float maxVal = max(rf, max(gf, bf));
    float minVal = min(rf, min(gf, bf));
    float delta = maxVal - minVal;

    float lf = (maxVal + minVal) / 2.0f;
    float sf = 0.0f;
    float hf = 0.0f;

    if (delta > 0.00001f) {
        sf = (lf > 0.5f) ? (delta / (2.0f - maxVal - minVal)) : (delta / (maxVal + minVal));

        if (maxVal == rf) {
            hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (maxVal == gf) {
            hf = (bf - rf) / delta + 2.0f;
        } else {
            hf = (rf - gf) / delta + 4.0f;
        }
        hf *= 60.0f;
    }

    *h = (int)(hf + 0.5f);
    *s = (int)(sf * 100.0f + 0.5f);
    *l = (int)(lf * 100.0f + 0.5f);
}

static void RGBtoCMYK(BYTE r, BYTE g, BYTE b, int *c, int *m, int *y, int *k)
{
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float kf = 1.0f - max(rf, max(gf, bf));
    if (kf >= 1.0f - 0.00001f) {
        *c = 0; *m = 0; *y = 0; *k = 100;
        return;
    }

    float cf = (1.0f - rf - kf) / (1.0f - kf);
    float mf = (1.0f - gf - kf) / (1.0f - kf);
    float yf = (1.0f - bf - kf) / (1.0f - kf);

    *c = (int)(cf * 100.0f + 0.5f);
    *m = (int)(mf * 100.0f + 0.5f);
    *y = (int)(yf * 100.0f + 0.5f);
    *k = (int)(kf * 100.0f + 0.5f);
}

static void FormatColorStrings(COLORREF c)
{
    BYTE r = GetRValue(c);
    BYTE g = GetGValue(c);
    BYTE b = GetBValue(c);

    /* HEX */
    wsprintfW(g_formats[FMT_HEX].displayValue, L"#%02X%02X%02X", r, g, b);
    wcsncpy(g_formats[FMT_HEX].copyValue, g_formats[FMT_HEX].displayValue, 63);

    /* RGB */
    wsprintfW(g_formats[FMT_RGB].displayValue, L"rgb(%d, %d, %d)", r, g, b);
    wcsncpy(g_formats[FMT_RGB].copyValue, g_formats[FMT_RGB].displayValue, 63);

    /* HSL */
    int h, s, l;
    RGBtoHSL(r, g, b, &h, &s, &l);
    wsprintfW(g_formats[FMT_HSL].displayValue, L"hsl(%d, %d%%, %d%%)", h, s, l);
    wcsncpy(g_formats[FMT_HSL].copyValue, g_formats[FMT_HSL].displayValue, 63);

    /* CMYK */
    int cVal, mVal, yVal, kVal;
    RGBtoCMYK(r, g, b, &cVal, &mVal, &yVal, &kVal);
    wsprintfW(g_formats[FMT_CMYK].displayValue, L"cmyk(%d%%, %d%%, %d%%, %d%%)", cVal, mVal, yVal, kVal);
    wcsncpy(g_formats[FMT_CMYK].copyValue, g_formats[FMT_CMYK].displayValue, 63);
}

/* ------------------------------------------------------------------ */
/* Tooltip window proc & rendering                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK TooltipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        POINT cur;
        GetCursorPos(&cur);
        UINT dpi = TinyDPI_GetDpiForPoint(cur);

        HBRUSH bgBrush = CreateSolidBrush(KEY_COLOR);
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        HFONT font = TinyFont_CreateMonospace(g_cfg.fontSize, TINY_FONT_WEIGHT_BOLD, dpi);
        HFONT oldFont = (HFONT)SelectObject(hdc, font);

        SIZE sizeLabel = {0, 0}, sizeVal = {0, 0}, sizeShort = {0, 0};
        int maxLabelW = 0, maxValW = 0, maxShortW = 0;
        int maxTextH = 0;

        for (int i = 0; i < FMT_COUNT; i++) {
            GetTextExtentPoint32W(hdc, g_formats[i].label, (int)wcslen(g_formats[i].label), &sizeLabel);
            if (sizeLabel.cx > maxLabelW) maxLabelW = sizeLabel.cx;
            if (sizeLabel.cy > maxTextH) maxTextH = sizeLabel.cy;

            GetTextExtentPoint32W(hdc, g_formats[i].displayValue, (int)wcslen(g_formats[i].displayValue), &sizeVal);
            if (sizeVal.cx > maxValW) maxValW = sizeVal.cx;
            if (sizeVal.cy > maxTextH) maxTextH = sizeVal.cy;

            GetTextExtentPoint32W(hdc, g_formats[i].shortcut, (int)wcslen(g_formats[i].shortcut), &sizeShort);
            if (sizeShort.cx > maxShortW) maxShortW = sizeShort.cx;
            if (sizeShort.cy > maxTextH) maxTextH = sizeShort.cy;
        }

        int rowH = maxTextH + MulDiv(3, (int)dpi, 96);
        int totalRowsH = FMT_COUNT * rowH;

        int padX = MulDiv(10, (int)dpi, 96);
        int padY = MulDiv(8, (int)dpi, 96);
        int gapX = MulDiv(10, (int)dpi, 96);
        int swatchH = MulDiv(14, (int)dpi, 96);
        int swatchGapY = MulDiv(6, (int)dpi, 96);

        int col1X = padX;
        int col1W = maxLabelW;
        int col2X = col1X + col1W + gapX;
        int col2W = maxValW;
        int col3X = col2X + col2W + gapX;
        int col3W = maxShortW;

        int boxW = col3X + col3W + padX;
        int boxH = padY + swatchH + swatchGapY + totalRowsH + padY;

        g_tooltipW = boxW;
        g_tooltipH = boxH;

        HBRUSH cardBg = CreateSolidBrush(RGB(24, 24, 28));
        HPEN cardBorder = CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
        HGDIOBJ oldBrush = SelectObject(hdc, cardBg);
        HGDIOBJ oldPen = SelectObject(hdc, cardBorder);

        RoundRect(hdc, 0, 0, boxW, boxH, MulDiv(8, (int)dpi, 96), MulDiv(8, (int)dpi, 96));

        DeleteObject(cardBg);
        DeleteObject(cardBorder);

        /* Draw Swatch on Top */
        int swatchW = boxW - 2 * padX;
        HBRUSH swBrush = CreateSolidBrush(g_lastColor);
        HPEN swBorder = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        SelectObject(hdc, swBrush);
        SelectObject(hdc, swBorder);

        RoundRect(hdc, padX, padY, padX + swatchW, padY + swatchH, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));

        DeleteObject(swBrush);
        DeleteObject(swBorder);

        /* Draw Text Rows below Swatch */
        SetBkMode(hdc, TRANSPARENT);
        int rowsStartY = padY + swatchH + swatchGapY;

        for (int i = 0; i < FMT_COUNT; i++) {
            int rowY = rowsStartY + i * rowH + (rowH - maxTextH) / 2;

            SetTextColor(hdc, RGB(160, 160, 175));
            TextOutW(hdc, col1X, rowY, g_formats[i].label, (int)wcslen(g_formats[i].label));

            if (i == g_cfg.format) {
                SetTextColor(hdc, RGB(255, 255, 255));
            } else {
                SetTextColor(hdc, RGB(230, 230, 235));
            }
            TextOutW(hdc, col2X, rowY, g_formats[i].displayValue, (int)wcslen(g_formats[i].displayValue));

            /* Secondary dim text color for shortcuts */
            SetTextColor(hdc, RGB(120, 125, 135));
            TextOutW(hdc, col3X, rowY, g_formats[i].shortcut, (int)wcslen(g_formats[i].shortcut));
        }

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldFont);
        DeleteObject(font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RecalculateTooltipSize(HDC hdc, UINT dpi)
{
    HFONT font = TinyFont_CreateMonospace(g_cfg.fontSize, TINY_FONT_WEIGHT_BOLD, dpi);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);

    SIZE sizeLabel = {0, 0}, sizeVal = {0, 0}, sizeShort = {0, 0};
    int maxLabelW = 0, maxValW = 0, maxShortW = 0;
    int maxTextH = 0;

    for (int i = 0; i < FMT_COUNT; i++) {
        GetTextExtentPoint32W(hdc, g_formats[i].label, (int)wcslen(g_formats[i].label), &sizeLabel);
        if (sizeLabel.cx > maxLabelW) maxLabelW = sizeLabel.cx;
        if (sizeLabel.cy > maxTextH) maxTextH = sizeLabel.cy;

        GetTextExtentPoint32W(hdc, g_formats[i].displayValue, (int)wcslen(g_formats[i].displayValue), &sizeVal);
        if (sizeVal.cx > maxValW) maxValW = sizeVal.cx;
        if (sizeVal.cy > maxTextH) maxTextH = sizeVal.cy;

        GetTextExtentPoint32W(hdc, g_formats[i].shortcut, (int)wcslen(g_formats[i].shortcut), &sizeShort);
        if (sizeShort.cx > maxShortW) maxShortW = sizeShort.cx;
        if (sizeShort.cy > maxTextH) maxTextH = sizeShort.cy;
    }

    int rowH = maxTextH + MulDiv(3, (int)dpi, 96);
    int totalRowsH = FMT_COUNT * rowH;

    int padX = MulDiv(10, (int)dpi, 96);
    int padY = MulDiv(8, (int)dpi, 96);
    int gapX = MulDiv(10, (int)dpi, 96);
    int swatchH = MulDiv(14, (int)dpi, 96);
    int swatchGapY = MulDiv(6, (int)dpi, 96);

    int col1W = maxLabelW;
    int col2W = maxValW;
    int col3W = maxShortW;

    int col1X = padX;
    int col2X = col1X + col1W + gapX;
    int col3X = col2X + col2W + gapX;

    int boxW = col3X + col3W + padX;
    int boxH = padY + swatchH + swatchGapY + totalRowsH + padY;

    g_tooltipW = boxW;
    g_tooltipH = boxH;

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

/* ------------------------------------------------------------------ */
/* Instance execution & loop                                          */
/* ------------------------------------------------------------------ */

static void UpdateTooltipPosition(POINT cur)
{
    UINT dpi = TinyDPI_GetDpiForPoint(cur);
    HDC desktopDC = GetDC(NULL);
    if (desktopDC != NULL) {
        RecalculateTooltipSize(desktopDC, dpi);
        ReleaseDC(NULL, desktopDC);
    }

    HMONITOR hMon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    int tx = cur.x + 16;
    int ty = cur.y + 16;

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(MONITORINFO);

    if (GetMonitorInfoW(hMon, &mi)) {
        RECT mrc = mi.rcWork;

        /* If tooltip extends past current monitor right edge, flip to left of cursor */
        if (tx + g_tooltipW > mrc.right)
            tx = cur.x - g_tooltipW - 12;

        /* If tooltip extends past current monitor bottom edge, flip above cursor */
        if (ty + g_tooltipH > mrc.bottom)
            ty = cur.y - g_tooltipH - 12;

        /* Clamp inside current monitor bounds */
        if (tx < mrc.left)
            tx = mrc.left + 4;
        if (ty < mrc.top)
            ty = mrc.top + 4;
    } else {
        /* Fallback for global virtual screen */
        if (tx + g_tooltipW > g_vx + g_vw)
            tx = cur.x - g_tooltipW - 12;
        if (ty + g_tooltipH > g_vy + g_vh)
            ty = cur.y - g_tooltipH - 12;
        if (tx < g_vx)
            tx = g_vx + 4;
        if (ty < g_vy)
            ty = g_vy + 4;
    }

    SetWindowPos(g_tooltip, HWND_TOPMOST, tx, ty, g_tooltipW, g_tooltipH, SWP_NOACTIVATE);
    InvalidateRect(g_tooltip, NULL, FALSE);
}

static void RunInstance(HANDLE ev)
{
    POINT lastCur = {-1, -1};
    COLORREF lastColor = CLR_INVALID;
    ULONGLONG start = GetTickCount64();
    HDC desktopDC = GetDC(NULL);

    while (g_running) {
        DWORD w = WaitForSingleObject(ev, LOOP_INTERVAL_MS);
        ULONGLONG now;
        MSG msg;
        POINT cur;
        COLORREF sample;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (w == WAIT_OBJECT_0) {
            /* Toggle event from second launch -> exit process */
            g_running = false;
            break;
        }        now = GetTickCount64();
        if (now - start >= ACTIVATE_GRACE_MS) {
            bool keyHandled = false;
            for (int i = 0; i < FMT_COUNT; i++) {
                int vkMain = '1' + i;
                int vkNum = VK_NUMPAD1 + i;
                if ((GetAsyncKeyState(vkMain) & 0x8000) != 0 ||
                    (GetAsyncKeyState(vkNum) & 0x8000) != 0) {
                    TinyClipboard_SetText(g_formats[i].copyValue);
                    g_running = false;
                    keyHandled = true;
                    break;
                }
            }
            if (keyHandled)
                break;

            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
                POINT curPos;
                GetCursorPos(&curPos);
                if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) || (GetAsyncKeyState(VK_ADD) & 0x8000)) {
                    g_cfg.fontSize = TinyFont_StepSize(g_cfg.fontSize, 1);
                    UpdateTooltipPosition(curPos);
                    Sleep(80);
                } else if ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) || (GetAsyncKeyState(VK_SUBTRACT) & 0x8000)) {
                    g_cfg.fontSize = TinyFont_StepSize(g_cfg.fontSize, -1);
                    UpdateTooltipPosition(curPos);
                    Sleep(80);
                } else if ((GetAsyncKeyState('0') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD0) & 0x8000)) {
                    g_cfg.fontSize = TinyFont_GetSystemDefaultSize();
                    UpdateTooltipPosition(curPos);
                    Sleep(80);
                }
            }

            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                /* Mouse click -> copy formatted color of configured format to clipboard and exit */
                TinyClipboard_SetText(g_formats[g_cfg.format].copyValue);
                g_running = false;
                break;
            }
            if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                /* Right click or ESC -> cancel and exit */
                g_running = false;
                break;
            }
        }

        GetCursorPos(&cur);
        sample = GetPixel(desktopDC, cur.x, cur.y);

        if (cur.x != lastCur.x || cur.y != lastCur.y || sample != lastColor) {
            lastCur = cur;
            lastColor = sample;
            g_lastColor = sample;
            FormatColorStrings(sample);
            UpdateTooltipPosition(cur);
        }
    }

    if (desktopDC != NULL)
        ReleaseDC(NULL, desktopDC);
}

static bool PrepareOverlay(void)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    POINT curPos;

    TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &g_vw, &g_vh);
    if (g_vw <= 0 || g_vh <= 0)
        return false;

    /* Register Tooltip Window Class */
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TooltipWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TinyColorPickerTooltip";
    if (!RegisterClassW(&wc))
        return false;

    GetCursorPos(&curPos);

    /* Create Tooltip Layered Window */
    g_tooltip = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"TinyColorPickerTooltip", L"", WS_POPUP,
        curPos.x + 16, curPos.y + 16, g_tooltipW, g_tooltipH,
        NULL, NULL, inst, NULL);

    if (g_tooltip == NULL)
        return false;

    SetLayeredWindowAttributes(g_tooltip, KEY_COLOR, 0, LWA_COLORKEY);
    ShowWindow(g_tooltip, SW_SHOWNOACTIVATE);
    SetWindowPos(g_tooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    return true;
}

static void DestroyOverlay(void)
{
    if (g_tooltip != NULL) {
        DestroyWindow(g_tooltip);
        g_tooltip = NULL;
    }
}

/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    int argc = 0;
    wchar_t argBuf[4096];
    wchar_t *argv[MAX_ARGV];
    HANDLE mutex;
    HANDLE ev;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    TinyDPI_EnablePerMonitorAwareness();

    lstrcpynW(argBuf, GetCommandLineW(), 4096);
    argc = SplitCommandLine(argBuf, argv, MAX_ARGV);
    ParseCommandLine(argc, argv, &g_cfg);

    mutex = CreateMutexW(NULL, FALSE, APPMUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, APPEVENT_NAME);
        if (ev != NULL) {
            SetEvent(ev);
            CloseHandle(ev);
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }

    ev = CreateEventW(NULL, FALSE, FALSE, APPEVENT_NAME);
    if (ev == NULL || !PrepareOverlay()) {
        if (ev != NULL)
            CloseHandle(ev);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    RunInstance(ev);

    DestroyOverlay();
    CloseHandle(ev);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
