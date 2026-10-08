#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "common/tiny_cli.h"
#include "common/tiny_dpi.h"
#include "common/font.h"
#include "common/clipboard.h"

#define MUTEX_NAME L"Global\\TinyIPSendMutex"
#define EVENT_NAME L"Global\\TinyIPSendEvent"

#define MAX_PAYLOADS 64
#define MAX_RECIPIENTS 128
#define MAX_PAYLOAD_PATH 1024
#define SEND_INLINE_MAX 2048
#define SEND_WAIT_MS 4000
#define MAX_SEND_CMDLINE 32768
// Pasted message bodies live on the heap so multi-megabyte pastes are not clipped.
#define MESSAGE_PREVIEW_CHARS 2048
// Largest body we are willing to load into the native multiline EDIT control.
// Beyond this the control becomes unusably slow to paint and scroll, so large
// bodies stay send-only and are displayed as a size summary instead.
#define MESSAGE_EDIT_MAX_CHARS 65536
#define MAX_MESSAGE_CHARS (64u * 1024u * 1024u) // 64M chars = 128MB wchar ceiling
#define MSGFILE_TMP_TEMPLATE L"tiny-ip-send-msg.tmp"

typedef struct {
    wchar_t path[MAX_PAYLOAD_PATH];
    wchar_t displayName[MAX_PATH];
} PayloadItem;

typedef struct {
    wchar_t name[128];
    wchar_t hostName[128];
    wchar_t ipAddr[64];
    wchar_t uid[128];
    bool active;
    bool selected;
} RecipientItem;

// Growable wide-character string used for the message body and for building the
// ipcmd command line. The body must be heap-allocated: pasted content can be
// multiple megabytes, which no fixed inline array can hold.
typedef struct {
    wchar_t* data;
    size_t len;
    size_t cap;
    size_t limit; // hard ceiling on len; 0 means MAX_SEND_CMDLINE
    bool failed;
} StrBuf;

typedef struct {
    PayloadItem payloads[MAX_PAYLOADS];
    int payloadCount;

    StrBuf message;          // authoritative message body (heap, unbounded-ish)
    bool messageMode; // True when editing message (i key)

    wchar_t toArg[256];

    wchar_t searchQuery[64];
    int searchLen;

    RecipientItem recipients[MAX_RECIPIENTS];
    int recipientCount;

    int filteredIndices[MAX_RECIPIENTS];
    int filteredCount;
    int highlightedFilteredIdx;
    int scrollOffset;
    int visibleRows;
    bool isRefreshing;

    wchar_t statusText[256];
    DWORD statusTime;
    bool sending;

    bool autoSend;
    bool registerContextMenu;
    bool unregisterContextMenu;
} AppState;

static AppState g_state = { 0 };
static HWND g_hWnd = NULL;
static HWND g_hEdit = NULL;
static HBRUSH g_hEditBrush = NULL;
static RECT g_msgEditRect = { 0 };
static WNDPROC g_oldEditProc = NULL;
static HANDLE g_hMutex = NULL;
static HANDLE g_hEvent = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontCaption = NULL;
// Set by any WM_KEYDOWN shortcut we handle ourselves so the synthesized WM_CHAR
// (Ctrl+V -> 'v', Ctrl+O -> 'o', ...) never leaks into the recipient search box.
static bool g_skipNextChar = false;
static bool g_lightTheme = false;
static bool g_themeManuallyToggled = false;
static RECT g_sendRect = { 0 };
static int g_listStartY = 0;
static int g_itemHeight = 24;

// ---------------------------------------------------------------------------
// StrBuf: growable wide-string used for the message body and the send command.
// ---------------------------------------------------------------------------
static void StrBuf_InitLimit(StrBuf* sb, size_t limit) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->limit = limit;
    sb->failed = false;
}

static void StrBuf_Init(StrBuf* sb) {
    StrBuf_InitLimit(sb, MAX_SEND_CMDLINE);
}

static void StrBuf_Free(StrBuf* sb) {
    if (sb->data) free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static bool StrBuf_Reserve(StrBuf* sb, size_t extra) {
    if (sb->failed) return false;
    size_t limit = sb->limit ? sb->limit : MAX_SEND_CMDLINE;
    if (extra > limit || sb->len > limit - extra) { sb->failed = true; return false; }

    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return true;

    size_t newCap = sb->cap ? sb->cap : 256;
    while (newCap < need) {
        if (newCap > limit / 2) { newCap = need; break; }
        newCap *= 2;
    }
    wchar_t* p = (wchar_t*)realloc(sb->data, newCap * sizeof(wchar_t));
    if (!p) { sb->failed = true; return false; }
    sb->data = p;
    sb->cap = newCap;
    return true;
}

static void StrBuf_AppendN(StrBuf* sb, const wchar_t* s, size_t n) {
    if (!s || sb->failed) return;
    if (!StrBuf_Reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n * sizeof(wchar_t));
    sb->len += n;
    sb->data[sb->len] = L'\0';
}

static void StrBuf_Append(StrBuf* sb, const wchar_t* s) {
    if (!s || sb->failed) return;
    StrBuf_AppendN(sb, s, wcslen(s));
}

static void StrBuf_AppendChar(StrBuf* sb, wchar_t c) {
    if (sb->failed) return;
    if (!StrBuf_Reserve(sb, 1)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = L'\0';
}

static void ShowStatusToast(HWND hWnd, const wchar_t* text) {
    if (!text) return;
    wcscpy_s(g_state.statusText, 256, text);
    g_state.statusTime = GetTickCount();
    if (hWnd) {
        SetTimer(hWnd, 200, 3000, NULL); // Auto-clear status toast after 3 seconds
        InvalidateRect(hWnd, NULL, FALSE);
    }
}

static bool IsSystemLightTheme(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size = sizeof(DWORD);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (value != 0);
        }
        RegCloseKey(hKey);
    }
    return false;
}

static bool FindIPMsgExecutable(wchar_t* outPath, DWORD maxLen);

// Helper: Extract filename from path
static const wchar_t* GetFileNameFromPath(const wchar_t* path) {
    const wchar_t* p = wcsrchr(path, L'\\');
    if (!p) p = wcsrchr(path, L'/');
    return p ? (p + 1) : path;
}

// Add a file payload item. Returns true when an entry was attached or was
// already attached (duplicate); false when rejected (missing / too long / list full).
static bool AddFilePayload(const wchar_t* path) {
    if (!path || path[0] == L'\0') return false;
    if (g_state.payloadCount >= MAX_PAYLOADS) return false;
    if (wcslen(path) >= MAX_PAYLOAD_PATH) return false;
    // Stale clipboard / shell entries can reference files that no longer exist.
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;

    for (int i = 0; i < g_state.payloadCount; i++) {
        if (_wcsicmp(g_state.payloads[i].path, path) == 0) return true; // avoid duplicates
    }
    PayloadItem* item = &g_state.payloads[g_state.payloadCount++];
    wcscpy_s(item->path, MAX_PAYLOAD_PATH, path);
    wcscpy_s(item->displayName, MAX_PATH, GetFileNameFromPath(path));
    return true;
}

// Reads every file from an HDROP into the payload list.
// NOTE: the CF_HDROP handle from GetClipboardData is already a valid HDROP and must
// NOT be passed to GlobalLock - GlobalLock returns NULL for it, which silently
// dropped every pasted file.
static int CollectFilesFromDrop(HDROP hd) {
    if (!hd) return 0;
    int added = 0;
    UINT count = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < count; i++) {
        UINT need = DragQueryFileW(hd, i, NULL, 0); // required chars, excluding NUL
        if (need == 0) continue;
        wchar_t* buf = (wchar_t*)malloc(((size_t)need + 1) * sizeof(wchar_t));
        if (!buf) continue;
        if (DragQueryFileW(hd, i, buf, need + 1) > 0) {
            if (AddFilePayload(buf)) added++;
        }
        free(buf);
    }
    return added;
}

// Appends text into the message body. Pasted text must land in the body, otherwise
// it is only shown in the payload list and never actually dispatched by ipcmd.
// Accepts multi-megabyte input; the body is heap-backed so nothing is clipped.
static void AppendMessageText(const wchar_t* text) {
    if (!text || text[0] == L'\0') return;

    if (g_state.message.len > 0) {
        StrBuf_AppendChar(&g_state.message, L'\r');
        StrBuf_AppendChar(&g_state.message, L'\n');
    }
    StrBuf_Append(&g_state.message, text);
}

// Replaces the whole message body (used for CLI --message and edit-box sync).
static void SetMessageText(const wchar_t* text) {
    StrBuf_InitLimit(&g_state.message, MAX_MESSAGE_CHARS);
    StrBuf_Append(&g_state.message, text ? text : L"");
}

// Copies the current body into the native edit box for in-place editing.
static void SyncEditFromMessage(void) {
    if (g_hEdit && IsWindow(g_hEdit)) {
        SetWindowTextW(g_hEdit, g_state.message.data ? g_state.message.data : L"");
    }
}

// Reads the edit box back into the authoritative body.
static void SyncMessageFromEdit(void) {
    if (!g_hEdit || !IsWindow(g_hEdit)) return;
    int len = GetWindowTextLengthW(g_hEdit);
    if (len <= 0) {
        StrBuf_InitLimit(&g_state.message, MAX_MESSAGE_CHARS);
        return;
    }
    wchar_t* buf = (wchar_t*)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_hEdit, buf, len + 1);
    SetMessageText(buf);
    free(buf);
}

// True when the body is small enough to hand to ipcmd as an inline argument.
static bool MessageFitsInline(void) {
    return g_state.message.len > 0 && g_state.message.len <= SEND_INLINE_MAX && !g_state.message.failed;
}

// Writes the body to a temp file as UTF-8 (ipcmd rejects non-UTF-8 msgfiles).
// Returns false if the file could not be created or written.
static bool WriteMessageTempFile(wchar_t* outPath, size_t outCap) {
    wchar_t tempDir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tempDir);
    if (n == 0 || n >= MAX_PATH) return false;
    swprintf_s(outPath, outCap, L"%s%s", tempDir, MSGFILE_TMP_TEMPLATE);

    // ipmsg requires UTF-8; the msgfile must not carry a BOM (it is read as-is).
    int u8Bytes = WideCharToMultiByte(CP_UTF8, 0, g_state.message.data, (int)g_state.message.len, NULL, 0, NULL, NULL);
    if (u8Bytes <= 0) return false;

    char* u8 = (char*)malloc((size_t)u8Bytes);
    if (!u8) return false;
    if (WideCharToMultiByte(CP_UTF8, 0, g_state.message.data, (int)g_state.message.len, u8, u8Bytes, NULL, NULL) <= 0) {
        free(u8);
        return false;
    }

    HANDLE hf = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (hf == INVALID_HANDLE_VALUE) { free(u8); return false; }

    DWORD written = 0;
    bool ok = WriteFile(hf, u8, (DWORD)u8Bytes, &written, NULL) && written == (DWORD)u8Bytes;
    CloseHandle(hf);
    free(u8);

    if (!ok) {
        DeleteFileW(outPath);
        outPath[0] = L'\0';
        return false;
    }
    return true;
}

