/*
 * Tiny Window Switcher — Minimal, fast keyboard-driven window switcher with DWM previews.
 * Source: src/window-switcher.c
 * Spec: docs/window-switcher.md
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>
#include <math.h>

#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"
#include "common/tiny_dpi.h"
#include "common/tiny_gui.h"
#include "common/font.h"

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

#ifndef DWM_TNS_RECTDESTINATION
#define DWM_TNS_RECTDESTINATION 0x00000001
#endif
#ifndef DWM_TNS_RECTNORMAL
#define DWM_TNS_RECTNORMAL 0x00000002
#endif
#ifndef DWM_TNS_ALPHA
#define DWM_TNS_ALPHA 0x00000004
#endif
#ifndef DWM_TNS_VISIBLE
#define DWM_TNS_VISIBLE 0x00000008
#endif
#ifndef DWM_TNS_OPACITY
#define DWM_TNS_OPACITY 0x00000004
#endif

#define APPMUTEX_NAME L"Global\\TinyWindowSwitcherMutex"
#define APPEVENT_NAME L"Global\\TinyWindowSwitcherEvent"

#define MAX_WINDOWS 128
#define MAX_APPS 64

/* --- Configurable UI Layout Constants --- */
#define UI_CARD_BASE_WIDTH     210   /* Card width */
#define UI_CARD_ASPECT_RATIO_W  16   /* Card aspect ratio width (16:10) */
#define UI_CARD_ASPECT_RATIO_H  10   /* Card aspect ratio height (16:10) */
#define UI_CARD_GAP             14   /* Gap between cards */
#define UI_CONTAINER_PADDING    16   /* Padding around center container */
#define UI_HEADER_HEIGHT        28   /* Top header bar height */
#define UI_HEADER_TEXT_HEIGHT   18   /* Header title text height */
#define UI_FONT_SIZE_HINT       12   /* Shortcut hint badge font size */
#define UI_FONT_SIZE_TITLE      12   /* Window title & header font size */

typedef enum {
    SORT_RECENT,
    SORT_NAME
} SortMode;

typedef enum {
    BG_BLUR,
    BG_TINT
} BgMode;

typedef enum {
    LAYOUT_CENTER,
    LAYOUT_FULL
} SwitcherLayout;

typedef struct {
    SortMode       sort;
    BgMode         bgMode;
    SwitcherLayout layout;
    int            blurAmount;   /* 1..100, default 20 */
    COLORREF       tintColor;    /* default RGB(0,0,0) */
    BYTE           tintAlpha;    /* 0..255, default 102 (~40%) or hex #RRGGBBAA */
    int            delayMs;      /* 0..5000, default 0 ms */
} Config;

typedef struct {
    HWND        hwnd;
    wchar_t     title[256];
    wchar_t     appName[128];
    DWORD       processId;
    wchar_t     hint[32];
    HTHUMBNAIL  hThumbnail;
    RECT        cardRect;
    RECT        previewRect;
    RECT        closeRect;
    int         mruOrder;
    int         appGroupIndex;
    int         windowIndexInApp;
} WindowItem;

typedef struct {
    wchar_t appName[128];
    wchar_t hint[32];
    int     firstItemIndex;
    int     itemCount;
} AppGroupInfo;

static Config       g_cfg = { SORT_NAME, BG_BLUR, LAYOUT_CENTER, 20, RGB(0, 0, 0), 102, 300 };
static WindowItem   g_items[MAX_WINDOWS];
static int          g_itemCount = 0;
static AppGroupInfo g_appGroups[MAX_APPS];
static int          g_appGroupCount = 0;

static int          g_selectedIndex = 0;
static int          g_hoverIndex = -1;
static int          g_hoverCloseIndex = -1;
static wchar_t      g_typedBuf[32] = {0};
static int          g_typedLen = 0;
static int          g_layoutCols = 4;

static HWND         g_hwndOverlay = NULL;
static HFONT        g_hFontHint = NULL;
static HFONT        g_hFontHintLarge = NULL;
static HFONT        g_hFontTitle = NULL;
static HFONT        g_hFontGroup = NULL;
static bool         g_altTabMode = false;

/* --- Hex Color Helper with Alpha Support --- */
static void ParseHexColorWithAlpha(const wchar_t *str, COLORREF *outColor, BYTE *outAlpha) {
    if (!str) return;
    if (*str == L'#') str++;
    else if (_wcsnicmp(str, L"0x", 2) == 0) str += 2;

    size_t len = wcslen(str);
    wchar_t *endPtr = NULL;
    unsigned long val = wcstoul(str, &endPtr, 16);
    if (endPtr == str) return;

    if (len >= 8) {
        BYTE r = (BYTE)((val >> 24) & 0xFF);
        BYTE g = (BYTE)((val >> 16) & 0xFF);
        BYTE b = (BYTE)((val >> 8) & 0xFF);
        BYTE a = (BYTE)(val & 0xFF);
        if (outColor) *outColor = RGB(r, g, b);
        if (outAlpha) *outAlpha = a;
    } else {
        BYTE r = (BYTE)((val >> 16) & 0xFF);
        BYTE g = (BYTE)((val >> 8) & 0xFF);
        BYTE b = (BYTE)(val & 0xFF);
        if (outColor) *outColor = RGB(r, g, b);
        if (outAlpha) *outAlpha = 255;
    }
}

