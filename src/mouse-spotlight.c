/*
 * mouse-spotlight — Dims the screen except for a spotlight around the mouse.
 * Source: mouse-spotlight.c
 *
 * Features:
 *   - Follows mouse in real time with silky smooth 60 FPS rendering.
 *   - Uses UpdateLayeredWindow with per-pixel alpha DIB for zero lag / zero stutter.
 *   - --size / -s controls spotlight size (base radius in pixels at 96 DPI).
 *   - --dim / -d controls background dim percentage (0..100%) or raw alpha (0..255).
 *   - Ctrl + / - resizes spotlight dynamically.
 *   - Ctrl + 0 resets spotlight size to initial value.
 *   - Esc, any key press, or mouse click exits utility cleanly (after activation grace).
 *   - Instant on/off, click-through overlay (WS_EX_TRANSPARENT).
 *   - Single-instance toggle pattern via IPC mutex & event.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"
#include "common/tiny_dpi.h"
#include "common/tiny_gui.h"

#define APPMUTEX_NAME L"Global\\TinyMouseSpotlightMutex"
#define APPEVENT_NAME L"Global\\TinyMouseSpotlightEvent"

#define DEFAULT_SIZE 100
#define MIN_SIZE     20
#define MAX_SIZE     1000

#define DEFAULT_DIM  65
#define LOOP_INTERVAL_MS 16
#define ACTIVATE_GRACE_MS 250
#define FEATHER_PX 6

static HWND g_dimWnd = NULL;
static HDC g_memDC = NULL;
static HBITMAP g_hBmp = NULL;
static DWORD *g_pixels = NULL;

static int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;
static int g_initialBaseSize = DEFAULT_SIZE;
static int g_baseSize = DEFAULT_SIZE;
static BYTE g_dimAlpha = 165; /* ~65% dark */

static LRESULT CALLBACK DimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool IsModifierKey(int vk)
{
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL:
            return true;
        default:
            return false;
    }
}

static void ClearDIBRect(int left, int top, int right, int bottom, DWORD color)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_vw) right = g_vw;
    if (bottom > g_vh) bottom = g_vh;

    for (int y = top; y < bottom; y++) {
        DWORD *row = g_pixels + (size_t)y * g_vw;
        for (int x = left; x < right; x++) {
            row[x] = color;
        }
    }
}

static void DrawSpotlightCircle(POINT cur, int radius)
{
    int localX = cur.x - g_vx;
    int localY = cur.y - g_vy;

    int outerR = radius + FEATHER_PX;
    int innerR = radius - FEATHER_PX;
    if (innerR < 0) innerR = 0;

    int left = localX - outerR - 1;
    int top = localY - outerR - 1;
    int right = localX + outerR + 2;
    int bottom = localY + outerR + 2;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g_vw) right = g_vw;
    if (bottom > g_vh) bottom = g_vh;

    long outerR2 = (long)outerR * outerR;
    long innerR2 = (long)innerR * innerR;

    DWORD dimPixel = ((DWORD)g_dimAlpha << 24);

    for (int y = top; y < bottom; y++) {
        int dy = y - localY;
        long dy2 = (long)dy * dy;
        DWORD *row = g_pixels + (size_t)y * g_vw;

        for (int x = left; x < right; x++) {
            int dx = x - localX;
            long dist2 = (long)dx * dx + dy2;

            if (dist2 <= innerR2) {
                row[x] = 0; /* fully transparent spotlight hole */
            } else if (dist2 >= outerR2) {
                row[x] = dimPixel;
            } else {
                float dist = sqrtf((float)dist2);
                float t = (dist - (float)innerR) / (float)(outerR - innerR);
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                BYTE a = (BYTE)(t * (float)g_dimAlpha);
                row[x] = ((DWORD)a << 24);
            }
        }
    }
}

