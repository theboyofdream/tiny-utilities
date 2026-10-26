/*
 * Kyper — zero-overhead AI-CLI agent control plane.
 * Source: kyper.c
 *
 * Spec: docs/spec-kyper.md. Agent guide: AGENTS.md / CHECKLIST.md.
 *
 * Behavior:
 *   Launch with credentials:  kyper --user <name> --pass <secret> [--port N] [--match a,b,c]
 *     -> single-instance HTTP API on 0.0.0.0:<port> (HTTP Basic auth, memory only).
 *     API (no HTML; JSON/text replies):
 *       GET  /          -> JSON: {status, statusError, agents:[{pid,name,path,mode}]}
 *       POST /spawn     -> form fields dir= args=  (spawn first-match binary)
 *       POST /steer     -> form fields pid= msg=   (pipe or console-inject)
 *       POST /kill      -> form fields pid=        (TerminateProcess)
 *   An mDNS responder thread announces the hub as kyper.local plus the
 *   DNS-SD service kyper._http._tcp.local, so LAN clients resolve it
 *   without a raw IP.
 *   Bare launch (no --user/--pass): acts as the toggle signal — a running hub exits cleanly.
 *   Ctrl+C exits cleanly.
 *
 * Steering (two combined mechanisms, per spec decision):
 *   - Hub-spawned agents: stdin is a pipe owned by the hub. Agents run in
 *     non-interactive batch mode; Steer writes one UTF-8 line into the pipe.
 *   - Externally launched agents (any terminal): best-effort AttachConsole(pid) +
 *     WriteConsoleInputW into their console input buffer; hub console is detached
 *     and re-attached around the injection.
 *
 * Hard rules kept: no services, no registry, no persistence, no config files,
 * no global hooks (console attach is per-process state, not a hook).
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include "common/tiny_http.h"
#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"
#include "common/tiny_tray.h"
#include "common/mdns.h"

#define APPMUTEX_NAME L"Global\\TinyKyperMutex"
#define APPEVENT_NAME L"Global\\TinyKyperEvent"

#define DEFAULT_PORT        8080
#define PORT_MIN            1
#define PORT_MAX            65535

#define DEFAULT_MATCHES     L"opencode,claude,codex"

#define MAX_MATCHES         16
#define MAX_MATCH_CHARS     63

#define MAX_AGENTS          32
#define MAX_SPAWN_SLOTS     16

#define MAX_PID_VALUE       1000000

#define MSG_MAX_CHARS       512       /* steer text limit */
#define ARGS_MAX_CHARS      512
#define DIR_MAX_CHARS       260

#define AUTH_REALM          "Kyper"
#define AUTH_TOKEN_CAP      64

#define MDNS_NAME           "kyper"      /* fixed hub identity -> kyper.local */
#define MDNS_PORT           5353
#define MDNS_BUF_CAP        8192

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int port;
    const wchar_t *user;
    const wchar_t *pass;
    wchar_t matches[MAX_MATCHES][MAX_MATCH_CHARS + 1];
    int matchCount;
    bool tray;
} Config;

typedef struct {
    DWORD pid;
    HANDLE stdinWrite;
} SpawnSlot;

static Config g_cfg;
static HANDLE g_toggleEvent = NULL;
static HANDLE g_wakeEvent = NULL;
static SOCKET g_server = INVALID_SOCKET;
static WSAEVENT g_acceptEvent = NULL;

static SpawnSlot g_spawns[MAX_SPAWN_SLOTS];
static CRITICAL_SECTION g_consoleCS;

static char g_authToken[AUTH_TOKEN_CAP];         /* Base64("user:pass") */
static wchar_t g_spawnExe[MAX_PATH];             /* matches[0] resolved at startup */
static wchar_t g_status[192];                    /* last action result text */
static bool g_statusError = false;
static bool g_running = true;
static struct sockaddr_in g_lanAddr;             /* this PC's LAN IPv4 (for A answers) */
static SOCKET g_mdnsSock = INVALID_SOCKET;

/* ------------------------------------------------------------------ */
/* Small utilities                                                      */
/* ------------------------------------------------------------------ */

static void Log(const char *line) {
    /* Console writes are lost while the console is detached for steering; harmless. */
    printf("kyper: %s\n", line);
}

static void SetStatus(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(g_status, sizeof(g_status) / sizeof(g_status[0]), fmt, ap);
    va_end(ap);
}

static void LowerInPlace(wchar_t *s) {
    for (; *s; s++) {
        if (*s >= L'A' && *s <= L'Z') *s = *s - L'A' + L'a';
    }
}

