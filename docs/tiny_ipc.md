# TinyIPC Common Module (`src/common/tiny_ipc.h`)

Single-instance mutex and event toggle IPC pattern helper for instant overlay tools.

## Overview
All overlay utilities follow a zero-daemon single-instance toggle contract:
- **First Launch**: Creates a named global mutex (`Global\Tiny<Tool>Mutex`) and a named auto-reset event (`Global\Tiny<Tool>Event`). Returns `true` so the utility launches its overlay and message loop.
- **Second Launch**: Detects existing mutex (`ERROR_ALREADY_EXISTS`), signals the named toggle event (`SetEvent`), and exits immediately with code `0`.
- **Toggle Listener**: The running primary instance monitors the named event (via `MsgWaitForMultipleObjectsEx` or a timer loop) and cleanly terminates upon receiving the signal.

## API Reference

### Functions
- `bool TinyIPC_AcquireOrToggle(const wchar_t *mutexName, const wchar_t *eventName, HANDLE *outEvent)`
  - `mutexName`: Named Win32 Mutex string (e.g. `L"Global\\TinyCaptureMutex"`).
  - `eventName`: Named Win32 Event string (e.g. `L"Global\\TinyCaptureEvent"`).
  - `outEvent`: Pointer to `HANDLE` where the created event handle is stored for the primary instance.
  - **Returns**: `true` if primary instance (proceed with execution); `false` if secondary toggle instance (event signaled, caller should exit immediately).