// Human-readable body size, e.g. "1.4 MB" / "812 KB" / "37 chars".
static void FormatMessageSize(size_t chars, wchar_t* out, size_t cap) {
    double bytes = (double)chars * sizeof(wchar_t);
    if (bytes >= 1024.0 * 1024.0) {
        swprintf_s(out, cap, L"%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024.0) {
        swprintf_s(out, cap, L"%.0f KB", bytes / 1024.0);
    } else {
        swprintf_s(out, cap, L"%zu chars", chars);
    }
}

// Filter recipients based on search query
static void FilterRecipients(void) {
    g_state.filteredCount = 0;
    for (int i = 0; i < g_state.recipientCount; i++) {
        if (g_state.searchLen == 0) {
            g_state.filteredIndices[g_state.filteredCount++] = i;
        } else {
            // Case-insensitive substring match against name, IP, or host
            wchar_t nameLower[128], queryLower[64];
            wcscpy_s(nameLower, 128, g_state.recipients[i].name);
            wcscpy_s(queryLower, 64, g_state.searchQuery);
            _wcslwr_s(nameLower, 128);
            _wcslwr_s(queryLower, 64);
            if (wcsstr(nameLower, queryLower) != NULL) {
                g_state.filteredIndices[g_state.filteredCount++] = i;
            }
        }
    }
    if (g_state.highlightedFilteredIdx >= g_state.filteredCount) {
        g_state.highlightedFilteredIdx = g_state.filteredCount > 0 ? (g_state.filteredCount - 1) : 0;
    }
    if (g_state.highlightedFilteredIdx < 0) {
        g_state.highlightedFilteredIdx = 0;
    }

    // Adjust scrollOffset to keep highlighted item in view
    int visRows = g_state.visibleRows > 0 ? g_state.visibleRows : 4;
    if (g_state.highlightedFilteredIdx < g_state.scrollOffset) {
        g_state.scrollOffset = g_state.highlightedFilteredIdx;
    }
    if (g_state.highlightedFilteredIdx >= g_state.scrollOffset + visRows) {
        g_state.scrollOffset = g_state.highlightedFilteredIdx - visRows + 1;
    }
    if (g_state.scrollOffset > g_state.filteredCount - visRows) {
        g_state.scrollOffset = g_state.filteredCount - visRows;
    }
    if (g_state.scrollOffset < 0) {
        g_state.scrollOffset = 0;
    }
}

static RecipientItem s_tempRecipients[MAX_RECIPIENTS];

// Compare two recipients to check if they refer to the same user
static bool AreRecipientsEqual(const RecipientItem* a, const RecipientItem* b) {
    if (!a || !b) return false;

    // Match by non-empty UID (IPMsg UID or <hash> is unique across sessions)
    if (a->uid[0] != L'\0' && b->uid[0] != L'\0') {
        if (_wcsicmp(a->uid, b->uid) == 0) return true;
        return false;
    }

    // Match by name
    if (_wcsicmp(a->name, b->name) == 0) {
        if (a->hostName[0] != L'\0' && b->hostName[0] != L'\0' &&
            wcscmp(a->hostName, L"-") != 0 && wcscmp(b->hostName, L"-") != 0 &&
            wcscmp(a->hostName, L"Custom") != 0 && wcscmp(b->hostName, L"Custom") != 0) {
            return (_wcsicmp(a->hostName, b->hostName) == 0);
        }
        return true;
    }

    // Match by IP address if both have valid IPs
    if (a->ipAddr[0] != L'\0' && b->ipAddr[0] != L'\0' &&
        wcscmp(a->ipAddr, L"-") != 0 && wcscmp(b->ipAddr, L"-") != 0) {
        if (_wcsicmp(a->ipAddr, b->ipAddr) == 0) {
            if (_wcsicmp(a->name, b->name) == 0 || _wcsicmp(a->name, b->ipAddr) == 0 || _wcsicmp(b->name, a->ipAddr) == 0) {
                return true;
            }
        }
    }

    // Match if name was an IP address matching b's IP address
    if (b->ipAddr[0] != L'\0' && wcscmp(b->ipAddr, L"-") != 0 && _wcsicmp(a->name, b->ipAddr) == 0) {
        return true;
    }
    if (a->ipAddr[0] != L'\0' && wcscmp(a->ipAddr, L"-") != 0 && _wcsicmp(b->name, a->ipAddr) == 0) {
        return true;
    }

    return false;
}

// Forward declaration
static bool QueryLiveIPMsgRecipients(const wchar_t* ipcmdPath);

// Refresh live recipient list (r shortcut)
static void RefreshRecipientList(HWND hWnd) {
    g_state.isRefreshing = true;
    if (hWnd) {
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);
    }

    RecipientItem highlightedRecipient = { 0 };
    bool hasHighlighted = false;
    if (g_state.filteredCount > 0 && g_state.highlightedFilteredIdx >= 0 &&
        g_state.highlightedFilteredIdx < g_state.filteredCount) {
        int oldRIdx = g_state.filteredIndices[g_state.highlightedFilteredIdx];
        if (oldRIdx >= 0 && oldRIdx < g_state.recipientCount) {
            highlightedRecipient = g_state.recipients[oldRIdx];
            hasHighlighted = true;
        }
    }

    wchar_t ipcmdPath[MAX_PATH];
    if (FindIPMsgExecutable(ipcmdPath, MAX_PATH)) {
        QueryLiveIPMsgRecipients(ipcmdPath);
        FilterRecipients();
        if (hasHighlighted) {
            for (int i = 0; i < g_state.filteredCount; i++) {
                int rIdx = g_state.filteredIndices[i];
                if (AreRecipientsEqual(&g_state.recipients[rIdx], &highlightedRecipient)) {
                    g_state.highlightedFilteredIdx = i;
                    break;
                }
            }
            int visRows = g_state.visibleRows > 0 ? g_state.visibleRows : 4;
            if (g_state.highlightedFilteredIdx < g_state.scrollOffset) {
                g_state.scrollOffset = g_state.highlightedFilteredIdx;
            }
            if (g_state.highlightedFilteredIdx >= g_state.scrollOffset + visRows) {
                g_state.scrollOffset = g_state.highlightedFilteredIdx - visRows + 1;
            }
            if (g_state.scrollOffset > g_state.filteredCount - visRows) {
                g_state.scrollOffset = g_state.filteredCount - visRows;
            }
            if (g_state.scrollOffset < 0) {
                g_state.scrollOffset = 0;
            }
        }
    }

    g_state.isRefreshing = false;
    g_state.statusText[0] = L'\0';
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}

// Query online recipients directly from ipcmd.exe executable
static bool QueryLiveIPMsgRecipients(const wchar_t* ipcmdPath) {
    wchar_t cmdLine[MAX_PATH + 32];
    swprintf_s(cmdLine, MAX_PATH + 32, L"\"%s\" list /all", ipcmdPath);

    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    BOOL ok = CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        return false;
    }

    char buffer[32768] = { 0 };
    DWORD totalRead = 0;
    DWORD bytesRead = 0;

    while (ReadFile(hReadPipe, buffer + totalRead, sizeof(buffer) - 1 - totalRead, &bytesRead, NULL) && bytesRead > 0) {
        totalRead += bytesRead;
        if (totalRead >= sizeof(buffer) - 1) break;
    }
    buffer[totalRead] = '\0';
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (totalRead == 0) return false;

    memset(s_tempRecipients, 0, sizeof(s_tempRecipients));

    // Parse output lines into recipient names
    char* context = NULL;
    char* line = strtok_s(buffer, "\r\n", &context);
    int count = 0;
    while (line && count < MAX_RECIPIENTS) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line != '\0' && strncmp(line, "ipmsg", 5) != 0 && strncmp(line, "stat=", 5) != 0) {
            char nameBuf[128] = { 0 };
            char hostBuf[128] = { 0 };
            char ipBuf[64] = { 0 };
            char uidBuf[128] = { 0 };
            bool isActive = (strstr(line, "act=1") != NULL);

            // Find " (" separating display name and details
            char* parenPos = strstr(line, " (");
            if (parenPos) {
                size_t nameLen = parenPos - line;
                if (nameLen >= 128) nameLen = 127;
                strncpy_s(nameBuf, 128, line, nameLen);

                char* details = parenPos + 2;
                if (*details == '/') details++; // Skip leading '/'

                // Split slash components: HOSTNAME / IP(0) / UID / act=X
                char detailsCopy[256];
                strncpy_s(detailsCopy, 256, details, _TRUNCATE);

                char* dCtx = NULL;
                char* hostPart = strtok_s(detailsCopy, "/", &dCtx);
                char* ipPart = strtok_s(NULL, "/", &dCtx);
                char* uidPart = strtok_s(NULL, "/", &dCtx);

                if (hostPart) strncpy_s(hostBuf, 128, hostPart, _TRUNCATE);
                if (ipPart) {
                    // Strip trailing (0) or (1)
                    char* pBracket = strchr(ipPart, '(');
                    if (pBracket) *pBracket = '\0';
                    strncpy_s(ipBuf, 64, ipPart, _TRUNCATE);
                }
                if (uidPart) {
                    // Extract <hash> from UID if present
                    char* angle1 = strchr(uidPart, '<');
                    char* angle2 = strchr(uidPart, '>');
                    if (angle1 && angle2 && angle2 > angle1) {
                        size_t hashLen = angle2 - angle1 + 1;
                        if (hashLen < 128) {
                            strncpy_s(uidBuf, 128, angle1, hashLen);
                        }
                    } else {
                        strncpy_s(uidBuf, 128, uidPart, _TRUNCATE);
                    }
                }
            } else {
                strncpy_s(nameBuf, 128, line, _TRUNCATE);
            }

            if (nameBuf[0] != '\0') {
                wchar_t wName[128] = { 0 };
                wchar_t wHost[128] = { 0 };
                wchar_t wIP[64] = { 0 };
                wchar_t wUid[128] = { 0 };

                MultiByteToWideChar(CP_ACP, 0, nameBuf, -1, wName, 128);
                if (hostBuf[0] != '\0') MultiByteToWideChar(CP_ACP, 0, hostBuf, -1, wHost, 128);
                if (ipBuf[0] != '\0') MultiByteToWideChar(CP_ACP, 0, ipBuf, -1, wIP, 64);
                if (uidBuf[0] != '\0') MultiByteToWideChar(CP_ACP, 0, uidBuf, -1, wUid, 128);

                wcscpy_s(s_tempRecipients[count].name, 128, wName);
                wcscpy_s(s_tempRecipients[count].hostName, 128, wHost);
                wcscpy_s(s_tempRecipients[count].ipAddr, 64, wIP[0] ? wIP : L"-");
                wcscpy_s(s_tempRecipients[count].uid, 128, wUid);
                s_tempRecipients[count].active = isActive;
                s_tempRecipients[count].selected = false;
                count++;
            }
        }
        line = strtok_s(NULL, "\r\n", &context);
    }

    if (count > 0 || totalRead > 0) {
        // Preserve selection state from existing recipients
        bool oldMatched[MAX_RECIPIENTS] = { 0 };

        for (int i = 0; i < count; i++) {
            for (int j = 0; j < g_state.recipientCount; j++) {
                if (!oldMatched[j] && AreRecipientsEqual(&g_state.recipients[j], &s_tempRecipients[i])) {
                    s_tempRecipients[i].selected = g_state.recipients[j].selected;
                    oldMatched[j] = true;
                    break;
                }
            }
        }

        // Preserve any previously selected recipients not returned in the new query
        // (e.g. custom recipients added via CLI or temporarily unlisted contacts)
        for (int j = 0; j < g_state.recipientCount && count < MAX_RECIPIENTS; j++) {
            if (!oldMatched[j] && g_state.recipients[j].selected) {
                s_tempRecipients[count] = g_state.recipients[j];
                count++;
            }
        }

        // Commit updated recipient list
        for (int i = 0; i < count; i++) {
            g_state.recipients[i] = s_tempRecipients[i];
        }
        g_state.recipientCount = count;
        return true;
    }

    return false;
}

