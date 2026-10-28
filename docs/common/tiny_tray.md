# TinyTray Common Module (`src/common/tiny_tray.h`)

Notification-area icon host for the console server tools (`kyper`, `whisper-cast`).

## Overview
- **Zero assets, zero heap**: the 16x16 icon is generated at runtime from raw 32bpp pixels (flat colored disc with a darker rim and a 1 px transparent margin), so tools stay single-`.c` files with no `.rc` resources.
- **Hidden host window**: a `WS_EX_TOOLWINDOW` message window is created on the calling thread and is never shown (zero size, no activate). Only the notification icon is visible.
- **Exit paths**: right-click opens a minimal `Exit` menu; double-click also exits. Both invoke the `onExit` callback supplied at creation.
- **Explorer restarts**: the icon is re-added on the `TaskbarCreated` broadcast.
- **Hard-rule note (user-approved 2026-10-26)**: the host window is a GUI window by API necessity, but it is never displayed.

## API Reference

### Constants
- `TINYTRAY_MSG_EXIT` (`WM_APP + 0x7A21`) — send to the tray host window to run the exact menu-exit code path (used by smoke tests and programmatic shutdown).
- `TINYTRAY_TIP_CAP` (128) — tooltip buffer capacity.
- `TINYTRAY_ID` (1) — icon ID.

### Functions
- `bool TinyTray_Create(const wchar_t *className, const wchar_t *tooltip, const wchar_t *balloonTitle, const wchar_t *balloonText, COLORREF iconRgb, void (*onExit)(void))`
  - Registers the host window class, creates the hidden window, and adds the icon. Idempotent: returns `true` if the icon already exists.
  - `iconRgb` colors the generated disc; `onExit` runs on menu Exit, double-click, or `TINYTRAY_MSG_EXIT`.
- `void TinyTray_PumpMessages(void)` — dispatches pending window messages. The tool's wait loop must call this whenever `MsgWaitForMultipleObjects` reports input, otherwise the right-click menu cannot appear.
- `void TinyTray_Destroy(void)` — removes the icon and destroys the host window (call before network teardown).

## Usage Notes
- The tray is optional at runtime: if `TinyTray_Create` fails, tools log "tray icon unavailable" and keep running headless.
- Because the host window lives on the main thread, `MsgWaitForMultipleObjects` (not plain `WaitForMultipleObjects`) must drive the loop.
