#ifndef TINY_IPC_H
#define TINY_IPC_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

/*
 * TinyIPC_AcquireOrToggle:
 * Single-instance toggle pattern implementation.
 *
 * If another instance is already running:
 *   - Signals the named event so the running instance toggles off/on.
 *   - Returns false (caller should exit immediately with 0).
 * If this is the primary instance:
 *   - Creates the named auto-reset event and outputs it to *outEvent.
 *   - Returns true (caller runs message loop).
 */
static inline bool TinyIPC_AcquireOrToggle(const wchar_t *mutexName, const wchar_t *eventName, HANDLE *outEvent) {
    HANDLE hMutex = CreateMutexW(NULL, FALSE, mutexName);
    if (!hMutex) {
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (hEvent) {
            SetEvent(hEvent);
            CloseHandle(hEvent);
        }
        CloseHandle(hMutex);
        return false;
    }

    HANDLE hEvent = CreateEventW(NULL, FALSE, FALSE, eventName);
    if (outEvent) {
        *outEvent = hEvent;
    }
    return true;
}

#endif /* TINY_IPC_H */
