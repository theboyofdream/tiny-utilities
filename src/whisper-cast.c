/*
 * WhisperCast — phone-to-PC text typer.
 * Source: whisper-cast.c
 *
 * Spec: docs/spec-whisper-cast.md. Agent guide: AGENTS.md / CHECKLIST.md.
 *
 * Behavior:
 *   First launch: single-instance mutex -> HTTP endpoint on 0.0.0.0:<port>
 *     (default 7447) + an mDNS responder thread announcing the host as
 *     <name>.local (default whisper-cast.local) and the DNS-SD service
 *     <name>._http._tcp.local, so LAN clients resolve it without a raw IP.
 *   API (no HTML; plain text replies):
 *     POST /type <UTF-8 body>  -> inject body into the focused window via
 *     SendInput (KEYEVENTF_UNICODE key DOWN/UP pairs, newline -> VK_RETURN,
 *     tab -> VK_TAB). A line-submit Enter is always appended.
 *   Second launch: signals the TOGGLE event -> running instance exits cleanly.
 *   Ctrl+C exits cleanly as well.
 *
 * Hard rules kept: no services, no registry, no persistence, no config files,
 * no global hooks (SendInput injects into the caller's own desktop session).
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include "common/tiny_http.h"
#include "common/tiny_cli.h"
#include "common/tiny_ipc.h"
#include "common/mdns.h"
#include "common/tiny_tray.h"

#define APPMUTEX_NAME L"Global\\TinyWhisperCastMutex"
#define APPEVENT_NAME L"Global\\TinyWhisperCastEvent"

#define DEFAULT_PORT        7447
#define PORT_MIN            1
#define PORT_MAX            65535

#define DEFAULT_NAME        L"whisper-cast"

#define MAX_NAME_CHARS      31          /* single DNS label cap */

#define MSG_WMAX_CHARS      2000        /* textarea hard cap (matches page JS) */

#define MDNS_PORT           5353
#define MDNS_BUF_CAP        8192

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int port;
    wchar_t name[MAX_NAME_CHARS + 1];
    bool tray;
} Config;

static Config g_cfg;
static HANDLE g_toggleEvent = NULL;
static HANDLE g_wakeEvent = NULL;
static SOCKET g_server = INVALID_SOCKET;
static WSAEVENT g_acceptEvent = NULL;
static SOCKET g_mdnsSock = INVALID_SOCKET;
static bool g_running = true;
static struct sockaddr_in g_lanAddr;    /* this PC's LAN IPv4 (for A answers) */

/* ------------------------------------------------------------------ */
/* Util                                                                 */
/* ------------------------------------------------------------------ */

