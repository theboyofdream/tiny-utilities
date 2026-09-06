#ifndef TINY_FONT_H
#define TINY_FONT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Centralized Font Size Constants (in logical pixels at standard 96 DPI) */
#define TINY_FONT_SIZE_MIN     8
#define TINY_FONT_SIZE_XS      9
#define TINY_FONT_SIZE_SMALL   10
#define TINY_FONT_SIZE_DEFAULT 11
#define TINY_FONT_SIZE_NORMAL  12
#define TINY_FONT_SIZE_MEDIUM  13
#define TINY_FONT_SIZE_LARGE   15
#define TINY_FONT_SIZE_XLARGE  18
#define TINY_FONT_SIZE_MAX     32

/* Centralized Font Weight Constants */
#define TINY_FONT_WEIGHT_LIGHT    FW_LIGHT    /* 300 */
#define TINY_FONT_WEIGHT_NORMAL   FW_NORMAL   /* 400 */
#define TINY_FONT_WEIGHT_MEDIUM   FW_MEDIUM   /* 500 */
#define TINY_FONT_WEIGHT_SEMIBOLD FW_SEMIBOLD /* 600 */
#define TINY_FONT_WEIGHT_BOLD     FW_BOLD     /* 700 */

/* Centralized UI Color Constants (COLORREF) */
#define TINY_COLOR_BG_DARK       RGB(24, 24, 24)
#define TINY_COLOR_TEXT_BRIGHT   RGB(238, 238, 238)
#define TINY_COLOR_TEXT_MUTED    RGB(119, 119, 119)
#define TINY_COLOR_ACCENT        RGB(0, 122, 204)
#define TINY_COLOR_BORDER        RGB(50, 50, 50)
#define TINY_COLOR_SELECTION     RGB(45, 90, 140)

/* Font Family Defaults */
#define TINY_FONT_FACE_JETBRAINS L"JetBrains Mono"
#define TINY_FONT_FACE_CONSOLAS  L"Consolas"
#define TINY_FONT_FACE_SEGOE     L"Segoe UI"

typedef struct {
    const wchar_t *target;
    bool found;
} TinyFontEnumCtx;

static inline int CALLBACK TinyFont_EnumProc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp) {
    (void)tm; (void)type;
    TinyFontEnumCtx *ctx = (TinyFontEnumCtx *)lp;
    if (ctx && ctx->target && _wcsicmp(lf->lfFaceName, ctx->target) == 0) {
        ctx->found = true;
        return 0;
    }
    return 1;
}

static inline bool TinyFont_IsAvailable(const wchar_t *family) {
    if (!family || !*family) return false;
    HDC hdc = GetDC(NULL);
    if (!hdc) return false;

    TinyFontEnumCtx ctx = { family, false };
    LOGFONTW lf = { 0 };
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, family, _TRUNCATE);
    EnumFontFamiliesExW(hdc, &lf, TinyFont_EnumProc, (LPARAM)&ctx, 0);
    ReleaseDC(NULL, hdc);
    return ctx.found;
}

static inline const wchar_t *TinyFont_GetBestMonospaceFace(void) {
    static wchar_t cached[LF_FACESIZE] = { 0 };
    if (cached[0] != L'\0') {
        return cached;
    }
    const wchar_t *candidates[] = {
        TINY_FONT_FACE_JETBRAINS,
        TINY_FONT_FACE_CONSOLAS,
        L"Fira Code",
        L"Cascadia Code",
        L"Cascadia Mono",
        L"Courier New"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (TinyFont_IsAvailable(candidates[i])) {
            wcsncpy_s(cached, LF_FACESIZE, candidates[i], _TRUNCATE);
            return cached;
        }
    }
    wcsncpy_s(cached, LF_FACESIZE, TINY_FONT_FACE_CONSOLAS, _TRUNCATE);
    return cached;
}

static inline const wchar_t *TinyFont_GetBestUIFace(void) {
    static wchar_t cached[LF_FACESIZE] = { 0 };
    if (cached[0] != L'\0') {
        return cached;
    }
    const wchar_t *candidates[] = {
        TINY_FONT_FACE_SEGOE,
        L"Tahoma",
        L"Arial"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (TinyFont_IsAvailable(candidates[i])) {
            wcsncpy_s(cached, LF_FACESIZE, candidates[i], _TRUNCATE);
            return cached;
        }
    }
    wcsncpy_s(cached, LF_FACESIZE, TINY_FONT_FACE_SEGOE, _TRUNCATE);
    return cached;
}

/*
 * TinyFont_GetSystemDefaultSize:
 * Returns the baseline unscaled UI font size at 96 DPI (11px, equivalent to standard Windows UI).
 */
