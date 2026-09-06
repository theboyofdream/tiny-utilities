/*
 * Capture — Screen capture & annotation utility.
 * Source: capture.c
 * Exe: capture.exe
 *
 * Modes: --fullscreen, --window, --snip
 * Options: --draw, --toolbar/--draw-toolbar (defaults to false), --clipboard, --save <path_or_dir>
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "common/tiny_gui.h"
#include "common/tiny_dpi.h"
#include "common/font.h"
#include "common/clipboard.h"

#define APPMUTEX_NAME L"Global\\TinyCaptureMutex"
#define APPEVENT_NAME L"Global\\TinyCaptureEvent"

#define MAX_ARGV 64
#define MAX_POINTS_PER_STROKE 2048
#define MAX_STROKES 256

#define TOOLBAR_H 48
#define SNIP_DIM_ALPHA 125 /* Rich ~49% dark translucent overlay */

typedef enum {
    MODE_FULLSCREEN,
    MODE_WINDOW,
    MODE_SNIP
} CaptureMode;

typedef enum {
    FMT_PNG,
    FMT_BMP
} ImageFormat;

typedef struct {
    CaptureMode mode;
    bool draw;
    bool toolbar;
    bool clipboard;
    bool save;
    ImageFormat format;
    int fontSize;
    wchar_t savePath[MAX_PATH];
} Config;

typedef struct {
    COLORREF color;
    int width;
    POINT points[MAX_POINTS_PER_STROKE];
    int pointCount;
} Stroke;

static Config g_cfg;
static HANDLE g_event = NULL;
static void ForceForeground(HWND hwnd);
static HBITMAP CaptureFullscreen(int *outW, int *outH);

static HBITMAP g_hbmCaptured = NULL;
static int g_imgW = 0;
static int g_imgH = 0;

/* Snip state */
static HBITMAP g_hbmDesktop = NULL;
static int g_vx = 0, g_vy = 0, g_vw = 0, g_vh = 0;
static bool g_snipDragging = false;
static POINT g_snipStart = {0, 0};
static POINT g_snipCurr = {0, 0};
static bool g_snipSuccess = false;
static RECT g_snipRect = {0, 0, 0, 0};

/* Annotation state */
static Stroke g_strokes[MAX_STROKES];
static int g_strokeCount = 0;
static Stroke g_currentStroke;
static bool g_isDrawing = false;
static bool g_annotationConfirmed = false;
static bool g_isInteractiveMode = false;
static bool g_userRequestedSaveDialog = false;
static int g_capX = 0, g_capY = 0;
static bool g_showToolbar = false;
static int g_tbX = 0, g_tbY = 0, g_tbW = 800, g_tbH = 58;

/* Mode Selector state */
static int g_selHoveredIdx = -1;
static int g_selFocusIdx = 0;
static bool g_selectorConfirmed = false;
static CaptureMode g_selectedMode = MODE_SNIP;
static bool g_mouseTracking = false;

static COLORREF g_colors[] = {
    RGB(255, 59, 48),   /* Red */
    RGB(255, 204, 0),   /* Yellow */
    RGB(0, 122, 255),   /* Blue */
    RGB(52, 199, 89),   /* Green */
    RGB(20, 20, 20),    /* Black */
    RGB(255, 255, 255)  /* White */
};
static int g_selectedColorIdx = 0;

static int g_penSizes[] = { 2, 4, 8, 12 };
static int g_selectedSizeIdx = 1; /* 4px default */

enum {
    TB_HOVER_NONE = -1,
    TB_HOVER_COLOR_0 = 0,
    TB_HOVER_COLOR_1 = 1,
    TB_HOVER_COLOR_2 = 2,
    TB_HOVER_COLOR_3 = 3,
    TB_HOVER_COLOR_4 = 4,
    TB_HOVER_COLOR_5 = 5,
    TB_HOVER_SIZE_0 = 10,
    TB_HOVER_SIZE_1 = 11,
    TB_HOVER_SIZE_2 = 12,
    TB_HOVER_SIZE_3 = 13,
    TB_HOVER_UNDO = 20,
    TB_HOVER_CLEAR = 21,
    TB_HOVER_COPY = 22,
    TB_HOVER_SAVE = 23,
    TB_HOVER_DONE = 24,
    TB_HOVER_CANCEL = 25
};
static int g_tbHoverItem = TB_HOVER_NONE;

static int GetToolbarHoverItem(int x, int y)
{
    if (!g_showToolbar) return TB_HOVER_NONE;
    int tbX = g_tbX, tbY = g_tbY, tbW = g_tbW, tbH = g_tbH;
    if (x < tbX || x > tbX + tbW || y < tbY || y > tbY + tbH) return TB_HOVER_NONE;

    POINT ptCursor = { x + g_vx, y + g_vy };
    UINT dpi = TinyDPI_GetDpiForPoint(ptCursor);

    /* Colors */
    for (int i = 0; i < 6; i++) {
        int cx = tbX + MulDiv(90 + i * 20, (int)dpi, 96);
        int cy = tbY + MulDiv(13, (int)dpi, 96);
        int dotR = MulDiv(7, (int)dpi, 96);
        if (abs(x - cx) <= dotR + 2 && abs(y - cy) <= dotR + MulDiv(7, (int)dpi, 96)) {
            return TB_HOVER_COLOR_0 + i;
        }
    }

    /* Sizes (2p, 4p, 8p, 12p) */
    for (int i = 0; i < 4; i++) {
        int left = tbX + MulDiv(224 + i * 32, (int)dpi, 96);
        int right = left + MulDiv(26, (int)dpi, 96);
        if (x >= left && x <= right) {
            return TB_HOVER_SIZE_0 + i;
        }
    }

    /* Undo */
    if (x >= (tbX + MulDiv(368, (int)dpi, 96)) && x <= (tbX + MulDiv(408, (int)dpi, 96))) return TB_HOVER_UNDO;

    /* Clear */
    if (x >= (tbX + MulDiv(414, (int)dpi, 96)) && x <= (tbX + MulDiv(454, (int)dpi, 96))) return TB_HOVER_CLEAR;

    /* Copy & Exit */
    if (x >= (tbX + MulDiv(472, (int)dpi, 96)) && x <= (tbX + MulDiv(538, (int)dpi, 96))) return TB_HOVER_COPY;

    /* Save */
    if (x >= (tbX + MulDiv(544, (int)dpi, 96)) && x <= (tbX + MulDiv(582, (int)dpi, 96))) return TB_HOVER_SAVE;

    /* Done */
    if (x >= (tbX + MulDiv(588, (int)dpi, 96)) && x <= (tbX + MulDiv(626, (int)dpi, 96))) return TB_HOVER_DONE;

    /* Cancel */
    if (x >= (tbX + MulDiv(632, (int)dpi, 96)) && x <= (tbX + MulDiv(664, (int)dpi, 96))) return TB_HOVER_CANCEL;

    return TB_HOVER_NONE;
}

/* ------------------------------------------------------------------ */
/* CLI Parsing                                                        */
/* ------------------------------------------------------------------ */

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
    cfg->mode = MODE_FULLSCREEN;
    cfg->draw = false;
    cfg->toolbar = false;
    cfg->clipboard = false;
    cfg->save = false;
    cfg->format = FMT_PNG;
    cfg->fontSize = TinyFont_GetSystemDefaultSize();
    cfg->savePath[0] = L'\0';

    bool explicitOutput = false;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--fullscreen") == 0) {
            cfg->mode = MODE_FULLSCREEN;
        } else if (_wcsicmp(argv[i], L"--window") == 0) {
            cfg->mode = MODE_WINDOW;
        } else if (_wcsicmp(argv[i], L"--snip") == 0) {
            cfg->mode = MODE_SNIP;
        } else if (_wcsicmp(argv[i], L"--draw") == 0) {
            cfg->draw = true;
        } else if (_wcsicmp(argv[i], L"--toolbar") == 0 ||
                   _wcsicmp(argv[i], L"--draw-toolbar") == 0 ||
                   _wcsicmp(argv[i], L"--show-toolbar") == 0) {
            cfg->toolbar = true;
            cfg->draw = true;
            if (i + 1 < argc) {
                if (_wcsicmp(argv[i + 1], L"true") == 0 || _wcsicmp(argv[i + 1], L"1") == 0) {
                    cfg->toolbar = true;
                    i++;
                } else if (_wcsicmp(argv[i + 1], L"false") == 0 || _wcsicmp(argv[i + 1], L"0") == 0) {
                    cfg->toolbar = false;
                    i++;
                }
            }
        } else if (_wcsnicmp(argv[i], L"--toolbar=", 10) == 0) {
            const wchar_t *val = argv[i] + 10;
            if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) {
                cfg->toolbar = false;
            } else {
                cfg->toolbar = true;
                cfg->draw = true;
            }
        } else if (_wcsnicmp(argv[i], L"--draw-toolbar=", 15) == 0) {
            const wchar_t *val = argv[i] + 15;
            if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) {
                cfg->toolbar = false;
            } else {
                cfg->toolbar = true;
                cfg->draw = true;
            }
        } else if (_wcsnicmp(argv[i], L"--show-toolbar=", 15) == 0) {
            const wchar_t *val = argv[i] + 15;
            if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) {
                cfg->toolbar = false;
            } else {
                cfg->toolbar = true;
                cfg->draw = true;
            }
        } else if (_wcsicmp(argv[i], L"--no-toolbar") == 0 ||
                   _wcsicmp(argv[i], L"--hide-toolbar") == 0 ||
                   _wcsicmp(argv[i], L"--no-draw-toolbar") == 0) {
            cfg->toolbar = false;
        } else if (_wcsicmp(argv[i], L"--font-size") == 0) {
            if (i + 1 < argc) {
                int fs = _wtoi(argv[++i]);
                if (fs >= 8 && fs <= 48) {
                    cfg->fontSize = fs;
                }
            }
        } else if (_wcsnicmp(argv[i], L"--font-size=", 12) == 0) {
            int fs = _wtoi(argv[i] + 12);
            if (fs >= 8 && fs <= 48) {
                cfg->fontSize = fs;
            }
        } else if (_wcsicmp(argv[i], L"--format") == 0) {
            if (i + 1 < argc) {
                i++;
                if (_wcsicmp(argv[i], L"bmp") == 0) {
                    cfg->format = FMT_BMP;
                } else if (_wcsicmp(argv[i], L"png") == 0) {
                    cfg->format = FMT_PNG;
                }
            }
        } else if (_wcsnicmp(argv[i], L"--format=", 9) == 0) {
            const wchar_t *val = argv[i] + 9;
            if (_wcsicmp(val, L"bmp") == 0) {
                cfg->format = FMT_BMP;
            } else if (_wcsicmp(val, L"png") == 0) {
                cfg->format = FMT_PNG;
            }
        } else if (_wcsicmp(argv[i], L"--clipboard") == 0) {
            cfg->clipboard = true;
            explicitOutput = true;
        } else if (_wcsicmp(argv[i], L"--save") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                i++;
                wcsncpy(cfg->savePath, argv[i], MAX_PATH - 1);
                cfg->savePath[MAX_PATH - 1] = L'\0';
            } else {
                wcscpy(cfg->savePath, L".");
            }
            cfg->save = true;
            explicitOutput = true;
        }
    }

    if (!explicitOutput) {
        cfg->clipboard = true;
    }
}

