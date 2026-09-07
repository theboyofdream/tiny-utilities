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
#include "common/tiny_dpi.h"
#include "common/font.h"
#include "common/clipboard.h"

#define MUTEX_NAME L"Global\\TinyIPSendMutex"
#define EVENT_NAME L"Global\\TinyIPSendEvent"

#define MAX_PAYLOADS 64
#define MAX_RECIPIENTS 128

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t displayName[MAX_PATH];
    bool isText;
    wchar_t textPreview[128];
} PayloadItem;

typedef struct {
    wchar_t name[128];
    wchar_t hostName[128];
    wchar_t ipAddr[64];
    wchar_t uid[128];
    bool active;
    bool selected;
} RecipientItem;

typedef struct {
    PayloadItem payloads[MAX_PAYLOADS];
    int payloadCount;

    wchar_t messageText[1024];
    int messageLen;
    bool messageMode; // True when editing message (i key)

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
static bool g_skipNextCharForInsertMode = false;
static bool g_skipNextCharForRefresh = false;
static bool g_skipNextCharForSlash = false;
static bool g_skipNextCharForSpace = false;
static bool g_skipNextCharForNewline = false;
static bool g_lightTheme = false;
static bool g_themeManuallyToggled = false;
static RECT g_sendRect = { 0 };
static int g_listStartY = 0;
static int g_itemHeight = 24;

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

// Add a file payload item
static void AddFilePayload(const wchar_t* path) {
    if (g_state.payloadCount >= MAX_PAYLOADS) return;
    for (int i = 0; i < g_state.payloadCount; i++) {
        if (_wcsicmp(g_state.payloads[i].path, path) == 0) return; // avoid duplicates
    }
    PayloadItem* item = &g_state.payloads[g_state.payloadCount++];
    wcscpy_s(item->path, MAX_PATH, path);
    wcscpy_s(item->displayName, MAX_PATH, GetFileNameFromPath(path));
    item->isText = false;
    item->textPreview[0] = L'\0';
}

// Add a text payload item
static void AddTextPayload(const wchar_t* text) {
    if (g_state.payloadCount >= MAX_PAYLOADS) return;
    PayloadItem* item = &g_state.payloads[g_state.payloadCount++];
    item->path[0] = L'\0';
    item->displayName[0] = L'\0';
    item->isText = true;
    wcsncpy_s(item->textPreview, 128, text, _TRUNCATE);
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

// Forward declaration
static bool QueryLiveIPMsgRecipients(const wchar_t* ipcmdPath);

// Refresh live recipient list (r shortcut)
static void RefreshRecipientList(HWND hWnd) {
    g_state.isRefreshing = true;
    if (hWnd) {
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);
    }

    wchar_t ipcmdPath[MAX_PATH];
    if (FindIPMsgExecutable(ipcmdPath, MAX_PATH)) {
        QueryLiveIPMsgRecipients(ipcmdPath);
        FilterRecipients();
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

                wcscpy_s(g_state.recipients[count].name, 128, wName);
                wcscpy_s(g_state.recipients[count].hostName, 128, wHost);
                wcscpy_s(g_state.recipients[count].ipAddr, 64, wIP[0] ? wIP : L"-");
                wcscpy_s(g_state.recipients[count].uid, 128, wUid);
                g_state.recipients[count].active = isActive;
                g_state.recipients[count].selected = false;
                count++;
            }
        }
        line = strtok_s(NULL, "\r\n", &context);
    }

    if (count > 0) {
        g_state.recipientCount = count;
        return true;
    }

    return false;
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
            if (_wcsicmp(pe.szExeFile, L"ipmsg.exe") == 0 || _wcsicmp(pe.szExeFile, L"ipcmd.exe") == 0) {
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
static void ParseCommandLine(AppState* state) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--context-menu") == 0) {
            if (i + 1 < argc) {
                if (_wcsicmp(argv[i + 1], L"register") == 0 || _wcsicmp(argv[i + 1], L"true") == 0 || wcscmp(argv[i + 1], L"1") == 0) {
                    state->registerContextMenu = true;
                    i++;
                } else if (_wcsicmp(argv[i + 1], L"unregister") == 0 || _wcsicmp(argv[i + 1], L"false") == 0 || wcscmp(argv[i + 1], L"0") == 0) {
                    state->unregisterContextMenu = true;
                    i++;
                } else if (argv[i + 1][0] == L'-') {
                    state->registerContextMenu = true;
                } else {
                    state->registerContextMenu = true;
                }
            } else {
                state->registerContextMenu = true;
            }
        } else if (_wcsicmp(argv[i], L"--register-context-menu") == 0 || _wcsicmp(argv[i], L"--register") == 0) {
            state->registerContextMenu = true;
        } else if (_wcsicmp(argv[i], L"--unregister-context-menu") == 0 || _wcsicmp(argv[i], L"--unregister") == 0) {
            state->unregisterContextMenu = true;
        } else if (_wcsicmp(argv[i], L"--send") == 0) {
            state->autoSend = true;
        } else if ((_wcsicmp(argv[i], L"--to") == 0 || _wcsicmp(argv[i], L"-t") == 0) && i + 1 < argc) {
            i++;
            wchar_t toBuf[256];
            wcscpy_s(toBuf, 256, argv[i]);
            wchar_t* nextToken = NULL;
            wchar_t* token = wcstok_s(toBuf, L",; ", &nextToken);
            while (token) {
                bool found = false;
                for (int r = 0; r < state->recipientCount; r++) {
                    if (_wcsicmp(state->recipients[r].name, token) == 0) {
                        state->recipients[r].selected = true;
                        found = true;
                        break;
                    }
                }
                if (!found && state->recipientCount < MAX_RECIPIENTS) {
                    wcscpy_s(state->recipients[state->recipientCount].name, 128, token);
                    wcscpy_s(state->recipients[state->recipientCount].hostName, 128, L"Custom");
                    state->recipients[state->recipientCount].active = true;
                    state->recipients[state->recipientCount].selected = true;
                    state->recipientCount++;
                }
                token = wcstok_s(NULL, L",; ", &nextToken);
            }
        } else if ((_wcsicmp(argv[i], L"--message") == 0 || _wcsicmp(argv[i], L"--msg") == 0 || _wcsicmp(argv[i], L"-m") == 0) && i + 1 < argc) {
            i++;
            wcscpy_s(state->messageText, 1024, argv[i]);
            state->messageLen = (int)wcslen(state->messageText);
        } else if (_wcsicmp(argv[i], L"--file") == 0 && i + 1 < argc) {
            i++;
            AddFilePayload(argv[i]);
        } else if (_wcsicmp(argv[i], L"--theme") == 0 && i + 1 < argc) {
            i++;
            if (_wcsicmp(argv[i], L"light") == 0) {
                g_lightTheme = true;
                g_themeManuallyToggled = true;
            } else if (_wcsicmp(argv[i], L"dark") == 0) {
                g_lightTheme = false;
                g_themeManuallyToggled = true;
            } else if (_wcsicmp(argv[i], L"system") == 0) {
                g_lightTheme = IsSystemLightTheme();
                g_themeManuallyToggled = false;
            }
        } else if (_wcsnicmp(argv[i], L"--theme=", 8) == 0) {
            const wchar_t *val = argv[i] + 8;
            if (_wcsicmp(val, L"light") == 0) {
                g_lightTheme = true;
                g_themeManuallyToggled = true;
            } else if (_wcsicmp(val, L"dark") == 0) {
                g_lightTheme = false;
                g_themeManuallyToggled = true;
            } else if (_wcsicmp(val, L"system") == 0) {
                g_lightTheme = IsSystemLightTheme();
                g_themeManuallyToggled = false;
            }
        } else if (argv[i][0] != L'-') {
            // Positional argument -> File path
            AddFilePayload(argv[i]);
        }
    }

    LocalFree(argv);
    FilterRecipients();
}