// Applies the --to comma/semicolon separated recipient list against the live
// recipient list. Must run AFTER InitRecipients(), since InitRecipients resets
// recipientCount. Unmatched names are added as custom offline-capable entries.
static void ApplyRecipientSelection(void) {
    if (g_state.toArg[0] == L'\0') return;

    wchar_t toBuf[256];
    wcscpy_s(toBuf, 256, g_state.toArg);
    wchar_t* nextToken = NULL;
    wchar_t* token = wcstok_s(toBuf, L",;", &nextToken);
    while (token) {
        // Trim surrounding whitespace of each token
        while (*token == L' ' || *token == L'\t') token++;
        size_t len = wcslen(token);
        while (len > 0 && (token[len - 1] == L' ' || token[len - 1] == L'\t')) {
            token[--len] = L'\0';
        }

        if (token[0] != L'\0') {
            bool found = false;
            for (int r = 0; r < g_state.recipientCount; r++) {
                if (_wcsicmp(g_state.recipients[r].name, token) == 0 ||
                    (g_state.recipients[r].ipAddr[0] != L'\0' &&
                     wcscmp(g_state.recipients[r].ipAddr, L"-") != 0 &&
                     _wcsicmp(g_state.recipients[r].ipAddr, token) == 0)) {
                    g_state.recipients[r].selected = true;
                    found = true;
                    break;
                }
            }
            if (!found && g_state.recipientCount < MAX_RECIPIENTS) {
                RecipientItem* item = &g_state.recipients[g_state.recipientCount++];
                wcscpy_s(item->name, 128, token);
                wcscpy_s(item->hostName, 128, L"Custom");
                item->active = true;
                item->selected = true;
            }
        }
        token = wcstok_s(NULL, L",;", &nextToken);
    }

    FilterRecipients();
}

// Check if IP Messenger process is currently running
static bool IsIPMsgProcessRunning(void) {
    bool running = false;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"ipmsg.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return running;
}

// Ensure IP Messenger process is running (starts it if disabled/not-started)
static bool EnsureIPMsgProcessRunning(const wchar_t* ipcmdPath) {
    if (IsIPMsgProcessRunning()) return true;

    wchar_t exePath[MAX_PATH];
    wcscpy_s(exePath, MAX_PATH, ipcmdPath);

    wchar_t* pSlash = wcsrchr(exePath, L'\\');
    if (!pSlash) pSlash = wcsrchr(exePath, L'/');
    if (pSlash) {
        wcscpy_s(pSlash + 1, MAX_PATH - (pSlash + 1 - exePath), L"ipmsg.exe");
    }

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(exePath, MAX_PATH, ipcmdPath);
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessW(exePath, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        wcscpy_s(g_state.statusText, 256, L"Warning: IP Messenger process was not running. Started background process.");
        g_state.statusTime = GetTickCount();
        return true;
    }
    return false;
}

// Populate online LAN recipients directly from ipcmd executable
static void InitRecipients(void) {
    g_state.recipientCount = 0;
    wchar_t ipcmdPath[MAX_PATH];
    if (FindIPMsgExecutable(ipcmdPath, MAX_PATH)) {
        EnsureIPMsgProcessRunning(ipcmdPath);
        QueryLiveIPMsgRecipients(ipcmdPath);
    } else {
        wcscpy_s(g_state.statusText, 256, L"Error: IP Messenger (ipcmd.exe) is not installed!");
        g_state.statusTime = GetTickCount();
    }
    FilterRecipients();
}

// Parse CLI arguments
static bool ParseCommandLine(const wchar_t* cmdLine, AppState* state) {
    if (!cmdLine) return true;
    wchar_t cmdBuf[2048];
    wcsncpy_s(cmdBuf, sizeof(cmdBuf)/sizeof(cmdBuf[0]), cmdLine, _TRUNCATE);
    wchar_t* argv[TINY_CLI_MAX_ARGS];
    int argc = TinyCLI_Tokenize(cmdBuf, argv, TINY_CLI_MAX_ARGS);
    if (argc <= 0) return true;

    const wchar_t* toArg = NULL;
    const wchar_t* msgArg = NULL;
    const wchar_t* themeArg = NULL;
    const wchar_t* ctxMenuArg = NULL;
    bool autoSend = false;
    bool regCtx = false;
    bool unregCtx = false;

    const CliOption opts[] = {
        { L"--to",                      CLI_OPT_STRING, &toArg,      0, 0, L"<recipients>",              L"Pre-select comma-separated recipient PC or user names" },
        { L"-t",                        CLI_OPT_STRING, &toArg,      0, 0, NULL,                         NULL },
        { L"--message",                 CLI_OPT_STRING, &msgArg,     0, 0, L"<text>",                    L"Pre-populate message body text" },
        { L"--msg",                     CLI_OPT_STRING, &msgArg,     0, 0, NULL,                         NULL },
        { L"-m",                        CLI_OPT_STRING, &msgArg,     0, 0, NULL,                         NULL },
        { L"--file",                    CLI_OPT_STRING, NULL,        0, 0, L"<path>",                    L"Attach file or text snippet payload" },
        { L"--theme",                   CLI_OPT_STRING, &themeArg,   0, 0, L"light|dark|system",         L"Theme mode (default: system)" },
        { L"--send",                    CLI_OPT_BOOL,   &autoSend,   0, 0, NULL,                         L"Dispatch payload immediately via ipcmd without GUI" },
        { L"--context-menu",            CLI_OPT_STRING, &ctxMenuArg, 0, 0, L"register|unregister",       L"Add or remove Explorer context menu entries" },
        { L"--register-context-menu",   CLI_OPT_BOOL,   &regCtx,     0, 0, NULL,                         L"Register Explorer context menu entries" },
        { L"--register",                CLI_OPT_BOOL,   &regCtx,     0, 0, NULL,                         NULL },
        { L"--unregister-context-menu", CLI_OPT_BOOL,   &unregCtx,   0, 0, NULL,                         L"Unregister Explorer context menu entries" },
        { L"--unregister",              CLI_OPT_BOOL,   &unregCtx,   0, 0, NULL,                         NULL },
    };

    if (TinyCLI_CheckHelp(argc, argv,
            L"ip-send",
            L"Terminal-inspired payload composer & dispatcher for IP Messenger",
            L"ip-send.exe [--to <recipients>] [--message <text>] [--file <path>] [--theme light|dark|system] [--send] [<files...>]",
            opts, sizeof(opts) / sizeof(opts[0]))) {
        return false;
    }

    TinyCLI_Parse(argc, argv, opts, sizeof(opts) / sizeof(opts[0]));

    if (autoSend) state->autoSend = true;
    if (regCtx) state->registerContextMenu = true;
    if (unregCtx) state->unregisterContextMenu = true;

    if (ctxMenuArg) {
        if (_wcsicmp(ctxMenuArg, L"register") == 0 || _wcsicmp(ctxMenuArg, L"true") == 0 || wcscmp(ctxMenuArg, L"1") == 0) {
            state->registerContextMenu = true;
        } else if (_wcsicmp(ctxMenuArg, L"unregister") == 0 || _wcsicmp(ctxMenuArg, L"false") == 0 || wcscmp(ctxMenuArg, L"0") == 0) {
            state->unregisterContextMenu = true;
        } else {
            state->registerContextMenu = true;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--context-menu") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == L'-') {
                state->registerContextMenu = true;
            }
        }
    }

    if (themeArg) {
        if (_wcsicmp(themeArg, L"light") == 0) {
            g_lightTheme = true;
            g_themeManuallyToggled = true;
        } else if (_wcsicmp(themeArg, L"dark") == 0) {
            g_lightTheme = false;
            g_themeManuallyToggled = true;
        } else if (_wcsicmp(themeArg, L"system") == 0) {
            g_lightTheme = IsSystemLightTheme();
            g_themeManuallyToggled = false;
        }
    }

    if (msgArg) {
        SetMessageText(msgArg);
    }

    // NOTE: --to is applied later (ApplyRecipientSelection) once InitRecipients()
    // has populated the live list, otherwise InitRecipients would wipe it.
    if (toArg) {
        wcscpy_s(state->toArg, 256, toArg);
    }

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--file") == 0 && i + 1 < argc) {
            AddFilePayload(argv[++i]);
        } else if (_wcsnicmp(argv[i], L"--file=", 7) == 0) {
            AddFilePayload(argv[i] + 7);
        } else if (argv[i][0] != L'-') {
            bool isVal = false;
            if (i > 1 && argv[i - 1][0] == L'-') {
                for (size_t o = 0; o < sizeof(opts)/sizeof(opts[0]); o++) {
                    if (_wcsicmp(argv[i - 1], opts[o].name) == 0 && opts[o].type != CLI_OPT_BOOL) {
                        isVal = true;
                        break;
                    }
                }
            }
            if (!isVal) {
                AddFilePayload(argv[i]);
            }
        }
    }

    FilterRecipients();
    return true;
}