/* ------------------------------------------------------------------ */
/* Image Utilities (BMP saving & Clipboard)                          */
/* ------------------------------------------------------------------ */

static bool SaveBitmapToFile(HBITMAP hbmp, const wchar_t *filepath)
{
    BITMAP bmp;
    if (!GetObject(hbmp, sizeof(BITMAP), &bmp))
        return false;

    HDC hdcScreen = GetDC(NULL);
    BITMAPINFOHEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bmp.bmWidth;
    bi.biHeight = bmp.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    DWORD rowSize = ((bmp.bmWidth * 24 + 31) / 32) * 4;
    DWORD imageSize = rowSize * bmp.bmHeight;
    bi.biSizeImage = imageSize;

    BYTE *pixels = (BYTE *)malloc(imageSize);
    if (!pixels) {
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader = bi;

    if (!GetDIBits(hdcScreen, hbmp, 0, (UINT)bmp.bmHeight, pixels, &bmi, DIB_RGB_COLORS)) {
        free(pixels);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }
    ReleaseDC(NULL, hdcScreen);

    BITMAPFILEHEADER bfh;
    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType = 0x4D42; /* 'BM' */
    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    HANDLE hFile = CreateFileW(filepath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        free(pixels);
        return false;
    }

    DWORD written = 0;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, NULL);
    WriteFile(hFile, &bi, sizeof(bi), &written, NULL);
    WriteFile(hFile, pixels, imageSize, &written, NULL);

    CloseHandle(hFile);
    free(pixels);
    return true;
}

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInputC;

typedef int (WINAPI *pfnGdiplusStartup)(ULONG_PTR *, const GdiplusStartupInputC *, void *);
typedef void (WINAPI *pfnGdiplusShutdown)(ULONG_PTR);
typedef int (WINAPI *pfnGdipCreateBitmapFromHBITMAP)(HBITMAP, HPALETTE, void **);
typedef int (WINAPI *pfnGdipSaveImageToFile)(void *, const WCHAR *, const CLSID *, const void *);
typedef int (WINAPI *pfnGdipDisposeImage)(void *);

static bool SaveBitmapToPngFile(HBITMAP hbmp, const wchar_t *filepath)
{
    HMODULE hGdiplus = LoadLibraryW(L"gdiplus.dll");
    if (!hGdiplus)
        return false;

    pfnGdiplusStartup pGdiplusStartup = (pfnGdiplusStartup)GetProcAddress(hGdiplus, "GdiplusStartup");
    pfnGdiplusShutdown pGdiplusShutdown = (pfnGdiplusShutdown)GetProcAddress(hGdiplus, "GdiplusShutdown");
    pfnGdipCreateBitmapFromHBITMAP pGdipCreateBitmapFromHBITMAP =
        (pfnGdipCreateBitmapFromHBITMAP)GetProcAddress(hGdiplus, "GdipCreateBitmapFromHBITMAP");
    pfnGdipSaveImageToFile pGdipSaveImageToFile =
        (pfnGdipSaveImageToFile)GetProcAddress(hGdiplus, "GdipSaveImageToFile");
    pfnGdipDisposeImage pGdipDisposeImage =
        (pfnGdipDisposeImage)GetProcAddress(hGdiplus, "GdipDisposeImage");

    if (!pGdiplusStartup || !pGdiplusShutdown || !pGdipCreateBitmapFromHBITMAP || !pGdipSaveImageToFile || !pGdipDisposeImage) {
        FreeLibrary(hGdiplus);
        return false;
    }

    ULONG_PTR token = 0;
    GdiplusStartupInputC input = { 1, NULL, FALSE, FALSE };
    if (pGdiplusStartup(&token, &input, NULL) != 0) {
        FreeLibrary(hGdiplus);
        return false;
    }

    void *gpBitmap = NULL;
    bool success = false;
    if (pGdipCreateBitmapFromHBITMAP(hbmp, NULL, &gpBitmap) == 0 && gpBitmap) {
        CLSID pngClsid = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
        if (pGdipSaveImageToFile(gpBitmap, filepath, &pngClsid, NULL) == 0) {
            success = true;
        }
        pGdipDisposeImage(gpBitmap);
    }

    pGdiplusShutdown(token);
    FreeLibrary(hGdiplus);
    return success;
}

static bool SaveCapturedImage(HBITMAP hbmp, const wchar_t *filepath, ImageFormat fmt)
{
    size_t len = wcslen(filepath);
    if (len >= 4 && (_wcsicmp(filepath + len - 4, L".png") == 0)) {
        if (SaveBitmapToPngFile(hbmp, filepath)) {
            return true;
        }
    } else if (len >= 4 && (_wcsicmp(filepath + len - 4, L".bmp") == 0)) {
        return SaveBitmapToFile(hbmp, filepath);
    }

    if (fmt == FMT_PNG) {
        if (SaveBitmapToPngFile(hbmp, filepath)) {
            return true;
        }
    }
    return SaveBitmapToFile(hbmp, filepath);
}

static bool CopyBitmapToClipboard(HWND hwnd, HBITMAP hbmp)
{
    return TinyClipboard_SetBitmap(hwnd, hbmp);
}

static wchar_t g_windowAppName[32] = {0};
static wchar_t g_windowTitle[64] = {0};

static void SanitizeFilenameComponent(wchar_t *str)
{
    wchar_t *p = str;
    while (*p) {
        wchar_t c = *p;
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*' ||
            c < 32) {
            *p = L'_';
        } else if (c == L' ') {
            *p = L'_';
        }
        p++;
    }
    size_t len = wcslen(str);
    while (len > 0 && (str[len - 1] == L'_' || str[len - 1] == L' ')) {
        str[--len] = L'\0';
    }
}

static void GetProcessBaseName(HWND hwnd, wchar_t *outName, DWORD maxLen)
{
    outName[0] = L'\0';
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;

    wchar_t fullPath[MAX_PATH] = {0};
    DWORD dwSize = MAX_PATH;
    typedef BOOL (WINAPI *pfnQueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        pfnQueryFullProcessImageNameW pQuery =
            (pfnQueryFullProcessImageNameW)GetProcAddress(hKernel, "QueryFullProcessImageNameW");
        if (pQuery && pQuery(hProc, 0, fullPath, &dwSize)) {
            wchar_t *fileName = wcsrchr(fullPath, L'\\');
            if (!fileName) fileName = wcsrchr(fullPath, L'/');
            if (fileName) {
                fileName++;
            } else {
                fileName = fullPath;
            }
            wchar_t *dot = wcsrchr(fileName, L'.');
            if (dot && _wcsicmp(dot, L".exe") == 0) {
                *dot = L'\0';
            }
            SanitizeFilenameComponent(fileName);
            wcsncpy(outName, fileName, maxLen - 1);
            outName[maxLen - 1] = L'\0';
        }
    }
    CloseHandle(hProc);
}