/* --- DWM Background Composition Helper --- */
static void ApplyBackgroundComposition(HWND hwnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (!hUser) return;

    typedef enum {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5
    } ACCENT_STATE;

    typedef struct {
        ACCENT_STATE AccentState;
        DWORD AccentFlags;
        DWORD GradientColor;
        DWORD AnimationId;
    } ACCENT_POLICY;

    typedef struct {
        DWORD Attribute;
        PVOID pData;
        SIZE_T cbData;
    } WINDOWCOMPOSITIONATTRIBDATA;

    typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

    pfnSetWindowCompositionAttribute setWindowComp =
        (pfnSetWindowCompositionAttribute)(void*)GetProcAddress(hUser, "SetWindowCompositionAttribute");

    if (setWindowComp) {
        ACCENT_POLICY policy = { ACCENT_DISABLED, 0, 0, 0 };

        if (g_cfg.bgMode == BG_BLUR) {
            if (g_cfg.blurAmount <= 30) {
                /* Light glass blur (standard Aero glass policy state 3) */
                BYTE alpha = (BYTE)((g_cfg.blurAmount * 80) / 30 + 5);
                DWORD gradientColor = (alpha << 24) | 0x00101010;
                policy.AccentState = ACCENT_ENABLE_BLURBEHIND;
                policy.AccentFlags = 2;
                policy.GradientColor = gradientColor;
            } else {
                /* Heavy acrylic blur (acrylic policy state 4) */
                BYTE alpha = (BYTE)(((g_cfg.blurAmount - 30) * 175) / 70 + 80);
                DWORD gradientColor = (alpha << 24) | 0x00181818;
                policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
                policy.AccentFlags = 2;
                policy.GradientColor = gradientColor;
            }
        } else if (g_cfg.bgMode == BG_TINT) {
            BYTE alpha = g_cfg.tintAlpha;
            BYTE r = GetRValue(g_cfg.tintColor);
            BYTE g = GetGValue(g_cfg.tintColor);
            BYTE b = GetBValue(g_cfg.tintColor);
            /* ABGR color format for DWM Accent Policy */
            DWORD gradientColor = (alpha << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
            policy.AccentState = ACCENT_ENABLE_TRANSPARENTGRADIENT;
            policy.AccentFlags = 2;
            policy.GradientColor = gradientColor;
        } else {
            policy.AccentState = ACCENT_DISABLED;
        }

        WINDOWCOMPOSITIONATTRIBDATA data = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
        setWindowComp(hwnd, &data);
    } else {
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = (g_cfg.bgMode == BG_BLUR);
        DwmEnableBlurBehindWindow(hwnd, &bb);
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
}

/* --- Executable Metadata Name Helper --- */
static void GetExeMetadataName(const wchar_t *exePath, wchar_t *outName, size_t maxLen) {
    outName[0] = L'\0';
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath, &dummy);
    if (size > 0) {
        BYTE *block = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
        if (block) {
            if (GetFileVersionInfoW(exePath, 0, size, block)) {
                struct LANGANDCODEPAGE {
                    WORD wLanguage;
                    WORD wCodePage;
                } *lpTranslate = NULL;
                UINT cbTranslate = 0;

                /* Try translation table */
                if (VerQueryValueW(block, L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) &&
                    cbTranslate >= sizeof(struct LANGANDCODEPAGE)) {
                    wchar_t subBlock[80];
                    wchar_t *val = NULL;
                    UINT valLen = 0;

                    /* 1. Try FileDescription */
                    swprintf_s(subBlock, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                        lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
                    if (VerQueryValueW(block, subBlock, (LPVOID*)&val, &valLen) && val && val[0] != L'\0') {
                        wcsncpy_s(outName, maxLen, val, _TRUNCATE);
                    }

                    /* 2. Try ProductName */
                    if (outName[0] == L'\0') {
                        swprintf_s(subBlock, 80, L"\\StringFileInfo\\%04x%04x\\ProductName",
                            lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
                        if (VerQueryValueW(block, subBlock, (LPVOID*)&val, &valLen) && val && val[0] != L'\0') {
                            wcsncpy_s(outName, maxLen, val, _TRUNCATE);
                        }
                    }
                }

                /* Fallback to default US English 040904b0 if translation table missed */
                if (outName[0] == L'\0') {
                    wchar_t *val = NULL;
                    UINT valLen = 0;
                    if (VerQueryValueW(block, L"\\StringFileInfo\\040904b0\\FileDescription", (LPVOID*)&val, &valLen) && val && val[0] != L'\0') {
                        wcsncpy_s(outName, maxLen, val, _TRUNCATE);
                    } else if (VerQueryValueW(block, L"\\StringFileInfo\\040904b0\\ProductName", (LPVOID*)&val, &valLen) && val && val[0] != L'\0') {
                        wcsncpy_s(outName, maxLen, val, _TRUNCATE);
                    } else if (VerQueryValueW(block, L"\\StringFileInfo\\040904e4\\FileDescription", (LPVOID*)&val, &valLen) && val && val[0] != L'\0') {
                        wcsncpy_s(outName, maxLen, val, _TRUNCATE);
                    }
                }
            }
            HeapFree(GetProcessHeap(), 0, block);
        }
    }
}

/* --- Process Name Helper --- */
static void GetProcessAppName(DWORD pid, wchar_t *outName, size_t maxLen) {
    outName[0] = L'\0';
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t exePath[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exePath, &sz)) {
            wchar_t metaName[128] = {0};
            GetExeMetadataName(exePath, metaName, 128);

            /* Use metadata FileDescription/ProductName if valid and not a generic host string */
            if (metaName[0] != L'\0' &&
                _wcsicmp(metaName, L"Host Process for Windows Services") != 0 &&
                _wcsicmp(metaName, L"Windows Host Process (Rundll32)") != 0) {
                wcsncpy_s(outName, maxLen, metaName, _TRUNCATE);
            } else {
                wchar_t *fname = wcsrchr(exePath, L'\\');
                if (fname) fname++; else fname = exePath;
                wcsncpy_s(outName, maxLen, fname, _TRUNCATE);
                wchar_t *dot = wcsrchr(outName, L'.');
                if (dot && _wcsicmp(dot, L".exe") == 0) {
                    *dot = L'\0';
                }
            }

            if (outName[0] >= L'a' && outName[0] <= L'z') {
                outName[0] = (wchar_t)towupper(outName[0]);
            }
        }
        CloseHandle(hProc);
    }
    if (outName[0] == L'\0') {
        wcsncpy_s(outName, maxLen, L"App", _TRUNCATE);
    }
}

/* --- UWP Host Process Helper --- */
static DWORD GetUWPRealProcessId(HWND hwndHost) {
    HWND hChild = FindWindowExW(hwndHost, NULL, L"Windows.UI.Core.CoreWindow", NULL);
    if (hChild) {
        DWORD realPid = 0;
        GetWindowThreadProcessId(hChild, &realPid);
        if (realPid != 0) return realPid;
    }
    return 0;
}

/* --- Window Enumeration Callback --- */
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (g_itemCount >= MAX_WINDOWS) return FALSE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != NULL && !(exStyle & WS_EX_APPWINDOW)) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    if (hwnd == GetShellWindow()) return TRUE;
    wchar_t clsName[64];
    GetClassNameW(hwnd, clsName, 64);
    if (_wcsicmp(clsName, L"Progman") == 0 || _wcsicmp(clsName, L"WorkerW") == 0) return TRUE;

    if (hwnd == g_hwndOverlay) return TRUE;

    WindowItem *item = &g_items[g_itemCount];
    item->hwnd = hwnd;
    GetWindowTextW(hwnd, item->title, sizeof(item->title)/sizeof(wchar_t));
    GetWindowThreadProcessId(hwnd, &item->processId);

    DWORD pidToQuery = item->processId;
    DWORD realUwpPid = GetUWPRealProcessId(hwnd);
    if (realUwpPid != 0) {
        pidToQuery = realUwpPid;
    }
    GetProcessAppName(pidToQuery, item->appName, sizeof(item->appName)/sizeof(wchar_t));

    if (_wcsicmp(item->appName, L"ApplicationFrameHost") == 0 || _wcsicmp(item->appName, L"App") == 0) {
        if (wcslen(item->title) > 0) {
            wcsncpy_s(item->appName, sizeof(item->appName)/sizeof(wchar_t), item->title, _TRUNCATE);
            wchar_t *sep = wcsstr(item->appName, L" — ");
            if (!sep) sep = wcsstr(item->appName, L" - ");
            if (sep) *sep = L'\0';
        }
    }

    item->mruOrder = g_itemCount;
    item->hThumbnail = NULL;
    item->appGroupIndex = -1;
    item->windowIndexInApp = 0;

    g_itemCount++;
    return TRUE;
}

/* --- Sorting Comparators --- */
static int CompareName(const void *a, const void *b) {
    const WindowItem *w1 = (const WindowItem *)a;
    const WindowItem *w2 = (const WindowItem *)b;
    int cmp = _wcsicmp(w1->appName, w2->appName);
    if (cmp != 0) return cmp;
    return _wcsicmp(w1->title, w2->title);
}

static int CompareMRU(const void *a, const void *b) {
    const WindowItem *w1 = (const WindowItem *)a;
    const WindowItem *w2 = (const WindowItem *)b;
    return w1->mruOrder - w2->mruOrder;
}

/* --- Candidate Hint Generation Helper --- */
static void GenerateCandidateHints(int appIdx, wchar_t candidates[][32], int *outCount) {
    int count = 0;
    const wchar_t *appName = g_appGroups[appIdx].appName;
    size_t appLen = wcslen(appName);
    if (appLen == 0) {
        wcsncpy_s(candidates[0], 32, L"A", _TRUNCATE);
        *outCount = 1;
        return;
    }

    #define ADD_CANDIDATE(str) do { \
        const wchar_t *s = (str); \
        if (s[0] != L'\0') { \
            bool exists = false; \
            for (int c = 0; c < count; c++) { \
                if (_wcsicmp(candidates[c], s) == 0) { exists = true; break; } \
            } \
            if (!exists && count < 16) { \
                wcsncpy_s(candidates[count++], 32, s, _TRUNCATE); \
            } \
        } \
    } while(0)

    wchar_t buf[32];

    /* 1. Base + Extension (e.g. Notepad++ vs Notepad) */
    for (int other = 0; other < g_appGroupCount; other++) {
        if (other == appIdx) continue;
        const wchar_t *otherName = g_appGroups[other].appName;
        size_t otherLen = wcslen(otherName);
        if (otherLen > 0 && appLen > otherLen && _wcsnicmp(appName, otherName, otherLen) == 0) {
            const wchar_t *suffix = appName + otherLen;
            swprintf_s(buf, 32, L"%c%s", (wchar_t)towupper(appName[0]), suffix);
            for (size_t k = 0; k < wcslen(buf); k++) buf[k] = (wchar_t)towupper(buf[k]);
            ADD_CANDIDATE(buf);
        }
    }

    /* 2. Word initials / Acronym (if multi-word name) */
    wchar_t acronym[32] = {0};
    int acrLen = 0;
    bool newWord = true;
    for (size_t i = 0; i < appLen && acrLen < 4; i++) {
        wchar_t ch = appName[i];
        if (iswalnum(ch)) {
            if (newWord) {
                acronym[acrLen++] = (wchar_t)towupper(ch);
                newWord = false;
            }
        } else {
            newWord = true;
        }
    }
    acronym[acrLen] = L'\0';
    if (acrLen > 1) {
        ADD_CANDIDATE(acronym);
    }

    /* 3. Check for shared 2-letter prefix with other apps (e.g. Zen vs Zed -> ZN vs ZD) */
    bool shares2CharPrefix = false;
    if (appLen > 1) {
        for (int other = 0; other < g_appGroupCount; other++) {
            if (other == appIdx) continue;
            const wchar_t *otherName = g_appGroups[other].appName;
            if (_wcsnicmp(appName, otherName, 2) == 0) {
                shares2CharPrefix = true;
                break;
            }
        }
    }

    if (shares2CharPrefix) {
        /* Find first distinguishing character vs each colliding app */
        for (int other = 0; other < g_appGroupCount; other++) {
            if (other == appIdx) continue;
            const wchar_t *otherName = g_appGroups[other].appName;
            if (_wcsnicmp(appName, otherName, 2) == 0) {
                size_t k = 2;
                while (k < appLen && k < wcslen(otherName) && towupper(appName[k]) == towupper(otherName[k])) {
                    k++;
                }
                if (k < appLen) {
                    buf[0] = (wchar_t)towupper(appName[0]);
                    buf[1] = (wchar_t)towupper(appName[k]);
                    buf[2] = L'\0';
                    ADD_CANDIDATE(buf);
                }
            }
        }
    }

    /* 4. Single-letter hint (e.g. C for Chrome, F for Firefox) */
    buf[0] = (wchar_t)towupper(appName[0]);
    buf[1] = L'\0';
    ADD_CANDIDATE(buf);

    /* 5. Contiguous 2-letter prefix (e.g. CH for Chrome, CO for Code) */
    if (appLen > 1) {
        buf[0] = (wchar_t)towupper(appName[0]);
        buf[1] = (wchar_t)towupper(appName[1]);
        buf[2] = L'\0';
        ADD_CANDIDATE(buf);
    }

    /* 6. Contiguous 3-letter & longer prefixes */
    for (size_t len = 3; len <= appLen && len < 8; len++) {
        for (size_t k = 0; k < len; k++) {
            buf[k] = (wchar_t)towupper(appName[k]);
        }
        buf[len] = L'\0';
        ADD_CANDIDATE(buf);
    }

    /* 7. Fallback with numbers */
    for (int n = 1; n <= 9; n++) {
        swprintf_s(buf, 32, L"%c%d", (wchar_t)towupper(appName[0]), n);
        ADD_CANDIDATE(buf);
    }

    #undef ADD_CANDIDATE
    *outCount = count;
}