/* Case-insensitive substring: haystack already lowercased; needle lowercased in place. */
static bool FindTokenInLower(const wchar_t *lowerHaystack, const wchar_t *token) {
    if (!token[0]) return false;
    wchar_t needle[MAX_MATCH_CHARS + 1];
    if (wcslen(token) >= sizeof(needle) / sizeof(needle[0])) return false;
    wcscpy_s(needle, sizeof(needle) / sizeof(needle[0]), token);
    LowerInPlace(needle);

    for (const wchar_t *p = lowerHaystack; *p; p++) {
        const wchar_t *q = needle;
        const wchar_t *r = p;
        while (*q && *r == *q) { q++; r++; }
        if (!*q) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Process tracking                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    DWORD pid;
    wchar_t name[64];
    wchar_t path[300];
} AgentInfo;

static void ClearSpawnSlot(SpawnSlot *slot) {
    if (slot->stdinWrite) CloseHandle(slot->stdinWrite);
    slot->pid = 0;
    slot->stdinWrite = NULL;
}

static SpawnSlot *FindSpawnSlot(DWORD pid) {
    for (int i = 0; i < MAX_SPAWN_SLOTS; i++) {
        if (g_spawns[i].pid == pid) return &g_spawns[i];
    }
    return NULL;
}

static void PruneSpawnSlots(void) {
    for (int i = 0; i < MAX_SPAWN_SLOTS; i++) {
        if (!g_spawns[i].pid) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               g_spawns[i].pid);
        bool dead = true;
        if (h) {
            dead = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
            CloseHandle(h);
        }
        if (dead) ClearSpawnSlot(&g_spawns[i]);
    }
}

static int ScanAgents(AgentInfo *out, int maxAgents) {
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (count >= maxAgents) break;

            /* Lowercase a name copy for matching. */
            wchar_t lower[64];
            wcsncpy_s(lower, sizeof(lower) / sizeof(lower[0]), pe.szExeFile, _TRUNCATE);
            LowerInPlace(lower);

            bool matched = false;
            for (int i = 0; i < g_cfg.matchCount; i++) {
                if (FindTokenInLower(lower, g_cfg.matches[i])) { matched = true; break; }
            }
            if (!matched) continue;

            out[count].pid = pe.th32ProcessID;
            wcscpy_s(out[count].name, sizeof(out[count].name) / sizeof(out[count].name[0]),
                     pe.szExeFile);
            out[count].path[0] = L'\0';

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                DWORD sz = sizeof(out[count].path) / sizeof(out[count].path[0]);
                QueryFullProcessImageNameW(h, 0, out[count].path, &sz);
                CloseHandle(h);
            }
            count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

/* ------------------------------------------------------------------ */
/* Steering                                                             */
/* ------------------------------------------------------------------ */

/* Remember console-mates (shell/terminals sharing our console) so the hub console
 * can be re-attached after steering injections. */
static int CaptureConsoleMates(DWORD *mates, int maxMates) {
    DWORD self = GetCurrentProcessId();
    DWORD list[8];
    DWORD count = GetConsoleProcessList(list, 8);
    if (count == 0 || count == (DWORD)-1) return 0;
    if (count > 8) count = 8;
    int n = 0;
    for (DWORD i = 0; i < count; i++) {
        if (list[i] != self && n < maxMates) mates[n++] = list[i];
    }
    return n;
}

static void ReattachConsole(const DWORD *mates, int mateCount) {
    for (int i = 0; i < mateCount; i++) {
        if (AttachConsole(mates[i])) return;
    }
    /* Fallback: last resort — allocate our own (visible) console. */
    AllocConsole();
    SetConsoleOutputCP(65001);
}