static void FormatSaveFilePath(const wchar_t *inputPath, wchar_t *outFilePath, DWORD maxLen, ImageFormat fmt, CaptureMode mode)
{
    DWORD attr = GetFileAttributesW(inputPath);
    bool isDir = false;
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        isDir = true;
    } else {
        size_t len = wcslen(inputPath);
        if (len > 0 && (inputPath[len - 1] == L'\\' || inputPath[len - 1] == L'/')) {
            isDir = true;
            CreateDirectoryW(inputPath, NULL);
        } else if (wcsstr(inputPath, L".bmp") == NULL && wcsstr(inputPath, L".BMP") == NULL &&
                   wcsstr(inputPath, L".png") == NULL && wcsstr(inputPath, L".PNG") == NULL) {
            CreateDirectoryW(inputPath, NULL);
            attr = GetFileAttributesW(inputPath);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                isDir = true;
            }
        }
    }

    if (isDir) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        const wchar_t *ext = (fmt == FMT_BMP) ? L"bmp" : L"png";

        if (mode == MODE_SNIP) {
            wsprintfW(outFilePath, L"%s\\snip_%04d%02d%02d_%02d%02d%02d.%s",
                     inputPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
        } else if (mode == MODE_WINDOW) {
            if (g_windowAppName[0] != L'\0' && g_windowTitle[0] != L'\0' && _wcsicmp(g_windowAppName, g_windowTitle) != 0) {
                wsprintfW(outFilePath, L"%s\\%s-%s_%04d%02d%02d_%02d%02d%02d.%s",
                         inputPath, g_windowAppName, g_windowTitle, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
            } else if (g_windowAppName[0] != L'\0') {
                wsprintfW(outFilePath, L"%s\\%s_%04d%02d%02d_%02d%02d%02d.%s",
                         inputPath, g_windowAppName, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
            } else if (g_windowTitle[0] != L'\0') {
                wsprintfW(outFilePath, L"%s\\%s_%04d%02d%02d_%02d%02d%02d.%s",
                         inputPath, g_windowTitle, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
            } else {
                wsprintfW(outFilePath, L"%s\\window_%04d%02d%02d_%02d%02d%02d.%s",
                         inputPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
            }
        } else {
            wsprintfW(outFilePath, L"%s\\capture_%04d%02d%02d_%02d%02d%02d.%s",
                     inputPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
        }
    } else {
        wcsncpy(outFilePath, inputPath, maxLen - 1);
        outFilePath[maxLen - 1] = L'\0';
    }
}

static bool PromptSavePngDialog(HWND hwndOwner, wchar_t *outPath, DWORD maxLen, CaptureMode mode)
{
    wchar_t defaultFullName[MAX_PATH] = {0};
    FormatSaveFilePath(L".", defaultFullName, MAX_PATH, FMT_PNG, mode);

    const wchar_t *defaultFileName = wcsrchr(defaultFullName, L'\\');
    if (!defaultFileName) defaultFileName = wcsrchr(defaultFullName, L'/');
    if (defaultFileName) defaultFileName++;
    else defaultFileName = defaultFullName;

    wchar_t fileBuf[MAX_PATH] = {0};
    wcsncpy(fileBuf, defaultFileName, MAX_PATH - 1);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0Bitmap Image (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn)) {
        wcsncpy(outPath, fileBuf, maxLen - 1);
        outPath[maxLen - 1] = L'\0';
        return true;
    }
    return false;
}

typedef enum {
    STAGE_SELECTOR,
    STAGE_SNIP,
    STAGE_WINDOW_PICK,
    STAGE_ANNOTATE
} CaptureStage;

static CaptureStage g_stage = STAGE_SELECTOR;

static void GetCenteredSelectorOptionRect(int idx, RECT *outRect)
{
    POINT ptCursor;
    GetCursorPos(&ptCursor);
    UINT dpi = TinyDPI_GetDpiForPoint(ptCursor);

    int itemW = MulDiv(220, (int)dpi, 96);
    int itemH = MulDiv(26, (int)dpi, 96);
    int gap = MulDiv(3, (int)dpi, 96);
    int footerGap = MulDiv(14, (int)dpi, 96);
    int footerH = MulDiv(18, (int)dpi, 96);
    int totalH = 3 * itemH + 2 * gap + footerGap + footerH;

    HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    int monLeft = 0, monW = g_vw, monTop = 0, monH = g_vh;
    if (GetMonitorInfoW(hMon, &mi)) {
        monLeft = mi.rcMonitor.left - g_vx;
        monTop = mi.rcMonitor.top - g_vy;
        monW = mi.rcMonitor.right - mi.rcMonitor.left;
        monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }

    int groupX = monLeft + (monW - itemW) / 2;
    int groupY = monTop + (monH - totalH) / 2;

    outRect->left = groupX;
    outRect->top = groupY + idx * (itemH + gap);
    outRect->right = outRect->left + itemW;
    outRect->bottom = outRect->top + itemH;
}

static HBITMAP CropBitmap(HBITMAP hbmSrc, RECT rect)
{
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0)
        return NULL;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcSrcMem = CreateCompatibleDC(hdcScreen);
    HDC hdcDstMem = CreateCompatibleDC(hdcScreen);

    HBITMAP hbmDst = CreateCompatibleBitmap(hdcScreen, w, h);

    HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrcMem, hbmSrc);
    HBITMAP hOldDst = (HBITMAP)SelectObject(hdcDstMem, hbmDst);

    BitBlt(hdcDstMem, 0, 0, w, h, hdcSrcMem, rect.left, rect.top, SRCCOPY);

    SelectObject(hdcSrcMem, hOldSrc);
    SelectObject(hdcDstMem, hOldDst);

    DeleteDC(hdcSrcMem);
    DeleteDC(hdcDstMem);
    ReleaseDC(NULL, hdcScreen);

    return hbmDst;
}

/* ------------------------------------------------------------------ */
/* Screen Capture Logic                                               */
/* ------------------------------------------------------------------ */

static HBITMAP CaptureFullscreen(int *outW, int *outH)
{
    int vx = 0, vy = 0, vw = 0, vh = 0;
    TinyGUI_GetVirtualScreenBounds(&vx, &vy, &vw, &vh);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, vw, vh);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    BitBlt(hdcMem, 0, 0, vw, vh, hdcScreen, vx, vy, SRCCOPY);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (outW) *outW = vw;
    if (outH) *outH = vh;
    return hbm;
}

static int g_winCapX = 0, g_winCapY = 0;

static HBITMAP CaptureActiveWindow(int *outW, int *outH)
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || hwnd == GetDesktopWindow()) {
        return CaptureFullscreen(outW, outH);
    }

    GetProcessBaseName(hwnd, g_windowAppName, 32);

    wchar_t rawTitle[128] = {0};
    if (GetWindowTextW(hwnd, rawTitle, 127) > 0) {
        SanitizeFilenameComponent(rawTitle);
        if (wcslen(rawTitle) > 0) {
            wcsncpy(g_windowTitle, rawTitle, 63);
            g_windowTitle[63] = L'\0';
        }
    }

    RECT r;
    BOOL dwmOk = FALSE;
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        typedef HRESULT (WINAPI *pfnDwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
        pfnDwmGetWindowAttribute pDwmGetWindowAttribute =
            (pfnDwmGetWindowAttribute)GetProcAddress(hDwm, "DwmGetWindowAttribute");
        if (pDwmGetWindowAttribute) {
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
            if (SUCCEEDED(pDwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
                dwmOk = TRUE;
            }
        }
        FreeLibrary(hDwm);
    }

    if (!dwmOk) {
        GetWindowRect(hwnd, &r);
    }

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        return CaptureFullscreen(outW, outH);
    }

    int vx = 0, vy = 0, vw = 0, vh = 0;
    TinyGUI_GetVirtualScreenBounds(&vx, &vy, &vw, &vh);
    g_winCapX = r.left - vx;
    g_winCapY = r.top - vy;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, r.left, r.top, SRCCOPY);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (outW) *outW = w;
    if (outH) *outH = h;
    return hbm;
}

/* ------------------------------------------------------------------ */
/* Interactive Window Picker Helpers                                  */
/* ------------------------------------------------------------------ */

static HWND g_pickerHoveredHwnd = NULL;
static RECT g_pickerHoveredRect = {0, 0, 0, 0};
static wchar_t g_pickerAppName[32] = {0};
static wchar_t g_pickerTitle[64] = {0};

typedef struct {
    POINT pt;
    HWND hIgnore;
    HWND hFound;
    RECT rectFound;
} WindowFromPointCtx;

static BOOL CALLBACK EnumWindowsPickerProc(HWND hwnd, LPARAM lParam)
{
    WindowFromPointCtx *ctx = (WindowFromPointCtx *)lParam;

    if (hwnd == ctx->hIgnore)
        return TRUE;

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 63);
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        return TRUE;
    }

    RECT r;
    BOOL dwmOk = FALSE;
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        typedef HRESULT (WINAPI *pfnDwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
        pfnDwmGetWindowAttribute pDwmGetWindowAttribute =
            (pfnDwmGetWindowAttribute)GetProcAddress(hDwm, "DwmGetWindowAttribute");
        if (pDwmGetWindowAttribute) {
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
            if (SUCCEEDED(pDwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
                dwmOk = TRUE;
            }
        }
        FreeLibrary(hDwm);
    }

    if (!dwmOk) {
        GetWindowRect(hwnd, &r);
    }

    if ((r.right - r.left) <= 10 || (r.bottom - r.top) <= 10)
        return TRUE;

    if (PtInRect(&r, ctx->pt)) {
        ctx->hFound = hwnd;
        ctx->rectFound = r;
        return FALSE;
    }

    return TRUE;
}