// Check if current process was launched with payload/file arguments
static bool HasPayloadArguments(void) {
    wchar_t cmdBuf[2048];
    wcsncpy_s(cmdBuf, sizeof(cmdBuf)/sizeof(cmdBuf[0]), GetCommandLineW(), _TRUNCATE);
    wchar_t* argv[TINY_CLI_MAX_ARGS];
    int argc = TinyCLI_Tokenize(cmdBuf, argv, TINY_CLI_MAX_ARGS);
    if (argc <= 0) return false;
    bool hasArgs = false;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--context-menu") == 0 ||
            _wcsicmp(argv[i], L"--register-context-menu") == 0 ||
            _wcsicmp(argv[i], L"--register") == 0 ||
            _wcsicmp(argv[i], L"--unregister-context-menu") == 0 ||
            _wcsicmp(argv[i], L"--unregister") == 0) {
            continue;
        }
        if (_wcsicmp(argv[i], L"--theme") == 0 && i + 1 < argc) {
            i++;
            continue;
        }
        if (_wcsnicmp(argv[i], L"--theme=", 8) == 0) {
            continue;
        }
        if (_wcsicmp(argv[i], L"--file") == 0 ||
            _wcsicmp(argv[i], L"--message") == 0 ||
            _wcsicmp(argv[i], L"--msg") == 0 ||
            _wcsicmp(argv[i], L"-m") == 0 ||
            _wcsicmp(argv[i], L"--to") == 0 ||
            _wcsicmp(argv[i], L"-t") == 0) {
            hasArgs = true;
            break;
        } else if (argv[i][0] != L'-') {
            hasArgs = true;
            break;
        }
    }
    return hasArgs;
}

static void RemoveLegacyContextMenu(void) {
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IPMsg\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IPMsg");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IPMsg\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IPMsg");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IPMsg\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IPMsg");

    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IP\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IP");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IP\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IP");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IP\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IP");

    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\IP Send\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\IP Send");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\IP Send\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\IP Send");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\IP Send\\command");
    // RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\IP Send");

    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IP-send\\command");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\Send via IP-send");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IP-send\\command");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\Send via IP-send");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IP-send\\command");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\Send via IP-send");
}

// Forward declaration
static bool FindIPMsgExecutable(wchar_t* outPath, DWORD maxLen);

// Registry context menu registration
static bool RegisterContextMenu(void) {
    RemoveLegacyContextMenu();

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32];
    swprintf_s(cmdLine, MAX_PATH + 32, L"\"%s\" \"%%1\"", exePath);

    // Resolve IP Messenger executable for context menu icon
    wchar_t ipcmdPath[MAX_PATH] = { 0 };
    wchar_t iconPath[MAX_PATH] = { 0 };
    if (FindIPMsgExecutable(ipcmdPath, MAX_PATH)) {
        wcscpy_s(iconPath, MAX_PATH, ipcmdPath);
        wchar_t* pSlash = wcsrchr(iconPath, L'\\');
        if (!pSlash) pSlash = wcsrchr(iconPath, L'/');
        if (pSlash) {
            wcscpy_s(pSlash + 1, MAX_PATH - (pSlash + 1 - iconPath), L"ipmsg.exe");
            if (GetFileAttributesW(iconPath) == INVALID_FILE_ATTRIBUTES) {
                wcscpy_s(iconPath, MAX_PATH, ipcmdPath);
            }
        }
    }

    const wchar_t* subKeys[] = {
        L"Software\\Classes\\*\\shell\\Send via IP-send",
        L"Software\\Classes\\Directory\\shell\\Send via IP-send"
    };

    const wchar_t* bgSubKey = L"Software\\Classes\\Directory\\Background\\shell\\Send via IP-send";
    wchar_t bgCmdLine[MAX_PATH + 32];
    swprintf_s(bgCmdLine, MAX_PATH + 32, L"\"%s\" \"%%V\"", exePath);

    HKEY hKey = NULL;
    for (int i = 0; i < 2; i++) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, subKeys[i], 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"Send via IP-send", (DWORD)(wcslen(L"Send via IP-send") + 1) * sizeof(wchar_t));
            if (iconPath[0] != L'\0') {
                RegSetValueExW(hKey, L"Icon", 0, REG_SZ, (const BYTE*)iconPath, (DWORD)(wcslen(iconPath) + 1) * sizeof(wchar_t));
            }
            HKEY hCmd = NULL;
            if (RegCreateKeyExW(hKey, L"command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hCmd, NULL) == ERROR_SUCCESS) {
                RegSetValueExW(hCmd, NULL, 0, REG_SZ, (const BYTE*)cmdLine, (DWORD)(wcslen(cmdLine) + 1) * sizeof(wchar_t));
                RegCloseKey(hCmd);
            }
            RegCloseKey(hKey);
        }
    }

    if (RegCreateKeyExW(HKEY_CURRENT_USER, bgSubKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"Send via IP-send", (DWORD)(wcslen(L"Send via IP-send") + 1) * sizeof(wchar_t));
        if (iconPath[0] != L'\0') {
            RegSetValueExW(hKey, L"Icon", 0, REG_SZ, (const BYTE*)iconPath, (DWORD)(wcslen(iconPath) + 1) * sizeof(wchar_t));
        }
        HKEY hCmd = NULL;
        if (RegCreateKeyExW(hKey, L"command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hCmd, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hCmd, NULL, 0, REG_SZ, (const BYTE*)bgCmdLine, (DWORD)(wcslen(bgCmdLine) + 1) * sizeof(wchar_t));
            RegCloseKey(hCmd);
        }
        RegCloseKey(hKey);
    }

    return true;
}

static bool UnregisterContextMenu(void) {
    RemoveLegacyContextMenu();
    return true;
}

// Detect IP Messenger executable location on Windows
static bool FindIPMsgExecutable(wchar_t* outPath, DWORD maxLen) {
    // 1. Search PATH environment variable for ipcmd.exe ONLY (not ipmsg.exe which is our own app name!)
    if (SearchPathW(NULL, L"ipcmd.exe", NULL, maxLen, outPath, NULL) > 0) return true;

    // 2. Check Windows Registry App Paths for ipcmd.exe
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\ipcmd.exe", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD bytes = maxLen * sizeof(wchar_t);
        if (RegQueryValueExW(hKey, NULL, NULL, &type, (BYTE*)outPath, &bytes) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return true;
        }
        RegCloseKey(hKey);
    }

    // 3. Check standard AppData & Program Files installation directories for ipcmd.exe
    wchar_t envDir[MAX_PATH];
    if (GetEnvironmentVariableW(L"LocalAppData", envDir, MAX_PATH) > 0) {
        wsprintfW(outPath, L"%s\\IPMsg\\ipcmd.exe", envDir);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return true;
    }

    if (GetEnvironmentVariableW(L"ProgramFiles", envDir, MAX_PATH) > 0) {
        wsprintfW(outPath, L"%s\\IPMsg\\ipcmd.exe", envDir);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return true;
    }

    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", envDir, MAX_PATH) > 0) {
        wsprintfW(outPath, L"%s\\IPMsg\\ipcmd.exe", envDir);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return true;
    }

    return false;
}

// Forward declaration
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Appends s as a properly quoted command-line argument (escapes embedded quotes).
static void StrBuf_AppendQuoted(StrBuf* sb, const wchar_t* s) {
    if (!s || sb->failed) return;
    StrBuf_AppendChar(sb, L'"');
    for (const wchar_t* p = s; *p; p++) {
        if (*p == L'"') StrBuf_AppendChar(sb, L'\\');
        if (*p == L'\r' || *p == L'\n') continue; // never embed raw newlines in an argument
        StrBuf_AppendChar(sb, *p);
    }
    StrBuf_AppendChar(sb, L'"');
}

// Builds the ipcmd send command per its verified usage grammar:
//   send [/file=path1 /file=path2...] [/noseal]
//        "((uid|ipaddr|ALL)[,uid...] | /userfile=path)"
//        ("msg_body" | /msgfile=path)
// Bodies that do not fit the ~32KB CreateProcessW command-line limit are handed
// over as /msgfile=<tempfile> instead, which ipcmd reads as UTF-8.
// On success *msgFileOut holds the temp path to delete after the send (or empty).
static bool BuildSendCommand(StrBuf* sb, const wchar_t* ipcmdPath, wchar_t* msgFileOut, size_t msgFileCap) {
    StrBuf_Init(sb);
    msgFileOut[0] = L'\0';

    if (g_state.payloadCount == 0 && g_state.message.len == 0) {
        ShowStatusToast(g_hWnd, L"Warning: Nothing to send. Attach a file or type a message.");
        return false;
    }
    if (g_state.message.failed) {
        ShowStatusToast(g_hWnd, L"Error: Message is too large to send.");
        return false;
    }

    StrBuf_AppendChar(sb, L'"');
    StrBuf_Append(sb, ipcmdPath);
    StrBuf_Append(sb, L"\" send");

    int attached = 0;
    for (int i = 0; i < g_state.payloadCount; i++) {
        const wchar_t* path = g_state.payloads[i].path;
        if (!path || path[0] == L'\0') continue;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            ShowStatusToast(g_hWnd, L"Warning: An attached file no longer exists. Aborted send.");
            StrBuf_Free(sb);
            return false;
        }
        StrBuf_Append(sb, L" /file=");
        StrBuf_AppendQuoted(sb, path);
        attached++;
    }

    // Recipients
    StrBuf targets;
    StrBuf_Init(&targets);
    int selected = 0;
    for (int i = 0; i < g_state.recipientCount; i++) {
        if (!g_state.recipients[i].selected) continue;
        const wchar_t* target =
            (g_state.recipients[i].ipAddr[0] != L'\0' && wcscmp(g_state.recipients[i].ipAddr, L"-") != 0)
                ? g_state.recipients[i].ipAddr
                : g_state.recipients[i].name;
        if (selected > 0) StrBuf_AppendChar(&targets, L',');
        StrBuf_Append(&targets, target);
        selected++;
    }

    if (selected == 0) {
        StrBuf_Free(&targets);
        ShowStatusToast(g_hWnd, L"Warning: No recipients selected. Press Space to select.");
        StrBuf_Free(sb);
        return false;
    }

    StrBuf_Append(sb, L" \"");
    StrBuf_Append(sb, targets.data);
    StrBuf_AppendChar(sb, L'"');
    StrBuf_Free(&targets);

    // Explicit separator: without it the recipient and body quotes would form
    // adjacent quoted runs ("a""b"), which CommandLineToArgv only splits by luck.
    StrBuf_AppendChar(sb, L' ');

    // ipcmd requires a message argument. Small bodies go inline; anything past
    // the command-line budget is staged in a UTF-8 temp file and passed as
    // /msgfile=, so multi-megabyte pastes are transmitted in full.
    if (g_state.message.len == 0) {
        StrBuf_AppendQuoted(sb, L"");
    } else if (MessageFitsInline()) {
        StrBuf_AppendQuoted(sb, g_state.message.data);
    } else {
        if (!WriteMessageTempFile(msgFileOut, msgFileCap)) {
            StrBuf_Free(sb);
            ShowStatusToast(g_hWnd, L"Error: Could not stage large message to a temp file.");
            return false;
        }
        StrBuf_Append(sb, L"/msgfile=");
        StrBuf_AppendQuoted(sb, msgFileOut);
    }

    if (sb->failed || !sb->data) {
        StrBuf_Free(sb);
        if (msgFileOut[0]) { DeleteFileW(msgFileOut); msgFileOut[0] = L'\0'; }
        ShowStatusToast(g_hWnd, L"Error: Could not build send command (too many attachments?).");
        return false;
    }
    (void)attached;
    return true;
}

