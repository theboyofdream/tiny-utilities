#ifndef TINY_CLIPBOARD_H
#define TINY_CLIPBOARD_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool TinyClipboard_OpenWithRetry(HWND hwndOwner, int maxTries, DWORD delayMs) {
    for (int i = 0; i < maxTries; i++) {
        if (OpenClipboard(hwndOwner)) {
            return true;
        }
        Sleep(delayMs);
    }
    return false;
}

static inline bool TinyClipboard_SetText(const wchar_t *text) {
    if (!text) return false;
    if (!TinyClipboard_OpenWithRetry(NULL, 5, 10)) return false;

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    size_t len = wcslen(text);
    size_t bytes = (len + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    wchar_t *pDst = (wchar_t *)GlobalLock(hMem);
    if (!pDst) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    memcpy(pDst, text, bytes);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

static inline bool TinyClipboard_GetText(wchar_t *buffer, size_t maxChars) {
    if (!buffer || maxChars == 0) return false;
    buffer[0] = L'\0';

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!TinyClipboard_OpenWithRetry(NULL, 5, 10)) return false;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return false;
    }

    const wchar_t *pSrc = (const wchar_t *)GlobalLock(hData);
    if (!pSrc) {
        CloseClipboard();
        return false;
    }

    wcsncpy_s(buffer, maxChars, pSrc, _TRUNCATE);
    GlobalUnlock(hData);
    CloseClipboard();
    return true;
}

static inline wchar_t *TinyClipboard_GetTextAlloc(void) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return NULL;
    if (!TinyClipboard_OpenWithRetry(NULL, 5, 10)) return NULL;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return NULL;
    }

    const wchar_t *pSrc = (const wchar_t *)GlobalLock(hData);
    if (!pSrc) {
        CloseClipboard();
        return NULL;
    }

    size_t len = wcslen(pSrc);
    wchar_t *buffer = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (buffer) {
        memcpy(buffer, pSrc, (len + 1) * sizeof(wchar_t));
    }

    GlobalUnlock(hData);
    CloseClipboard();
    return buffer;
}

static inline bool TinyClipboard_SetBitmap(HWND hwndOwner, HBITMAP hbmp) {
    if (!hbmp) return false;
    if (!TinyClipboard_OpenWithRetry(hwndOwner, 5, 10)) return false;

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    HBITMAP hCopy = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0, 0);
    if (hCopy) {
        SetClipboardData(CF_BITMAP, hCopy);
    }

    CloseClipboard();
    return hCopy != NULL;
}

static inline bool TinyClipboard_HasText(void) {
    return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT);
}

static inline bool TinyClipboard_HasFiles(void) {
    return IsClipboardFormatAvailable(CF_HDROP);
}

#ifdef __cplusplus
}
#endif

#endif /* TINY_CLIPBOARD_H */