static bool UpdatePickerHover(HWND hwndOverlay, POINT pt)
{
    WindowFromPointCtx ctx;
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
            g_pickerAppName[0] = L'\0';
            g_pickerTitle[0] = L'\0';
            return true;
        }
        return false;
    }

    if (hTarget == g_pickerHoveredHwnd) {
        return false;
    }

    g_pickerHoveredHwnd = hTarget;
    g_pickerHoveredRect = ctx.rectFound;
    GetProcessBaseName(hTarget, g_pickerAppName, 32);

    wchar_t rawTitle[128] = {0};
    g_pickerTitle[0] = L'\0';
    if (GetWindowTextW(hTarget, rawTitle, 127) > 0) {
        SanitizeFilenameComponent(rawTitle);
        if (wcslen(rawTitle) > 0) {
            wcsncpy(g_pickerTitle, rawTitle, 63);
            g_pickerTitle[63] = L'\0';
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Annotation / Doodle Utilities                                      */
/* ------------------------------------------------------------------ */

static void CompositeStrokesToBitmap(HBITMAP hbmp)
{
    if (g_strokeCount <= 0)
        return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hbmp);

    for (int i = 0; i < g_strokeCount; i++) {
        Stroke *st = &g_strokes[i];
        if (st->pointCount < 2)
            continue;

        LOGBRUSH lb = { BS_SOLID, st->color, 0 };
        HPEN hPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, st->width, &lb, 0, NULL);
        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);

        Polyline(hdcMem, st->points, st->pointCount);

        SelectObject(hdcMem, hOldPen);
        DeleteObject(hPen);
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static void UpdateToolbarRect(HWND hwnd)
{
    POINT ptCursor;
    GetCursorPos(&ptCursor);
    UINT dpi = TinyDPI_GetDpiForPoint(ptCursor);

    int tbW = MulDiv(672, (int)dpi, 96);
    int tbH = MulDiv(40, (int)dpi, 96);

    int oldX = g_tbX;
    int oldY = g_tbY;

    HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        int monLeft = mi.rcMonitor.left - g_vx;
        int monTop = mi.rcMonitor.top - g_vy;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        int tbX = monLeft + (monW - tbW) / 2;
        if (monW >= tbW + 32) {
            if (tbX < monLeft + 16) tbX = monLeft + 16;
            if (tbX > monLeft + monW - tbW - 16) tbX = monLeft + monW - tbW - 16;
        }

        int tbY = monTop + MulDiv(20, (int)dpi, 96);
        if (monH >= tbH + 32) {
            if (tbY > monTop + monH - tbH - 16) tbY = monTop + monH - tbH - 16;
        }

        g_tbX = tbX;
        g_tbY = tbY;
        g_tbW = tbW;
        g_tbH = tbH;
    } else {
        int tbX = (g_vw - tbW) / 2;
        if (tbX < 16) tbX = 16;
        if (tbX > g_vw - tbW - 16) tbX = g_vw - tbW - 16;

        int tbY = MulDiv(20, (int)dpi, 96);
        if (tbY > g_vh - tbH - 8) tbY = g_vh - tbH - 8;

        g_tbX = tbX;
        g_tbY = tbY;
        g_tbW = tbW;
        g_tbH = tbH;
    }

    if (hwnd && (oldX != g_tbX || oldY != g_tbY)) {
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* Unified Zero-Flicker Overlay Window Procedure                      */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK UnifiedCaptureWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        UpdateToolbarRect(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmBuf = CreateCompatibleBitmap(hdc, g_vw, g_vh);
        HBITMAP hOldBuf = (HBITMAP)SelectObject(hdcMem, hbmBuf);

        POINT ptCursor;
        GetCursorPos(&ptCursor);
        UINT dpi = TinyDPI_GetDpiForPoint(ptCursor);

        int fSize = g_cfg.fontSize;
        int subSize = 9;
        int menuFontSize = 13;
        int titleFontSize = 11;

        HFONT hFont = TinyFont_CreateUI(fSize, TINY_FONT_WEIGHT_MEDIUM, dpi);
        HFONT hMenuFont = TinyFont_CreateUI(menuFontSize, TINY_FONT_WEIGHT_SEMIBOLD, dpi);
        HFONT hTitleFont = TinyFont_CreateUI(titleFontSize, TINY_FONT_WEIGHT_BOLD, dpi);
        HFONT hSubFont = TinyFont_CreateMonospace(subSize, TINY_FONT_WEIGHT_MEDIUM, dpi);
        HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

        HDC hdcDesktop = NULL;
        HBITMAP hOldDesk = NULL;
        if (g_hbmDesktop) {
            hdcDesktop = CreateCompatibleDC(hdc);
            hOldDesk = (HBITMAP)SelectObject(hdcDesktop, g_hbmDesktop);
            TinyGUI_ApplyDimOverlay(hdcMem, hdcDesktop, g_vw, g_vh, SNIP_DIM_ALPHA);
        } else {
            HBRUSH hBgBrush = CreateSolidBrush(RGB(15, 15, 18));
            RECT fullR = {0, 0, g_vw, g_vh};
            FillRect(hdcMem, &fullR, hBgBrush);
            DeleteObject(hBgBrush);
        }

        SetBkMode(hdcMem, TRANSPARENT);

        if (g_stage == STAGE_SELECTOR) {
            /* Vertical List of 3 Options centered as a group on screen (ip-send style, left-aligned items) */
            const wchar_t *labels[3] = {
                L"1. Snip (Region)",
                L"2. Window",
                L"3. Full Screen"
            };

            SelectObject(hdcMem, hMenuFont);

            for (int i = 0; i < 3; i++) {
                RECT rowR;
                GetCenteredSelectorOptionRect(i, &rowR);

                bool isHighlighted = (g_selFocusIdx == i || g_selHoveredIdx == i);

                int pointerW = MulDiv(18, (int)dpi, 96);
                RECT ptrR = { rowR.left, rowR.top, rowR.left + pointerW, rowR.bottom };
                RECT labelR = { rowR.left + pointerW, rowR.top, rowR.right, rowR.bottom };

                if (isHighlighted) {
                    SetTextColor(hdcMem, RGB(96, 165, 250)); /* ip-send accentBlue */
                    DrawTextW(hdcMem, L">", -1, &ptrR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextW(hdcMem, labels[i], -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    SetTextColor(hdcMem, RGB(145, 150, 165)); /* ip-send textDim */
                    DrawTextW(hdcMem, labels[i], -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
            }

            /* Subtext Prompt Underneath Centered Group */
            HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            int monLeft = 0, monW = g_vw, monTop = 0, monH = g_vh;
            if (GetMonitorInfoW(hMon, &mi)) {
                monLeft = mi.rcMonitor.left - g_vx;
                monTop = mi.rcMonitor.top - g_vy;
                monW = mi.rcMonitor.right - mi.rcMonitor.left;
                monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            }

            int itemH = MulDiv(26, (int)dpi, 96);
            int gap = MulDiv(3, (int)dpi, 96);
            int footerGap = MulDiv(14, (int)dpi, 96);
            int footerH = MulDiv(18, (int)dpi, 96);
            int totalH = 3 * itemH + 2 * gap + footerGap + footerH;
            int groupY = monTop + (monH - totalH) / 2;
            int promptY = groupY + 3 * itemH + 2 * gap + footerGap;
            RECT promptR = { monLeft, promptY, monLeft + monW, promptY + footerH };

            SelectObject(hdcMem, hFont);
            SetTextColor(hdcMem, RGB(145, 150, 165));
            DrawTextW(hdcMem, L"[ 1 / 2 / 3 ] Select    [ Enter ] Confirm    [ Esc ] Cancel", -1, &promptR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        } else if (g_stage == STAGE_SNIP) {
            if (g_snipDragging && hdcDesktop) {
                RECT sel;
                sel.left = min(g_snipStart.x, g_snipCurr.x);
                sel.top = min(g_snipStart.y, g_snipCurr.y);
                sel.right = max(g_snipStart.x, g_snipCurr.x);
                sel.bottom = max(g_snipStart.y, g_snipCurr.y);

                int sw = sel.right - sel.left;
                int sh = sel.bottom - sel.top;

                if (sw > 0 && sh > 0) {
                    BitBlt(hdcMem, sel.left, sel.top, sw, sh, hdcDesktop, sel.left, sel.top, SRCCOPY);

                    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 255));
                    HBITMAP hOldPen = (HBITMAP)SelectObject(hdcMem, hPen);
                    HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
                    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hNullBrush);
                    Rectangle(hdcMem, sel.left, sel.top, sel.right, sel.bottom);
                    SelectObject(hdcMem, hOldBrush);
                    SelectObject(hdcMem, hOldPen);
                    DeleteObject(hPen);

                    wchar_t badge[64];
                    wsprintfW(badge, L"%d × %d px", sw, sh);
                    int badgeH = MulDiv(24, (int)dpi, 96);
                    int badgeW = MulDiv(110, (int)dpi, 96);
                    RECT badgeR = { sel.left, sel.top - badgeH, sel.left + badgeW, sel.top - 2 };
                    if (badgeR.top < 0) {
                        badgeR.top = sel.top + 4;
                        badgeR.bottom = sel.top + badgeH;
                    }
                    badgeR.right = badgeR.left + badgeW;
                    HBRUSH hBadgeBg = CreateSolidBrush(RGB(20, 20, 20));
                    FillRect(hdcMem, &badgeR, hBadgeBg);
                    DeleteObject(hBadgeBg);

                    SetTextColor(hdcMem, RGB(255, 255, 255));
                    DrawTextW(hdcMem, badge, -1, &badgeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        } else if (g_stage == STAGE_WINDOW_PICK) {
            if (hdcDesktop && (g_pickerHoveredRect.right - g_pickerHoveredRect.left) > 0) {
                RECT sel;
                sel.left = g_pickerHoveredRect.left - g_vx;
                sel.top = g_pickerHoveredRect.top - g_vy;
                sel.right = g_pickerHoveredRect.right - g_vx;
                sel.bottom = g_pickerHoveredRect.bottom - g_vy;

                if (sel.left < 0) sel.left = 0;
                if (sel.top < 0) sel.top = 0;
                if (sel.right > g_vw) sel.right = g_vw;
                if (sel.bottom > g_vh) sel.bottom = g_vh;

                int sw = sel.right - sel.left;
                int sh = sel.bottom - sel.top;

                if (sw > 0 && sh > 0) {
                    BitBlt(hdcMem, sel.left, sel.top, sw, sh, hdcDesktop, sel.left, sel.top, SRCCOPY);

                    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 255));
                    HBITMAP hOldPen = (HBITMAP)SelectObject(hdcMem, hPen);
                    HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
                    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hNullBrush);
                    Rectangle(hdcMem, sel.left, sel.top, sel.right, sel.bottom);
                    SelectObject(hdcMem, hOldBrush);
                    SelectObject(hdcMem, hOldPen);
                    DeleteObject(hPen);
                }
            }
        } else if (g_stage == STAGE_ANNOTATE) {
            if (g_hbmCaptured) {
                HDC hdcImg = CreateCompatibleDC(hdc);
                HBITMAP hOldImg = (HBITMAP)SelectObject(hdcImg, g_hbmCaptured);
                BitBlt(hdcMem, g_capX, g_capY, g_imgW, g_imgH, hdcImg, 0, 0, SRCCOPY);
                SelectObject(hdcImg, hOldImg);
                DeleteDC(hdcImg);

                if (g_imgW < g_vw || g_imgH < g_vh) {
                    HPEN hImgPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 255));
                    HBRUSH hNullB = (HBRUSH)GetStockObject(NULL_BRUSH);
                    HPEN hOldP = (HPEN)SelectObject(hdcMem, hImgPen);
                    HBRUSH hOldNB = (HBRUSH)SelectObject(hdcMem, hNullB);
                    Rectangle(hdcMem, g_capX - 1, g_capY - 1, g_capX + g_imgW + 1, g_capY + g_imgH + 1);
                    SelectObject(hdcMem, hOldNB);
                    SelectObject(hdcMem, hOldP);
                    DeleteObject(hImgPen);
                }
            }

            for (int s = 0; s < g_strokeCount; s++) {
                Stroke *st = &g_strokes[s];
                if (st->pointCount < 2) continue;
                LOGBRUSH lb = { BS_SOLID, st->color, 0 };
                HPEN hPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, st->width, &lb, 0, NULL);
                HPEN hOldP = (HPEN)SelectObject(hdcMem, hPen);
                POINT pts[MAX_POINTS_PER_STROKE];
                for (int p = 0; p < st->pointCount; p++) {
                    pts[p].x = g_capX + st->points[p].x;
                    pts[p].y = g_capY + st->points[p].y;
                }
                Polyline(hdcMem, pts, st->pointCount);
                SelectObject(hdcMem, hOldP);
                DeleteObject(hPen);
            }

            if (g_isDrawing && g_currentStroke.pointCount >= 2) {
                LOGBRUSH lb = { BS_SOLID, g_currentStroke.color, 0 };
                HPEN hPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, g_currentStroke.width, &lb, 0, NULL);
                HPEN hOldP = (HPEN)SelectObject(hdcMem, hPen);
                POINT pts[MAX_POINTS_PER_STROKE];
                for (int p = 0; p < g_currentStroke.pointCount; p++) {
                    pts[p].x = g_capX + g_currentStroke.points[p].x;
                    pts[p].y = g_capY + g_currentStroke.points[p].y;
                }
                Polyline(hdcMem, pts, g_currentStroke.pointCount);
                SelectObject(hdcMem, hOldP);
                DeleteObject(hPen);
            }

            if (g_showToolbar) {
                int tbX = g_tbX, tbY = g_tbY, tbW = g_tbW, tbH = g_tbH;
                RECT toolR = { tbX, tbY, tbX + tbW, tbY + tbH };
                HBRUSH hToolBrush = CreateSolidBrush(RGB(22, 24, 28));
                HPEN hToolPen = CreatePen(PS_SOLID, 1, RGB(55, 58, 68));
                HBRUSH hOldTB = (HBRUSH)SelectObject(hdcMem, hToolBrush);
                HPEN hOldTP = (HPEN)SelectObject(hdcMem, hToolPen);

                RoundRect(hdcMem, toolR.left, toolR.top, toolR.right, toolR.bottom, MulDiv(12, (int)dpi, 96), MulDiv(12, (int)dpi, 96));

                POINT tailPts[3] = {
                    { tbX + tbW / 2 - MulDiv(7, (int)dpi, 96), tbY + tbH },
                    { tbX + tbW / 2 + MulDiv(7, (int)dpi, 96), tbY + tbH },
                    { tbX + tbW / 2,     tbY + tbH + MulDiv(6, (int)dpi, 96) }
                };
                Polygon(hdcMem, tailPts, 3);

                /* Vertical Section Dividers */
                HPEN hDivPen = CreatePen(PS_SOLID, 1, RGB(46, 49, 58));
                SelectObject(hdcMem, hDivPen);
                int divYs[2] = { tbY + MulDiv(6, (int)dpi, 96), tbY + tbH - MulDiv(6, (int)dpi, 96) };
                int divXs[] = {
                    tbX + MulDiv(76, (int)dpi, 96),
                    tbX + MulDiv(214, (int)dpi, 96),
                    tbX + MulDiv(358, (int)dpi, 96),
                    tbX + MulDiv(462, (int)dpi, 96)
                };
                for (int d = 0; d < 4; d++) {
                    MoveToEx(hdcMem, divXs[d], divYs[0], NULL);
                    LineTo(hdcMem, divXs[d], divYs[1]);
                }
                DeleteObject(hDivPen);

                SelectObject(hdcMem, hOldTB);
                SelectObject(hdcMem, hOldTP);
                DeleteObject(hToolBrush);
                DeleteObject(hToolPen);

                /* Section 1: Title & Space shortcut */
                SelectObject(hdcMem, hTitleFont);
                SetTextColor(hdcMem, RGB(230, 230, 235));
                RECT toolTitleR = { tbX + MulDiv(10, (int)dpi, 96), tbY + MulDiv(4, (int)dpi, 96), tbX + MulDiv(72, (int)dpi, 96), tbY + MulDiv(20, (int)dpi, 96) };
                DrawTextW(hdcMem, L"Doodle", -1, &toolTitleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subToolTitleR = { tbX + MulDiv(10, (int)dpi, 96), tbY + MulDiv(22, (int)dpi, 96), tbX + MulDiv(72, (int)dpi, 96), tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"Space", -1, &subToolTitleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                /* Section 2: Colors */
                const wchar_t *colorKeys[6] = { L"R", L"Y", L"B", L"G", L"K", L"W" };
                int dotR = MulDiv(6, (int)dpi, 96);
                for (int i = 0; i < 6; i++) {
                    int cx = tbX + MulDiv(90 + i * 20, (int)dpi, 96);
                    int cy = tbY + MulDiv(12, (int)dpi, 96);
                    bool isHover = (g_tbHoverItem == (TB_HOVER_COLOR_0 + i));
                    bool isSel = (i == g_selectedColorIdx);

                    HBRUSH hColorBrush = CreateSolidBrush(g_colors[i]);
                    HPEN hColorPen = CreatePen(PS_SOLID, 1, RGB(55, 58, 68));
                    HBRUSH hOldB = (HBRUSH)SelectObject(hdcMem, hColorBrush);
                    HPEN hOldP = (HPEN)SelectObject(hdcMem, hColorPen);

                    Ellipse(hdcMem, cx - dotR, cy - dotR, cx + dotR, cy + dotR);

                    SelectObject(hdcMem, hOldB);
                    SelectObject(hdcMem, hOldP);
                    DeleteObject(hColorBrush);
                    DeleteObject(hColorPen);

                    if (isSel) {
                        HPEN hRingPen = CreatePen(PS_SOLID, 2, RGB(37, 99, 235));
                        HBRUSH hNullB = (HBRUSH)GetStockObject(NULL_BRUSH);
                        HPEN hOldR = (HPEN)SelectObject(hdcMem, hRingPen);
                        HBRUSH hOldNB = (HBRUSH)SelectObject(hdcMem, hNullB);

                        Ellipse(hdcMem, cx - dotR - 2, cy - dotR - 2, cx + dotR + 2, cy + dotR + 2);

                        SelectObject(hdcMem, hOldR);
                        SelectObject(hdcMem, hOldNB);
                        DeleteObject(hRingPen);
                    } else if (isHover) {
                        HPEN hHoverPen = CreatePen(PS_SOLID, 1, RGB(160, 165, 180));
                        HBRUSH hNullB = (HBRUSH)GetStockObject(NULL_BRUSH);
                        HPEN hOldR = (HPEN)SelectObject(hdcMem, hHoverPen);
                        HBRUSH hOldNB = (HBRUSH)SelectObject(hdcMem, hNullB);

                        Ellipse(hdcMem, cx - dotR - 2, cy - dotR - 2, cx + dotR + 2, cy + dotR + 2);

                        SelectObject(hdcMem, hOldR);
                        SelectObject(hdcMem, hOldNB);
                        DeleteObject(hHoverPen);
                    }

                    SelectObject(hdcMem, hSubFont);
                    SetTextColor(hdcMem, isSel ? RGB(220, 225, 240) : RGB(130, 135, 150));
                    RECT subColorR = { cx - MulDiv(8, (int)dpi, 96), tbY + MulDiv(22, (int)dpi, 96), cx + MulDiv(8, (int)dpi, 96), tbY + MulDiv(36, (int)dpi, 96) };
                    DrawTextW(hdcMem, colorKeys[i], -1, &subColorR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                /* Section 3: Pen Width Chips */
                const wchar_t *sizeKeys[4] = { L"2", L"4", L"8", L"1" };
                HPEN hNullPen = (HPEN)GetStockObject(NULL_PEN);
                HPEN hOldNullPen = (HPEN)SelectObject(hdcMem, hNullPen);
                for (int i = 0; i < 4; i++) {
                    int btnLeft = tbX + MulDiv(224 + i * 32, (int)dpi, 96);
                    int btnRight = btnLeft + MulDiv(26, (int)dpi, 96);
                    RECT btnR = { btnLeft, tbY + MulDiv(4, (int)dpi, 96), btnRight, tbY + MulDiv(20, (int)dpi, 96) };
                    bool isSel = (i == g_selectedSizeIdx);
                    bool isHover = (g_tbHoverItem == (TB_HOVER_SIZE_0 + i));

                    COLORREF bg = isSel ? RGB(37, 99, 235) : (isHover ? RGB(52, 56, 68) : RGB(36, 38, 46));
                    HBRUSH hBtnB = CreateSolidBrush(bg);
                    HBRUSH hOldBtnB = (HBRUSH)SelectObject(hdcMem, hBtnB);
                    RoundRect(hdcMem, btnR.left, btnR.top, btnR.right, btnR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                    SelectObject(hdcMem, hOldBtnB);
                    DeleteObject(hBtnB);

                    SelectObject(hdcMem, hFont);
                    wchar_t szStr[16];
                    wsprintfW(szStr, L"%dp", g_penSizes[i]);
                    SetTextColor(hdcMem, isSel ? RGB(255, 255, 255) : (isHover ? RGB(245, 245, 250) : RGB(210, 210, 215)));
                    DrawTextW(hdcMem, szStr, -1, &btnR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(hdcMem, hSubFont);
                    SetTextColor(hdcMem, isSel ? RGB(220, 225, 240) : RGB(130, 135, 150));
                    RECT subSizeR = { btnLeft, tbY + MulDiv(22, (int)dpi, 96), btnRight, tbY + MulDiv(36, (int)dpi, 96) };
                    DrawTextW(hdcMem, sizeKeys[i], -1, &subSizeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                /* Section 4: History Tools (Undo / Clear) */
                /* Undo */
                int undoL = tbX + MulDiv(368, (int)dpi, 96);
                int undoR_x = tbX + MulDiv(408, (int)dpi, 96);
                RECT undoR = { undoL, tbY + MulDiv(4, (int)dpi, 96), undoR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool undoHover = (g_tbHoverItem == TB_HOVER_UNDO);
                HBRUSH hUndoB = CreateSolidBrush(undoHover ? RGB(52, 56, 68) : RGB(36, 38, 46));
                HBRUSH hOldActB = (HBRUSH)SelectObject(hdcMem, hUndoB);
                RoundRect(hdcMem, undoR.left, undoR.top, undoR.right, undoR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hUndoB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, undoHover ? RGB(255, 255, 255) : RGB(220, 220, 225));
                DrawTextW(hdcMem, L"Undo", -1, &undoR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subUndoR = { undoL, tbY + MulDiv(22, (int)dpi, 96), undoR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"Z", -1, &subUndoR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                /* Clear */
                int clearL = tbX + MulDiv(414, (int)dpi, 96);
                int clearR_x = tbX + MulDiv(454, (int)dpi, 96);
                RECT clearR = { clearL, tbY + MulDiv(4, (int)dpi, 96), clearR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool clearHover = (g_tbHoverItem == TB_HOVER_CLEAR);
                HBRUSH hClearB = CreateSolidBrush(clearHover ? RGB(52, 56, 68) : RGB(36, 38, 46));
                hOldActB = (HBRUSH)SelectObject(hdcMem, hClearB);
                RoundRect(hdcMem, clearR.left, clearR.top, clearR.right, clearR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hClearB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, clearHover ? RGB(255, 255, 255) : RGB(220, 220, 225));
                DrawTextW(hdcMem, L"Clear", -1, &clearR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subClearR = { clearL, tbY + MulDiv(22, (int)dpi, 96), clearR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"X", -1, &subClearR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                /* Section 5: Action Buttons (Copy / Save / Done / Cancel) */
                /* Copy & Exit */
                int copyL = tbX + MulDiv(472, (int)dpi, 96);
                int copyR_x = tbX + MulDiv(538, (int)dpi, 96);
                RECT copyR = { copyL, tbY + MulDiv(4, (int)dpi, 96), copyR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool copyHover = (g_tbHoverItem == TB_HOVER_COPY);
                HBRUSH hCopyB = CreateSolidBrush(copyHover ? RGB(52, 56, 68) : RGB(36, 38, 46));
                hOldActB = (HBRUSH)SelectObject(hdcMem, hCopyB);
                RoundRect(hdcMem, copyR.left, copyR.top, copyR.right, copyR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hCopyB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, copyHover ? RGB(255, 255, 255) : RGB(220, 220, 225));
                DrawTextW(hdcMem, L"Copy", -1, &copyR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subCopyR = { copyL, tbY + MulDiv(22, (int)dpi, 96), copyR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"C", -1, &subCopyR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                /* Save */
                int saveL = tbX + MulDiv(544, (int)dpi, 96);
                int saveR_x = tbX + MulDiv(582, (int)dpi, 96);
                RECT saveR = { saveL, tbY + MulDiv(4, (int)dpi, 96), saveR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool saveHover = (g_tbHoverItem == TB_HOVER_SAVE);
                HBRUSH hSaveB = CreateSolidBrush(saveHover ? RGB(52, 56, 68) : RGB(36, 38, 46));
                hOldActB = (HBRUSH)SelectObject(hdcMem, hSaveB);
                RoundRect(hdcMem, saveR.left, saveR.top, saveR.right, saveR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hSaveB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, saveHover ? RGB(255, 255, 255) : RGB(220, 220, 225));
                DrawTextW(hdcMem, L"Save", -1, &saveR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subSaveR = { saveL, tbY + MulDiv(22, (int)dpi, 96), saveR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"S", -1, &subSaveR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                /* Done */
                int doneL = tbX + MulDiv(588, (int)dpi, 96);
                int doneR_x = tbX + MulDiv(626, (int)dpi, 96);
                RECT doneR = { doneL, tbY + MulDiv(4, (int)dpi, 96), doneR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool doneHover = (g_tbHoverItem == TB_HOVER_DONE);
                HBRUSH hDoneB = CreateSolidBrush(doneHover ? RGB(59, 130, 246) : RGB(37, 99, 235));
                hOldActB = (HBRUSH)SelectObject(hdcMem, hDoneB);
                RoundRect(hdcMem, doneR.left, doneR.top, doneR.right, doneR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hDoneB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, RGB(255, 255, 255));
                DrawTextW(hdcMem, L"Done", -1, &doneR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subDoneR = { doneL, tbY + MulDiv(22, (int)dpi, 96), doneR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"Enter", -1, &subDoneR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                /* Cancel */
                int cancelL = tbX + MulDiv(632, (int)dpi, 96);
                int cancelR_x = tbX + MulDiv(664, (int)dpi, 96);
                RECT cancelR = { cancelL, tbY + MulDiv(4, (int)dpi, 96), cancelR_x, tbY + MulDiv(20, (int)dpi, 96) };
                bool cancelHover = (g_tbHoverItem == TB_HOVER_CANCEL);
                HBRUSH hCancelB = CreateSolidBrush(cancelHover ? RGB(115, 35, 42) : RGB(75, 30, 35));
                hOldActB = (HBRUSH)SelectObject(hdcMem, hCancelB);
                RoundRect(hdcMem, cancelR.left, cancelR.top, cancelR.right, cancelR.bottom, MulDiv(4, (int)dpi, 96), MulDiv(4, (int)dpi, 96));
                SelectObject(hdcMem, hOldActB);
                DeleteObject(hCancelB);

                SelectObject(hdcMem, hFont);
                SetTextColor(hdcMem, RGB(255, 255, 255));
                DrawTextW(hdcMem, L"Esc", -1, &cancelR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hSubFont);
                SetTextColor(hdcMem, RGB(130, 135, 150));
                RECT subCancelR = { cancelL, tbY + MulDiv(22, (int)dpi, 96), cancelR_x, tbY + MulDiv(36, (int)dpi, 96) };
                DrawTextW(hdcMem, L"X", -1, &subCancelR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hOldNullPen);
            } else {
                RECT hintR = { g_vw - 320, g_vh - 36, g_vw - 16, g_vh - 10 };
                HBRUSH hHintB = CreateSolidBrush(RGB(22, 24, 28));
                HPEN hHintP = CreatePen(PS_SOLID, 1, RGB(55, 58, 68));
                HBRUSH hOldHB = (HBRUSH)SelectObject(hdcMem, hHintB);
                HPEN hOldHP = (HPEN)SelectObject(hdcMem, hHintP);

                RoundRect(hdcMem, hintR.left, hintR.top, hintR.right, hintR.bottom, 8, 8);

                SelectObject(hdcMem, hOldHB);
                SelectObject(hdcMem, hOldHP);
                DeleteObject(hHintB);
                DeleteObject(hHintP);

                SelectObject(hdcMem, hSubFont);
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, RGB(190, 195, 205));
                DrawTextW(hdcMem, L"Space: Toolbar | Z: Undo | Enter: Done", -1, &hintR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);
        DeleteObject(hMenuFont);
        DeleteObject(hTitleFont);
        DeleteObject(hSubFont);

        BitBlt(hdc, 0, 0, g_vw, g_vh, hdcMem, 0, 0, SRCCOPY);

        if (hdcDesktop) {
            SelectObject(hdcDesktop, hOldDesk);
            DeleteDC(hdcDesktop);
        }

        SelectObject(hdcMem, hOldBuf);
        DeleteObject(hbmBuf);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (g_stage == STAGE_SELECTOR) {
            int prevHover = g_selHoveredIdx;
            g_selHoveredIdx = -1;

            for (int i = 0; i < 3; i++) {
                RECT rowR;
                GetCenteredSelectorOptionRect(i, &rowR);
                POINT pt = { x, y };
                if (PtInRect(&rowR, pt)) {
                    g_selHoveredIdx = i;
                    g_selFocusIdx = i;
                    break;
                }
            }

            if (g_selHoveredIdx != prevHover) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        if (g_stage == STAGE_SNIP) {
            if (g_snipDragging) {
                g_snipCurr.x = x;
                g_snipCurr.y = y;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        if (g_stage == STAGE_WINDOW_PICK) {
            POINT pt;
            pt.x = x + g_vx;
            pt.y = y + g_vy;
            if (UpdatePickerHover(hwnd, pt)) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        if (g_stage == STAGE_ANNOTATE) {
            if (!g_isDrawing) {
                UpdateToolbarRect(hwnd);
                if (g_showToolbar) {
                    int prevHover = g_tbHoverItem;
                    g_tbHoverItem = GetToolbarHoverItem(x, y);
                    if (g_tbHoverItem != prevHover) {
                        RECT tbR = { g_tbX - 4, g_tbY - 4, g_tbX + g_tbW + 4, g_tbY + g_tbH + 4 };
                        InvalidateRect(hwnd, &tbR, FALSE);
                    }
                }
            } else {
                int px = x - g_capX;
                int py = y - g_capY;

                if (g_currentStroke.pointCount < MAX_POINTS_PER_STROKE) {
                    g_currentStroke.points[g_currentStroke.pointCount].x = px;
                    g_currentStroke.points[g_currentStroke.pointCount].y = py;
                    g_currentStroke.pointCount++;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (g_stage == STAGE_SELECTOR) {
            if (g_selHoveredIdx >= 0) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        if (g_stage == STAGE_SNIP || g_stage == STAGE_WINDOW_PICK) {
            SetCursor(LoadCursor(NULL, IDC_CROSS));
            return TRUE;
        }
        if (g_stage == STAGE_ANNOTATE) {
            POINT pt;
            GetCursorPos(&pt);
            int x = pt.x - g_vx;
            int y = pt.y - g_vy;
            if (g_showToolbar && x >= g_tbX && x <= (g_tbX + g_tbW) && y >= g_tbY && y <= (g_tbY + g_tbH)) {
                if (GetToolbarHoverItem(x, y) != TB_HOVER_NONE) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                } else {
                    SetCursor(LoadCursor(NULL, IDC_ARROW));
                }
                return TRUE;
            }
            SetCursor(LoadCursor(NULL, IDC_CROSS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (g_stage == STAGE_SELECTOR) {
            for (int i = 0; i < 3; i++) {
                RECT rowR;
                GetCenteredSelectorOptionRect(i, &rowR);
                POINT pt = { x, y };
                if (PtInRect(&rowR, pt)) {
                    if (i == 0) {
                        g_stage = STAGE_SNIP;
                        g_cfg.mode = MODE_SNIP;
                        g_snipDragging = false;
                    } else if (i == 1) {
                        g_stage = STAGE_WINDOW_PICK;
                        g_cfg.mode = MODE_WINDOW;
                        g_pickerHoveredHwnd = NULL;
                        SetRectEmpty(&g_pickerHoveredRect);
                    } else {
                        g_hbmCaptured = (HBITMAP)CopyImage(g_hbmDesktop, IMAGE_BITMAP, 0, 0, 0);
                        g_imgW = g_vw;
                        g_imgH = g_vh;
                        g_capX = 0;
                        g_capY = 0;
                        g_stage = STAGE_ANNOTATE;
                        g_showToolbar = true;
                        g_cfg.draw = true;
                        g_cfg.toolbar = true;
                        UpdateToolbarRect(hwnd);
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            return 0;
        }

        if (g_stage == STAGE_SNIP) {
            g_snipDragging = true;
            g_snipStart.x = x;
            g_snipStart.y = y;
            g_snipCurr = g_snipStart;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (g_stage == STAGE_WINDOW_PICK) {
            RECT sel;
            sel.left = g_pickerHoveredRect.left - g_vx;
            sel.top = g_pickerHoveredRect.top - g_vy;
            sel.right = g_pickerHoveredRect.right - g_vx;
            sel.bottom = g_pickerHoveredRect.bottom - g_vy;

            if (sel.left < 0) sel.left = 0;
            if (sel.top < 0) sel.top = 0;
            if (sel.right > g_vw) sel.right = g_vw;
            if (sel.bottom > g_vh) sel.bottom = g_vh;

            if ((sel.right - sel.left) > 4 && (sel.bottom - sel.top) > 4) {
                g_snipRect = sel;
                g_winCapX = sel.left;
                g_winCapY = sel.top;
                wcsncpy(g_windowAppName, g_pickerAppName, 31);
                wcsncpy(g_windowTitle, g_pickerTitle, 63);

                g_hbmCaptured = CropBitmap(g_hbmDesktop, sel);
                g_imgW = sel.right - sel.left;
                g_imgH = sel.bottom - sel.top;
                g_capX = sel.left;
                g_capY = sel.top;

                if (g_cfg.draw) {
                    g_stage = STAGE_ANNOTATE;
                    g_showToolbar = true;
                    g_cfg.toolbar = true;
                    UpdateToolbarRect(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    g_annotationConfirmed = true;
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }

        if (g_stage == STAGE_ANNOTATE) {
            int tbX = g_tbX, tbY = g_tbY, tbW = g_tbW, tbH = g_tbH;
            if (g_showToolbar && x >= tbX && x <= (tbX + tbW) && y >= tbY && y <= (tbY + tbH)) {
                int item = GetToolbarHoverItem(x, y);
                if (item >= TB_HOVER_COLOR_0 && item <= TB_HOVER_COLOR_5) {
                    g_selectedColorIdx = item - TB_HOVER_COLOR_0;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (item >= TB_HOVER_SIZE_0 && item <= TB_HOVER_SIZE_3) {
                    g_selectedSizeIdx = item - TB_HOVER_SIZE_0;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (item == TB_HOVER_UNDO) {
                    if (g_strokeCount > 0) {
                        g_strokeCount--;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                if (item == TB_HOVER_CLEAR) {
                    g_strokeCount = 0;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (item == TB_HOVER_COPY) {
                    CompositeStrokesToBitmap(g_hbmCaptured);
                    g_cfg.clipboard = true;
                    g_annotationConfirmed = true;
                    g_userRequestedSaveDialog = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (item == TB_HOVER_SAVE) {
                    CompositeStrokesToBitmap(g_hbmCaptured);
                    g_cfg.save = true;
                    if (g_cfg.savePath[0] == L'\0') {
                        wcscpy(g_cfg.savePath, L".");
                    }
                    g_annotationConfirmed = true;
                    g_userRequestedSaveDialog = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (item == TB_HOVER_DONE) {
                    CompositeStrokesToBitmap(g_hbmCaptured);
                    g_annotationConfirmed = true;
                    g_userRequestedSaveDialog = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (item == TB_HOVER_CANCEL) {
                    g_annotationConfirmed = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                return 0;
            } else {
                int px = x - g_capX;
                int py = y - g_capY;
                g_currentStroke.color = g_colors[g_selectedColorIdx];
                g_currentStroke.width = g_penSizes[g_selectedSizeIdx];
                g_currentStroke.points[0].x = px;
                g_currentStroke.points[0].y = py;
                g_currentStroke.pointCount = 1;
                g_isDrawing = true;
                SetCapture(hwnd);
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_stage == STAGE_SNIP && g_snipDragging) {
            g_snipDragging = false;
            ReleaseCapture();
            g_snipCurr.x = GET_X_LPARAM(lParam);
            g_snipCurr.y = GET_Y_LPARAM(lParam);

            g_snipRect.left = min(g_snipStart.x, g_snipCurr.x);
            g_snipRect.top = min(g_snipStart.y, g_snipCurr.y);
            g_snipRect.right = max(g_snipStart.x, g_snipCurr.x);
            g_snipRect.bottom = max(g_snipStart.y, g_snipCurr.y);

            int sw = g_snipRect.right - g_snipRect.left;
            int sh = g_snipRect.bottom - g_snipRect.top;

            if (sw > 4 && sh > 4) {
                g_hbmCaptured = CropBitmap(g_hbmDesktop, g_snipRect);
                g_imgW = sw;
                g_imgH = sh;
                g_capX = g_snipRect.left;
                g_capY = g_snipRect.top;

                if (g_cfg.draw) {
                    g_stage = STAGE_ANNOTATE;
                    g_showToolbar = true;
                    g_cfg.toolbar = true;
                    UpdateToolbarRect(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    g_annotationConfirmed = true;
                    DestroyWindow(hwnd);
                }
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        if (g_stage == STAGE_ANNOTATE && g_isDrawing) {
            g_isDrawing = false;
            ReleaseCapture();
            if (g_currentStroke.pointCount >= 2 && g_strokeCount < MAX_STROKES) {
                g_strokes[g_strokeCount++] = g_currentStroke;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        g_annotationConfirmed = false;
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        if (TinyFont_HandleZoomKey(wParam, &g_cfg.fontSize)) {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (g_stage == STAGE_SELECTOR) {
            if (wParam == VK_ESCAPE) {
                g_annotationConfirmed = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == '1' || wParam == 'S' || wParam == VK_NUMPAD1) {
                g_stage = STAGE_SNIP;
                g_cfg.mode = MODE_SNIP;
                g_snipDragging = false;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == '2' || wParam == 'W' || wParam == VK_NUMPAD2) {
                g_stage = STAGE_WINDOW_PICK;
                g_cfg.mode = MODE_WINDOW;
                g_pickerHoveredHwnd = NULL;
                SetRectEmpty(&g_pickerHoveredRect);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == '3' || wParam == 'F' || wParam == VK_NUMPAD3) {
                g_hbmCaptured = (HBITMAP)CopyImage(g_hbmDesktop, IMAGE_BITMAP, 0, 0, 0);
                g_imgW = g_vw;
                g_imgH = g_vh;
                g_capX = 0;
                g_capY = 0;
                g_stage = STAGE_ANNOTATE;
                g_showToolbar = true;
                g_cfg.draw = true;
                g_cfg.toolbar = true;
                UpdateToolbarRect(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_UP || wParam == VK_LEFT) {
                g_selFocusIdx = (g_selFocusIdx + 2) % 3;
                g_selHoveredIdx = g_selFocusIdx;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_DOWN || wParam == VK_RIGHT || wParam == VK_TAB) {
                g_selFocusIdx = (g_selFocusIdx + 1) % 3;
                g_selHoveredIdx = g_selFocusIdx;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (g_selFocusIdx == 0) {
                    g_stage = STAGE_SNIP;
                    g_cfg.mode = MODE_SNIP;
                    g_snipDragging = false;
                } else if (g_selFocusIdx == 1) {
                    g_stage = STAGE_WINDOW_PICK;
                    g_cfg.mode = MODE_WINDOW;
                    g_pickerHoveredHwnd = NULL;
                    SetRectEmpty(&g_pickerHoveredRect);
                } else {
                    g_hbmCaptured = (HBITMAP)CopyImage(g_hbmDesktop, IMAGE_BITMAP, 0, 0, 0);
                    g_imgW = g_vw;
                    g_imgH = g_vh;
                    g_capX = 0;
                    g_capY = 0;
                    g_stage = STAGE_ANNOTATE;
                    g_showToolbar = true;
                    g_cfg.draw = true;
                    g_cfg.toolbar = true;
                    UpdateToolbarRect(hwnd);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            return 0;
        }

        if (g_stage == STAGE_SNIP) {
            if (wParam == VK_ESCAPE) {
                g_annotationConfirmed = false;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }

        if (g_stage == STAGE_WINDOW_PICK) {
            if (wParam == VK_ESCAPE) {
                g_annotationConfirmed = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                RECT sel;
                sel.left = g_pickerHoveredRect.left - g_vx;
                sel.top = g_pickerHoveredRect.top - g_vy;
                sel.right = g_pickerHoveredRect.right - g_vx;
                sel.bottom = g_pickerHoveredRect.bottom - g_vy;

                if (sel.left < 0) sel.left = 0;
                if (sel.top < 0) sel.top = 0;
                if (sel.right > g_vw) sel.right = g_vw;
                if (sel.bottom > g_vh) sel.bottom = g_vh;

                if ((sel.right - sel.left) > 4 && (sel.bottom - sel.top) > 4) {
                    g_hbmCaptured = CropBitmap(g_hbmDesktop, sel);
                    g_imgW = sel.right - sel.left;
                    g_imgH = sel.bottom - sel.top;
                    g_capX = sel.left;
                    g_capY = sel.top;

                    if (g_cfg.draw) {
                        g_stage = STAGE_ANNOTATE;
                        g_showToolbar = true;
                        g_cfg.toolbar = true;
                        UpdateToolbarRect(hwnd);
                        InvalidateRect(hwnd, NULL, FALSE);
                    } else {
                        g_annotationConfirmed = true;
                        DestroyWindow(hwnd);
                    }
                }
                return 0;
            }
            return 0;
        }

        if (g_stage == STAGE_ANNOTATE) {
            if (wParam == VK_SPACE) {
                g_showToolbar = !g_showToolbar;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                g_annotationConfirmed = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == VK_RETURN) {
                CompositeStrokesToBitmap(g_hbmCaptured);
                g_annotationConfirmed = true;
                g_userRequestedSaveDialog = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == 'R') { g_selectedColorIdx = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'Y') { g_selectedColorIdx = 1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'B') { g_selectedColorIdx = 2; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'G') { g_selectedColorIdx = 3; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'K') { g_selectedColorIdx = 4; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'W') { g_selectedColorIdx = 5; InvalidateRect(hwnd, NULL, FALSE); return 0; }

            if (wParam == '2' || wParam == VK_NUMPAD2) { g_selectedSizeIdx = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == '4' || wParam == VK_NUMPAD4) { g_selectedSizeIdx = 1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == '8' || wParam == VK_NUMPAD8) { g_selectedSizeIdx = 2; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == '1' || wParam == VK_NUMPAD1) { g_selectedSizeIdx = 3; InvalidateRect(hwnd, NULL, FALSE); return 0; }

            if (wParam == 'X') { g_strokeCount = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'Z') {
                if (g_strokeCount > 0) {
                    g_strokeCount--;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            if (wParam == 'C') {
                CompositeStrokesToBitmap(g_hbmCaptured);
                g_cfg.clipboard = true;
                g_annotationConfirmed = true;
                g_userRequestedSaveDialog = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == 'S') {
                CompositeStrokesToBitmap(g_hbmCaptured);
                g_cfg.save = true;
                if (g_cfg.savePath[0] == L'\0') {
                    wcscpy(g_cfg.savePath, L".");
                }
                g_annotationConfirmed = true;
                g_userRequestedSaveDialog = true;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        break;
    }

    case WM_DISPLAYCHANGE: {
        TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &g_vw, &g_vh);
        SetWindowPos(hwnd, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateToolbarRect(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ForceForeground(HWND hwnd)
{
    DWORD dwCurThread = GetCurrentThreadId();
    HWND hFg = GetForegroundWindow();
    DWORD dwFgThread = hFg ? GetWindowThreadProcessId(hFg, NULL) : 0;

    if (dwFgThread && dwFgThread != dwCurThread) {
        AttachThreadInput(dwFgThread, dwCurThread, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(dwFgThread, dwCurThread, FALSE);
    } else {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

static bool RunUnifiedCapturePipeline(CaptureStage initialStage)
{
    g_stage = initialStage;
    g_strokeCount = 0;
    g_isDrawing = false;
    g_showToolbar = g_cfg.toolbar;
    g_annotationConfirmed = false;
    g_selHoveredIdx = -1;
    g_selFocusIdx = 0;

    TinyGUI_GetVirtualScreenBounds(&g_vx, &g_vy, &g_vw, &g_vh);
    if (!g_hbmDesktop) {
        g_hbmDesktop = CaptureFullscreen(&g_vw, &g_vh);
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = UnifiedCaptureWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, (initialStage == STAGE_SELECTOR) ? IDC_ARROW : IDC_CROSS);
    wc.lpszClassName = L"TinyUnifiedCaptureClass";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"TinyUnifiedCaptureClass", L"Capture",
        WS_POPUP | WS_VISIBLE,
        g_vx, g_vy, g_vw, g_vh,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (!hwnd) {
        UnregisterClassW(L"TinyUnifiedCaptureClass", GetModuleHandle(NULL));
        return false;
    }

    ForceForeground(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_event && WaitForSingleObject(g_event, 0) == WAIT_OBJECT_0) {
            g_annotationConfirmed = false;
            DestroyWindow(hwnd);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(L"TinyUnifiedCaptureClass", GetModuleHandle(NULL));
    return g_annotationConfirmed;
}

/* ------------------------------------------------------------------ */
/* WinMain                                                            */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow)
{
    TinyDPI_EnablePerMonitorAwareness();

    wchar_t *cmdLineW = GetCommandLineW();
    wchar_t cmdCopy[2048];
    wcsncpy(cmdCopy, cmdLineW, 2047);
    cmdCopy[2047] = L'\0';

    wchar_t *argv[MAX_ARGV];
    int argc = SplitCommandLine(cmdCopy, argv, MAX_ARGV);

    bool isInteractiveMode = (argc <= 1);
    g_isInteractiveMode = isInteractiveMode;

    ParseCommandLine(argc, argv, &g_cfg);

    /* IPC Single Instance Check */
    HANDLE hMutex = CreateMutexW(NULL, FALSE, APPMUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE hEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, APPEVENT_NAME);
        if (hEv) {
            SetEvent(hEv);
            CloseHandle(hEv);
        }
        if (hMutex)
            CloseHandle(hMutex);
        return 0;
    }

    g_event = CreateEventW(NULL, FALSE, FALSE, APPEVENT_NAME);

    CaptureStage startStage = STAGE_SELECTOR;
    if (isInteractiveMode) {
        startStage = STAGE_SELECTOR;
        g_cfg.draw = true;
        g_cfg.toolbar = true;
        g_cfg.clipboard = true;
    } else if (g_cfg.mode == MODE_SNIP) {
        startStage = STAGE_SNIP;
    } else if (g_cfg.mode == MODE_WINDOW) {
        startStage = STAGE_WINDOW_PICK;
    } else {
        if (g_cfg.draw) {
            startStage = STAGE_ANNOTATE;
        } else {
            g_hbmCaptured = CaptureFullscreen(&g_imgW, &g_imgH);
            if (g_hbmCaptured) {
                if (g_cfg.clipboard) CopyBitmapToClipboard(NULL, g_hbmCaptured);
                if (g_cfg.save && g_cfg.savePath[0] != L'\0') {
                    wchar_t finalFilePath[MAX_PATH];
                    FormatSaveFilePath(g_cfg.savePath, finalFilePath, MAX_PATH, g_cfg.format, g_cfg.mode);
                    SaveCapturedImage(g_hbmCaptured, finalFilePath, g_cfg.format);
                }
                DeleteObject(g_hbmCaptured);
            }
            if (g_event) CloseHandle(g_event);
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
    }

    if (!RunUnifiedCapturePipeline(startStage)) {
        if (g_hbmDesktop) DeleteObject(g_hbmDesktop);
        if (g_hbmCaptured) DeleteObject(g_hbmCaptured);
        if (g_event) CloseHandle(g_event);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    /* Output Step */
    if (g_hbmCaptured) {
        if (g_cfg.clipboard) {
            CopyBitmapToClipboard(NULL, g_hbmCaptured);
        }

        if (g_userRequestedSaveDialog) {
            if (!g_cfg.save || g_cfg.savePath[0] == L'\0') {
                wchar_t savePath[MAX_PATH] = {0};
                if (PromptSavePngDialog(NULL, savePath, MAX_PATH, g_cfg.mode)) {
                    SaveCapturedImage(g_hbmCaptured, savePath, (wcsstr(savePath, L".bmp") || wcsstr(savePath, L".BMP")) ? FMT_BMP : FMT_PNG);
                }
            } else {
                wchar_t finalFilePath[MAX_PATH];
                FormatSaveFilePath(g_cfg.savePath, finalFilePath, MAX_PATH, g_cfg.format, g_cfg.mode);
                SaveCapturedImage(g_hbmCaptured, finalFilePath, g_cfg.format);
            }
        } else if (g_cfg.save && g_cfg.savePath[0] != L'\0') {
            wchar_t finalFilePath[MAX_PATH];
            FormatSaveFilePath(g_cfg.savePath, finalFilePath, MAX_PATH, g_cfg.format, g_cfg.mode);
            SaveCapturedImage(g_hbmCaptured, finalFilePath, g_cfg.format);
        }
    }

    /* Cleanup */
    if (g_hbmDesktop) {
        DeleteObject(g_hbmDesktop);
        g_hbmDesktop = NULL;
    }
    if (g_hbmCaptured) {
        DeleteObject(g_hbmCaptured);
        g_hbmCaptured = NULL;
    }
    if (g_event) CloseHandle(g_event);
    if (hMutex) CloseHandle(hMutex);

    return 0;
}