// Backend Send Execution
static bool ExecuteSend(HWND hWnd) {
    // Check if IP Messenger is installed on system
    wchar_t ipcmdPath[MAX_PATH];
    bool ipmsgFound = FindIPMsgExecutable(ipcmdPath, MAX_PATH);

    if (!ipmsgFound) {
        ShowStatusToast(hWnd, L"Error: IP Messenger (ipcmd.exe) is not installed!");

        MessageBoxW(hWnd,
            L"IP Messenger (ipcmd.exe) is not installed on your system.\n\nPlease install IP Messenger from https://ipmsg.org to send messages and files.",
            L"IP Send - Error",
            MB_ICONERROR | MB_OK);
        return false;
    }

    if (g_hEdit && IsWindow(g_hEdit) && g_state.messageMode) {
        SyncMessageFromEdit();
    }

    EnsureIPMsgProcessRunning(ipcmdPath);

    wchar_t msgFile[MAX_PATH] = { 0 };
    StrBuf cmdLine;
    if (!BuildSendCommand(&cmdLine, ipcmdPath, msgFile, MAX_PATH)) {
        g_state.statusTime = GetTickCount();
        if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
        return false;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // CreateProcessW may modify the command line, so pass a writable buffer.
    BOOL ok = CreateProcessW(NULL, cmdLine.data, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    if (ok) {
        // ipcmd exits immediately after handing the payload to the ipmsg daemon.
        WaitForSingleObject(pi.hProcess, SEND_WAIT_MS);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        wchar_t sizeBuf[32];
        FormatMessageSize(g_state.message.len, sizeBuf, 32);
        wchar_t status[256];
        if (g_state.message.len > 0) {
            swprintf_s(status, 256, L"Sent successfully! (%s body)", sizeBuf);
        } else {
            wcscpy_s(status, 256, L"Sent successfully!");
        }
        ShowStatusToast(hWnd, status);
    } else {
        ShowStatusToast(hWnd, L"Error executing IP Messenger command.");
    }

    StrBuf_Free(&cmdLine);
    // ipcmd has read the staged body by the time it exits, so the temp file can go.
    if (msgFile[0]) DeleteFileW(msgFile);
    g_state.sending = ok;

    if (hWnd) {
        InvalidateRect(hWnd, NULL, FALSE);
        if (ok) SetTimer(hWnd, 100, 400, NULL); // Short delay before exit
    }

    return ok;
}

// File Open Dialog (Ctrl+O)
static void OpenFileDialog(HWND hWnd) {
    wchar_t fileBuf[8192] = { 0 };
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 8192;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        wchar_t* dir = fileBuf;
        wchar_t* file = fileBuf + wcslen(dir) + 1;
        if (*file == L'\0') {
            // Single file selected
            AddFilePayload(dir);
        } else {
            // Multi-file selection
            while (*file) {
                wchar_t fullPath[MAX_PATH];
                swprintf_s(fullPath, MAX_PATH, L"%s\\%s", dir, file);
                AddFilePayload(fullPath);
                file += wcslen(file) + 1;
            }
        }
        InvalidateRect(hWnd, NULL, FALSE);
    }
}

// Subclass Procedure for native EDIT control
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_INSERT) {
            SyncMessageFromEdit();
            g_state.messageMode = false;
            ShowWindow(hWnd, SW_HIDE);
            SetFocus(g_hWnd);
            InvalidateRect(g_hWnd, NULL, FALSE);
            return 0;
        }
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wParam == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DWORD start = 0, end = 0;
            SendMessageW(hWnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start != end) {
                SendMessageW(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            } else if (start > 0) {
                int textLen = GetWindowTextLengthW(hWnd);
                if (textLen > 0) {
                    wchar_t* buf = (wchar_t*)malloc((textLen + 1) * sizeof(wchar_t));
                    if (buf) {
                        GetWindowTextW(hWnd, buf, textLen + 1);
                        int pos = (int)start;
                        while (pos > 0 && (buf[pos - 1] == L' ' || buf[pos - 1] == L'\t' || buf[pos - 1] == L'\r' || buf[pos - 1] == L'\n')) {
                            pos--;
                        }
                        while (pos > 0 && buf[pos - 1] != L' ' && buf[pos - 1] != L'\t' && buf[pos - 1] != L'\r' && buf[pos - 1] != L'\n') {
                            pos--;
                        }
                        free(buf);
                        SendMessageW(hWnd, EM_SETSEL, (WPARAM)pos, (LPARAM)start);
                        SendMessageW(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
                    }
                }
            }
            return 0;
        }
        if (wParam == 'D' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            g_lightTheme = !g_lightTheme;
            g_themeManuallyToggled = true;
            InvalidateRect(g_hWnd, NULL, FALSE);
            InvalidateRect(hWnd, NULL, TRUE);
            return 0;
        }
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            OpenFileDialog(g_hWnd);
            return 0;
        }
        if (wParam == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SyncMessageFromEdit();
            ExecuteSend(g_hWnd);
            return 0;
        }
        break;

    case WM_CHAR:
        if (wParam == 0x7F || (wParam == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000))) {
            return 0; // Suppress control char 0x7F from Ctrl+Backspace
        }
        break;

    case WM_KILLFOCUS:
        SyncMessageFromEdit();
        break;
    }
    return CallWindowProcW(g_oldEditProc, hWnd, msg, wParam, lParam);
}

// Handle Paste (Ctrl+V)
// Files (CF_HDROP) become attachments; plain text becomes the message body.
static void HandlePaste(HWND hWnd) {
    if (g_state.messageMode && g_hEdit && IsWindow(g_hEdit)) {
        SendMessageW(g_hEdit, WM_PASTE, 0, 0);
        SyncMessageFromEdit();
        InvalidateRect(hWnd, NULL, FALSE);
        return;
    }

    bool gotFile = false;
    bool gotText = false;
    int addedFiles = 0;
    size_t pastedChars = 0;

    // The clipboard is a shared resource and may be briefly held by another app.
    if (!TinyClipboard_OpenWithRetry(hWnd, 5, 10)) {
        ShowStatusToast(hWnd, L"Warning: Clipboard is busy. Try pasting again.");
        return;
    }

    // Prefer files over text when the clipboard carries both (Explorer file copy).
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hDrop = (HDROP)GetClipboardData(CF_HDROP);
        addedFiles = CollectFilesFromDrop(hDrop);
        gotFile = (addedFiles > 0);
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const wchar_t* clipText = (const wchar_t*)GlobalLock(hData);
            if (clipText && clipText[0] != L'\0') {
                // Pastes can be many megabytes; AppendMessageText grows the heap
                // buffer rather than clipping into a fixed array.
                pastedChars = wcslen(clipText);
                AppendMessageText(clipText);
                gotText = !g_state.message.failed;
                if (!gotText) {
                    CloseClipboard();
                    StrBuf_Free(&g_state.message);
                    StrBuf_InitLimit(&g_state.message, MAX_MESSAGE_CHARS);
                    ShowStatusToast(hWnd, L"Error: Pasted text is too large (max 128 MB).");
                    return;
                }
            }
            if (clipText) GlobalUnlock(hData);
        }
    } else if (IsClipboardFormatAvailable(CF_TEXT)) {
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData) {
            const char* ansiText = (const char*)GlobalLock(hData);
            if (ansiText && ansiText[0] != '\0') {
                int wlen = MultiByteToWideChar(CP_ACP, 0, ansiText, -1, NULL, 0);
                if (wlen > 1) {
                    wchar_t* wbuf = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
                    if (wbuf) {
                        MultiByteToWideChar(CP_ACP, 0, ansiText, -1, wbuf, wlen);
                        pastedChars = (size_t)(wlen - 1);
                        AppendMessageText(wbuf);
                        free(wbuf);
                        gotText = !g_state.message.failed;
                    }
                }
            }
            if (ansiText) GlobalUnlock(hData);
        }
    }

    CloseClipboard();

    if (gotFile) {
        wchar_t status[128];
        swprintf_s(status, 128, L"Pasted %d file(s). Press Enter to send.", addedFiles);
        ShowStatusToast(hWnd, status);
    } else if (gotText) {
        // Report the real size so multi-megabyte pastes are visibly accepted.
        wchar_t sizeBuf[32], status[160];
        FormatMessageSize(g_state.message.len, sizeBuf, 32);
        if (g_state.message.len <= MESSAGE_EDIT_MAX_CHARS) {
            swprintf_s(status, 160, L"Pasted %s of text. Press Enter to send.", sizeBuf);
        } else {
            swprintf_s(status, 160, L"Pasted %s of text. Press Enter to send as message.", sizeBuf);
        }
        (void)pastedChars;
        ShowStatusToast(hWnd, status);
    } else {
        ShowStatusToast(hWnd, L"Warning: Nothing usable on the clipboard.");
    }

    // Keep the edit box in step, but only when the body is small enough for the
    // native control to stay responsive.
    if (g_state.message.len <= MESSAGE_EDIT_MAX_CHARS) {
        SyncEditFromMessage();
    }

    InvalidateRect(hWnd, NULL, FALSE);
}



static void DrawDashH(HDC hdc, int x1, int x2, int y, COLORREF col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(hdc, hPen);
    int x = x1;
    while (x < x2) {
        int end = x + 4;
        if (end > x2) end = x2;
        MoveToEx(hdc, x, y, NULL);
        LineTo(hdc, end, y);
        x += 8;
    }
    SelectObject(hdc, old);
    DeleteObject(hPen);
}