static void PresentSpotlight(void)
{
    if (!g_dimWnd || !g_memDC) return;

    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    POINT dst = { g_vx, g_vy };
    POINT src = { 0, 0 };
    SIZE sz = { g_vw, g_vh };

    UpdateLayeredWindow(g_dimWnd, NULL, &dst, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

static bool InitDIBBuffer(void)
{
    if (g_hBmp) DeleteObject(g_hBmp);
    if (g_memDC) DeleteDC(g_memDC);

    HDC hdcScreen = GetDC(NULL);
    g_memDC = CreateCompatibleDC(hdcScreen);
    ReleaseDC(NULL, hdcScreen);

    if (!g_memDC) return false;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_vw;
    bi.bmiHeader.biHeight = -g_vh; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_hBmp = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, (void **)&g_pixels, NULL, 0);
    if (!g_hBmp || !g_pixels) return false;

    SelectObject(g_memDC, g_hBmp);

    /* Fill background with initial dim color */
    DWORD dimPixel = ((DWORD)g_dimAlpha << 24);
    size_t totalPixels = (size_t)g_vw * g_vh;
    for (size_t i = 0; i < totalPixels; i++) {
        g_pixels[i] = dimPixel;
    }

    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* 1. IPC Single-Instance Toggle check */
    HANDLE hEvent = NULL;
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &hEvent)) {
        return 0;
    }

    /* 2. Enable Per-Monitor v2 DPI Awareness */
    TinyDPI_EnablePerMonitorAwareness();

    /* 3. CLI Argument Parsing */
    int cliSize = DEFAULT_SIZE;
    int cliDim = DEFAULT_DIM;

    CliOption opts[] = {
        { L"--size", CLI_OPT_INT, &cliSize, MIN_SIZE, MAX_SIZE },
        { L"-s",     CLI_OPT_INT, &cliSize, MIN_SIZE, MAX_SIZE },
        { L"--dim",  CLI_OPT_INT, &cliDim,  0, 255 },
        { L"-d",     CLI_OPT_INT, &cliDim,  0, 255 }
    };
    TinyCLI_ParseCommandLine(opts, sizeof(opts) / sizeof(opts[0]));

    if (cliSize < MIN_SIZE) cliSize = MIN_SIZE;
    if (cliSize > MAX_SIZE) cliSize = MAX_SIZE;
    g_baseSize = cliSize;
    g_initialBaseSize = cliSize;

    if (cliDim <= 100) {
        g_dimAlpha = (BYTE)((cliDim * 255) / 100);
    } else {
        g_dimAlpha = (BYTE)(cliDim > 255 ? 255 : cliDim);
    }

    /* 4. Determine Virtual Screen Bounds */
    TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &g_vw, &g_vh);
    if (g_vw <= 0 || g_vh <= 0) {
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }

    /* 5. Initialize DIB Buffer */
    if (!InitDIBBuffer()) {
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }

    /* 6. Register Window Class and Create Overlay Window */
    const wchar_t *clsName = L"TinyMouseSpotlightDim";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DimWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = clsName;
    if (!RegisterClassW(&wc)) {
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }

    g_dimWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        clsName, L"", WS_POPUP,
        g_vx, g_vy, g_vw, g_vh,
        NULL, NULL, hInstance, NULL
    );

    if (!g_dimWnd) {
        if (hEvent) CloseHandle(hEvent);
        return 1;
    }

    ShowWindow(g_dimWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_dimWnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_NOACTIVATE);

    /* Flush initial key states */
    for (int vk = 1; vk < 255; vk++) {
        GetAsyncKeyState(vk);
    }

    DWORD startMs = GetTickCount();
    POINT lastPos = { -99999, -99999 };
    int lastRadius = -1;

    /* 7. Main Event Loop */
    MSG msg;
    bool running = true;

    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        /* Check IPC toggle signal */
        if (hEvent && WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        /* Check Esc key to exit immediately */
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            break;
        }

        /* Check Hotkeys for resizing (Ctrl + / - / 0) */
        bool ctrlDown = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);

        if (ctrlDown) {
            if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) || (GetAsyncKeyState(VK_ADD) & 0x8000)) {
                g_baseSize += 3;
                if (g_baseSize > MAX_SIZE) g_baseSize = MAX_SIZE;
            }
            if ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) || (GetAsyncKeyState(VK_SUBTRACT) & 0x8000)) {
                g_baseSize -= 3;
                if (g_baseSize < MIN_SIZE) g_baseSize = MIN_SIZE;
            }
            if ((GetAsyncKeyState('0') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD0) & 0x8000)) {
                g_baseSize = g_initialBaseSize;
            }
        } else {
            /* Dismissal check: after activation grace period, any key press or mouse click exits */
            if (GetTickCount() - startMs > ACTIVATE_GRACE_MS) {
                for (int vk = 1; vk < 255; vk++) {
                    if (!IsModifierKey(vk) && (GetAsyncKeyState(vk) & 0x8000)) {
                        running = false;
                        break;
                    }
                }
                if (!running) break;
            }
        }

        /* Get cursor position & compute DPI scaled spotlight radius */
        POINT cur;
        if (GetCursorPos(&cur)) {
            UINT dpi = TinyDPI_GetDpiForPoint(cur);
            int scaledRadius = MulDiv(g_baseSize, (int)dpi, 96);

            /* Check if virtual screen bounds changed */
            int vx = 0, vy = 0, vw = 0, vh = 0;
            TinyGUI_GetVirtualScreenBounds(&vx, &vy, &vw, &vh);
            if (vx != g_vx || vy != g_vy || vw != g_vw || vh != g_vh) {
                g_vx = vx; g_vy = vy; g_vw = vw; g_vh = vh;
                InitDIBBuffer();
                SetWindowPos(g_dimWnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_NOACTIVATE);
                lastPos.x = -99999;
            }

            if (cur.x != lastPos.x || cur.y != lastPos.y || scaledRadius != lastRadius) {
                DWORD dimPixel = ((DWORD)g_dimAlpha << 24);

                /* Restore old circle bounding box back to dim color */
                if (lastRadius > 0 && lastPos.x != -99999) {
                    int oldLocalX = lastPos.x - g_vx;
                    int oldLocalY = lastPos.y - g_vy;
                    int oldR = lastRadius + FEATHER_PX + 1;
                    ClearDIBRect(oldLocalX - oldR, oldLocalY - oldR, oldLocalX + oldR, oldLocalY + oldR, dimPixel);
                }

                /* Draw new circle at current cursor position */
                DrawSpotlightCircle(cur, scaledRadius);
                PresentSpotlight();

                lastPos = cur;
                lastRadius = scaledRadius;
            }
        }

        Sleep(LOOP_INTERVAL_MS);
    }

    /* 8. Cleanup */
    if (g_dimWnd) {
        DestroyWindow(g_dimWnd);
        g_dimWnd = NULL;
    }
    if (g_hBmp) {
        DeleteObject(g_hBmp);
        g_hBmp = NULL;
    }
    if (g_memDC) {
        DeleteDC(g_memDC);
        g_memDC = NULL;
    }
    if (hEvent) {
        CloseHandle(hEvent);
    }

    return 0;
}