/* --- Hint Generation Algorithm --- */
static void ComputeAppHints(void) {
    g_appGroupCount = 0;

    /* Build unique app list while preserving item groupings */
    for (int i = 0; i < g_itemCount; i++) {
        int found = -1;
        for (int g = 0; g < g_appGroupCount; g++) {
            if (_wcsicmp(g_appGroups[g].appName, g_items[i].appName) == 0) {
                found = g;
                break;
            }
        }
        if (found == -1 && g_appGroupCount < MAX_APPS) {
            found = g_appGroupCount++;
            wcsncpy_s(g_appGroups[found].appName, 128, g_items[i].appName, _TRUNCATE);
            g_appGroups[found].firstItemIndex = i;
            g_appGroups[found].itemCount = 0;
            g_appGroups[found].hint[0] = L'\0';
        }
        if (found != -1) {
            g_items[i].appGroupIndex = found;
            g_items[i].windowIndexInApp = g_appGroups[found].itemCount++;
        }
    }

    /* Candidate storage */
    wchar_t appCandidates[MAX_APPS][16][32];
    int candidateCounts[MAX_APPS];

    for (int g = 0; g < g_appGroupCount; g++) {
        GenerateCandidateHints(g, appCandidates[g], &candidateCounts[g]);
    }

    /* Keep track of assigned hints */
    wchar_t assignedHints[MAX_APPS][32];
    for (int g = 0; g < g_appGroupCount; g++) {
        assignedHints[g][0] = L'\0';
    }

    /* Assign each app the first available non-colliding candidate from its candidate list */
    for (int g = 0; g < g_appGroupCount; g++) {
        for (int c = 0; c < candidateCounts[g]; c++) {
            const wchar_t *cand = appCandidates[g][c];
            bool taken = false;
            for (int other = 0; other < g_appGroupCount; other++) {
                if (other == g) continue;
                if (_wcsicmp(assignedHints[other], cand) == 0) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                wcsncpy_s(assignedHints[g], 32, cand, _TRUNCATE);
                break;
            }
        }

        /* Fallback if all candidates taken */
        if (assignedHints[g][0] == L'\0') {
            swprintf_s(assignedHints[g], 32, L"%c%d", (wchar_t)towupper(g_appGroups[g].appName[0]), g + 1);
        }
    }

    /* Save assigned hints back to g_appGroups */
    for (int g = 0; g < g_appGroupCount; g++) {
        wcsncpy_s(g_appGroups[g].hint, 32, assignedHints[g], _TRUNCATE);
    }

    /* Assign window hints */
    for (int i = 0; i < g_itemCount; i++) {
        int g = g_items[i].appGroupIndex;
        if (g >= 0 && g < g_appGroupCount) {
            if (g_appGroups[g].itemCount > 1) {
                swprintf_s(g_items[i].hint, 32, L"%s%d", g_appGroups[g].hint, g_items[i].windowIndexInApp + 1);
            } else {
                wcsncpy_s(g_items[i].hint, 32, g_appGroups[g].hint, _TRUNCATE);
            }
        } else {
            wcsncpy_s(g_items[i].hint, 32, L"W", _TRUNCATE);
        }
    }
}

/* --- Window Activation --- */
static void ActivateWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    /* Force foreground switch */
    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);

    AttachThreadInput(currentThread, targetThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    AttachThreadInput(currentThread, targetThread, FALSE);
}

/* --- Cleanup Thumbnails --- */
static void CleanupThumbnails(void) {
    for (int i = 0; i < g_itemCount; i++) {
        if (g_items[i].hThumbnail) {
            DwmUnregisterThumbnail(g_items[i].hThumbnail);
            g_items[i].hThumbnail = NULL;
        }
    }
}

static RECT g_containerRect = {0, 0, 0, 0};

/* --- Filter Matching Helper --- */
static bool IsItemAppOrHintMatch(const WindowItem *item, const wchar_t *buf, int len) {
    if (len == 0) return true;

    /* 1. Exact hint match */
    if (_wcsicmp(item->hint, buf) == 0) return true;

    /* 2. Hint prefix match */
    if (_wcsnicmp(item->hint, buf, len) == 0) return true;

    /* 3. App name prefix match */
    if (_wcsnicmp(item->appName, buf, len) == 0) return true;

    /* 4. App name word-boundary prefix match (e.g. "File Pilot" matched by "P") */
    const wchar_t *p = item->appName;
    while (*p) {
        if ((p == item->appName || *(p - 1) == L' ' || *(p - 1) == L'-' || *(p - 1) == L'_') &&
            _wcsnicmp(p, buf, len) == 0) {
            return true;
        }
        p++;
    }

    return false;
}

static void GetFilteredIndices(int *outIndices, int *outCount) {
    if (g_typedLen == 0) {
        int count = 0;
        for (int i = 0; i < g_itemCount; i++) {
            outIndices[count++] = i;
        }
        *outCount = count;
        return;
    }

    /* Tier 1 & 2: App Name or Hint prefix / word-start matches */
    int count = 0;
    for (int i = 0; i < g_itemCount; i++) {
        if (IsItemAppOrHintMatch(&g_items[i], g_typedBuf, g_typedLen)) {
            outIndices[count++] = i;
        }
    }

    if (count > 0) {
        *outCount = count;
        return;
    }

    /* Tier 3: Title prefix and title substring fallback */
    wchar_t lowerBuf[32];
    for (int k = 0; k < g_typedLen && k < 31; k++) {
        lowerBuf[k] = (wchar_t)towlower(g_typedBuf[k]);
    }
    lowerBuf[g_typedLen < 31 ? g_typedLen : 31] = L'\0';

    for (int i = 0; i < g_itemCount; i++) {
        wchar_t lowerTitle[256];
        size_t titleLen = wcslen(g_items[i].title);
        size_t k = 0;
        for (k = 0; k < titleLen && k < 255; k++) {
            lowerTitle[k] = (wchar_t)towlower(g_items[i].title[k]);
        }
        lowerTitle[k] = L'\0';

        if (wcsstr(lowerTitle, lowerBuf) != NULL) {
            outIndices[count++] = i;
        }
    }
    *outCount = count;
}

static UINT g_currentDpi = 0;

