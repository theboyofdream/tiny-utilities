/*
 * Find My Mouse (CursorFocus) — toggle a PowerToys-style cursor spotlight overlay.
 * Source: find-my-mouse.c (renamed from cursor.c)
 *
 * Spec: PRD.md (sections 5, 7, 8 + "no border, soft spotlight" requirement).
 * Agent guide: AGENTS.md / CHECKLIST.md.
 *
 * Visual: the entire virtual desktop is dimmed by a translucent black overlay;
 * a flat, border-less, translucent white disc follows the cursor
 * ("dull black overlay / dull white spotlight, no border").
 *
 * Implementation (two layers, keeps memory well under PRD 10 MB):
 *   - Dim layer: one full-screen window, uniform alpha via
 *     SetLayeredWindowAttributes(...LWA_ALPHA). No per-pixel buffer.
 *   - Glow layer: one small per-pixel-alpha window (2*R + pad). Its DIB is
 *     re-rendered only during the focus-in/focus-out animation; on cursor move
 *     we only SetWindowPos it (no redraw at rest). Both layers are click-through.
 *
 * Behavior:
 *   First launch:  create mutex, create dim + glow overlay, run loop.
 *   Second launch: mutex already exists -> signal TOGGLE event -> exit immediately.
 *   Toggle (running): deactivate -> destroy overlays -> exit.
 *   On activation a focus-in pulse plays: a large transparent disc centered on
 *   the cursor shrinks to spot size while opacity ramps up to normal, then it
 *   settles into the steady disc that follows the cursor. Dismissal (any click/
 *   key, after a short activation grace, or a toggle-off) plays the pulse in
 *   reverse before the process exits.
 *   The glow window is positioned only by MoveGlow; PresentGlow never relocates
 *   it (UpdateLayeredWindow would otherwise yank it to screen 0,0).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>
#include "common/tiny_gui.h"
#include "common/tiny_dpi.h"

#define APPMUTEX_NAME L"Global\\TinyCursorFocusMutex"
#define APPEVENT_NAME L"Global\\TinyCursorFocusEvent"

#define LOOP_INTERVAL_MS 16 /* <= 60 FPS (PRD 8.3 / 10) */
#define ACTIVATE_GRACE_MS 250 /* ignore hotkey release right after activation */

#define BASE_SPOT_RADIUS 75          /* steady-state disc base radius at 96 DPI */
#define BASE_PULSE_RADIUS_MAX 130    /* focus-in start / focus-out end base radius at 96 DPI */
#define SPOT_ALPHA 160               /* steady-state premultiplied white inside disc */
#define GLOW_PAD 12                  /* transparent margin so the disc is never clipped */
#define MAX_GLOW_SIZE 512            /* fixed DIB allocation supporting up to 300% DPI */
#define FOCUS_IN_MS 180              /* shrink + fade-in duration on activation */
#define FOCUS_OUT_MS 210             /* grow + fade-out duration on dismissal */
#define PULSE_IN_ALPHA 40            /* initial opacity of the large focus-in disc */
#define DIM_ALPHA 160                /* uniform alpha: screen dimmed to ~37% */

static HWND g_dim = NULL;  /* full-screen uniform dim window */
static HWND g_glow = NULL; /* small per-pixel glow window */
static HDC g_glowDC = NULL;
static HBITMAP g_glowBmp = NULL;
static DWORD *g_glowBits = NULL;
static int g_vx = 0, g_vy = 0; /* virtual screen origin (screen coords) */
static int g_glowX = 0, g_glowY = 0; /* glow window top-left (screen coords) */
static bool g_running = true;
static bool g_pulsed = false;  /* no focus-in/out animation currently playing */

static UINT g_curDpi = 96;
static int g_spotRadius = BASE_SPOT_RADIUS;
static int g_pulseRadiusMax = BASE_PULSE_RADIUS_MAX;

/* ------------------------------------------------------------------ */
/* Overlay windows (PRD 8)                                            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK DimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* class brush is black; let the default eraser fill the surface once */
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK GlowWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void DrawGlowDisc(int radius, int alpha)
{
    int c = MAX_GLOW_SIZE / 2;
    long R2 = (long)radius * (long)radius;
    /* flat, border-less disc: constant translucency inside the radius */
    DWORD white = ((DWORD)alpha << 24) | ((DWORD)alpha << 16) |
                  ((DWORD)alpha << 8) | (DWORD)alpha;
    for (int y = 0; y < MAX_GLOW_SIZE; y++) {
        DWORD *row = g_glowBits + (size_t)y * MAX_GLOW_SIZE;
        int dy = y - c;
        for (int x = 0; x < MAX_GLOW_SIZE; x++) {
            int dx = x - c;
            if ((long)dx * dx + (long)dy * dy <= R2)
                row[x] = white;
            else
                row[x] = 0; /* fully transparent outside the disc */
        }
    }
}