// Check if current process was launched with payload/file arguments
static bool HasPayloadArguments(void) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
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
    LocalFree(argv);
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

// Registry context menu registration
static bool RegisterContextMenu(void) {
    RemoveLegacyContextMenu();

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 32];
    swprintf_s(cmdLine, MAX_PATH + 32, L"\"%s\" \"%%1\"", exePath);

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

    if (g_hEdit && IsWindow(g_hEdit)) {
        GetWindowTextW(g_hEdit, g_state.messageText, 1024);
        g_state.messageLen = (int)wcslen(g_state.messageText);
    }

    EnsureIPMsgProcessRunning(ipcmdPath);

    // Build recipient list
    wchar_t toList[512] = { 0 };
    int selectedCount = 0;
    for (int i = 0; i < g_state.recipientCount; i++) {
        if (g_state.recipients[i].selected) {
            const wchar_t* target = (g_state.recipients[i].ipAddr[0] != L'\0' && wcscmp(g_state.recipients[i].ipAddr, L"-") != 0)
                ? g_state.recipients[i].ipAddr
                : g_state.recipients[i].name;
            if (selectedCount > 0) wcscat_s(toList, 512, L",");
            wcscat_s(toList, 512, target);
            selectedCount++;
        }
    }

    if (selectedCount == 0) {
        ShowStatusToast(hWnd, L"Warning: No recipients selected. Press Space to select.");
        return false;
    }

    // Build command line for ipcmd send
    // ipcmd send [/file=path1 ...] "uid1,uid2" "msg_body"
    wchar_t cmdLine[4096] = { 0 };
    wcscpy_s(cmdLine, 4096, L"\"");
    wcscat_s(cmdLine, 4096, ipcmdPath);
    wcscat_s(cmdLine, 4096, L"\" send");

    for (int i = 0; i < g_state.payloadCount; i++) {
        if (!g_state.payloads[i].isText && g_state.payloads[i].path[0] != L'\0') {
            wchar_t filePart[MAX_PATH + 16];
            wsprintfW(filePart, L" /file=\"%s\"", g_state.payloads[i].path);
            lstrcatW(cmdLine, filePart);
        }
    }

    wchar_t toPart[540];
    wsprintfW(toPart, L" \"%s\"", toList);
    lstrcatW(cmdLine, toPart);

    if (g_state.messageLen > 0) {
        wchar_t msgPart[1100];
        swprintf_s(msgPart, 1100, L" \"%s\"", g_state.messageText);
        lstrcatW(cmdLine, msgPart);
    } else {
        lstrcatW(cmdLine, L" \"\"");
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        wcscpy_s(g_state.statusText, 256, L"Sent successfully!");
    } else {
        wcscpy_s(g_state.statusText, 256, L"Error executing IP Messenger command.");
    }

    g_state.statusTime = GetTickCount();
    g_state.sending = true;

    if (hWnd) {
        InvalidateRect(hWnd, NULL, FALSE);
        SetTimer(hWnd, 100, 400, NULL); // Short delay before exit
    }

    return true;
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
            GetWindowTextW(hWnd, g_state.messageText, 1024);
            g_state.messageLen = (int)wcslen(g_state.messageText);
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
            GetWindowTextW(hWnd, g_state.messageText, 1024);
            g_state.messageLen = (int)wcslen(g_state.messageText);
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
        GetWindowTextW(hWnd, g_state.messageText, 1024);
        g_state.messageLen = (int)wcslen(g_state.messageText);
        break;
    }
    return CallWindowProcW(g_oldEditProc, hWnd, msg, wParam, lParam);
}