/* Build TEXT+ENTER records and write them into the target console buffer. */
static bool ConsoleInjectLine(DWORD pid, const wchar_t *msg, const wchar_t **err) {
    EnterCriticalSection(&g_consoleCS);

    DWORD mates[8];
    int mateCount = CaptureConsoleMates(mates, 8);

    bool ok = false;
    if (!FreeConsole()) {
        *err = L"kyper: cannot detach own console for injection";
    } else if (!AttachConsole(pid)) {
        FreeConsole();
        ReattachConsole(mates, mateCount);
        *err = L"kyper: target has no console to inject (not a console process)";
    } else {
        HANDLE conin = CreateFileW(L"CONIN$",
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (conin == INVALID_HANDLE_VALUE) {
            *err = L"kyper: cannot open target console input";
        } else {
            INPUT_RECORD recs[(MSG_MAX_CHARS + 8) * 2];
            int recCount = 0;
            for (const wchar_t *p = msg; *p && recCount + 2 <= (int)(sizeof(recs) / sizeof(recs[0])); p++) {
                wchar_t ch = *p;
                if (ch == L'\r') continue;
                bool special = (ch == L'\n');                    /* \n -> Enter */
                WORD vk   = special ? VK_RETURN : 0;
                WORD scan = special ? 0x1C     : 0;
                wchar_t uni = special ? L'\r' : ch;

                INPUT_RECORD *r = &recs[recCount++];
                ZeroMemory(r, sizeof(INPUT_RECORD));
                r->EventType = KEY_EVENT;
                r->Event.KeyEvent.bKeyDown = TRUE;
                r->Event.KeyEvent.wRepeatCount = 1;
                r->Event.KeyEvent.wVirtualKeyCode = vk;
                r->Event.KeyEvent.wVirtualScanCode = scan;
                r->Event.KeyEvent.uChar.UnicodeChar = uni;

                INPUT_RECORD *r2 = &recs[recCount++];
                ZeroMemory(r2, sizeof(INPUT_RECORD));
                r2->EventType = KEY_EVENT;
                r2->Event.KeyEvent.bKeyDown = FALSE;
                r2->Event.KeyEvent.wRepeatCount = 1;
                r2->Event.KeyEvent.wVirtualKeyCode = vk;
                r2->Event.KeyEvent.wVirtualScanCode = scan;
                r2->Event.KeyEvent.uChar.UnicodeChar = uni;
            }

            /* Agents read line-oriented input: submit the line (like PipeSteer appends \n). */
            if (recCount + 2 <= (int)(sizeof(recs) / sizeof(recs[0]))) {
                INPUT_RECORD *r = &recs[recCount++];
                ZeroMemory(r, sizeof(INPUT_RECORD));
                r->EventType = KEY_EVENT;
                r->Event.KeyEvent.bKeyDown = TRUE;
                r->Event.KeyEvent.wRepeatCount = 1;
                r->Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
                r->Event.KeyEvent.wVirtualScanCode = 0x1C;
                r->Event.KeyEvent.uChar.UnicodeChar = L'\r';

                INPUT_RECORD *r2 = &recs[recCount++];
                ZeroMemory(r2, sizeof(INPUT_RECORD));
                r2->EventType = KEY_EVENT;
                r2->Event.KeyEvent.bKeyDown = FALSE;
                r2->Event.KeyEvent.wRepeatCount = 1;
                r2->Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
                r2->Event.KeyEvent.wVirtualScanCode = 0x1C;
                r2->Event.KeyEvent.uChar.UnicodeChar = L'\r';
            }

            bool wrote = true;
            for (int off = 0; off < recCount && wrote; off += 256) {
                int chunk = recCount - off;
                if (chunk > 256) chunk = 256;
                DWORD written = 0;
                if (!WriteConsoleInputW(conin, &recs[off], (DWORD)chunk, &written) ||
                    written != (DWORD)chunk) {
                    wrote = false;
                }
            }
            CloseHandle(conin);
            ok = wrote;
            if (!wrote) *err = L"kyper: console injection was rejected";
        }
        FreeConsole();
        ReattachConsole(mates, mateCount);
    }

    LeaveCriticalSection(&g_consoleCS);
    return ok;
}

/* Owned-pipe steering for hub-spawned agents. False + *err==NULL => try console fallback. */
static bool PipeSteer(DWORD pid, const wchar_t *msg, const wchar_t **err) {
    SpawnSlot *slot = FindSpawnSlot(pid);
    if (!slot) return false;

    char utf8[MSG_MAX_CHARS * 4 + 4];
    int n = TinyHTTP_WideToUtf8(msg, utf8, sizeof(utf8) - 2);
    if (n < 0) {
        *err = L"kyper: message encoding failed";
        return false;
    }
    if (n == 0) {
        *err = L"kyper: empty message";
        return false;
    }
    utf8[n] = '\n';
    utf8[n + 1] = '\0';

    /* Slot stays alive across messages; the pipe handle is kept for repeat steers. */
    DWORD sent = 0;
    if (WriteFile(slot->stdinWrite, utf8, (DWORD)(n + 1), &sent, NULL) && sent > 0) {
        return true;
    }

    DWORD gle = GetLastError();
    ClearSpawnSlot(slot);
    if (gle == ERROR_NO_DATA || gle == ERROR_BROKEN_PIPE) {
        *err = L"stdio pipe broken (agent exited or closed stdin)";
    } else {
        *err = L"kyper: pipe write failed";
    }
    return false;
}

/* Combined steering: owned pipe first (hub-spawned agents), console injection fallback. */
static bool SteerAgent(DWORD pid, const wchar_t *msg, const wchar_t **err) {
    *err = NULL;
    if (PipeSteer(pid, msg, err)) return true;
    return ConsoleInjectLine(pid, msg, err);
}

/* ------------------------------------------------------------------ */
/* Spawn                                                                */
/* ------------------------------------------------------------------ */

static bool ArgsAreSafe(const wchar_t *s) {
    for (const wchar_t *p = s; *p; p++) {
        if ((unsigned)*p < 0x20 || (unsigned)*p == 0x7F) return false;
    }
    return true;
}

/* Spawn the first-match binary with owned-stdin pipe in the given directory.
 * On success *outPid receives the new PID. */
static bool SpawnAgent(const wchar_t *dir, const wchar_t *args, DWORD *outPid, wchar_t *err) {
    *outPid = 0;
    if (!g_spawnExe[0]) {
        wcscpy_s(err, 256, L"Spawn binary unresolved — adjust --match <first-name>.");
        return false;
    }
    if (!dir[0]) {
        wcscpy_s(err, 256, L"Working directory is required.");
        return false;
    }
    if (wcslen(args) > ARGS_MAX_CHARS || !ArgsAreSafe(args)) {
        wcscpy_s(err, 256, L"Invalid args (length or control characters).");
        return false;
    }

    DWORD attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        wcscpy_s(err, 256, L"Working directory does not exist.");
        return false;
    }

    PruneSpawnSlots();
    int freeSlot = -1;
    for (int i = 0; i < MAX_SPAWN_SLOTS; i++) {
        if (!g_spawns[i].pid) { freeSlot = i; break; }
    }
    if (freeSlot < 0) {
        wcscpy_s(err, 256, L"Registry full (hub-spawned agent slots exhausted).");
        return false;
    }

    /* Owned stdin pipe (inheritable read end goes to the child). */
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        wcscpy_s(err, 256, L"Cannot create steering pipe.");
        return false;
    }
    SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

    /* Child stdout/stderr -> hub console (visible; batch output), fallback NUL. */
    HANDLE hChildOut = NULL;
    HANDLE mine = GetStdHandle(STD_OUTPUT_HANDLE);
    if (mine && mine != INVALID_HANDLE_VALUE) {
        DuplicateHandle(GetCurrentProcess(), mine, GetCurrentProcess(), &hChildOut,
                        0, TRUE, DUPLICATE_SAME_ACCESS);
    }
    if (!hChildOut) {
        hChildOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    wchar_t cmdline[MAX_PATH + ARGS_MAX_CHARS + 8];
    swprintf_s(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), L"\"%ls\" %ls", g_spawnExe, args);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = hChildOut;
    si.hStdError = hChildOut;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, dir, &si, &pi);
    CloseHandle(hRead);
    CloseHandle(hChildOut);

    if (!ok) {
        CloseHandle(hWrite);
        swprintf_s(err, 256, L"CreateProcess failed (Win32 error %lu).", GetLastError());
        return false;
    }

    g_spawns[freeSlot].pid = pi.dwProcessId;
    g_spawns[freeSlot].stdinWrite = hWrite;
    *outPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* ------------------------------------------------------------------ */