static void UpdateFontsForDpi(UINT dpi) {
    if (dpi == 0) dpi = 96;
    if (g_currentDpi == dpi && g_hFontHint != NULL) return;
    g_currentDpi = dpi;

    if (g_hFontHint)      DeleteObject(g_hFontHint);
    if (g_hFontHintLarge) DeleteObject(g_hFontHintLarge);
    if (g_hFontTitle)     DeleteObject(g_hFontTitle);
    if (g_hFontGroup)     DeleteObject(g_hFontGroup);

    const wchar_t *uiFace = TinyFont_GetBestUIFace();
    g_hFontHint      = TinyFont_Create(uiFace, UI_FONT_SIZE_HINT, FW_BOLD, false, dpi);
    g_hFontHintLarge = TinyFont_Create(uiFace, 16, FW_BOLD, false, dpi);
    g_hFontTitle     = TinyFont_Create(uiFace, UI_FONT_SIZE_TITLE, FW_NORMAL, false, dpi);
    g_hFontGroup     = TinyFont_Create(uiFace, UI_FONT_SIZE_TITLE, FW_NORMAL, false, dpi);
}

/* --- Layout Calculation --- */
static void ComputeLayout(HWND hwnd, int *outW, int *outH) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(hMon, &mi);

    int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    *outW = monW;
    *outH = monH;

    SetWindowPos(hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, monW, monH, SWP_NOACTIVATE);

    UINT dpi = TinyDPI_GetDpiForMonitor(hMon);
    if (dpi == 0) dpi = 96;
    UpdateFontsForDpi(dpi);

    float scale = (float)dpi / 96.0f;
    if (scale <= 0.0f) scale = 1.0f;

    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);

    /* Reset card rects */
    for (int i = 0; i < g_itemCount; i++) {
        SetRectEmpty(&g_items[i].cardRect);
        SetRectEmpty(&g_items[i].previewRect);
        SetRectEmpty(&g_items[i].closeRect);
    }

    int countToLayout = visibleCount > 0 ? visibleCount : 1;

    int cardW = (int)(UI_CARD_BASE_WIDTH * scale);
    int cardH = (cardW * UI_CARD_ASPECT_RATIO_H) / UI_CARD_ASPECT_RATIO_W;
    int gap = (int)(UI_CARD_GAP * scale);
    int headerH = (int)(UI_HEADER_HEIGHT * scale);
    int headerBarH = (int)(20 * scale);
    int headerGap = (int)(4 * scale);
    int padSide = (int)(2 * scale);
    int padBottom = (int)(2 * scale);
    int closeW = (int)(18 * scale);
    int closeH = (int)(18 * scale);

    if (g_cfg.layout == LAYOUT_FULL) {
        int marginX = (int)(20 * scale);
        int marginY = (int)(16 * scale);

        g_containerRect.left = marginX;
        g_containerRect.top = marginY;
        g_containerRect.right = monW - marginX;
        g_containerRect.bottom = monH - marginY;

        int availW = monW - (marginX * 2);
        int availH = monH - (marginY * 2) - headerH;

        int cols = (availW + gap) / (cardW + gap);
        if (cols < 3) cols = 3;
        if (cols > 8) cols = 8;

        int rows = (countToLayout + cols - 1) / cols;
        if (rows < 1) rows = 1;

        g_layoutCols = cols;

        cardW = (availW - (cols - 1) * gap) / cols;
        cardH = (cardW * UI_CARD_ASPECT_RATIO_H) / UI_CARD_ASPECT_RATIO_W;

        int totalGridH = rows * cardH + (rows - 1) * gap;
        if (totalGridH > availH && rows > 0) {
            cardH = (availH - (rows - 1) * gap) / rows;
            cardW = (cardH * UI_CARD_ASPECT_RATIO_W) / UI_CARD_ASPECT_RATIO_H;
            if (cardW > (availW - (cols - 1) * gap) / cols) {
                cardW = (availW - (cols - 1) * gap) / cols;
            }
        }

        if (cardW < (int)(120 * scale)) cardW = (int)(120 * scale);
        if (cardH < (int)(80 * scale)) cardH = (int)(80 * scale);

        int startY = marginY + headerH;

        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            int r = v / cols;
            int c = v % cols;

            int cx = marginX + c * (cardW + gap);
            int cy = startY + r * (cardH + gap);

            g_items[i].cardRect.left = cx;
            g_items[i].cardRect.top = cy;
            g_items[i].cardRect.right = cx + cardW;
            g_items[i].cardRect.bottom = cy + cardH;

            g_items[i].closeRect.left = cx + cardW - closeW - (int)(3 * scale);
            g_items[i].closeRect.top = cy + (int)(2 * scale);
            g_items[i].closeRect.right = cx + cardW - (int)(3 * scale);
            g_items[i].closeRect.bottom = cy + (int)(2 * scale) + closeH;

            g_items[i].previewRect.left = cx + padSide;
            g_items[i].previewRect.top = cy + headerBarH + headerGap;
            g_items[i].previewRect.right = cx + cardW - padSide;
            g_items[i].previewRect.bottom = cy + cardH - padBottom;
            if (g_items[i].previewRect.bottom <= g_items[i].previewRect.top) {
                g_items[i].previewRect.bottom = g_items[i].previewRect.top + 1;
            }
        }
    } else {
        /* Centered Grid Container Layout */
        int padding = (int)(UI_CONTAINER_PADDING * scale);

        int maxCols = (monW - padding * 2 + gap) / (cardW + gap);
        if (maxCols < 1) maxCols = 1;
        if (maxCols > 6) maxCols = 6;

        int cols = 5;
        if (countToLayout <= maxCols) cols = countToLayout > 0 ? countToLayout : 1;
        else cols = maxCols;

        int rows = (countToLayout + cols - 1) / cols;
        if (rows < 1) rows = 1;

        g_layoutCols = cols;

        int gridW = cols * cardW + (cols - 1) * gap;
        int gridH = rows * cardH + (rows - 1) * gap;

        int containerW = gridW + padding * 2;
        int containerH = gridH + padding * 2 + headerH;

        int maxContW = monW * 94 / 100;
        int maxContH = monH * 92 / 100;

        if (containerW > maxContW && cols > 0) {
            containerW = maxContW;
            int availGridW = containerW - padding * 2;
            cardW = (availGridW - (cols - 1) * gap) / cols;
            cardH = (cardW * UI_CARD_ASPECT_RATIO_H) / UI_CARD_ASPECT_RATIO_W;
            gridW = cols * cardW + (cols - 1) * gap;
            gridH = rows * cardH + (rows - 1) * gap;
            containerH = gridH + padding * 2 + headerH;
        }

        if (containerH > maxContH && rows > 0) {
            containerH = maxContH;
            int availGridH = containerH - padding * 2 - headerH;
            cardH = (availGridH - (rows - 1) * gap) / rows;
            cardW = (cardH * UI_CARD_ASPECT_RATIO_W) / UI_CARD_ASPECT_RATIO_H;
        }

        int containerX = (monW - containerW) / 2;
        int containerY = (monH - containerH) / 2;

        g_containerRect.left = containerX;
        g_containerRect.top = containerY;
        g_containerRect.right = containerX + containerW;
        g_containerRect.bottom = containerY + containerH;

        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            int r = v / cols;
            int c = v % cols;

            int cx = containerX + padding + c * (cardW + gap);
            int cy = containerY + padding + headerH + r * (cardH + gap);

            g_items[i].cardRect.left = cx;
            g_items[i].cardRect.top = cy;
            g_items[i].cardRect.right = cx + cardW;
            g_items[i].cardRect.bottom = cy + cardH;

            g_items[i].previewRect.left = cx + padSide;
            g_items[i].previewRect.top = cy + headerBarH + headerGap;
            g_items[i].previewRect.right = cx + cardW - padSide;
            g_items[i].previewRect.bottom = cy + cardH - padBottom;
            if (g_items[i].previewRect.bottom <= g_items[i].previewRect.top) {
                g_items[i].previewRect.bottom = g_items[i].previewRect.top + 1;
            }
        }
    }
}