// Handle Paste (Ctrl+V)
static void HandlePaste(HWND hWnd) {
    if (g_state.messageMode && g_hEdit && IsWindow(g_hEdit)) {
        SendMessageW(g_hEdit, WM_PASTE, 0, 0);
        GetWindowTextW(g_hEdit, g_state.messageText, 1024);
        g_state.messageLen = (int)wcslen(g_state.messageText);
        InvalidateRect(hWnd, NULL, FALSE);
        return;
    }

    // Check for dropped files or text on clipboard
    if (OpenClipboard(hWnd)) {
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            HANDLE hDrop = GetClipboardData(CF_HDROP);
            if (hDrop) {
                HDROP hd = (HDROP)GlobalLock(hDrop);
                if (hd) {
                    UINT count = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
                    for (UINT i = 0; i < count; i++) {
                        wchar_t path[MAX_PATH];
                        DragQueryFileW(hd, i, path, MAX_PATH);
                        AddFilePayload(path);
                    }
                    GlobalUnlock(hDrop);
                }
            }
        } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
                wchar_t* clipText = (wchar_t*)GlobalLock(hData);
                if (clipText) {
                    AddTextPayload(clipText);
                    GlobalUnlock(hData);
                }
            }
        }
        CloseClipboard();
        InvalidateRect(hWnd, NULL, FALSE);
    }
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
            wsprintfW(nameBuf, L"@ %s", item->isText ? L"text snippet" : item->displayName);
            TextOutW(memDC, marginX, y, nameBuf, (int)lstrlenW(nameBuf));
            y += 18;

            SetTextColor(memDC, textDim);
            const wchar_t* detail = item->isText ? item->textPreview : item->path;
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

    int editH = 28;
    if (g_state.messageLen > 0 || g_state.messageMode) {
        RECT calcRect = { 0, 0, maxTextWidth - 8, 10000 };
        const wchar_t* measureStr = (g_state.messageLen > 0) ? g_state.messageText : L"A";
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
        if (g_state.messageLen > 0) {
            SetTextColor(memDC, textBright);
            RECT drawRect = { marginX, y + 2, marginX + maxTextWidth, y + editH };
            DrawTextW(memDC, g_state.messageText, -1, &drawRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            y += editH + 8;
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
            if (g_hEdit && IsWindow(g_hEdit)) {
                GetWindowTextW(g_hEdit, g_state.messageText, 1024);
                g_state.messageLen = (int)wcslen(g_state.messageText);
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
            g_state.messageMode = true;
            if (g_hEdit) {
                SetWindowTextW(g_hEdit, g_state.messageText);
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
                GetWindowTextW(g_hEdit, g_state.messageText, 1024);
                g_state.messageLen = (int)wcslen(g_state.messageText);
                ShowWindow(g_hEdit, SW_HIDE);
            }
            g_state.messageMode = false;
        }

        if (pt.y >= g_listStartY && pt.y < g_listStartY + g_state.visibleRows * g_itemHeight) {
            int rowIdx = (pt.y - g_listStartY) / g_itemHeight;
            int itemIdx = g_state.scrollOffset + rowIdx;
            if (itemIdx >= 0 && itemIdx < g_state.filteredCount) {
                int rIdx = g_state.filteredIndices[itemIdx];
                if (g_state.recipients[rIdx].active) {
                    g_state.recipients[rIdx].selected = !g_state.recipients[rIdx].selected;
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
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(incomingCmd, &argc);
            if (argv) {
                for (int i = 1; i < argc; i++) {
                    if (_wcsicmp(argv[i], L"--file") == 0 && i + 1 < argc) {
                        AddFilePayload(argv[++i]);
                    } else if ((_wcsicmp(argv[i], L"--message") == 0 || _wcsicmp(argv[i], L"--msg") == 0 || _wcsicmp(argv[i], L"-m") == 0) && i + 1 < argc) {
                        i++;
                        wcscpy_s(g_state.messageText, 1024, argv[i]);
                        g_state.messageLen = (int)wcslen(g_state.messageText);
                        if (g_hEdit && IsWindow(g_hEdit)) {
                            SetWindowTextW(g_hEdit, g_state.messageText);
                        }
                    } else if ((_wcsicmp(argv[i], L"--to") == 0 || _wcsicmp(argv[i], L"-t") == 0) && i + 1 < argc) {
                        i++;
                    } else if (_wcsicmp(argv[i], L"--theme") == 0 && i + 1 < argc) {
                        i++;
                        if (_wcsicmp(argv[i], L"light") == 0) {
                            g_lightTheme = true;
                            g_themeManuallyToggled = true;
                        } else if (_wcsicmp(argv[i], L"dark") == 0) {
                            g_lightTheme = false;
                            g_themeManuallyToggled = true;
                        } else if (_wcsicmp(argv[i], L"system") == 0) {
                            g_lightTheme = IsSystemLightTheme();
                            g_themeManuallyToggled = false;
                        }
                    } else if (_wcsnicmp(argv[i], L"--theme=", 8) == 0) {
                        const wchar_t *val = argv[i] + 8;
                        if (_wcsicmp(val, L"light") == 0) {
                            g_lightTheme = true;
                            g_themeManuallyToggled = true;
                        } else if (_wcsicmp(val, L"dark") == 0) {
                            g_lightTheme = false;
                            g_themeManuallyToggled = true;
                        } else if (_wcsicmp(val, L"system") == 0) {
                            g_lightTheme = IsSystemLightTheme();
                            g_themeManuallyToggled = false;
                        }
                    } else if (argv[i][0] != L'-') {
                        AddFilePayload(argv[i]);
                    }
                }
                LocalFree(argv);
            }
            SetForegroundWindow(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
            return TRUE;
        }
        return FALSE;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; i++) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hDrop, i, path, MAX_PATH);
            AddFilePayload(path);
        }
        DragFinish(hDrop);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (TinyFont_HandleZoomKey(wParam, &g_fontSize)) {
            RecreateIPSendFonts(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrlDown && (wParam == 'O' || wParam == 'o')) {
            OpenFileDialog(hWnd);
            return 0;
        }

        if (ctrlDown && (wParam == 'V' || wParam == 'v')) {
            HandlePaste(hWnd);
            return 0;
        }

        if (ctrlDown && (wParam == 'D' || wParam == 'd')) {
            g_lightTheme = !g_lightTheme;
            g_themeManuallyToggled = true;
            if (g_hEdit && IsWindow(g_hEdit)) {
                InvalidateRect(g_hEdit, NULL, TRUE);
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            if (g_state.messageMode) {
                if (g_hEdit) {
                    GetWindowTextW(g_hEdit, g_state.messageText, 1024);
                    g_state.messageLen = (int)wcslen(g_state.messageText);
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
            g_state.messageMode = !g_state.messageMode;
            if (g_state.messageMode) {
                if (g_hEdit) {
                    SetWindowTextW(g_hEdit, g_state.messageText);
                    SetWindowPos(g_hEdit, NULL, g_msgEditRect.left, g_msgEditRect.top,
                                 g_msgEditRect.right - g_msgEditRect.left,
                                 g_msgEditRect.bottom - g_msgEditRect.top,
                                 SWP_NOZORDER | SWP_SHOWWINDOW);
                    SetFocus(g_hEdit);
                    int len = (int)wcslen(g_state.messageText);
                    SendMessageW(g_hEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                }
            } else {
                if (g_hEdit) {
                    GetWindowTextW(g_hEdit, g_state.messageText, 1024);
                    g_state.messageLen = (int)wcslen(g_state.messageText);
                    ShowWindow(g_hEdit, SW_HIDE);
                }
                SetFocus(hWnd);
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (!g_state.messageMode && (wParam == 'R' || wParam == 'r') && !ctrlDown && g_state.searchLen == 0) {
            RefreshRecipientList(hWnd);
            g_skipNextCharForRefresh = true;
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
            g_skipNextCharForSlash = true;
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
                    if (g_state.recipients[rIdx].active) {
                        g_state.recipients[rIdx].selected = !g_state.recipients[rIdx].selected;
                        InvalidateRect(hWnd, NULL, FALSE);
                    } else {
                        wchar_t warnBuf[256];
                        swprintf_s(warnBuf, 256, L"Warning: '%s' is offline.", g_state.recipients[rIdx].name);
                        ShowStatusToast(hWnd, warnBuf);
                    }
                    g_skipNextCharForSpace = true;
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
        if (g_skipNextCharForNewline) {
            g_skipNextCharForNewline = false;
            return 0;
        }

        if (g_skipNextCharForSpace) {
            g_skipNextCharForSpace = false;
            return 0;
        }

        if (g_skipNextCharForInsertMode) {
            g_skipNextCharForInsertMode = false;
            return 0;
        }

        if (g_skipNextCharForRefresh) {
            g_skipNextCharForRefresh = false;
            return 0;
        }

        if (g_skipNextCharForSlash) {
            g_skipNextCharForSlash = false;
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
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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
    g_state.visibleRows = 4;
    InitRecipients();
    ParseCommandLine(&g_state);

    // Handle CLI context menu registration commands
    if (g_state.registerContextMenu) {
        RegisterContextMenu();
        if (g_hEvent) CloseHandle(g_hEvent);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    if (g_state.unregisterContextMenu) {
        UnregisterContextMenu();
        if (g_hEvent) CloseHandle(g_hEvent);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

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
