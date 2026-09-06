# Clipboard Common Module (`src/common/clipboard.h`)

Header-only Win32 Clipboard integration for Unicode text, bitmaps, and file drops with retry-locking resilience.

## Overview
Interacting with the Windows Clipboard (`OpenClipboard`) can occasionally fail if another background application or clipboard manager currently holds the lock. `clipboard.h` wraps all clipboard operations in retry loops (`TinyClipboard_OpenWithRetry`) to ensure reliable operations across interactive utilities.

## API Reference

### Functions
- `bool TinyClipboard_SetText(const wchar_t *text)`
  Empties the clipboard and copies a null-terminated UTF-16 Unicode string using `CF_UNICODETEXT` format.
- `bool TinyClipboard_GetText(wchar_t *buffer, size_t maxChars)`
  Reads UTF-16 Unicode text from the clipboard into a fixed buffer. Returns `true` on success.
- `wchar_t *TinyClipboard_GetTextAlloc(void)`
  Allocates a new heap string containing the clipboard's Unicode text. Caller is responsible for freeing with `free()`.
- `bool TinyClipboard_SetBitmap(HWND hwndOwner, HBITMAP hbmp)`
  Copies an `HBITMAP` image onto the clipboard (`CF_BITMAP`) using `CopyImage`.
- `bool TinyClipboard_HasText(void)`
  Checks if `CF_UNICODETEXT` or `CF_TEXT` is present on the clipboard.
- `bool TinyClipboard_HasFiles(void)`
  Checks if `CF_HDROP` (file drop list) is present on the clipboard.