static inline int TinyFont_GetSystemDefaultSize(void) {
    NONCLIENTMETRICSW ncm = { 0 };
    ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0)) {
        int h = abs((int)ncm.lfMessageFont.lfHeight);
        HDC hdc = GetDC(NULL);
        int sysDpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) ReleaseDC(NULL, hdc);
        if (sysDpi <= 0) sysDpi = 96;
        if (h > 0) {
            int baseSize = MulDiv(h, 96, sysDpi);
            if (baseSize >= TINY_FONT_SIZE_MIN && baseSize <= 14) {
                return baseSize;
            }
        }
    }
    return TINY_FONT_SIZE_DEFAULT;
}

/*
 * TinyFont_StepSize:
 * Clamps font size within [TINY_FONT_SIZE_MIN, TINY_FONT_SIZE_MAX].
 */
static inline int TinyFont_StepSize(int currentSize, int delta) {
    int next = currentSize + delta;
    if (next < TINY_FONT_SIZE_MIN) next = TINY_FONT_SIZE_MIN;
    if (next > TINY_FONT_SIZE_MAX) next = TINY_FONT_SIZE_MAX;
    return next;
}

/*
 * TinyFont_HandleZoomKey:
 * Processes Ctrl + / Ctrl - / Ctrl 0 keypresses to increase, decrease, or reset runtime font size.
 * Returns true if key was handled and font size was updated.
 */
static inline bool TinyFont_HandleZoomKey(WPARAM wParam, int *pFontSize) {
    if (!pFontSize) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;

    if (wParam == VK_OEM_PLUS || wParam == VK_ADD || wParam == '=') {
        *pFontSize = TinyFont_StepSize(*pFontSize, 1);
        return true;
    } else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT || wParam == '-') {
        *pFontSize = TinyFont_StepSize(*pFontSize, -1);
        return true;
    } else if (wParam == '0' || wParam == VK_NUMPAD0) {
        *pFontSize = TinyFont_GetSystemDefaultSize();
        return true;
    }
    return false;
}

/*
 * TinyFont_ScaleSize:
 * Computes the pixel height for a given font size and monitor DPI
 * so that the physical size on screen (in inches / millimeters) remains identical
 * across all displays (laptops and external monitors).
 */
static inline int TinyFont_ScaleSize(int fontSize, UINT dpi) {
    if (fontSize <= 0) fontSize = TinyFont_GetSystemDefaultSize();
    if (dpi <= 0) dpi = 96;
    return MulDiv(fontSize, (int)dpi, 96);
}

static inline HFONT TinyFont_Create(const wchar_t *faceName, int fontSize, int weight, bool italic, UINT dpi) {
    int scaledSize = TinyFont_ScaleSize(fontSize, dpi);
    int height = -scaledSize;

    const wchar_t *face = faceName;
    if (!face || !*face) {
        face = TinyFont_GetBestUIFace();
    }

    return CreateFontW(
        height, 0, 0, 0,
        weight, italic ? TRUE : FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        face
    );
}

static inline HFONT TinyFont_CreateMonospace(int fontSize, int weight, UINT dpi) {
    const wchar_t *monoFace = TinyFont_GetBestMonospaceFace();
    int scaledSize = TinyFont_ScaleSize(fontSize, dpi);
    int height = -scaledSize;

    return CreateFontW(
        height, 0, 0, 0,
        weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        monoFace
    );
}

static inline HFONT TinyFont_CreateUI(int fontSize, int weight, UINT dpi) {
    const wchar_t *uiFace = TinyFont_GetBestUIFace();
    int scaledSize = TinyFont_ScaleSize(fontSize, dpi);
    int height = -scaledSize;

    return CreateFontW(
        height, 0, 0, 0,
        weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_SWISS,
        uiFace
    );
}

static inline bool TinyFont_MeasureText(HDC hdc, HFONT hFont, const wchar_t *text, SIZE *outSize) {
    if (!hdc || !text || !outSize) return false;
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    BOOL res = GetTextExtentPoint32W(hdc, text, (int)wcslen(text), outSize);
    if (hOld) SelectObject(hdc, hOld);
    return res != FALSE;
}

static inline int TinyFont_GetFontHeight(HDC hdc, HFONT hFont) {
    if (!hdc) return 0;
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    TEXTMETRICW tm = { 0 };
    GetTextMetricsW(hdc, &tm);
    if (hOld) SelectObject(hdc, hOld);
    return tm.tmHeight;
}

#ifdef __cplusplus
}
#endif

#endif /* TINY_FONT_H */
