/*
 * color-thief-algorithm.h — Color Thief (Median Cut Quantization / MMCQ) accent color algorithm.
 *
 * Provides dominant wallpaper accent color detection for Win32 apps using GDI sampling.
 * Strict GDI only (user32.lib + gdi32.lib compatible). No external dependencies.
 */

#ifndef TINY_COLOR_THIEF_ALGORITHM_H
#define TINY_COLOR_THIEF_ALGORITHM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>

#define CT_SIGBITS 5
#define CT_RSHIFT (8 - CT_SIGBITS)
#define CT_HIST_SIZE (1 << (3 * CT_SIGBITS)) /* 32768 */
#define CT_MAX_BOXES 8
#define CT_GRID_SIZE 32 /* 32x32 sample grid */

typedef struct {
    int r1, r2;
    int g1, g2;
    int b1, b2;
    int count;
} ColorThiefVBox;

static int ColorThiefCountVBox(const unsigned short *hist, ColorThiefVBox *vbox)
{
    int count = 0;
    for (int r = vbox->r1; r <= vbox->r2; r++) {
        for (int g = vbox->g1; g <= vbox->g2; g++) {
            for (int b = vbox->b1; b <= vbox->b2; b++) {
                int idx = (r << 10) | (g << 5) | b;
                count += hist[idx];
            }
        }
    }
    return count;
}

static void ColorThiefSplitVBox(const unsigned short *hist, const ColorThiefVBox *vbox,
                                ColorThiefVBox *v1, ColorThiefVBox *v2)
{
    int dr = vbox->r2 - vbox->r1;
    int dg = vbox->g2 - vbox->g1;
    int db = vbox->b2 - vbox->b1;

    int maxDim = 0; /* 0: R, 1: G, 2: B */
    if (dg >= dr && dg >= db)
        maxDim = 1;
    else if (db >= dr && db >= dg)
        maxDim = 2;

    int total = vbox->count;
    int half = total / 2;
    int sum = 0;
    int splitVal = 0;

    *v1 = *vbox;
    *v2 = *vbox;

    if (maxDim == 0) {
        splitVal = vbox->r1;
        for (int r = vbox->r1; r <= vbox->r2; r++) {
            int rowSum = 0;
            for (int g = vbox->g1; g <= vbox->g2; g++) {
                for (int b = vbox->b1; b <= vbox->b2; b++) {
                    rowSum += hist[(r << 10) | (g << 5) | b];
                }
            }
            sum += rowSum;
            if (sum >= half) {
                splitVal = r;
                break;
            }
        }
        if (splitVal == vbox->r2 && splitVal > vbox->r1)
            splitVal--;
        v1->r2 = splitVal;
        v2->r1 = splitVal + 1;
    } else if (maxDim == 1) {
        splitVal = vbox->g1;
        for (int g = vbox->g1; g <= vbox->g2; g++) {
            int colSum = 0;
            for (int r = vbox->r1; r <= vbox->r2; r++) {
                for (int b = vbox->b1; b <= vbox->b2; b++) {
                    colSum += hist[(r << 10) | (g << 5) | b];
                }
            }
            sum += colSum;
            if (sum >= half) {
                splitVal = g;
                break;
            }
        }
        if (splitVal == vbox->g2 && splitVal > vbox->g1)
            splitVal--;
        v1->g2 = splitVal;
        v2->g1 = splitVal + 1;
    } else {
        splitVal = vbox->b1;
        for (int b = vbox->b1; b <= vbox->b2; b++) {
            int depSum = 0;
            for (int r = vbox->r1; r <= vbox->r2; r++) {
                for (int g = vbox->g1; g <= vbox->g2; g++) {
                    depSum += hist[(r << 10) | (g << 5) | b];
                }
            }
            sum += depSum;
            if (sum >= half) {
                splitVal = b;
                break;
            }
        }
        if (splitVal == vbox->b2 && splitVal > vbox->b1)
            splitVal--;
        v1->b2 = splitVal;
        v2->r1 = splitVal; /* v2->b1 = splitVal + 1 */
        v2->b1 = splitVal + 1;
    }

    v1->count = ColorThiefCountVBox(hist, v1);
    v2->count = ColorThiefCountVBox(hist, v2);
}

static void ColorThiefGetAvgRGB(const unsigned short *hist, const ColorThiefVBox *vbox,
                                int *outR, int *outG, int *outB)
{
    int ntot = 0;
    long rsum = 0, gsum = 0, bsum = 0;
    for (int r = vbox->r1; r <= vbox->r2; r++) {
        for (int g = vbox->g1; g <= vbox->g2; g++) {
            for (int b = vbox->b1; b <= vbox->b2; b++) {
                int count = hist[(r << 10) | (g << 5) | b];
                if (count > 0) {
                    ntot += count;
                    rsum += (long)count * (r * 8 + 4);
                    gsum += (long)count * (g * 8 + 4);
                    bsum += (long)count * (b * 8 + 4);
                }
            }
        }
    }
    if (ntot > 0) {
        *outR = (int)(rsum / ntot);
        *outG = (int)(gsum / ntot);
        *outB = (int)(bsum / ntot);
    } else {
        *outR = (vbox->r1 + vbox->r2) * 4;
        *outG = (vbox->g1 + vbox->g2) * 4;
        *outB = (vbox->b1 + vbox->b2) * 4;
    }
}