static void PresentGlow(void)
{
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    POINT dst = {g_glowX, g_glowY}; /* keep window where MoveGlow put it */
    POINT src = {0, 0};
    SIZE sz = {MAX_GLOW_SIZE, MAX_GLOW_SIZE};
    UpdateLayeredWindow(g_glow, NULL, &dst, &sz, g_glowDC, &src, 0, &bf,
                        ULW_ALPHA);
}

static void MoveGlow(const POINT *p)
{
    /* position in virtual-screen coordinates (SetWindowPos accepts negatives) */
    g_glowX = p->x - MAX_GLOW_SIZE / 2;
    g_glowY = p->y - MAX_GLOW_SIZE / 2;
    SetWindowPos(g_glow, HWND_TOPMOST,
                 g_glowX, g_glowY,
                 MAX_GLOW_SIZE, MAX_GLOW_SIZE,
                 SWP_NOACTIVATE);
}

static bool PrepareSpotlight(void)
{
    int vw = 0, vh = 0;
    TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &vw, &vh);
    if (vw <= 0 || vh <= 0)
        return false;

    HINSTANCE inst = GetModuleHandleW(NULL);

    /* --- dim layer: uniform alpha over the whole virtual screen --- */
    {
        static const wchar_t *cls = L"TinyCursorFocusDim";
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.lpfnWndProc = DimWndProc;
            wc.hInstance = inst;
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = cls;
            if (!RegisterClassW(&wc))
                return false;
            reg = true;
        }
        g_dim = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            cls, L"", WS_POPUP,
            g_vx, g_vy, vw, vh,
            NULL, NULL, inst, NULL);
        if (g_dim == NULL)
            return false;
        SetLayeredWindowAttributes(g_dim, 0, DIM_ALPHA, LWA_ALPHA);
        ShowWindow(g_dim, SW_SHOWNOACTIVATE);
        SetWindowPos(g_dim, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    /* --- glow layer: tiny per-pixel flat disc, rendered once --- */
    {
        static const wchar_t *cls = L"TinyCursorFocusGlow";
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.lpfnWndProc = GlowWndProc;
            wc.hInstance = inst;
            wc.lpszClassName = cls;
            if (!RegisterClassW(&wc))
                return false;
            reg = true;
        }

        POINT cur;
        if (!GetCursorPos(&cur))
            return false;
        g_curDpi = TinyDPI_GetDpiForPoint(cur);
        g_spotRadius = MulDiv(BASE_SPOT_RADIUS, (int)g_curDpi, 96);
        g_pulseRadiusMax = MulDiv(BASE_PULSE_RADIUS_MAX, (int)g_curDpi, 96);

        g_glowX = cur.x - MAX_GLOW_SIZE / 2;
        g_glowY = cur.y - MAX_GLOW_SIZE / 2;

        g_glow = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                WS_EX_TOOLWINDOW,
            cls, L"", WS_POPUP,
            g_glowX, g_glowY,
            MAX_GLOW_SIZE, MAX_GLOW_SIZE,
            NULL, NULL, inst, NULL);
        if (g_glow == NULL)
            return false;

        g_glowDC = CreateCompatibleDC(NULL);
        if (g_glowDC == NULL)
            return false;

        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = MAX_GLOW_SIZE;
        bi.bmiHeader.biHeight = -MAX_GLOW_SIZE; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_glowBmp = CreateDIBSection(g_glowDC, &bi, DIB_RGB_COLORS,
                                     (void **)&g_glowBits, NULL, 0);
        if (g_glowBmp == NULL)
            return false;
        SelectObject(g_glowDC, g_glowBmp);

        g_pulsed = false;
        DrawGlowDisc(g_pulseRadiusMax, PULSE_IN_ALPHA); /* big faint start frame */
        PresentGlow();
        ShowWindow(g_glow, SW_SHOWNOACTIVATE);
        SetWindowPos(g_glow, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    return true;
}

static void DestroySpotlight(void)
{
    if (g_glowBmp != NULL)
        DeleteObject(g_glowBmp);
    if (g_glowDC != NULL)
        DeleteDC(g_glowDC);
    if (g_glow != NULL)
        DestroyWindow(g_glow);
    if (g_dim != NULL)
        DestroyWindow(g_dim);
    g_glowBmp = NULL;
    g_glowDC = NULL;
    g_glow = NULL;
    g_dim = NULL;
    g_glowBits = NULL;
}

/* ------------------------------------------------------------------ */
/* Tracking loop (PRD 7.4 / 8.3): follow cursor, only move on change. */
/* ------------------------------------------------------------------ */

static bool IsAnyMouseOrKeyDown(void)
{
    for (int vk = 0x01; vk <= 0xFE; vk++) {
        if ((GetAsyncKeyState(vk) & 0x8000) != 0)
            return true;
    }
    return false;
}