/* --- Render Overlay --- */
static void PaintOverlay(HWND hwnd, HDC hdc) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int width = rcClient.right - rcClient.left;
    int height = rcClient.bottom - rcClient.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    COLORREF bgFillColor = RGB(0, 0, 0);
    HBRUSH bgBrush = CreateSolidBrush(bgFillColor);
    FillRect(memDC, &rcClient, bgBrush);
    DeleteObject(bgBrush);

    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);

    if (g_selectedIndex >= visibleCount) {
        g_selectedIndex = visibleCount > 0 ? (visibleCount - 1) : 0;
    }
    if (g_selectedIndex < 0 && visibleCount > 0) {
        g_selectedIndex = 0;
    }

    /* Hide DWM thumbnails for non-visible items */
    for (int i = 0; i < g_itemCount; i++) {
        bool isVis = false;
        for (int v = 0; v < visibleCount; v++) {
            if (visibleIndices[v] == i) { isVis = true; break; }
        }
        if (!isVis && g_items[i].hThumbnail) {
            DWM_THUMBNAIL_PROPERTIES props = {0};
            props.dwFlags = DWM_TNS_VISIBLE;
            props.fVisible = FALSE;
            DwmUpdateThumbnailProperties(g_items[i].hThumbnail, &props);
        }
    }

    SetBkMode(memDC, TRANSPARENT);
    HFONT hOldFont = (HFONT)SelectObject(memDC, g_hFontGroup);
    SetTextColor(memDC, RGB(165, 175, 190));

    wchar_t headerText[256];
    if (g_typedLen > 0) {
        if (visibleCount > 0) {
            swprintf_s(headerText, 256, L"Window Switcher — Filter: '%s' (%d matching)", g_typedBuf, visibleCount);
        } else {
            swprintf_s(headerText, 256, L"Window Switcher — Filter: '%s' (No matching windows)", g_typedBuf);
        }
    } else {
        const wchar_t *layoutName = (g_cfg.layout == LAYOUT_FULL ? L"Full" : L"Center");
        const wchar_t *sortName = (g_cfg.sort == SORT_NAME ? L"Name" : L"Recent");
        const wchar_t *bgName = (g_cfg.bgMode == BG_BLUR ? L"Blur" : L"Tint");
        swprintf_s(headerText, 256, L"Window Switcher  |  Layout: %s  |  Sort: %s  |  BG: %s", layoutName, sortName, bgName);
    }

    float scale = (float)g_currentDpi / 96.0f;
    if (scale <= 0.0f) scale = 1.0f;
    int padding = (int)(UI_CONTAINER_PADDING * scale);
    int headerLeft = (g_cfg.layout == LAYOUT_CENTER) ? (g_containerRect.left + padding) : g_containerRect.left;
    int headerTop  = (g_cfg.layout == LAYOUT_CENTER) ? (g_containerRect.top + padding) : g_containerRect.top;
    RECT headerRect = { headerLeft, headerTop, g_containerRect.right - (g_cfg.layout == LAYOUT_CENTER ? padding : 0), headerTop + (int)(UI_HEADER_TEXT_HEIGHT * scale) };
    DrawTextW(memDC, headerText, -1, &headerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    int padTop = (int)(2 * scale);
    int headerBarH = (int)(20 * scale);
    int closeW = (int)(18 * scale);

    /* Render visible matching cards */
    for (int v = 0; v < visibleCount; v++) {
        int i = visibleIndices[v];
        WindowItem *item = &g_items[i];
        RECT r = item->cardRect;

        bool isSelected = (v == g_selectedIndex);
        bool isHovered = (v == g_hoverIndex);
        bool isCloseHovered = (v == g_hoverCloseIndex);
        bool showClose = (isSelected || isHovered || isCloseHovered);

        COLORREF cardBg = isSelected ? RGB(32, 38, 48) : (isHovered ? RGB(26, 30, 38) : RGB(16, 18, 23));
        COLORREF cardBorder = isSelected ? RGB(0, 140, 255) : (isHovered ? RGB(70, 105, 150) : RGB(40, 44, 52));

        HBRUSH cBrush = CreateSolidBrush(cardBg);
        HPEN cPen = CreatePen(PS_SOLID, isSelected ? (int)(2 * scale) : 1, cardBorder);
        SelectObject(memDC, cBrush);
        SelectObject(memDC, cPen);
        RoundRect(memDC, r.left, r.top, r.right, r.bottom, (int)(8 * scale), (int)(8 * scale));
        DeleteObject(cBrush);
        DeleteObject(cPen);

        /* Calculate hint badge size */
        SelectObject(memDC, g_hFontHint);
        SIZE textSize;
        GetTextExtentPoint32W(memDC, item->hint, (int)wcslen(item->hint), &textSize);
        int badgeW = textSize.cx + (int)(8 * scale);
        int minBadgeW = (int)(18 * scale);
        if (badgeW < minBadgeW) badgeW = minBadgeW;

        RECT badgeRect;
        if (showClose) {
            /* Close button visible: place cross at far right, hint badge to its left */
            item->closeRect.left = r.right - closeW - (int)(3 * scale);
            item->closeRect.top = r.top + padTop;
            item->closeRect.right = r.right - (int)(3 * scale);
            item->closeRect.bottom = r.top + padTop + headerBarH;

            COLORREF closeColor = isCloseHovered ? RGB(255, 95, 95) : RGB(225, 65, 65);
            SelectObject(memDC, g_hFontHint);
            SetTextColor(memDC, closeColor);
            DrawTextW(memDC, L"✕", -1, &item->closeRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            int badgeRight = item->closeRect.left - (int)(2 * scale);
            badgeRect.left = badgeRight - badgeW;
            badgeRect.top = r.top + padTop;
            badgeRect.right = badgeRight;
            badgeRect.bottom = r.top + padTop + headerBarH;
        } else {
            /* Close button hidden: hint badge sits flush at right edge with trimmed padding */
            SetRectEmpty(&item->closeRect);
            badgeRect.left = r.right - (int)(3 * scale) - badgeW;
            badgeRect.top = r.top + padTop;
            badgeRect.right = r.right - (int)(3 * scale);
            badgeRect.bottom = r.top + padTop + headerBarH;
        }

        /* Render Shortcut Hint Badge */
        COLORREF badgeBg = isSelected ? RGB(0, 120, 240) : (isHovered ? RGB(35, 85, 155) : RGB(44, 58, 80));
        HBRUSH badgeBrush = CreateSolidBrush(badgeBg);
        SelectObject(memDC, badgeBrush);
        SelectObject(memDC, GetStockObject(NULL_PEN));
        RoundRect(memDC, badgeRect.left, badgeRect.top, badgeRect.right, badgeRect.bottom, (int)(4 * scale), (int)(4 * scale));
        DeleteObject(badgeBrush);

        SetTextColor(memDC, RGB(255, 255, 255));
        SelectObject(memDC, g_hFontHint);
        DrawTextW(memDC, item->hint, -1, &badgeRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        /* App Name and Window Title */
        SelectObject(memDC, g_hFontTitle);
        SetTextColor(memDC, isSelected ? RGB(235, 242, 255) : RGB(175, 185, 198));

        wchar_t appTitleBuf[384];
        if (wcslen(item->title) > 0 && _wcsicmp(item->appName, item->title) != 0) {
            swprintf_s(appTitleBuf, 384, L"%s — %s", item->appName, item->title);
        } else {
            swprintf_s(appTitleBuf, 384, L"%s", item->appName);
        }

        RECT appHeaderRect = { r.left + (int)(5 * scale), r.top + padTop, badgeRect.left - (int)(4 * scale), r.top + padTop + headerBarH };
        DrawTextW(memDC, appTitleBuf, -1, &appHeaderRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

        if (!item->hThumbnail) {
            DwmRegisterThumbnail(hwnd, item->hwnd, &item->hThumbnail);
        }
        if (item->hThumbnail) {
            SIZE srcSize = { 0, 0 };
            RECT destRect = item->previewRect;

            if (FAILED(DwmQueryThumbnailSourceSize(item->hThumbnail, &srcSize)) || srcSize.cx <= 0 || srcSize.cy <= 0) {
                RECT rcWin = {0};
                if (GetWindowRect(item->hwnd, &rcWin)) {
                    srcSize.cx = rcWin.right - rcWin.left;
                    srcSize.cy = rcWin.bottom - rcWin.top;
                }
            }

            if (srcSize.cx > 0 && srcSize.cy > 0) {
                int destW = item->previewRect.right - item->previewRect.left;
                int destH = item->previewRect.bottom - item->previewRect.top;

                int fitW = destW;
                int fitH = (fitW * srcSize.cy) / srcSize.cx;

                if (fitH > destH) {
                    fitH = destH;
                    fitW = (fitH * srcSize.cx) / srcSize.cy;
                }

                if (fitW < 1) fitW = 1;
                if (fitH < 1) fitH = 1;

                int offsetX = item->previewRect.left + (destW - fitW) / 2;
                int offsetY = item->previewRect.top + (destH - fitH) / 2;

                destRect.left = offsetX;
                destRect.top = offsetY;
                destRect.right = offsetX + fitW;
                destRect.bottom = offsetY + fitH;
            }

            DWM_THUMBNAIL_PROPERTIES props;
            ZeroMemory(&props, sizeof(props));
            props.dwFlags = DWM_TNS_RECTDESTINATION | DWM_TNS_VISIBLE | DWM_TNS_OPACITY;
            props.rcDestination = destRect;
            props.fVisible = TRUE;
            props.opacity = 255;
            DwmUpdateThumbnailProperties(item->hThumbnail, &props);
        }
    }

    SelectObject(memDC, hOldFont);
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, hOldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}

#define TIMER_AUTO_ACTIVATE 101

#define WM_SWITCHER_ALT_UP       (WM_USER + 101)
#define WM_SWITCHER_TAB          (WM_USER + 102)
#define WM_SWITCHER_ESCAPE       (WM_USER + 103)
#define WM_SWITCHER_ACTIVATE     (WM_USER + 104)
#define WM_SWITCHER_ARROW        (WM_USER + 105)
#define WM_SWITCHER_BACKSPACE    (WM_USER + 106)
#define WM_SWITCHER_CHAR         (WM_USER + 107)

static HWND   g_pendingHwndToActivate = NULL;
static HHOOK  g_hKeyboardHook = NULL;

static void InvalidateOverlay(void);

static void CancelPendingAutoActivate(HWND hwndOverlay) {
    if (g_pendingHwndToActivate != NULL) {
        KillTimer(hwndOverlay, TIMER_AUTO_ACTIVATE);
        g_pendingHwndToActivate = NULL;
    }
}

static bool IsAltKeyDown(void) {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

static void TriggerAutoActivate(HWND hwndOverlay, HWND targetHwnd) {
    if (!IsWindow(targetHwnd)) return;

    if (g_cfg.delayMs <= 0) {
        if (IsAltKeyDown()) {
            g_pendingHwndToActivate = targetHwnd;
            KillTimer(hwndOverlay, TIMER_AUTO_ACTIVATE);
            return;
        }
        ActivateWindow(targetHwnd);
        PostQuitMessage(0);
        return;
    }

    /* Delayed activation: PAUSED while Alt is held down */
    if (IsAltKeyDown()) {
        g_pendingHwndToActivate = targetHwnd;
        KillTimer(hwndOverlay, TIMER_AUTO_ACTIVATE);
        return;
    }

    /* Resets and starts delay countdown timer */
    KillTimer(hwndOverlay, TIMER_AUTO_ACTIVATE);
    g_pendingHwndToActivate = targetHwnd;
    SetTimer(hwndOverlay, TIMER_AUTO_ACTIVATE, (UINT)g_cfg.delayMs, NULL);
}

/* --- Match Typed Input to Hints / Titles --- */
static void MatchTypedBuffer(HWND hwnd) {
    int dummyW, dummyH;
    ComputeLayout(hwnd, &dummyW, &dummyH);

    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);

    if (g_typedLen > 0) {
        /* 1. Auto-activate if typed buffer is an exact match for a single item's hint */
        int exactHintMatches = 0;
        int exactIdx = -1;
        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            if (_wcsicmp(g_items[i].hint, g_typedBuf) == 0) {
                exactHintMatches++;
                exactIdx = i;
            }
        }
        if (exactHintMatches == 1 && exactIdx != -1) {
            for (int v = 0; v < visibleCount; v++) {
                if (visibleIndices[v] == exactIdx) { g_selectedIndex = v; break; }
            }
            TriggerAutoActivate(hwnd, g_items[exactIdx].hwnd);
            return;
        }

        /* 2. Auto-activate if single unique matching window remains matching hint/app prefix */
        if (visibleCount == 1) {
            int idx = visibleIndices[0];
            if (_wcsnicmp(g_items[idx].hint, g_typedBuf, g_typedLen) == 0 ||
                _wcsnicmp(g_items[idx].appName, g_typedBuf, g_typedLen) == 0)
            {
                g_selectedIndex = 0;
                TriggerAutoActivate(hwnd, g_items[idx].hwnd);
                return;
            }
        }
    }

    if (visibleCount > 0) {
        if (g_selectedIndex >= visibleCount) {
            g_selectedIndex = 0;
        }
        int targetIdx = visibleIndices[g_selectedIndex];
        TriggerAutoActivate(hwnd, g_items[targetIdx].hwnd);
    } else {
        CancelPendingAutoActivate(hwnd);
    }
}

/* --- Unified Input Action Handlers --- */
static void HandleTab(HWND hwnd, bool shift) {
    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);
    int count = visibleCount > 0 ? visibleCount : 1;

    if (shift) {
        g_selectedIndex = (g_selectedIndex - 1 + count) % count;
    } else {
        g_selectedIndex = (g_selectedIndex + 1) % count;
    }
    if (visibleCount > 0 && g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
        TriggerAutoActivate(hwnd, g_items[visibleIndices[g_selectedIndex]].hwnd);
    }
    InvalidateOverlay();
}

static void HandleArrow(HWND hwnd, WPARAM vk) {
    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);
    int count = visibleCount > 0 ? visibleCount : 1;

    if (vk == VK_LEFT) {
        g_selectedIndex = (g_selectedIndex - 1 + count) % count;
    } else if (vk == VK_RIGHT) {
        g_selectedIndex = (g_selectedIndex + 1) % count;
    } else if (vk == VK_UP) {
        int cols = g_layoutCols > 0 ? g_layoutCols : 4;
        if (g_selectedIndex >= cols) {
            g_selectedIndex -= cols;
        } else {
            int lastRowStart = (count - 1) / cols * cols;
            int target = lastRowStart + (g_selectedIndex % cols);
            if (target >= count) target = count - 1;
            g_selectedIndex = target;
        }
    } else if (vk == VK_DOWN) {
        int cols = g_layoutCols > 0 ? g_layoutCols : 4;
        if (g_selectedIndex + cols < count) {
            g_selectedIndex += cols;
        } else {
            g_selectedIndex = g_selectedIndex % cols;
        }
    }

    if (visibleCount > 0 && g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
        TriggerAutoActivate(hwnd, g_items[visibleIndices[g_selectedIndex]].hwnd);
    }
    InvalidateOverlay();
}