/* Kill                                                                 */
/* ------------------------------------------------------------------ */

static bool KillAgent(DWORD pid, wchar_t *err) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        swprintf_s(err, 256, L"Cannot open process %lu.", pid);
        return false;
    }
    BOOL ok = TerminateProcess(h, 0);
    CloseHandle(h);
    if (!ok) {
        swprintf_s(err, 256, L"TerminateProcess failed for %lu.", pid);
        return false;
    }
    SpawnSlot *slot = FindSpawnSlot(pid);
    if (slot) ClearSpawnSlot(slot);
    return true;
}

/* ------------------------------------------------------------------ */
/* mDNS responder (announces kyper.local -> our HTTP port)               */
/* ------------------------------------------------------------------ */

static bool AsciiEquals(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    while (*a == '.') a++;
    while (*b == '.') b++;
    return (*a == '\0' && *b == '\0');
}

/* mdns_string_extract leaves a trailing dot and does NOT NUL-terminate; clean it. */
static void ExtractMdnsName(const void *data, size_t size, size_t nameOffset,
                            char *out, size_t cap) {
    if (!cap) return;
    size_t ofs = nameOffset;
    mdns_string_t s = mdns_string_extract(data, size, &ofs, out, cap);
    if (s.length > 0 && s.length < cap && out[s.length - 1] == '.')
        s.length--;                      /* strip trailing dot */
    out[s.length < cap ? s.length : cap - 1] = '\0';
}

static int MdnsCallback(int sock, const struct sockaddr *from, size_t addrlen,
                        mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                        uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                        size_t name_offset, size_t name_length, size_t record_offset,
                        size_t record_length, void *user_data) {
    (void)from; (void)addrlen; (void)query_id; (void)rclass; (void)ttl;
    (void)record_offset; (void)record_length; (void)user_data;

    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;

    /* Extract printable question name. */
    char qname[256];
    ExtractMdnsName(data, size, name_offset, qname, sizeof(qname));

    char host[] = MDNS_NAME ".local";
    char inst[] = MDNS_NAME "._http._tcp.local";
    char svc[] = "_http._tcp.local";

    if (!AsciiEquals(qname, host) && !AsciiEquals(qname, svc)) return 0;

    mdns_record_t answer;
    mdns_record_t additional[3];
    int addCount = 0;

    mdns_record_t aRec, ptrRec, srvRec;
    ZeroMemory(&aRec, sizeof(aRec));
    ZeroMemory(&ptrRec, sizeof(ptrRec));
    ZeroMemory(&srvRec, sizeof(srvRec));

    /* A record for kyper.local. */
    aRec.name.str = host;
    aRec.name.length = strlen(host);
    aRec.type = MDNS_RECORDTYPE_A;
    aRec.data.a.addr = g_lanAddr;

    /* PTR for service discovery: _http._tcp.local -> kyper._http._tcp.local */
    ptrRec.name.str = svc;
    ptrRec.name.length = strlen(svc);
    ptrRec.type = MDNS_RECORDTYPE_PTR;
    ptrRec.data.ptr.name.str = inst;
    ptrRec.data.ptr.name.length = strlen(inst);

    /* SRV advertises our HTTP port, host resolution. */
    srvRec.name.str = inst;
    srvRec.name.length = strlen(inst);
    srvRec.type = MDNS_RECORDTYPE_SRV;
    srvRec.data.srv.priority = 0;
    srvRec.data.srv.weight = 0;
    srvRec.data.srv.port = (uint16_t)g_cfg.port;
    srvRec.data.srv.name.str = host;
    srvRec.data.srv.name.length = strlen(host);

    if (AsciiEquals(qname, host)) {
        answer = aRec;
        additional[addCount++] = ptrRec;
        additional[addCount++] = srvRec;
    } else {
        answer = ptrRec;
        additional[addCount++] = srvRec;
        additional[addCount++] = aRec;
    }

    static char txBuf[MDNS_BUF_CAP];
    mdns_query_answer_multicast(sock, txBuf, sizeof(txBuf),
                                answer, NULL, 0, additional, (size_t)addCount);
    return 0;
}