/* Focus-in frame: big faint disc (g_pulseRadiusMax, PULSE_IN_ALPHA) shrinks to
 * the steady disc (g_spotRadius, SPOT_ALPHA), radius via ease-in quadratic so it
 * settles into the spot. Re-renders the glow DIB; once elapsed it draws the
 * steady disc and marks the animation done. */
static void RenderFocusInFrame(ULONGLONG dt)
{
    if (dt >= FOCUS_IN_MS) {
        DrawGlowDisc(g_spotRadius, SPOT_ALPHA);
        PresentGlow();
        g_pulsed = true;
        return;
    }
    double f = (double)dt / (double)FOCUS_IN_MS;
    double k = f * f; /* ease-in (quadratic) */
    int radius = g_pulseRadiusMax +
                 (int)((double)(g_spotRadius - g_pulseRadiusMax) * k);
    int alpha = PULSE_IN_ALPHA +
                (int)((double)(SPOT_ALPHA - PULSE_IN_ALPHA) * k);
    DrawGlowDisc(radius, alpha);
    PresentGlow();
}

/* Focus-out frame (dismissal): reverse of focus-in — the steady disc grows
 * back out to g_pulseRadiusMax while fading to fully transparent. Returns true
 * when the fade-out is complete (loop should exit). */
static bool RenderFocusOutFrame(ULONGLONG dt)
{
    if (dt >= FOCUS_OUT_MS) {
        DrawGlowDisc(g_pulseRadiusMax, 0);
        PresentGlow();
        return true;
    }
    double f = (double)dt / (double)FOCUS_OUT_MS;
    double d = 1.0 - f;
    double k = 1.0 - d * d; /* ease-out (quadratic) */
    int radius = g_spotRadius +
                 (int)((double)(g_pulseRadiusMax - g_spotRadius) * k);
    int alpha = (int)((double)SPOT_ALPHA * (1.0 - k));
    DrawGlowDisc(radius, alpha);
    PresentGlow();
    return false;
}

static void RunInstance(HANDLE ev)
{
    POINT last = {-1, -1};
    ULONGLONG start = GetTickCount64();
    ULONGLONG outStart = 0;
    bool dismissing = false;

    while (g_running) {
        DWORD w = WaitForSingleObject(ev, LOOP_INTERVAL_MS);

        MSG msg;
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

        /* toggle-off, or any mouse click / key press after the activation grace,
         * starts the focus-out fade; the process exits once it finishes */
        if (!dismissing &&
            (w == WAIT_OBJECT_0 ||
             (GetTickCount64() - start >= ACTIVATE_GRACE_MS &&
              IsAnyMouseOrKeyDown()))) {
            dismissing = true;
            outStart = GetTickCount64();
        }

        if (dismissing) {
            if (RenderFocusOutFrame(GetTickCount64() - outStart)) {
                g_running = false;
                break;
            }
        } else if (!g_pulsed) {
            RenderFocusInFrame(GetTickCount64() - start);
        }

        POINT now;
        GetCursorPos(&now);
        UINT dpi = TinyDPI_GetDpiForPoint(now);
        if (dpi != g_curDpi) {
            g_curDpi = dpi;
            g_spotRadius = MulDiv(BASE_SPOT_RADIUS, (int)dpi, 96);
            g_pulseRadiusMax = MulDiv(BASE_PULSE_RADIUS_MAX, (int)dpi, 96);
            if (g_pulsed && !dismissing) {
                DrawGlowDisc(g_spotRadius, SPOT_ALPHA);
                PresentGlow();
            }
        }

        if (!dismissing && (now.x != last.x || now.y != last.y)) {
            last = now;
            MoveGlow(&now); /* no redraw — glow bitmap is static */
        }
    }
}

static void RunFirstInstance(HANDLE mutex)
{
    HANDLE ev = NULL;
    int rc = 0;

    ev = CreateEventW(NULL, FALSE, FALSE, APPEVENT_NAME);
    if (ev == NULL || !PrepareSpotlight()) {
        rc = 1;
        goto done;
    }
    RunInstance(ev);

done:
    DestroySpotlight();
    if (ev != NULL)
        CloseHandle(ev);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    exit(rc);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    TinyDPI_EnablePerMonitorAwareness(); /* 1:1 physical-pixel coordinates for the overlay */

    HANDLE mutex = CreateMutexW(NULL, FALSE, APPMUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* second launch: signal TOGGLE and exit (PRD 7.3 / 5.2) */
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, APPEVENT_NAME);
        if (ev != NULL) {
            SetEvent(ev);
            CloseHandle(ev);
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }

    RunFirstInstance(mutex);
    return 0; /* unreachable, RunFirstInstance exits */
}