/* Calculate color saturation score (0..255) to prefer vibrant accent colors */
static int ColorThiefGetSaturationScore(int r, int g, int b)
{
    int maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (maxC == 0)
        return 0;
    return (maxC - minC) * 255 / maxC;
}

typedef HRESULT(WINAPI *DwmGetColorizationColorFn)(DWORD *, BOOL *);

static COLORREF GetWallpaperDominantColorThief(void)
{
    const COLORREF fallback = RGB(0, 120, 215);

    /* 1. Primary choice: System DWM Accent Color (exact wallpaper accent color calculated by Windows) */
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm != NULL) {
        DwmGetColorizationColorFn fn =
            (DwmGetColorizationColorFn)GetProcAddress(dwm, "DwmGetColorizationColor");
        if (fn != NULL) {
            DWORD colorization = 0;
            BOOL opaque = FALSE;
            if (SUCCEEDED(fn(&colorization, &opaque)) && colorization != 0) {
                FreeLibrary(dwm);
                BYTE r = (BYTE)((colorization >> 16) & 0xFF);
                BYTE g = (BYTE)((colorization >> 8) & 0xFF);
                BYTE b = (BYTE)(colorization & 0xFF);
                return RGB(r, g, b);
            }
        }
        FreeLibrary(dwm);
    }

    /* 2. Fallback: Color Thief MMCQ quantization over screen sample */
    HDC screenDC = GetDC(NULL);
    if (screenDC == NULL)
        return fallback;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        ReleaseDC(NULL, screenDC);
        return fallback;
    }

    HDC memDC = CreateCompatibleDC(screenDC);
    if (memDC == NULL) {
        ReleaseDC(NULL, screenDC);
        return fallback;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = CT_GRID_SIZE;
    bi.bmiHeader.biHeight = -CT_GRID_SIZE; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD *pixels = NULL;
    HBITMAP dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (dib == NULL || pixels == NULL) {
        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        return fallback;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    SetStretchBltMode(memDC, HALFTONE);
    SetBrushOrgEx(memDC, 0, 0, NULL);
    StretchBlt(memDC, 0, 0, CT_GRID_SIZE, CT_GRID_SIZE,
               screenDC, vx, vy, vw, vh, SRCCOPY);

    unsigned short *hist = (unsigned short *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(unsigned short) * CT_HIST_SIZE);
    if (hist == NULL) {
        SelectObject(memDC, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        return fallback;
    }

    int minR = 31, maxR = 0;
    int minG = 31, maxG = 0;
    int minB = 31, maxB = 0;
    int totalSamples = 0;

    int totalPixels = CT_GRID_SIZE * CT_GRID_SIZE;
    for (int i = 0; i < totalPixels; i++) {
        DWORD c = pixels[i];
        int r = (int)(((c >> 16) & 0xFF) >> CT_RSHIFT);
        int g = (int)(((c >> 8) & 0xFF) >> CT_RSHIFT);
        int b = (int)((c & 0xFF) >> CT_RSHIFT);

        int idx = (r << 10) | (g << 5) | b;
        hist[idx]++;
        totalSamples++;

        if (r < minR) minR = r;
        if (r > maxR) maxR = r;
        if (g < minG) minG = g;
        if (g > maxG) maxG = g;
        if (b < minB) minB = b;
        if (b > maxB) maxB = b;
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    if (totalSamples == 0 || minR > maxR) {
        HeapFree(GetProcessHeap(), 0, hist);
        return fallback;
    }

    ColorThiefVBox boxes[CT_MAX_BOXES];
    int numBoxes = 1;
    boxes[0].r1 = minR; boxes[0].r2 = maxR;
    boxes[0].g1 = minG; boxes[0].g2 = maxG;
    boxes[0].b1 = minB; boxes[0].b2 = maxB;
    boxes[0].count = totalSamples;

    /* Quantize via Median Cut */
    while (numBoxes < CT_MAX_BOXES) {
        int maxBoxIdx = -1;
        int maxCount = -1;
        for (int i = 0; i < numBoxes; i++) {
            if (boxes[i].count > maxCount &&
                (boxes[i].r2 > boxes[i].r1 || boxes[i].g2 > boxes[i].g1 || boxes[i].b2 > boxes[i].b1)) {
                maxCount = boxes[i].count;
                maxBoxIdx = i;
            }
        }
        if (maxBoxIdx == -1 || maxCount <= 1)
            break;

        ColorThiefVBox v1, v2;
        ColorThiefSplitVBox(hist, &boxes[maxBoxIdx], &v1, &v2);
        boxes[maxBoxIdx] = v1;
        boxes[numBoxes++] = v2;
    }

    /* Select best accent color from generated MMCQ palette boxes */
    int bestIdx = 0;
    int bestScore = -1;

    for (int i = 0; i < numBoxes; i++) {
        if (boxes[i].count <= 0)
            continue;
        int r, g, b;
        ColorThiefGetAvgRGB(hist, &boxes[i], &r, &g, &b);
        int sat = ColorThiefGetSaturationScore(r, g, b);

        /* Balance pixel frequency (count) and color saturation for accent detection */
        int score = boxes[i].count + (sat * 2);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }

    int finalR, finalG, finalB;
    ColorThiefGetAvgRGB(hist, &boxes[bestIdx], &finalR, &finalG, &finalB);
    HeapFree(GetProcessHeap(), 0, hist);
    return RGB(finalR, finalG, finalB);
}

#endif /* TINY_COLOR_THIEF_ALGORITHM_H */