/* One recvfrom per iteration (lib sets the socket non-blocking on win32). */
static DWORD WINAPI MdnsThread(LPVOID arg) {
    (void)arg;

    struct sockaddr_in bindAddr;
    ZeroMemory(&bindAddr, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(MDNS_PORT);
    bindAddr.sin_addr.s_addr = g_lanAddr.sin_addr.s_addr;

    g_mdnsSock = mdns_socket_open_ipv4(&bindAddr);
    if (g_mdnsSock < 0) {
        g_mdnsSock = INVALID_SOCKET;
        Log("mDNS responder unavailable (bind 5353 busy?); use http://<ip> instead");
        return 0;
    }
    Log("mDNS responder active: " MDNS_NAME ".local");

    static char rx[MDNS_BUF_CAP];
    while (g_running) {
        mdns_socket_listen(g_mdnsSock, rx, sizeof(rx), MdnsCallback, NULL);
        Sleep(20);
    }

    closesocket(g_mdnsSock);
    g_mdnsSock = INVALID_SOCKET;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Status JSON (API mode — no HTML)                                     */
/* ------------------------------------------------------------------ */

#define JSON_CAP            16384

static char g_json[JSON_CAP];
static size_t g_jsonLen = 0;

static void JsonAppend(const char *fmt, ...) {
    if (g_jsonLen >= JSON_CAP - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_json + g_jsonLen, JSON_CAP - g_jsonLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_jsonLen += (size_t)n;
        if (g_jsonLen >= JSON_CAP) g_jsonLen = JSON_CAP - 1;
        g_json[g_jsonLen] = '\0';
    }
}

/* Append text as a JSON string (quoted + escaped). */
static void JsonString(const char *text) {
    JsonAppend("\"");
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[3] = { '\\', *p, '\0' };
            JsonAppend("%s", esc);
        } else if ((unsigned char)*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
            JsonAppend("%s", esc);
        } else {
            char one[2] = { *p, '\0' };
            JsonAppend("%s", one);
        }
    }
    JsonAppend("\"");
}

/* GET / : running agents + last action status. */
static void BuildStatusJson(void) {
    PruneSpawnSlots();

    AgentInfo agents[MAX_AGENTS];
    int nAgents = ScanAgents(agents, MAX_AGENTS);

    char statusU[600];
    statusU[0] = '\0';
    TinyHTTP_WideToUtf8(g_status, statusU, sizeof(statusU));

    g_jsonLen = 0;
    g_json[0] = '\0';
    JsonAppend("{\"port\":%d,\"statusError\":%s,\"status\":",
               g_cfg.port, g_statusError ? "true" : "false");
    JsonString(statusU);
    JsonAppend(",\"agents\":[");

    for (int i = 0; i < nAgents; i++) {
        bool piped = FindSpawnSlot(agents[i].pid) != NULL;
        char nameU[200];
        char pathU[940];
        nameU[0] = '\0';
        pathU[0] = '\0';
        TinyHTTP_WideToUtf8(agents[i].name, nameU, sizeof(nameU));
        TinyHTTP_WideToUtf8(agents[i].path, pathU, sizeof(pathU));

        JsonAppend("%s{\"pid\":%lu,\"name\":", i ? "," : "", agents[i].pid);
        JsonString(nameU);
        JsonAppend(",\"path\":");
        JsonString(pathU);
        JsonAppend(",\"mode\":\"%s\"}", piped ? "pipe" : "console");
    }
    JsonAppend("]}");
}

/* ------------------------------------------------------------------ */
/* HTTP handling                                                        */
/* ------------------------------------------------------------------ */

/* Plain-text result of the last action (shared by all POST endpoints). */
static void ReplyStatusText(SOCKET client) {
    char text[600];
    text[0] = '\0';
    TinyHTTP_WideToUtf8(g_status, text, sizeof(text));
    TinyHTTP_ReplyUtf8(client,
                       g_statusError ? 400 : 200,
                       g_statusError ? "Bad Request" : "OK",
                       "text/plain; charset=utf-8", text);
}