static void HandleChar(HWND hwnd, wchar_t ch) {
    ch = (wchar_t)towupper(ch);
    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);

    if (g_typedLen == 1 && g_typedBuf[0] == ch && visibleCount > 1) {
        g_selectedIndex = (g_selectedIndex + 1) % visibleCount;
        if (g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
            TriggerAutoActivate(hwnd, g_items[visibleIndices[g_selectedIndex]].hwnd);
        }
        InvalidateOverlay();
        return;
    }

    if (g_typedLen < 30) {
        g_typedBuf[g_typedLen++] = ch;
        g_typedBuf[g_typedLen] = L'\0';
        MatchTypedBuffer(hwnd);
        InvalidateOverlay();
    }
}

static void HandleBackspace(HWND hwnd) {
    if (g_typedLen > 0) {
        g_typedBuf[--g_typedLen] = L'\0';
        MatchTypedBuffer(hwnd);
        InvalidateOverlay();
    }
}

static void HandleAltUp(HWND hwnd) {
    int visibleIndices[MAX_WINDOWS];
    int visibleCount = 0;
    GetFilteredIndices(visibleIndices, &visibleCount);
    if (visibleCount > 0 && g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
        g_pendingHwndToActivate = g_items[visibleIndices[g_selectedIndex]].hwnd;
    }

    if (g_pendingHwndToActivate && IsWindow(g_pendingHwndToActivate)) {
        if (g_cfg.delayMs <= 0) {
            ActivateWindow(g_pendingHwndToActivate);
            PostQuitMessage(0);
            return;
        }
        KillTimer(hwnd, TIMER_AUTO_ACTIVATE);
        SetTimer(hwnd, TIMER_AUTO_ACTIVATE, (UINT)g_cfg.delayMs, NULL);
    }
}