static void DrawDashV(HDC hdc, int x, int y1, int y2, COLORREF col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(hdc, hPen);
    int y = y1;
    while (y < y2) {
        int end = y + 4;
        if (end > y2) end = y2;
        MoveToEx(hdc, x, y, NULL);
        LineTo(hdc, x, end);
        y += 8;
    }
    SelectObject(hdc, old);
    DeleteObject(hPen);
}

static void DrawCross(HDC hdc, int x, int y, COLORREF col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, x - 3, y, NULL);
    LineTo(hdc, x + 4, y);
    MoveToEx(hdc, x, y - 3, NULL);
    LineTo(hdc, x, y + 4);
    SelectObject(hdc, old);
    DeleteObject(hPen);
}

// Custom GDI Render
static void RenderTUIWindow(HWND hWnd, HDC hdc) {
    RECT client;
    GetClientRect(hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBM = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

    COLORREF bgVoid, rowInvert, lineDim, textDim, textMid, textBright, accentBlue, errorCoral;
    if (g_lightTheme) {
        bgVoid = RGB(0xF8, 0xF9, 0xFC);
        rowInvert = RGB(0x25, 0x63, 0xEB);
        lineDim = RGB(0xCD, 0xD2, 0xDE);
        textDim = RGB(0x78, 0x80, 0x90);
        textMid = RGB(0x40, 0x48, 0x58);
        textBright = RGB(0x11, 0x14, 0x1A);
        accentBlue = RGB(0x25, 0x63, 0xEB);
        errorCoral = RGB(0xDC, 0x26, 0x26);
    } else {
        bgVoid = RGB(0x12, 0x12, 0x12);
        rowInvert = RGB(0x4A, 0x90, 0xE2);
        lineDim = RGB(0x3A, 0x3A, 0x3A);
        textDim = RGB(0x77, 0x77, 0x77);
        textMid = RGB(0x9A, 0x99, 0x96);
        textBright = RGB(0xEE, 0xEE, 0xEE);
        accentBlue = RGB(0x4A, 0x90, 0xE2);
        errorCoral = RGB(0xFF, 0x67, 0x85);
    }

    HBRUSH hBgBrush = CreateSolidBrush(bgVoid);
    FillRect(memDC, &client, hBgBrush);

    SetBkMode(memDC, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(memDC, g_hFontNormal);

    int bx1 = 8, by1 = 8, bx2 = width - 9, by2 = height - 9;

    int marginX = 26;

    DrawDashH(memDC, bx1 + 4, bx2 - 3, by1, lineDim);
    DrawDashH(memDC, bx1 + 4, bx2 - 3, by2, lineDim);
    DrawDashV(memDC, bx1, by1 + 4, by2 - 3, lineDim);
    DrawDashV(memDC, bx2, by1 + 4, by2 - 3, lineDim);
    DrawCross(memDC, bx1, by1, lineDim);
    DrawCross(memDC, bx2, by1, lineDim);
    DrawCross(memDC, bx1, by2, lineDim);
    DrawCross(memDC, bx2, by2, lineDim);

    DeleteObject(hBgBrush);

    SelectObject(memDC, g_hFontCaption);
    const wchar_t* title = L"Send";
    SIZE titleSize;
    GetTextExtentPoint32W(memDC, title, 4, &titleSize);

    int titleX = marginX;
    int titleY = by1 + 10;

    SetTextColor(memDC, textDim);
    TextOutW(memDC, titleX, titleY, title, 4);

    SelectObject(memDC, g_hFontNormal);

    SIZE chw;
    GetTextExtentPoint32W(memDC, L"M", 1, &chw);
    int cw = chw.cx;
    int y = titleY + titleSize.cy + 10;
    int topContainerStart = y;

    if (g_state.payloadCount > 0) {
        for (int i = 0; i < g_state.payloadCount && i < 4; i++) {
            PayloadItem* item = &g_state.payloads[i];
            SetTextColor(memDC, textBright);
            wchar_t nameBuf[MAX_PATH + 4];
            swprintf_s(nameBuf, MAX_PATH + 4, L"@ %s", item->displayName);
            TextOutW(memDC, marginX, y, nameBuf, (int)lstrlenW(nameBuf));
            y += 18;

            SetTextColor(memDC, textDim);
            const wchar_t* detail = item->path;
            TextOutW(memDC, marginX + cw * 2, y, detail, (int)lstrlenW(detail));
            y += 20;
        }
        if (g_state.payloadCount > 4) {
            wchar_t moreBuf[64];
            swprintf_s(moreBuf, 64, L"+ %d more files...", g_state.payloadCount - 4);
            SetTextColor(memDC, textDim);
            TextOutW(memDC, marginX + cw * 2, y, moreBuf, (int)lstrlenW(moreBuf));
            y += 20;
        }
        y += 6;
    }

    int maxTextWidth = width - (marginX * 2);
    if (maxTextWidth < 200) maxTextWidth = 200;

    // Only ever lay out a bounded preview. Passing a multi-megabyte body to
    // DrawTextW (even with DT_CALCRECT) would stall the UI thread.
    wchar_t preview[MESSAGE_PREVIEW_CHARS + 1];
    bool truncated = false;
    size_t previewLen = 0;
    if (g_state.message.len > 0) {
        previewLen = g_state.message.len;
        if (previewLen > MESSAGE_PREVIEW_CHARS) {
            previewLen = MESSAGE_PREVIEW_CHARS;
            truncated = true;
        }
        memcpy(preview, g_state.message.data, previewLen * sizeof(wchar_t));
        preview[previewLen] = L'\0';
    }

    int editH = 28;
    if (g_state.message.len > 0 || g_state.messageMode) {
        RECT calcRect = { 0, 0, maxTextWidth - 8, 10000 };
        const wchar_t* measureStr = (g_state.message.len > 0) ? preview : L"A";
        DrawTextW(memDC, measureStr, -1, &calcRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
        int textH = calcRect.bottom - calcRect.top;
        if (textH < 20) textH = 20;
        editH = textH + 8;
        if (editH < 28) editH = 28;
        if (editH > 160) editH = 160;
    }

    g_msgEditRect.left = marginX;
    g_msgEditRect.top = y;
    g_msgEditRect.right = marginX + maxTextWidth;
    g_msgEditRect.bottom = y + editH;

    if (g_state.messageMode) {
        if (g_hEdit) {
            SetWindowPos(g_hEdit, NULL, g_msgEditRect.left, g_msgEditRect.top + 2, maxTextWidth, editH - 2, SWP_NOZORDER | SWP_SHOWWINDOW);
        }

        HPEN hBorderPen = CreatePen(PS_SOLID, 1, accentBlue);
        HPEN oldP = (HPEN)SelectObject(memDC, hBorderPen);
        HBRUSH oldB = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, g_msgEditRect.left - 4, g_msgEditRect.top - 2, g_msgEditRect.right + 4, g_msgEditRect.bottom + 2);
        SelectObject(memDC, oldP);
        SelectObject(memDC, oldB);
        DeleteObject(hBorderPen);

        y += editH + 10;
    } else {
        if (g_hEdit && IsWindowVisible(g_hEdit)) {
            ShowWindow(g_hEdit, SW_HIDE);
        }
        if (g_state.message.len > 0) {
            SetTextColor(memDC, textBright);
            RECT drawRect = { marginX, y + 2, marginX + maxTextWidth, y + editH };
            DrawTextW(memDC, preview, -1, &drawRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            y += editH + 8;

            // Show the true total size when the visible text is only a preview.
            if (truncated) {
                wchar_t sizeBuf[32], noteBuf[96];
                FormatMessageSize(g_state.message.len, sizeBuf, 32);
                swprintf_s(noteBuf, 96, L"... (%s of text)", sizeBuf);
                SetTextColor(memDC, textDim);
                TextOutW(memDC, marginX, y, noteBuf, (int)lstrlenW(noteBuf));
                y += 20;
            }
        } else {
            SetTextColor(memDC, textDim);
            TextOutW(memDC, marginX, y, L"No message. Press Insert to edit", 32);
            y += 24;
        }
    }

    int minTopContainerH = 44;
    int actualTopH = y - topContainerStart;
    if (actualTopH < minTopContainerH) {
        y = topContainerStart + minTopContainerH;
    }

    SetTextColor(memDC, textBright);
    wchar_t searchPrompt[128];
    if (!g_state.messageMode) {
        wsprintfW(searchPrompt, L"/ %s_", g_state.searchQuery);
    } else {
        wsprintfW(searchPrompt, L"/ %s", g_state.searchQuery);
    }
    TextOutW(memDC, marginX, y, searchPrompt, (int)lstrlenW(searchPrompt));
    y += 24;

    g_listStartY = y;
    g_itemHeight = 24;

    SelectObject(memDC, g_hFontBold);
    const wchar_t* sendLabel = L"Send";
    SIZE sendSize;
    GetTextExtentPoint32W(memDC, sendLabel, (int)wcslen(sendLabel), &sendSize);

    bool hasStatus = (g_state.statusText[0] != L'\0' && (GetTickCount() - g_state.statusTime < 3500));
    int statusHeight = hasStatus ? 20 : 0;

    int btnPadX = 12;
    int btnPadY = 4;
    int btnW = sendSize.cx + btnPadX * 2;
    int btnH = sendSize.cy + btnPadY * 2;

    int footY = by2 - 12 - sendSize.cy - statusHeight;
    if (footY < y + 30) footY = y + 30;

    int btnRight = width - marginX;
    int btnLeft = btnRight - btnW;
    int btnTop = footY - btnPadY;
    int btnBottom = footY + sendSize.cy + btnPadY;
    RECT sendRect = { btnLeft, btnTop, btnRight, btnBottom };
    g_sendRect = sendRect;

    // Available height for recipient list:
    int listAvailH = (btnTop - 8) - g_listStartY;
    int visRows = listAvailH / g_itemHeight;
    if (visRows < 1) visRows = 1;
    if (visRows != g_state.visibleRows) {
        g_state.visibleRows = visRows;
        FilterRecipients();
    }
    int maxDisplayItems = visRows;

    SelectObject(memDC, g_hFontNormal);

    int listX = marginX + cw * 4;

    if (g_state.isRefreshing) {
        SetTextColor(memDC, textBright);
        TextOutW(memDC, listX, y, L"Refreshing... Please wait", 25);
    } else if (g_state.filteredCount == 0) {
        SetTextColor(memDC, textDim);
        TextOutW(memDC, listX, y, L"No users found. Press 'r' to refresh", 36);
    } else {
        int endIndex = g_state.scrollOffset + maxDisplayItems;
        if (endIndex > g_state.filteredCount) endIndex = g_state.filteredCount;

        for (int i = g_state.scrollOffset; i < endIndex; i++) {
            int rIdx = g_state.filteredIndices[i];
            RecipientItem* r = &g_state.recipients[rIdx];
            bool isHighlighted = (!g_state.messageMode && i == g_state.highlightedFilteredIdx);

            int x = listX;

            SetTextColor(memDC, isHighlighted ? accentBlue : textDim);
            TextOutW(memDC, x, y, isHighlighted ? L">" : L" ", 1);
            x += cw * 2;

            SetTextColor(memDC, r->selected ? accentBlue : textDim);
            TextOutW(memDC, x, y, r->selected ? L"\x2713" : L" ", 1);
            x += cw * 2;

            COLORREF nameColor = isHighlighted ? accentBlue : (r->active ? textBright : textDim);
            SetTextColor(memDC, nameColor);
            TextOutW(memDC, x, y, r->name, (int)wcslen(r->name));

            y += g_itemHeight;
        }
    }

    SelectObject(memDC, g_hFontBold);

    wchar_t toSummary[256] = L"To: ";
    int selCount = 0;
    for (int i = 0; i < g_state.recipientCount; i++) {
        if (g_state.recipients[i].selected) {
            if (selCount > 0) wcscat_s(toSummary, 256, L", ");
            wcscat_s(toSummary, 256, g_state.recipients[i].name);
            selCount++;
        }
    }
    if (selCount == 0) wcscat_s(toSummary, 256, L"none");

    RECT toRect = { marginX, footY - 1, btnLeft - 12, footY + sendSize.cy + 1 };
    SetTextColor(memDC, selCount > 0 ? textBright : textDim);
    DrawTextW(memDC, toSummary, -1, &toRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (selCount > 0) {
        HBRUSH hBtnBrush = CreateSolidBrush(g_lightTheme ? RGB(0xEE, 0xF2, 0xFF) : RGB(0x1E, 0x29, 0x3B));
        FillRect(memDC, &sendRect, hBtnBrush);
        DeleteObject(hBtnBrush);

        HPEN hBtnPen = CreatePen(PS_SOLID, 1, g_lightTheme ? RGB(0xBF, 0xDB, 0xFE) : RGB(0x3B, 0x82, 0xF6));
        HPEN oldPen = (HPEN)SelectObject(memDC, hBtnPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, sendRect.left, sendRect.top, sendRect.right, sendRect.bottom);
        SelectObject(memDC, oldPen);
        SelectObject(memDC, oldBrush);
        DeleteObject(hBtnPen);

        SetTextColor(memDC, accentBlue);
    } else {
        HBRUSH hBtnBrush = CreateSolidBrush(g_lightTheme ? RGB(0xEE, 0xF0, 0xF4) : RGB(0x18, 0x18, 0x18));
        FillRect(memDC, &sendRect, hBtnBrush);
        DeleteObject(hBtnBrush);

        HPEN hBtnPen = CreatePen(PS_SOLID, 1, lineDim);
        HPEN oldPen = (HPEN)SelectObject(memDC, hBtnPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, sendRect.left, sendRect.top, sendRect.right, sendRect.bottom);
        SelectObject(memDC, oldPen);
        SelectObject(memDC, oldBrush);
        DeleteObject(hBtnPen);

        SetTextColor(memDC, textDim);
    }

    RECT btnTextRect = sendRect;
    DrawTextW(memDC, sendLabel, -1, &btnTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (hasStatus) {
        bool isAlert = (wcsstr(g_state.statusText, L"Error") != NULL || wcsstr(g_state.statusText, L"Warning") != NULL);
        SetTextColor(memDC, isAlert ? errorCoral : accentBlue);
        SelectObject(memDC, g_hFontNormal);
        RECT statusRect = { marginX, footY + sendSize.cy + 4, width - marginX, by2 - 4 };
        DrawTextW(memDC, g_state.statusText, -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    SelectObject(memDC, oldFont);

    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBM);
    DeleteObject(memBM);
    DeleteDC(memDC);
}

static int g_fontSize = 13;

static void RecreateIPSendFonts(HWND hWnd) {
    if (g_fontSize <= 0) g_fontSize = 13;
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    if (g_hFontCaption) DeleteObject(g_hFontCaption);
    UINT dpi = hWnd ? TinyDPI_GetDpiForWindow(hWnd) : 96;
    g_hFontNormal = TinyFont_CreateMonospace(g_fontSize, TINY_FONT_WEIGHT_NORMAL, dpi);
    g_hFontBold = TinyFont_CreateMonospace(g_fontSize, TINY_FONT_WEIGHT_BOLD, dpi);
    g_hFontCaption = TinyFont_CreateMonospace(g_fontSize, TINY_FONT_WEIGHT_NORMAL, dpi);
    if (g_hEdit && IsWindow(g_hEdit)) {
        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    }
}

// Window Procedure
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        DragAcceptFiles(hWnd, TRUE);
        g_fontSize = 13;
        g_lightTheme = IsSystemLightTheme();
        RecreateIPSendFonts(hWnd);

        g_hEdit = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT | WS_TABSTOP,
            0, 0, 0, 0,
            hWnd,
            (HMENU)1001,
            ((LPCREATESTRUCT)lParam)->hInstance,
            NULL
        );
        if (g_hEdit) {
            SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
        }
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        if ((HWND)lParam == g_hEdit) {
            HDC hdcEdit = (HDC)wParam;
            COLORREF bgVoid = g_lightTheme ? RGB(0xF8, 0xF9, 0xFC) : RGB(0x12, 0x12, 0x12);
            COLORREF textBright = g_lightTheme ? RGB(0x11, 0x14, 0x1A) : RGB(0xEE, 0xEE, 0xEE);
            SetTextColor(hdcEdit, textBright);
            SetBkColor(hdcEdit, bgVoid);
            if (g_hEditBrush) DeleteObject(g_hEditBrush);
            g_hEditBrush = CreateSolidBrush(bgVoid);
            return (LRESULT)g_hEditBrush;
        }
        break;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == 1001 && HIWORD(wParam) == EN_CHANGE) {
            // EN_CHANGE also fires while we programmatically push text into the
            // box; only mirror it back when the body is editable in place.
            if (g_hEdit && IsWindow(g_hEdit) && g_state.messageMode &&
                g_state.message.len <= MESSAGE_EDIT_MAX_CHARS) {
                SyncMessageFromEdit();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }
        break;
    }

    case WM_SETTINGCHANGE: {
        if (!g_themeManuallyToggled) {
            g_lightTheme = IsSystemLightTheme();
            if (g_hEdit && IsWindow(g_hEdit)) {
                InvalidateRect(g_hEdit, NULL, TRUE);
            }
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        RecreateIPSendFonts(hWnd);
        RECT *prc = (RECT *)lParam;
        if (prc) {
            SetWindowPos(hWnd, NULL, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_NCACTIVATE:
        return TRUE; // Prevent default Windows non-client frame painting on focus loss (white border)

    case WM_NCPAINT:
        return 0; // Prevent non-client frame painting

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        RECT rcWin;
        GetWindowRect(hWnd, &rcWin);
        int edge = 8;
        bool atL = pt.x < rcWin.left + edge;
        bool atR = pt.x > rcWin.right - edge;
        bool atT = pt.y < rcWin.top + edge;
        bool atB = pt.y > rcWin.bottom - edge;
        if (atT && atL) return HTTOPLEFT;
        if (atT && atR) return HTTOPRIGHT;
        if (atB && atL) return HTBOTTOMLEFT;
        if (atB && atR) return HTBOTTOMRIGHT;
        if (atT) return HTTOP;
        if (atB) return HTBOTTOM;
        if (atL) return HTLEFT;
        if (atR) return HTRIGHT;

        POINT cpt = pt;
        ScreenToClient(hWnd, &cpt);

        if (PtInRect(&g_sendRect, cpt)) {
            return HTCLIENT;
        }

        if (cpt.y < 40) return HTCAPTION;

        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = 360;
        pMMI->ptMinTrackSize.y = 260;
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        int lines = zDelta / WHEEL_DELTA;
        if (lines != 0) {
            g_state.scrollOffset -= lines * 2;
            int maxOffset = g_state.filteredCount - g_state.visibleRows;
            if (maxOffset < 0) maxOffset = 0;
            if (g_state.scrollOffset < 0) g_state.scrollOffset = 0;
            if (g_state.scrollOffset > maxOffset) g_state.scrollOffset = maxOffset;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_NCCALCSIZE:
        if (wParam) return 0;
        break;

    case WM_SIZE:
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (PtInRect(&g_sendRect, pt)) {
            ExecuteSend(hWnd);
            return 0;
        }

        if (PtInRect(&g_msgEditRect, pt)) {
            // Very large bodies stay send-only: loading megabytes into a native
            // multiline EDIT makes the window unusable to scroll and paint.
            if (g_state.message.len > MESSAGE_EDIT_MAX_CHARS) {
                ShowStatusToast(hWnd, L"Message is too large to edit here. It will be sent as-is.");
                return 0;
            }
            g_state.messageMode = true;
            if (g_hEdit) {
                SyncEditFromMessage();
                SetWindowPos(g_hEdit, NULL, g_msgEditRect.left, g_msgEditRect.top,
                             g_msgEditRect.right - g_msgEditRect.left,
                             g_msgEditRect.bottom - g_msgEditRect.top,
                             SWP_NOZORDER | SWP_SHOWWINDOW);
                SetFocus(g_hEdit);
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (g_state.messageMode && !PtInRect(&g_msgEditRect, pt)) {
            if (g_hEdit) {
                SyncMessageFromEdit();
                ShowWindow(g_hEdit, SW_HIDE);
            }
            g_state.messageMode = false;
        }

        if (pt.y >= g_listStartY && pt.y < g_listStartY + g_state.visibleRows * g_itemHeight) {
            int rowIdx = (pt.y - g_listStartY) / g_itemHeight;
            int itemIdx = g_state.scrollOffset + rowIdx;
            if (itemIdx >= 0 && itemIdx < g_state.filteredCount) {
                int rIdx = g_state.filteredIndices[itemIdx];
                if (g_state.recipients[rIdx].selected) {
                    g_state.recipients[rIdx].selected = false;
                    InvalidateRect(hWnd, NULL, FALSE);
                } else if (g_state.recipients[rIdx].active) {
                    g_state.recipients[rIdx].selected = true;
                    InvalidateRect(hWnd, NULL, FALSE);
                } else {
                    wchar_t warnBuf[256];
                    swprintf_s(warnBuf, 256, L"Warning: '%s' is offline.", g_state.recipients[rIdx].name);
                    ShowStatusToast(hWnd, warnBuf);
                }
                g_state.highlightedFilteredIdx = itemIdx;
                return 0;
            }
        }

        // Set focus back to search / UI
        SetFocus(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_COPYDATA: {
        PCOPYDATASTRUCT pcds = (PCOPYDATASTRUCT)lParam;
        if (pcds && pcds->lpData && pcds->cbData >= sizeof(wchar_t)) {
            const wchar_t* incomingCmd = (const wchar_t*)pcds->lpData;
            ParseCommandLine(incomingCmd, &g_state);
            ApplyRecipientSelection();
            if (g_hEdit && IsWindow(g_hEdit) && g_state.message.len <= MESSAGE_EDIT_MAX_CHARS) {
                SyncEditFromMessage();
            }
            SetForegroundWindow(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
            return TRUE;
        }
        return FALSE;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        int added = CollectFilesFromDrop(hDrop);
        DragFinish(hDrop);
        if (added > 0) {
            wchar_t status[128];
            swprintf_s(status, 128, L"Attached %d file(s).", added);
            ShowStatusToast(hWnd, status);
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (TinyFont_HandleZoomKey(wParam, &g_fontSize)) {
            RecreateIPSendFonts(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
            g_skipNextChar = true; // drop the synthesized WM_CHAR (e.g. '=')
            return 0;
        }
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrlDown && (wParam == 'O' || wParam == 'o')) {
            OpenFileDialog(hWnd);
            g_skipNextChar = true; // drop the synthesized 'o' WM_CHAR
            return 0;
        }

        if (ctrlDown && (wParam == 'V' || wParam == 'v')) {
            HandlePaste(hWnd);
            g_skipNextChar = true; // drop the synthesized 'v' WM_CHAR
            return 0;
        }

        if (ctrlDown && (wParam == 'D' || wParam == 'd')) {
            g_lightTheme = !g_lightTheme;
            g_themeManuallyToggled = true;
            if (g_hEdit && IsWindow(g_hEdit)) {
                InvalidateRect(g_hEdit, NULL, TRUE);
            }
            InvalidateRect(hWnd, NULL, FALSE);
            g_skipNextChar = true; // drop the synthesized 'd' WM_CHAR
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            if (g_state.messageMode) {
                if (g_hEdit) {
                    SyncMessageFromEdit();
                    ShowWindow(g_hEdit, SW_HIDE);
                }
                g_state.messageMode = false;
                SetFocus(hWnd);
                InvalidateRect(hWnd, NULL, FALSE);
            } else {
                DestroyWindow(hWnd);
            }
            return 0;
        }

        if (wParam == VK_INSERT) {
            if (!g_state.messageMode && g_state.message.len > MESSAGE_EDIT_MAX_CHARS) {
                // Send-only for huge bodies; opening the native EDIT would freeze.
                ShowStatusToast(hWnd, L"Message is too large to edit here. It will be sent as-is.");
                g_skipNextChar = true;
                return 0;
            }
            g_state.messageMode = !g_state.messageMode;
            if (g_state.messageMode) {
                if (g_hEdit) {
                    SyncEditFromMessage();
                    SetWindowPos(g_hEdit, NULL, g_msgEditRect.left, g_msgEditRect.top,
                                 g_msgEditRect.right - g_msgEditRect.left,
                                 g_msgEditRect.bottom - g_msgEditRect.top,
                                 SWP_NOZORDER | SWP_SHOWWINDOW);
                    SetFocus(g_hEdit);
                    int len = (int)g_state.message.len;
                    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                }
            } else {
                if (g_hEdit) {
                    SyncMessageFromEdit();
                    ShowWindow(g_hEdit, SW_HIDE);
                }
                SetFocus(hWnd);
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (!g_state.messageMode && (wParam == 'R' || wParam == 'r') && !ctrlDown && g_state.searchLen == 0) {
            RefreshRecipientList(hWnd);
            g_skipNextChar = true; // 'r' is a command here, not a search character
            return 0;
        }

        if (!g_state.messageMode && (wParam == VK_OEM_2 || wParam == '/')) {
            MessageBoxW(hWnd,
                L"\x2191/\x2193      Navigate recipients\n"
                L"Space    Toggle selection\n"
                L"Enter    Send\n"
                L"Insert   Edit message (normal text box)\n"
                L"r        Refresh recipients\n"
                L"Ctrl+O   Attach file\n"
                L"Ctrl+D   Toggle Light / Dark theme\n"
                L"Ctrl+/-  Adjust font size\n"
                L"/        Show this help\n"
                L"Esc      Close / Exit message mode",
                L"IP - Keyboard Shortcuts",
                MB_OK);
            g_skipNextChar = true; // '/' is a command here, not a search character
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (wParam == VK_RETURN) {
            ExecuteSend(hWnd);
            return 0;
        }

        if (!g_state.messageMode) {
            // Recipient search navigation & space toggle
            if (wParam == VK_UP) {
                if (g_state.highlightedFilteredIdx > 0) {
                    g_state.highlightedFilteredIdx--;
                    FilterRecipients();
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }

            if (wParam == VK_DOWN) {
                if (g_state.highlightedFilteredIdx < g_state.filteredCount - 1) {
                    g_state.highlightedFilteredIdx++;
                    FilterRecipients();
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }

            if (wParam == VK_SPACE) {
                if (g_state.filteredCount > 0 && g_state.highlightedFilteredIdx < g_state.filteredCount) {
                    int rIdx = g_state.filteredIndices[g_state.highlightedFilteredIdx];
                    if (g_state.recipients[rIdx].selected) {
                        g_state.recipients[rIdx].selected = false;
                        InvalidateRect(hWnd, NULL, FALSE);
                    } else if (g_state.recipients[rIdx].active) {
                        g_state.recipients[rIdx].selected = true;
                        InvalidateRect(hWnd, NULL, FALSE);
                    } else {
                        wchar_t warnBuf[256];
                        swprintf_s(warnBuf, 256, L"Warning: '%s' is offline.", g_state.recipients[rIdx].name);
                        ShowStatusToast(hWnd, warnBuf);
                    }
                    g_skipNextChar = true; // Space toggles, never searches
                }
                return 0;
            }

            if (wParam == VK_BACK) {
                if (g_state.searchLen > 0) {
                    g_state.searchLen--;
                    g_state.searchQuery[g_state.searchLen] = L'\0';
                    FilterRecipients();
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }
        }
        break;
    }

    case WM_CHAR: {
        // Swallow the character that Windows synthesizes for a shortcut we already
        // handled in WM_KEYDOWN, so it never lands in the recipient search query.
        if (g_skipNextChar) {
            g_skipNextChar = false;
            return 0;
        }

        wchar_t ch = (wchar_t)wParam;
        if (ch < 32 && ch != '\r' && ch != '\n' && ch != '\t') return 0;

        // Don't capture 'r' or '/' as search char if query is empty and triggering refresh/hints
        if ((ch == L'r' || ch == L'R') && g_state.searchLen == 0) return 0;
        if (ch == L'/' && g_state.searchLen == 0) return 0;
        if (ch == L' ' && g_state.searchLen == 0) return 0; // Space toggles selection when search empty

        if (ch >= 32 && g_state.searchLen < 60) {
            g_state.searchQuery[g_state.searchLen++] = ch;
            g_state.searchQuery[g_state.searchLen] = L'\0';
            FilterRecipients();
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == 100 && g_state.sending) {
            KillTimer(hWnd, 100);
            DestroyWindow(hWnd);
            return 0;
        }
        if (wParam == 200) {
            KillTimer(hWnd, 200);
            g_state.statusText[0] = L'\0';
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RenderTUIWindow(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontCaption) DeleteObject(g_hFontCaption);
        if (g_hEditBrush) DeleteObject(g_hEditBrush);
        StrBuf_Free(&g_state.message); // may hold a multi-megabyte pasted body
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // Message body is heap-backed with its own (much larger) ceiling than the
    // send command buffer.
    StrBuf_InitLimit(&g_state.message, MAX_MESSAGE_CHARS);

    if (!ParseCommandLine(GetCommandLineW(), &g_state)) {
        return 0;
    }

    if (g_state.registerContextMenu) {
        RegisterContextMenu();
        return 0;
    }

    if (g_state.unregisterContextMenu) {
        UnregisterContextMenu();
        return 0;
    }

    TinyDPI_EnablePerMonitorAwareness();

    // Single instance Mutex & Event check
    g_hMutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HasPayloadArguments()) {
            // Forward payload arguments to the active composer window
            HWND targetWnd = NULL;
            for (int retry = 0; retry < 50; retry++) {
                targetWnd = FindWindowW(L"TinyIPComposerWindowClass", NULL);
                if (targetWnd) break;
                Sleep(40);
            }

            if (targetWnd) {
                COPYDATASTRUCT cds;
                cds.dwData = 0x544950; // 'TIP'
                const wchar_t* cmd = GetCommandLineW();
                cds.cbData = (DWORD)(wcslen(cmd) + 1) * sizeof(wchar_t);
                cds.lpData = (PVOID)cmd;
                SendMessageW(targetWnd, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
            }

            if (g_hMutex) CloseHandle(g_hMutex);
            return 0;
        }

        // Double-launch without arguments: toggle off the running instance
        g_hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVENT_NAME);
        if (g_hEvent) {
            SetEvent(g_hEvent);
            CloseHandle(g_hEvent);
        }
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }
    g_hEvent = CreateEventW(NULL, FALSE, FALSE, EVENT_NAME);

    // Initialize default state & recipients
    // Order matters: ParseCommandLine() (above) only records --to, InitRecipients()
    // rebuilds the live list, then ApplyRecipientSelection() applies --to against it.
    g_state.visibleRows = 4;
    InitRecipients();
    ApplyRecipientSelection();

    // Auto-send CLI execution
    if (g_state.autoSend) {
        ExecuteSend(NULL);
        if (g_hEvent) CloseHandle(g_hEvent);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    // Register Window Class
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"TinyIPComposerWindowClass";
    RegisterClassExW(&wc);

    int winW = 500;
    int winH = 400;

    POINT ptCursor;
    GetCursorPos(&ptCursor);
    HMONITOR hMon = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    int posX = 100, posY = 100;
    if (GetMonitorInfoW(hMon, &mi)) {
        posX = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
        posY = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;
    } else {
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        posX = (scrW - winW) / 2;
        posY = (scrH - winH) / 2;
    }

    HWND hWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Send via IP",
        WS_POPUP | WS_THICKFRAME,
        posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) return 1;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    bool quit = false;
    while (!quit) {
        DWORD w = g_hEvent
            ? MsgWaitForMultipleObjects(1, &g_hEvent, FALSE, 100, QS_ALLINPUT)
            : MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            DestroyWindow(hWnd);
            break;
        }

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_hEvent) CloseHandle(g_hEvent);
    if (g_hMutex) CloseHandle(g_hMutex);

    return 0;
}