static void HandleClient(SOCKET client) {
    TinyHTTPRequest req;
    if (TinyHTTP_LoadRequest(client, &req) != 0) return;

    bool authorized = (_strnicmp(req.auth, "Basic ", 6) == 0 &&
                       strcmp(req.auth + 6, g_authToken) == 0);
    if (!authorized) {
        TinyHTTP_ReplyUnauthorized(client, AUTH_REALM);
        return;
    }

    bool isGet = (strcmp(req.method, "GET") == 0);
    bool isRoot = (req.target[0] == '/' &&
                   (req.target[1] == '\0' || req.target[1] == '?'));

    if (isGet && isRoot) {
        BuildStatusJson();
        TinyHTTP_ReplyUtf8(client, 200, "OK", "application/json", g_json);
        return;
    }

    wchar_t pidStr[16];
    wchar_t err[256];
    err[0] = L'\0';

    if (strcmp(req.method, "POST") == 0 && strncmp(req.target, "/spawn", 6) == 0) {
        wchar_t dir[DIR_MAX_CHARS + 1];
        wchar_t args[ARGS_MAX_CHARS + 1];
        dir[0] = L'\0';
        args[0] = L'\0';
        TinyHTTP_FormFieldWide(req.body, "dir", dir, DIR_MAX_CHARS + 1);
        TinyHTTP_FormFieldWide(req.body, "args", args, ARGS_MAX_CHARS + 1);

        DWORD newPid = 0;
        if (SpawnAgent(dir, args, &newPid, err)) {
            g_statusError = false;
            SetStatus(L"Spawned agent PID %lu in %ls (pipe steering).", newPid, dir);
        } else {
            g_statusError = true;
            SetStatus(L"%ls", err);
        }
        ReplyStatusText(client);
        return;
    }

    if (strcmp(req.method, "POST") == 0 && strncmp(req.target, "/steer", 6) == 0) {
        wchar_t msg[MSG_MAX_CHARS + 1];
        msg[0] = L'\0';
        TinyHTTP_FormFieldWide(req.body, "pid", pidStr, sizeof(pidStr) / sizeof(pidStr[0]));
        TinyHTTP_FormFieldWide(req.body, "msg", msg, MSG_MAX_CHARS + 1);

        DWORD pid = (pidStr[0] >= L'0' && pidStr[0] <= L'9')
                        ? (DWORD)wcstoul(pidStr, NULL, 10) : 0;
        const wchar_t *sErr = NULL;

        if (!msg[0]) {
            g_statusError = true;
            SetStatus(L"No steer message provided.");
        } else if (pid == 0 || pid >= MAX_PID_VALUE) {
            g_statusError = true;
            SetStatus(L"Invalid PID.");
        } else if (SteerAgent(pid, msg, &sErr)) {
            g_statusError = false;
            SetStatus(L"Sent text to agent %lu.", pid);
        } else {
            g_statusError = true;
            SetStatus(L"%ls", sErr ? sErr : L"Steer failed.");
        }
        ReplyStatusText(client);
        return;
    }

    if (strcmp(req.method, "POST") == 0 && strncmp(req.target, "/kill", 5) == 0) {
        TinyHTTP_FormFieldWide(req.body, "pid", pidStr, sizeof(pidStr) / sizeof(pidStr[0]));
        DWORD pid = (pidStr[0] >= L'0' && pidStr[0] <= L'9')
                        ? (DWORD)wcstoul(pidStr, NULL, 10) : 0;
        if (pid == 0 || pid >= MAX_PID_VALUE) {
            g_statusError = true;
            SetStatus(L"Invalid PID.");
        } else if (KillAgent(pid, err)) {
            g_statusError = false;
            SetStatus(L"Agent %lu terminated.", pid);
        } else {
            g_statusError = true;
            SetStatus(L"%ls", err);
        }
        ReplyStatusText(client);
        return;
    }

    TinyHTTP_ReplyNotFound(client);
}

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CONFIG_OK = 0,
    CONFIG_HELP_SHOWN,
    CONFIG_ERROR
} ConfigStatus;