/* --- Low-Level Keyboard Hook --- */
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_hwndOverlay) {
        PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT)lParam;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isKeyUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        if (isKeyUp && (p->vkCode == VK_MENU || p->vkCode == VK_LMENU || p->vkCode == VK_RMENU)) {
            PostMessageW(g_hwndOverlay, WM_SWITCHER_ALT_UP, 0, 0);
        } else if (isKeyDown) {
            if (p->vkCode == VK_TAB) {
                bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                PostMessageW(g_hwndOverlay, WM_SWITCHER_TAB, (WPARAM)shift, 0);
                return 1;
            } else if (p->vkCode == VK_ESCAPE) {
                PostMessageW(g_hwndOverlay, WM_SWITCHER_ESCAPE, 0, 0);
                return 1;
            } else if (p->vkCode == VK_RETURN || p->vkCode == VK_SPACE) {
                PostMessageW(g_hwndOverlay, WM_SWITCHER_ACTIVATE, 0, 0);
                return 1;
            } else if (p->vkCode == VK_LEFT || p->vkCode == VK_RIGHT || p->vkCode == VK_UP || p->vkCode == VK_DOWN) {
                PostMessageW(g_hwndOverlay, WM_SWITCHER_ARROW, (WPARAM)p->vkCode, 0);
                return 1;
            } else if (p->vkCode == VK_BACK) {
                PostMessageW(g_hwndOverlay, WM_SWITCHER_BACKSPACE, 0, 0);
                return 1;
            } else if ((p->vkCode >= 'A' && p->vkCode <= 'Z') || (p->vkCode >= '0' && p->vkCode <= '9') || p->vkCode == VK_OEM_PLUS) {
                PostMessageW(g_hwndOverlay, WM_SWITCHER_CHAR, (WPARAM)p->vkCode, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

/* --- Overlay Window Procedure --- */
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static ULONGLONG g_startTime = 0;
    static ULONGLONG g_lastCloseTime = 0;
    static bool      g_hasBeenActivated = false;

    switch (msg) {
    case WM_CREATE:
        g_startTime        = GetTickCount64();
        g_lastCloseTime    = 0;
        g_hasBeenActivated = false;
        UpdateFontsForDpi(96);
        return 0;

    case WM_DPICHANGED: {
        UINT newDpi = LOWORD(wParam);
        UpdateFontsForDpi(newDpi);
        int w = 0, h = 0;
        ComputeLayout(hwnd, &w, &h);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintOverlay(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int oldHover = g_hoverIndex;
        int oldHoverClose = g_hoverCloseIndex;
        g_hoverIndex = -1;
        g_hoverCloseIndex = -1;

        int visibleIndices[MAX_WINDOWS];
        int visibleCount = 0;
        GetFilteredIndices(visibleIndices, &visibleCount);

        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            if (PtInRect(&g_items[i].closeRect, (POINT){x, y})) {
                g_hoverCloseIndex = v;
                g_hoverIndex = v;
                break;
            } else if (PtInRect(&g_items[i].cardRect, (POINT){x, y})) {
                g_hoverIndex = v;
                break;
            }
        }
        if (g_hoverIndex != oldHover || g_hoverCloseIndex != oldHoverClose) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT && g_hoverCloseIndex >= 0) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == TIMER_AUTO_ACTIVATE) {
            KillTimer(hwnd, TIMER_AUTO_ACTIVATE);
            if (IsAltKeyDown()) {
                /* Still holding Alt -> remain paused */
                return 0;
            }
            if (g_pendingHwndToActivate && IsWindow(g_pendingHwndToActivate)) {
                ActivateWindow(g_pendingHwndToActivate);
                PostQuitMessage(0);
            }
            g_pendingHwndToActivate = NULL;
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (GetTickCount64() - g_startTime < 250) return 0;

        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        int visibleIndices[MAX_WINDOWS];
        int visibleCount = 0;
        GetFilteredIndices(visibleIndices, &visibleCount);

        /* 1. Check if user clicked the Red Cross close icon */
        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            if (PtInRect(&g_items[i].closeRect, (POINT){x, y})) {
                HWND targetCloseHwnd = g_items[i].hwnd;
                DWORD targetPid = g_items[i].processId;
                bool isShiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                CancelPendingAutoActivate(hwnd);
                g_lastCloseTime = GetTickCount64();

                if (isShiftHeld) {
                    /* Shift + Click: Force terminate process immediately */
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, targetPid);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    } else {
                        EndTask(targetCloseHwnd, FALSE, TRUE);
                    }
                } else {
                    /* Normal Click: Graceful close */
                    DWORD_PTR dwResult = 0;
                    SendMessageTimeoutW(targetCloseHwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 100, &dwResult);
                }

                if (g_items[i].hThumbnail) {
                    DwmUnregisterThumbnail(g_items[i].hThumbnail);
                    g_items[i].hThumbnail = NULL;
                }

                for (int k = i; k < g_itemCount - 1; k++) {
                    g_items[k] = g_items[k + 1];
                }
                g_itemCount--;

                if (g_itemCount == 0) {
                    CancelPendingAutoActivate(hwnd);
                    PostQuitMessage(0);
                    return 0;
                }

                ComputeAppHints();
                int dummyW, dummyH;
                ComputeLayout(hwnd, &dummyW, &dummyH);

                int newVisibleIndices[MAX_WINDOWS];
                int newVisibleCount = 0;
                GetFilteredIndices(newVisibleIndices, &newVisibleCount);

                if (g_selectedIndex >= newVisibleCount) {
                    g_selectedIndex = newVisibleCount > 0 ? (newVisibleCount - 1) : 0;
                }

                /* Re-assert switcher foreground focus and do not auto-activate on close */
                SetForegroundWindow(hwnd);
                g_hoverCloseIndex = -1;
                g_hoverIndex = -1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        /* 2. Check if user clicked the card container -> activate window */
        for (int v = 0; v < visibleCount; v++) {
            int i = visibleIndices[v];
            if (PtInRect(&g_items[i].cardRect, (POINT){x, y})) {
                g_selectedIndex = v;
                CancelPendingAutoActivate(hwnd);
                ActivateWindow(g_items[i].hwnd);
                PostQuitMessage(0);
                return 0;
            }
        }

        /* 3. Click outside overlay closes switcher */
        CancelPendingAutoActivate(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    case WM_SWITCHER_TAB:
        HandleTab(hwnd, (bool)wParam);
        return 0;

    case WM_SWITCHER_ESCAPE:
        CancelPendingAutoActivate(hwnd);
        PostQuitMessage(0);
        return 0;

    case WM_SWITCHER_ACTIVATE: {
        CancelPendingAutoActivate(hwnd);
        int visibleIndices[MAX_WINDOWS];
        int visibleCount = 0;
        GetFilteredIndices(visibleIndices, &visibleCount);
        if (visibleCount > 0 && g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
            ActivateWindow(g_items[visibleIndices[g_selectedIndex]].hwnd);
        }
        PostQuitMessage(0);
        return 0;
    }

    case WM_SWITCHER_ARROW:
        HandleArrow(hwnd, wParam);
        return 0;

    case WM_SWITCHER_BACKSPACE:
        HandleBackspace(hwnd);
        return 0;

    case WM_SWITCHER_CHAR:
        HandleChar(hwnd, (wchar_t)wParam);
        return 0;

    case WM_SWITCHER_ALT_UP:
        HandleAltUp(hwnd);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (wParam == VK_ESCAPE) {
            CancelPendingAutoActivate(hwnd);
            PostQuitMessage(0);
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            CancelPendingAutoActivate(hwnd);
            int visibleIndices[MAX_WINDOWS];
            int visibleCount = 0;
            GetFilteredIndices(visibleIndices, &visibleCount);
            if (g_selectedIndex >= 0 && g_selectedIndex < visibleCount) {
                ActivateWindow(g_items[visibleIndices[g_selectedIndex]].hwnd);
            }
            PostQuitMessage(0);
            return 0;
        }
        if (wParam == VK_TAB) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            HandleTab(hwnd, shift);
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
            HandleArrow(hwnd, wParam);
            return 0;
        }
        if (wParam == VK_BACK) {
            HandleBackspace(hwnd);
            return 0;
        }
        if (IsAltKeyDown() && ((wParam >= 'A' && wParam <= 'Z') || (wParam >= '0' && wParam <= '9') || wParam == VK_OEM_PLUS)) {
            HandleChar(hwnd, (wchar_t)wParam);
            return 0;
        }
        return 0;
    }

    case WM_CHAR: {
        wchar_t ch = (wchar_t)wParam;
        if (ch >= 32 && ch < 127) {
            HandleChar(hwnd, ch);
        }
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) {
            HandleAltUp(hwnd);
        }
        return 0;
    }

    case WM_ACTIVATE: {
        WORD activeState = LOWORD(wParam);
        if (activeState == WA_ACTIVE || activeState == WA_CLICKACTIVE) {
            g_hasBeenActivated = true;
        } else if (activeState == WA_INACTIVE) {
            if (GetTickCount64() - g_lastCloseTime < 600) {
                /* Window just closed in background; reclaim focus and keep overlay open */
                SetForegroundWindow(hwnd);
                return 0;
            }
            if (!IsAltKeyDown() && g_hasBeenActivated && (GetTickCount64() - g_startTime > 350)) {
                CancelPendingAutoActivate(hwnd);
                PostQuitMessage(0);
            }
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_hKeyboardHook) {
            UnhookWindowsHookEx(g_hKeyboardHook);
            g_hKeyboardHook = NULL;
        }
        CancelPendingAutoActivate(hwnd);
        CleanupThumbnails();
        if (g_hFontHint) DeleteObject(g_hFontHint);
        if (g_hFontHintLarge) DeleteObject(g_hFontHintLarge);
        if (g_hFontTitle) DeleteObject(g_hFontTitle);
        if (g_hFontGroup) DeleteObject(g_hFontGroup);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* --- Show Help --- */
static void ShowHelp(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut && hOut != INVALID_HANDLE_VALUE) {
            const char *helpText =
                "Tiny Window Switcher v1.0\n"
                "Usage: window-switcher.exe [options]\n\n"
                "Options:\n"
                "  -l, --layout <mode>      Layout mode: center or full (default: center)\n"
                "  -s, --sort <mode>        Sorting mode: recent or name (default: name)\n"
                "  -bg, --background <mode> Background effect: blur or tint (default: blur)\n"
                "  --blur <amount>          Blur intensity (1..100, default: 20)\n"
                "  --tint-color <color>     Tint hex color e.g. #000000 or #00000005 (default: #000000)\n"
                "  -d, --delay <ms>         Auto-activation delay in milliseconds (0..5000, default: 300)\n"
                "  -h, --help               Show this help message\n"
                "  -v, --version            Show version information\n";
            DWORD written = 0;
            WriteFile(hOut, helpText, (DWORD)strlen(helpText), &written, NULL);
        }
        FreeConsole();
    } else {
        MessageBoxW(NULL,
            L"Tiny Window Switcher v1.0\n\n"
            L"Usage: window-switcher.exe [options]\n\n"
            L"Options:\n"
            L"  -l, --layout <mode>      Layout mode: center or full (default: center)\n"
            L"  -s, --sort <mode>        Sorting mode: recent or name (default: name)\n"
            L"  -bg, --background <mode> Background effect: blur or tint (default: blur)\n"
            L"  --blur <amount>          Blur intensity (1..100, default: 20)\n"
            L"  --tint-color <color>     Tint hex color e.g. #000000 or #00000005 (default: #000000)\n"
            L"  -d, --delay <ms>         Auto-activation delay in milliseconds (0..5000, default: 300)\n"
            L"  -h, --help               Show this help message\n"
            L"  -v, --version            Show version information",
            L"Tiny Window Switcher — Help",
            MB_OK | MB_ICONINFORMATION);
    }
}