static bool NameIsValid(const wchar_t *s) {
    if (!s[0]) return false;
    for (const wchar_t *p = s; *p; p++) {
        if (*p == L'.') return false;                        /* one DNS label */
        if ((unsigned)*p >= 0x80) return false;              /* mDNS is ASCII */
        if (!((unsigned)*p >= L'a' && (unsigned)*p <= L'z') &&
            !((unsigned)*p >= L'A' && (unsigned)*p <= L'Z') &&
            !((unsigned)*p >= L'0' && (unsigned)*p <= L'9') &&
            *p != L'-' && *p != L'_') {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Text injection (SendInput, unicode, newline -> VK_RETURN)            */
/* ------------------------------------------------------------------ */

#define INJECT_CHUNK_CHARS 32

static bool InjectText(const wchar_t *text, size_t len) {
    INPUT inputs[INJECT_CHUNK_CHARS * 2];
    size_t at = 0;

    for (size_t i = 0; i <= len; i++) {        /* one artificial pass for trailing Enter */
        wchar_t ch = (i < len) ? text[i] : L'\n';
        if (i == len && len == 0) break;       /* empty payload: no trailing Enter */
        if (ch == L'\r') continue;

        WORD vk = 0;
        WORD scan = 0;
        DWORD flags = KEYEVENTF_UNICODE;
        if (ch == L'\n') {
            vk = VK_RETURN;
            scan = 0x1C;
            flags = 0;
        } else if (ch == L'\t') {
            vk = VK_TAB;
            scan = 0x0F;
            flags = 0;
        }

        ZeroMemory(&inputs[at], sizeof(INPUT));
        inputs[at].type = INPUT_KEYBOARD;
        inputs[at].ki.wVk = vk;
        inputs[at].ki.wScan = vk ? scan : (WORD)ch;
        inputs[at].ki.dwFlags = flags;
        ZeroMemory(&inputs[at + 1], sizeof(INPUT));
        inputs[at + 1].type = INPUT_KEYBOARD;
        inputs[at + 1].ki.wVk = vk;
        inputs[at + 1].ki.wScan = vk ? scan : (WORD)ch;
        inputs[at + 1].ki.dwFlags = flags | KEYEVENTF_KEYUP;
        at += 2;

        if (at == (sizeof(inputs) / sizeof(inputs[0])) || i == len) {
            UINT pushed = SendInput((UINT)at, inputs, sizeof(INPUT));
            if (pushed != at) return false;
            at = 0;
            if (i != len) Sleep(2);            /* let the focus app keep up */
        }
        if (i == len) break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* mDNS responder                                                       */
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

    /* Our identity strings. */
    char host[64];
    char inst[96];
    char svc[] = "_http._tcp.local";
    snprintf(host, sizeof(host), "%ls.local", g_cfg.name);
    snprintf(inst, sizeof(inst), "%ls._http._tcp.local", g_cfg.name);

    if (!AsciiEquals(qname, host) && !AsciiEquals(qname, svc)) return 0;

    mdns_record_t answer;
    mdns_record_t additional[3];
    int addCount = 0;

    mdns_record_t aRec, ptrRec, srvRec;
    ZeroMemory(&aRec, sizeof(aRec));
    ZeroMemory(&ptrRec, sizeof(ptrRec));
    ZeroMemory(&srvRec, sizeof(srvRec));

    /* A record for <name>.local. */
    aRec.name.str = host;
    aRec.name.length = strlen(host);
    aRec.type = MDNS_RECORDTYPE_A;
    aRec.data.a.addr = g_lanAddr;

    /* PTR for service discovery: _http._tcp.local -> <name>._http._tcp.local */
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
        printf("whisper-cast: mDNS responder unavailable (bind 5353 busy?); "
               "use http://<ip>:%d instead\n", g_cfg.port);
        return 0;
    }

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
/* HTTP serving (POST /type only — no HTML page by design)              */
/* ------------------------------------------------------------------ */

static void HandleClient(SOCKET client) {
    TinyHTTPRequest req;
    if (TinyHTTP_LoadRequest(client, &req) != 0) return;

    if (strcmp(req.method, "POST") == 0 && strcmp(req.target, "/type") == 0) {
        if (req.bodyLen == 0) {
            TinyHTTP_ReplyUtf8(client, 200, "OK", "text/plain", "OK (empty)");
            return;
        }

        wchar_t text[MSG_WMAX_CHARS + 1];
        int wn = TinyHTTP_Utf8ToWide(req.body, req.bodyLen, text, MSG_WMAX_CHARS + 1);
        if (wn <= 0 || wn > MSG_WMAX_CHARS) {
            TinyHTTP_ReplyUtf8(client, 400, "Bad Request", "text/plain", "Encode");
            return;
        }
        if (InjectText(text, (size_t)wn)) {
            TinyHTTP_ReplyUtf8(client, 200, "OK", "text/plain", "OK");
        } else {
            TinyHTTP_ReplyUtf8(client, 500, "SendInput Rejected", "text/plain", "Rejected");
        }
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
    cfg->tray = true;
    wchar_t *nameBuf = cfg->name;
    wcscpy_s(nameBuf, MAX_NAME_CHARS + 1, DEFAULT_NAME);

    const CliOption opts[] = {
        { L"--port", CLI_OPT_INT,    &cfg->port, PORT_MIN, PORT_MAX, L"<1-65535>", L"HTTP port (default: 7447)" },
        { L"-p",     CLI_OPT_INT,    &cfg->port, PORT_MIN, PORT_MAX, NULL,         NULL },
        { L"--name", CLI_OPT_STRING, &nameBuf,   0, 0, L"<label>",  L"Host label; serves <label>.local (default: whisper-cast)" },
        { L"-n",     CLI_OPT_STRING, &nameBuf,   0, 0, NULL,        NULL },
        { L"--tray", CLI_OPT_BOOL,   &cfg->tray, 0, 0, L"<true|false>", L"Notification-area icon with Exit (default: true)" },
    };

    if (TinyCLI_CheckHelpCommandLine(
            L"whisper-cast",
            L"Phone-to-PC text typer via LAN HTTP + SendInput",
            L"whisper-cast [--port <1-65535>] [--name <label>] [--tray <true|false>]",
            opts, sizeof(opts) / sizeof(opts[0]))) {
        *status = CONFIG_HELP_SHOWN;
        return false;
    }

    TinyCLI_ParseCommandLine(opts, sizeof(opts) / sizeof(opts[0]));

    if (!NameIsValid(nameBuf)) {
        printf("whisper-cast: --name must be a single ASCII label "
               "(letters, digits, '-', '_') without dots\n");
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
    printf("whisper-cast: exit requested from tray\n");
    g_running = false;
    if (g_wakeEvent) SetEvent(g_wakeEvent);   /* wake the main wait instantly */
}

static void PrintBanner(void) {
    wchar_t ip[16];
    bool haveIp = TinyHTTP_GetFirstLanIPv4(ip, sizeof(ip) / sizeof(ip[0]));
    if (haveIp) {
        wprintf(L"whisper-cast: serving http://%ls:%d (POST /type with UTF-8 text)\n",
                ip, g_cfg.port);
        wprintf(L"whisper-cast: resolves as http://%ls.local:%d via mDNS\n",
                g_cfg.name, g_cfg.port);
    } else {
        wprintf(L"whisper-cast: no LAN IPv4 found, mDNS degraded; start anyway on port %d\n",
                g_cfg.port);
    }
    wprintf(L"whisper-cast: focus the target window on this PC, then send text from your client\n");
    wprintf(L"whisper-cast: run 'whisper-cast' again to toggle off; Ctrl+C or tray Exit also exits\n\n");
}

int wmain(void) {
    /* Redirected stdout must log immediately (learned during kyper bring-up). */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    ConfigStatus cfgStatus;
    if (!BuildConfig(&g_cfg, &cfgStatus)) {
        return (cfgStatus == CONFIG_HELP_SHOWN) ? 0 : 1;
    }

    HANDLE toggle = NULL;
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &toggle)) {
        printf("whisper-cast: toggle signal sent — running instance exits\n");
        return 0;
    }
    g_toggleEvent = toggle;

    SetConsoleOutputCP(65001);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (!TinyHTTP_Startup()) {
        printf("whisper-cast: WSAStartup failed (%d)\n", WSAGetLastError());
        return 1;
    }
    g_wakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!TinyHTTP_GetFirstLanSockaddr(&g_lanAddr)) {
        ZeroMemory(&g_lanAddr, sizeof(g_lanAddr));
        g_lanAddr.sin_family = AF_INET;
        g_lanAddr.sin_addr.s_addr = INADDR_ANY; /* degraded: A answers point nowhere */
        printf("whisper-cast: no LAN IPv4 found\n");
    }

    g_server = TinyHTTP_Listen(g_cfg.port);
    if (g_server == INVALID_SOCKET) {
        printf("whisper-cast: cannot bind port %d (Winsock error %d)\n",
               g_cfg.port, WSAGetLastError());
        return 1;
    }

    g_acceptEvent = TinyHTTP_WireAcceptEvent(g_server);
    if (!g_acceptEvent) {
        printf("whisper-cast: WSAEventSelect failed (%d)\n", WSAGetLastError());
        return 1;
    }

    PrintBanner();
    HANDLE mdnsThread = CreateThread(NULL, 0, MdnsThread, NULL, 0, NULL);

    /* Notification-area icon (hidden host window, message pump below). */
    bool trayOk = false;
    if (g_cfg.tray) {
        wchar_t tip[96];
        wsprintfW(tip, L"whisper-cast :%d — POST /type", g_cfg.port);
        trayOk = TinyTray_Create(L"TinyWhisperCastTrayHost", tip, L"whisper-cast",
                                 L"Text typer listening — right-click the icon to exit",
                                 RGB(35, 134, 86), TrayExitRequest);
        printf(trayOk ? "whisper-cast: tray icon active (right-click -> Exit)\n"
                      : "whisper-cast: tray icon unavailable (running without icon)\n");
    }

    HANDLE handles[3];
    handles[0] = g_toggleEvent;
    handles[1] = g_wakeEvent;
    handles[2] = g_acceptEvent;

    while (g_running) {
        /* MsgWait (not plain WaitFor): the tray host window needs a message
         * pump, otherwise its right-click menu never appears. */
        DWORD r = MsgWaitForMultipleObjects(3, handles, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 + 3) {
            TinyTray_PumpMessages();   /* tray menu / hidden-window input */
            continue;
        }
        if (!g_running) break;

        if (r == WAIT_OBJECT_0 + 0) {
            g_running = false;                  /* let the mDNS thread stop observing */
            printf("whisper-cast: toggle signal received — shutting down\n");
            break;
        }
        if (r == WAIT_OBJECT_0 + 2) {
            WSANETWORKEVENTS ne;
            WSAEnumNetworkEvents(g_server, g_acceptEvent, &ne);
            if (ne.lNetworkEvents & FD_ACCEPT) {
                for (;;) {
                    SOCKET client = TinyHTTP_Accept(g_server);
                    if (client == INVALID_SOCKET) break;
                    HandleClient(client);
                    closesocket(client);
                }
            }
            for (;;) {
                SOCKET late = TinyHTTP_Accept(g_server);
                if (late == INVALID_SOCKET) break;
                HandleClient(late);
                closesocket(late);
            }
        }
    }

    if (mdnsThread) {
        /* mDNS thread exits within ~20ms of g_running=false and closes its own socket. */
        WaitForSingleObject(mdnsThread, 2000);
        CloseHandle(mdnsThread);
    }
    TinyTray_Destroy();                      /* remove icon before network teardown */
    if (g_acceptEvent) WSACloseEvent(g_acceptEvent);
    if (g_server != INVALID_SOCKET) closesocket(g_server);
    if (g_wakeEvent) CloseHandle(g_wakeEvent);
    TinyHTTP_Cleanup();
    printf("whisper-cast: exit\n");
    return 0;
}