static bool BuildConfig(Config *cfg, ConfigStatus *status) {
    *status = CONFIG_OK;
    cfg->port = DEFAULT_PORT;
    cfg->user = NULL;
    cfg->pass = NULL;
    cfg->tray = true;

    /* TinyCLI_ParseCommandLine stores pointers into its own stack buffer, so all
     * string values must be copied out before this function returns. */
    static wchar_t g_userBuf[64];
    static wchar_t g_passBuf[64];
    static wchar_t g_matchBuf[MAX_MATCHES * (MAX_MATCH_CHARS + 2)];

    const wchar_t *userStr = NULL;
    const wchar_t *passStr = NULL;
    const wchar_t *matchStr = NULL;

    const CliOption opts[] = {
        { L"--port",  CLI_OPT_INT,    &cfg->port,  PORT_MIN, PORT_MAX, L"<1-65535>", L"HTTP API port (default: 8080)" },
        { L"-p",      CLI_OPT_INT,    &cfg->port,  PORT_MIN, PORT_MAX, NULL,         NULL },
        { L"--user",  CLI_OPT_STRING, &userStr,    0, 0, L"<name>",   L"Basic auth user (required to start)" },
        { L"-u",      CLI_OPT_STRING, &userStr,    0, 0, NULL,        NULL },
        { L"--pass",  CLI_OPT_STRING, &passStr,    0, 0, L"<secret>", L"Basic auth password (required to start)" },
        { L"--match", CLI_OPT_STRING, &matchStr,   0, 0, L"<a,b,c>",  L"Agent name match list (default: opencode,claude,codex)" },
        { L"-m",      CLI_OPT_STRING, &matchStr,   0, 0, NULL,        NULL },
        { L"--tray",  CLI_OPT_BOOL,   &cfg->tray,  0, 0, L"<true|false>", L"Notification-area icon with Exit (default: true)" },
    };

    if (TinyCLI_CheckHelpCommandLine(
            L"kyper",
            L"Zero-overhead AI-CLI agent control plane",
            L"kyper --user <name> --pass <secret> [--port <1-65535>] [--match <a,b,c>] [--tray <true|false>]",
            opts, sizeof(opts) / sizeof(opts[0]))) {
        *status = CONFIG_HELP_SHOWN;
        return false;
    }

    TinyCLI_ParseCommandLine(opts, sizeof(opts) / sizeof(opts[0]));

    if (userStr) wcsncpy_s(g_userBuf, sizeof(g_userBuf) / sizeof(g_userBuf[0]), userStr, _TRUNCATE);
    if (passStr) wcsncpy_s(g_passBuf, sizeof(g_passBuf) / sizeof(g_passBuf[0]), passStr, _TRUNCATE);
    cfg->user = userStr ? g_userBuf : NULL;
    cfg->pass = passStr ? g_passBuf : NULL;

    /* Match list: comma-split into fixed buffers. */
    cfg->matchCount = 0;
    wcscpy_s(g_matchBuf, sizeof(g_matchBuf) / sizeof(g_matchBuf[0]),
             matchStr ? matchStr : DEFAULT_MATCHES);
    wchar_t *ctx = NULL;
    wchar_t *tok = wcstok_s(g_matchBuf, L",", &ctx);
    while (tok && cfg->matchCount < MAX_MATCHES) {
        while (*tok == L' ') tok++;
        size_t len = wcslen(tok);
        while (len > 0 && tok[len - 1] == L' ') tok[--len] = L'\0';
        if (len > 0) {
            wcsncpy_s(cfg->matches[cfg->matchCount],
                      sizeof(cfg->matches[cfg->matchCount]) / sizeof(wchar_t),
                      tok, _TRUNCATE);
            LowerInPlace(cfg->matches[cfg->matchCount]);
            if (wcslen(cfg->matches[cfg->matchCount]) > MAX_MATCH_CHARS) {
                *status = CONFIG_ERROR;
                return false;
            }
            cfg->matchCount++;
        }
        tok = wcstok_s(NULL, L",", &ctx);
    }

    if (cfg->matchCount == 0) {
        *status = CONFIG_ERROR;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running = false;
        if (g_wakeEvent) SetEvent(g_wakeEvent);
        return TRUE;
    }
    return FALSE;
}

/* Invoked by tiny_tray menu "Exit" / double-click / TINYTRAY_MSG_EXIT. */
static void TrayExitRequest(void) {
    Log("exit requested from tray");
    g_running = false;
    if (g_wakeEvent) SetEvent(g_wakeEvent);   /* wake the main wait instantly */
}

static void PrintBanner(void) {
    wchar_t ip[16];
    if (!TinyHTTP_GetFirstLanIPv4(ip, sizeof(ip) / sizeof(ip[0]))) {
        wcscpy_s(ip, sizeof(ip) / sizeof(ip[0]), L"127.0.0.1");
    }
    wprintf(L"kyper: agent control plane on http://%ls:%d (user: %ls)\n",
            ip, g_cfg.port, g_cfg.user);
    wprintf(L"kyper: mDNS announce: kyper.local (+ kyper._http._tcp.local)\n");
    wprintf(L"kyper: match list:");
    for (int i = 0; i < g_cfg.matchCount; i++) {
        wprintf(L" %ls", g_cfg.matches[i]);
    }
    if (g_spawnExe[0]) {
        wprintf(L"\nkyper: spawn binary: %ls (stdin piped -> batch mode)", g_spawnExe);
    } else {
        wprintf(L"\nkyper: spawn binary unresolved (spawn disabled)");
    }
    wprintf(L"\nkyper: GET / returns JSON status; POST /spawn | /steer pid,msg | /kill pid");
    wprintf(L"\nkyper: run 'kyper' again to toggle off; Ctrl+C or tray Exit also exits\n\n");
}

/* Build "user:pass" Base64 token into g_authToken. */
static bool BuildAuthToken(void) {
    char userA[64], passA[64], creds[130];
    if (TinyHTTP_WideToUtf8(g_cfg.user, userA, sizeof(userA)) < 0 ||
        TinyHTTP_WideToUtf8(g_cfg.pass, passA, sizeof(passA)) < 0) {
        return false;
    }
    snprintf(creds, sizeof(creds), "%s:%s", userA, passA);
    return TinyHTTP_Base64Encode(creds, strlen(creds), g_authToken, AUTH_TOKEN_CAP);
}