/* --- Parse CLI Arguments --- */
static bool ParseCLI(void) {
    const wchar_t *layoutStr = NULL;
    const wchar_t *sortStr = NULL;
    const wchar_t *bgModeStr = NULL;
    const wchar_t *tintColorStr = NULL;
    bool showHelp = false;
    bool showVersion = false;

    CliOption options[] = {
        { L"--layout",        CLI_OPT_STRING, &layoutStr,         0, 0 },
        { L"-l",              CLI_OPT_STRING, &layoutStr,         0, 0 },
        { L"--sort",         CLI_OPT_STRING, &sortStr,           0, 0 },
        { L"-s",             CLI_OPT_STRING, &sortStr,           0, 0 },
        { L"--background",   CLI_OPT_STRING, &bgModeStr,         0, 0 },
        { L"-bg",            CLI_OPT_STRING, &bgModeStr,         0, 0 },
        { L"--blur",         CLI_OPT_INT,    &g_cfg.blurAmount,  1, 100 },
        { L"--tint-color",   CLI_OPT_STRING, &tintColorStr,      0, 0 },
        { L"--delay",        CLI_OPT_INT,    &g_cfg.delayMs,     0, 5000 },
        { L"-d",             CLI_OPT_INT,    &g_cfg.delayMs,     0, 5000 },
        { L"--help",         CLI_OPT_BOOL,   &showHelp,          0, 0 },
        { L"-h",             CLI_OPT_BOOL,   &showHelp,          0, 0 },
        { L"--version",      CLI_OPT_BOOL,   &showVersion,       0, 0 },
        { L"-v",             CLI_OPT_BOOL,   &showVersion,       0, 0 },
    };

    TinyCLI_ParseCommandLine(options, sizeof(options)/sizeof(options[0]));

    if (showHelp || showVersion) {
        ShowHelp();
        return false;
    }

    if (layoutStr) {
        if (_wcsicmp(layoutStr, L"full") == 0) g_cfg.layout = LAYOUT_FULL;
        else if (_wcsicmp(layoutStr, L"center") == 0) g_cfg.layout = LAYOUT_CENTER;
        else if (_wcsicmp(layoutStr, L"tile") == 0) g_cfg.layout = LAYOUT_CENTER;
    }

    if (sortStr) {
        if (_wcsicmp(sortStr, L"recent") == 0) g_cfg.sort = SORT_RECENT;
        else if (_wcsicmp(sortStr, L"mru") == 0) g_cfg.sort = SORT_RECENT;
        else if (_wcsicmp(sortStr, L"name") == 0) g_cfg.sort = SORT_NAME;
    }

    if (bgModeStr) {
        if (_wcsicmp(bgModeStr, L"blur") == 0) g_cfg.bgMode = BG_BLUR;
        else if (_wcsicmp(bgModeStr, L"tint") == 0) g_cfg.bgMode = BG_TINT;
    }

    if (tintColorStr) {
        ParseHexColorWithAlpha(tintColorStr, &g_cfg.tintColor, &g_cfg.tintAlpha);
        if (!bgModeStr) {
            g_cfg.bgMode = BG_TINT;
        }
    }

    return true;
}

/* --- Invalidate Overlay --- */
static void InvalidateOverlay(void) {
    if (g_hwndOverlay) InvalidateRect(g_hwndOverlay, NULL, FALSE);
}

/* --- Entry Point --- */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    TinyDPI_EnablePerMonitorAwareness();

    if (!ParseCLI()) {
        return 0;
    }

    HANDLE hEvent = NULL;
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &hEvent)) {
        /* Already running -> toggle event sent -> exit */
        return 0;
    }

    /* Detect if Alt key is currently held down when launched */
    g_altTabMode = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    /* Enumerate open windows */
    g_itemCount = 0;
    EnumWindows(EnumWindowsProc, 0);

    if (g_itemCount == 0) {
        CloseHandle(hEvent);
        return 0;
    }

    /* Apply Sorting */
    if (g_cfg.sort == SORT_NAME) {
        qsort(g_items, g_itemCount, sizeof(WindowItem), CompareName);
    } else {
        qsort(g_items, g_itemCount, sizeof(WindowItem), CompareMRU);
    }

    /* Compute Shortest Unique Prefix Hints */
    ComputeAppHints();

    /* Register Overlay Window Class */
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"TinyWindowSwitcherClass";
    RegisterClassExW(&wc);

    int overlayW = 800;
    int overlayH = 500;

    g_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Tiny Window Switcher",
        WS_POPUP | WS_VISIBLE,
        0, 0, overlayW, overlayH,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndOverlay) {
        CloseHandle(hEvent);
        return 0;
    }

    ApplyBackgroundComposition(g_hwndOverlay);

    ComputeLayout(g_hwndOverlay, &overlayW, &overlayH);
    SetForegroundWindow(g_hwndOverlay);

    /* Selection defaults to second item if Alt+Tab mode, else first item */
    if (g_altTabMode && g_itemCount > 1) {
        g_selectedIndex = 1;
    } else {
        g_selectedIndex = 0;
    }

    InvalidateOverlay();

    /* Trigger initial auto-activate timer on launch */
    if (g_itemCount > 0 && g_selectedIndex >= 0 && g_selectedIndex < g_itemCount) {
        TriggerAutoActivate(g_hwndOverlay, g_items[g_selectedIndex].hwnd);
    }

    /* Install session-lifetime low-level keyboard hook */
    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    /* Message & IPC Loop */
    MSG msg;
    while (1) {
        DWORD waitRes = MsgWaitForMultipleObjectsEx(1, &hEvent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitRes == WAIT_OBJECT_0) {
            if (IsAltKeyDown()) {
                /* Alt is held: treat IPC toggle as Tab / advance selection */
                HandleTab(g_hwndOverlay, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                continue;
            }
            /* Alt is not held -> user explicitly launched toggle to dismiss */
            break;
        }

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto Cleanup;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

Cleanup:
    if (g_hKeyboardHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = NULL;
    }
    if (g_hwndOverlay) {
        DestroyWindow(g_hwndOverlay);
    }
    UnregisterClassW(wc.lpszClassName, hInstance);
    CloseHandle(hEvent);

    return 0;
}