int wmain(void) {
    /* Buffering: redirected stdout is fully buffered; make logs instantaneous. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    ConfigStatus cfgStatus;
    if (!BuildConfig(&g_cfg, &cfgStatus)) {
        if (cfgStatus == CONFIG_ERROR) {
            fprintf(stderr, "kyper: no agent match resolved — check --match <a,b,c>\n");
        }
        return (cfgStatus == CONFIG_HELP_SHOWN) ? 0 : 1;
    }

    /* Bare invocation (no credentials) acts as a pure toggle signal. */
    if (!g_cfg.user || !g_cfg.pass || !g_cfg.user[0] || !g_cfg.pass[0]) {
        if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &g_toggleEvent)) {
            printf("kyper: toggle signal sent — running hub exits\n");
            return 0;
        }
        CloseHandle(g_toggleEvent);
        g_toggleEvent = NULL;
        printf("kyper: no hub running; start with --user <name> --pass <secret>\n");
        return 1;
    }

    /* Full launch: single instance + clean toggle-off contract. */
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &g_toggleEvent)) {
        printf("kyper: toggle signal sent — running hub exits\n");
        return 0;
    }

    if (!BuildAuthToken()) {
        printf("kyper: credentials not encodable or too long\n");
        return 1;
    }

    /* Resolve spawn binary from the first match token. */
    {
        wchar_t found[MAX_PATH];
        if (SearchPathW(NULL, g_cfg.matches[0], L".exe", MAX_PATH, found, NULL) > 0) {
            wcscpy_s(g_spawnExe, MAX_PATH, found);
        } else {
            g_spawnExe[0] = L'\0';
        }
    }

    SetConsoleOutputCP(65001);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (!TinyHTTP_Startup()) {
        printf("kyper: WSAStartup failed (%d)\n", WSAGetLastError());
        return 1;
    }
    InitializeCriticalSection(&g_consoleCS);
    g_wakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!TinyHTTP_GetFirstLanSockaddr(&g_lanAddr)) {
        ZeroMemory(&g_lanAddr, sizeof(g_lanAddr));
        g_lanAddr.sin_family = AF_INET;
        g_lanAddr.sin_addr.s_addr = INADDR_ANY; /* degraded: A answers point nowhere */
        printf("kyper: no LAN IPv4 found\n");
    }

    g_server = TinyHTTP_Listen(g_cfg.port);
    if (g_server == INVALID_SOCKET) {
        printf("kyper: cannot bind port %d (Winsock error %d)\n", g_cfg.port, WSAGetLastError());
        return 1;
    }

    g_acceptEvent = TinyHTTP_WireAcceptEvent(g_server);
    if (!g_acceptEvent) {
        printf("kyper: WSAEventSelect failed (%d)\n", WSAGetLastError());
        return 1;
    }

    PrintBanner();
    HANDLE mdnsThread = CreateThread(NULL, 0, MdnsThread, NULL, 0, NULL);

    /* Notification-area icon (hidden host window, message pump below). */
    bool trayOk = false;
    if (g_cfg.tray) {
        wchar_t tip[96];
        wsprintfW(tip, L"kyper :%d — agent hub", g_cfg.port);
        trayOk = TinyTray_Create(L"TinyKyperTrayHost", tip, L"kyper",
                                 L"Agent hub listening — right-click the icon to exit",
                                 RGB(0, 120, 215), TrayExitRequest);
        Log(trayOk ? "tray icon active (right-click -> Exit)"
                   : "tray icon unavailable (running without icon)");
    }

    HANDLE handles[3];
    handles[0] = g_toggleEvent;
    handles[1] = g_wakeEvent;
    handles[2] = g_acceptEvent;

    while (g_running) {
        /* MsgWait (not plain WaitFor): the tray host window must get a
         * message pump, or its right-click menu never appears. */
        DWORD r = MsgWaitForMultipleObjects(3, handles, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 + 3) {
            TinyTray_PumpMessages();   /* tray menu / hidden-window input */
            continue;
        }
        if (!g_running) break;

        if (r == WAIT_OBJECT_0 + 0) {
            Log("toggle signal received — shutting down");
            break;
        }
        if (r == WAIT_OBJECT_0 + 2) {
            WSANETWORKEVENTS ne;
            /* Atomic retrieve + reset of FD_ACCEPT state (never misses backlog). */
            WSAEnumNetworkEvents(g_server, g_acceptEvent, &ne);
            if (ne.lNetworkEvents & FD_ACCEPT) {
                for (;;) {
                    SOCKET client = TinyHTTP_Accept(g_server);
                    if (client == INVALID_SOCKET) break;
                    HandleClient(client);
                    closesocket(client);
                }
            }
            /* Catch connections completing during the drain window. */
            for (;;) {
                SOCKET late = TinyHTTP_Accept(g_server);
                if (late == INVALID_SOCKET) break;
                HandleClient(late);
                closesocket(late);
            }
        }
    }

    TinyTray_Destroy();                      /* remove icon before network teardown */
    if (g_acceptEvent) WSACloseEvent(g_acceptEvent);
    if (g_server != INVALID_SOCKET) closesocket(g_server);
    if (g_wakeEvent) CloseHandle(g_wakeEvent);
    if (mdnsThread) {
        g_running = false;                   /* already false on all exit paths */
        WaitForSingleObject(mdnsThread, 2000);
        CloseHandle(mdnsThread);
    }
    DeleteCriticalSection(&g_consoleCS);
    TinyHTTP_Cleanup();
    Log("exit");
    return 0;
}
