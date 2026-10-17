/*
 * mwb.c - mouse/keyboard sharing, clipboard sync (text + files), file transfer,
 *         drag-and-drop across the screen edge, 4-char pairing code. Win32, pure C.
 *
 * Build (clang, MSVC target; #pragma comment pulls the libs):
 *   clang -Wall -Wextra mwb.c -o mwb.exe -Wl,/subsystem:windows
 * Build (clang/gcc, MinGW target):
 *   clang mwb.c -o mwb.exe -mwindows -lws2_32 -luser32 -lshell32 -lcomdlg32 -lole32 -luuid -lbcrypt
 *
 * Run:
 *   mwb                          asks: server or client (dialog)
 *   mwb server                   PC with the physical mouse/keyboard; shows a 4-char pairing code
 *   mwb client                   other PC, auto-discovers the server, asks for the code
 *   mwb client 192.168.1.20      direct connect (no discovery)
 *   options: --otp AB12 (client, skips the code prompt)   --log file (override log path)
 *
 * Files created next to the exe (or %LOCALAPPDATA%\mwb if that folder is read-only):
 *   mwb.log          everything that happens (events only: never keystrokes, clipboard
 *                    contents or the pairing code); rotated to mwb.log.old at 5 MB
 *   received\        files received via "Send file(s)..."
 *
 * Server's RIGHT edge -> client's LEFT edge, and back. Scroll Lock = emergency return
 * to local control on the server.
 *
 * Ports: UDP 24800 discovery | TCP 24801 input+clipboard | TCP 24802 files
 *
 * Auth: server shows a random 4-char code (tray tooltip/balloon). Client proves it with
 *   HMAC-SHA256 challenge/response (mutual); a session key derived from it authenticates
 *   the file channel. 3 wrong tries = new code. NOT encrypted: traffic is readable on the
 *   LAN; a 4-char code only stops casual/accidental connections. Trusted networks only.
 *
 * Clipboard: text and files (files are fetched lazily when you paste or drop; no size limit).
 * Drag-and-drop: hold the left button on a file in Explorer/desktop, cross the edge, release
 *   on the other PC. Implemented as: cancel the local drag (Esc), Ctrl+C the selection,
 *   offer it to the peer, which starts a real OLE drag with the injected mouse.
 *
 * Single instance: Global\TinyMwbMutex / Global\TinyMwbEvent (common/tiny_ipc.h).
 *   Launching a second copy toggles the first one off -- it stops cleanly.
 * CLI: mwb [server | client [server-ip]] [--otp CODE] [--log FILE] [--config FILE]
 *   (--help = help box)
 * Optional read-only config: mwb.txt beside the exe (or --config PATH) with
 *   mode/side/clipboard keys; CLI always wins; the file is only ever READ.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <mstcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#include "common/tiny_cli.h"              /* declarative CLI tokenizing/parsing/help */
#include "common/tiny_ipc.h"              /* single-instance mutex+event toggle */
#include "common/tiny_dpi.h"              /* per-monitor v2 awareness */
#include "common/tiny_config.h"           /* read-only key=value config loader */

#define APPMUTEX_NAME L"Global\\TinyMwbMutex"
#define APPEVENT_NAME L"Global\\TinyMwbEvent"

#define UDP_PORT   24800
#define TCP_PORT   24801
#define FILE_PORT  24802
#define FILE_MAGIC      0x3146574Du     /* "MWF1" push: {magic, namelen, size} + name + bytes */
#define FILE_MAGIC_PULL 0x3150574Du     /* "MWP1" pull: {magic, gen, index} -> size64 + bytes  */
#define CHUNK       (256 * 1024)
#define MAX_PAYLOAD (8 * 1024 * 1024)
#define MAX_TEXT    (4 * 1024 * 1024)
#define MAX_OFFER   20000
#define LOG_MAX     (5 * 1024 * 1024)

#define WM_TRAY      (WM_APP + 1)
#define WM_STATUS    (WM_APP + 2)
#define WM_BALLOON   (WM_APP + 3)
#define WM_DRAGCROSS (WM_APP + 4)
#define WM_APP_TEXT  (WM_APP + 10)
#define WM_APP_OFFER (WM_APP + 11)
enum { ID_SEND = 1, ID_CLIP, ID_CODE, ID_OPEN,
       ID_SIDE_R, ID_SIDE_L, ID_SIDE_U, ID_SIDE_D, ID_EXIT };

enum { M_MOVE = 1, M_BUTTON, M_WHEEL, M_KEY, M_ENTER, M_LEAVE, M_CLIP_TEXT, M_CLIP_FILES, M_HELLO };
#define MWB_OS_WIN32 1
#define MWB_OS_MAC   2
/*
 * M_MOVE   a=dx        b=dy
 * M_BUTTON a=button    b=down        (0=L 1=R 2=M 3=X1 4=X2)
 * M_WHEEL  a=delta     b=horizontal
 * M_KEY    a=vk        b=scancode    c=flags (bit0=up, bit1=extended)
 * M_ENTER  a=pos 0..10000 along the entry edge  c bit0 = left button held (drag)
 *            c bits1-2 = client entry edge: 0=left 1=right 2=top 3=bottom  server -> client
 * M_LEAVE  a=pos 0..10000 along the tunnel edge  client -> server: give control back;
 *            server -> client: stop remote mode
 * M_CLIP_TEXT  c=len, then len bytes UTF-8
 * M_CLIP_FILES a=generation b=flags(bit0=drag) c=len, then payload:
 *              u32 count, then per entry {u64 size, u32 isdir, u32 namelen, utf8 relative name}
 */
typedef struct { uint32_t type; int32_t a, b, c; } Msg;
typedef struct { uint32_t magic; uint32_t namelen; uint64_t size; } FileHdr;

static SOCKET        g_sock = INVALID_SOCKET;
static volatile LONG g_remote = 0;      /* server: 1 = input is being forwarded */
static volatile LONG g_skip_move = 0;   /* server: swallow the first move after toggle-on */
static volatile ULONG g_peer_ip = 0;    /* IPv4 (network order) of connected peer, 0 = none */
static int  g_is_server = 0;
static int  g_peer_os = 0;              /* MWB_OS_* of the connected peer, 0 = unknown */
static int  g_sw, g_sh, g_cx, g_cy;     /* primary screen size, center */

/* tunnel side: which edge of OUR screen leads to the peer.
 * Session-only setting (right default); set from the tray menu. */
#define MWB_SIDE_RIGHT 0
#define MWB_SIDE_LEFT  1
#define MWB_SIDE_UP    2       /* peer sits above: tunnel at our TOP edge    */
#define MWB_SIDE_DOWN  3       /* peer sits below: tunnel at our BOTTOM edge */
static int g_side = MWB_SIDE_RIGHT;

static HWND  g_hwnd, g_clipwnd;
static UINT  g_taskbar_created;
static CRITICAL_SECTION g_lock, g_send_lock, g_offer_lock;
static wchar_t g_tip[128], g_bt[64], g_bm[256], g_peer_str[48];
static wchar_t g_base[MAX_PATH], g_recv_dir[MAX_PATH];
static FILE *g_logf;
static int   g_console;

static char    g_otp[8];                /* server: current code; client: entered code */
static wchar_t g_otp_w[8];
static int     g_fail;
static uint8_t g_key[32];               /* session key (HMAC of the code + nonces) */
static volatile LONG g_have_key = 0;
static HANDLE  g_pair_event;
static HANDLE  g_toggle_event;          /* TinyIPC: second launch sets it = toggle-off */

static UINT g_cfDesc, g_cfContents, g_cfOrigin, g_cfExclude;
static volatile LONG g_clip_sync = 1;
static volatile LONG g_sync_mouse = 1;      /* config: mouse = on|off (cursor handover + pointer events) */
static volatile LONG g_sync_keyboard = 1;   /* config: keyboard = on|off */
static volatile LONG g_sync_files = 1;      /* config: files = on|off (offers, pull, cross-PC drag) */
static volatile LONG g_drag_expect = 0, g_drag_active = 0, g_drag_cancel = 0, g_drag_offer = 0;
static uint32_t g_gen;

/* ---------- log / tray text ---------- */

static void logmsg(const char *fmt, ...) {
    char line[700];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ",
                     t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    if (g_console) { fputs(line, stdout); fputc('\n', stdout); fflush(stdout); }
    if (g_logf)    { fputs(line, g_logf); fputc('\n', g_logf); fflush(g_logf); }
    LeaveCriticalSection(&g_lock);
}

static void set_status(const wchar_t *fmt, ...) {
    wchar_t buf[128];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 127, fmt, ap);
    va_end(ap);
    buf[127] = 0;
    EnterCriticalSection(&g_lock);
    wcscpy(g_tip, buf);
    LeaveCriticalSection(&g_lock);
    if (g_hwnd) PostMessageW(g_hwnd, WM_STATUS, 0, 0);
}

static void notify(const wchar_t *title, const wchar_t *fmt, ...) {
    wchar_t buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 255, fmt, ap);
    va_end(ap);
    buf[255] = 0;
    EnterCriticalSection(&g_lock);
    wcsncpy(g_bt, title, 63); g_bt[63] = 0;
    wcscpy(g_bm, buf);
    LeaveCriticalSection(&g_lock);
    if (g_hwnd) PostMessageW(g_hwnd, WM_BALLOON, 0, 0);
}

static void status_idle(void) {
    if (g_peer_ip) {
        if (g_is_server && g_remote) set_status(L"mwb: controlling remote (%ls)", g_peer_str);
        else                         set_status(L"mwb: connected to %ls", g_peer_str);
    } else if (g_is_server) {
        set_status(L"mwb server: code %ls - waiting for client", g_otp_w);
    } else {
        set_status(L"mwb client: searching for server");
    }
}

static void show_code(void) { notify(L"mwb pairing code", L"Enter %ls on the other PC", g_otp_w); }

static void set_peer(struct in_addr a) {
    InetNtopW(AF_INET, &a, g_peer_str, 48);
    g_peer_ip = a.s_addr;
    logmsg("connected: %ls", g_peer_str);
    status_idle();
}

static void clear_peer(void) {
    g_peer_ip = 0;
    g_peer_os = 0;
    g_have_key = 0;
    logmsg("disconnected");
    status_idle();
}

static void tray_fill(NOTIFYICONDATAW *n, UINT flags) {
    memset(n, 0, sizeof *n);
    n->cbSize = sizeof *n;
    n->hWnd = g_hwnd;
    n->uID = 1;
    n->uFlags = flags;
}

static void tray_add(void) {
    NOTIFYICONDATAW n;
    tray_fill(&n, NIF_ICON | NIF_MESSAGE | NIF_TIP);
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    EnterCriticalSection(&g_lock); wcsncpy(n.szTip, g_tip, 127); LeaveCriticalSection(&g_lock);
    Shell_NotifyIconW(NIM_ADD, &n);
}

static void tray_tip(void) {
    NOTIFYICONDATAW n;
    tray_fill(&n, NIF_TIP);
    EnterCriticalSection(&g_lock); wcsncpy(n.szTip, g_tip, 127); LeaveCriticalSection(&g_lock);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void tray_balloon(void) {
    NOTIFYICONDATAW n;
    tray_fill(&n, NIF_INFO);
    n.dwInfoFlags = NIIF_INFO;
    EnterCriticalSection(&g_lock);
    wcsncpy(n.szInfoTitle, g_bt, 63);
    wcsncpy(n.szInfo, g_bm, 255);
    LeaveCriticalSection(&g_lock);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void tray_del(void) {
    NOTIFYICONDATAW n;
    tray_fill(&n, 0);
    Shell_NotifyIconW(NIM_DELETE, &n);
}

/* ---------- socket helpers ---------- */

static int recv_all(SOCKET s, void *buf, int n) {
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

static int send_all(SOCKET s, const void *buf, int n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        int r = send(s, p, n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

/* one message (+ optional payload) is sent atomically w.r.t. other threads */
static void send_locked(const Msg *m, const void *data, int len) {
    EnterCriticalSection(&g_send_lock);
    SOCKET s = g_sock;
    int ok = s != INVALID_SOCKET && send_all(s, m, (int)sizeof *m) && (len == 0 || send_all(s, data, len));
    LeaveCriticalSection(&g_send_lock);
    if (s != INVALID_SOCKET && !ok) g_remote = 0;
}

static void send_msg(uint32_t t, int a, int b, int c) {
    Msg m; m.type = t; m.a = a; m.b = b; m.c = c;
    send_locked(&m, NULL, 0);
}

/* non-blocking variant used by the mouse hook for M_MOVE only: while the clipboard
 * thread is pushing a multi-MB payload the regular lock is held for a while; a low-level
 * hook must return immediately, so contended moves are dropped (they are plain deltas,
 * losing one is invisible). Button/wheel/M_ENTER keep the blocking path. */
static void send_move_try(int dx, int dy) {
    if (!TryEnterCriticalSection(&g_send_lock)) return;
    SOCKET s = g_sock;
    int ok = 0;
    if (s != INVALID_SOCKET) {
        Msg m; m.type = M_MOVE; m.a = dx; m.b = dy; m.c = 0;
        ok = send_all(s, &m, (int)sizeof m);
    }
    LeaveCriticalSection(&g_send_lock);
    if (s != INVALID_SOCKET && !ok) g_remote = 0;
}

static void send_var(uint32_t t, int a, int b, const void *data, int len) {
    Msg m; m.type = t; m.a = a; m.b = b; m.c = len;
    send_locked(&m, data, len);
}

static void tune_sock(SOCKET s, DWORD rcv_ms, DWORD snd_ms) {
    BOOL on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof on);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_ms, sizeof rcv_ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&snd_ms, sizeof snd_ms);
}

/* Windows sends keepalives at ~2 h intervals by default; without tuning, a peer that
 * vanishes (sleep, cable pull) leaves read_msg() blocked for hours. 5 s idle / 1 s
 * interval turns that into a few seconds. */
static void keep_alive(SOCKET s) {
    DWORD ka[3] = { 1, 5000, 1000 }, ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, ka, sizeof ka, NULL, 0, &ret, NULL, NULL);
}

static void init_screen(void) {
    SetProcessDPIAware();
    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    g_cx = g_sw / 2;
    g_cy = g_sh / 2;
}

static void spawn(LPTHREAD_START_ROUTINE fn, LPVOID arg) {
    HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (h) CloseHandle(h);
}

static const wchar_t *last_comp(const wchar_t *p) {
    const wchar_t *b = p;
    for (; *p; p++) if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

/* ---------- folders, log file ---------- */

static void init_base_dir(void) {
    GetModuleFileNameW(NULL, g_base, MAX_PATH);
    wchar_t *p = wcsrchr(g_base, L'\\');
    if (p) *p = 0;
    wchar_t t[MAX_PATH];
    _snwprintf(t, MAX_PATH - 1, L"%ls\\.mwb_w", g_base);
    t[MAX_PATH - 1] = 0;
    HANDLE h = CreateFileW(t, GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
    wchar_t la[MAX_PATH];                       /* exe folder not writable */
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n && n < MAX_PATH - 8) {
        _snwprintf(g_base, MAX_PATH - 1, L"%ls\\mwb", la);
        CreateDirectoryW(g_base, NULL);
    }
}

static void open_default_log(void) {
    wchar_t p[MAX_PATH], old[MAX_PATH];
    _snwprintf(p, MAX_PATH - 1, L"%ls\\mwb.log", g_base);
    p[MAX_PATH - 1] = 0;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &d) && (d.nFileSizeHigh || d.nFileSizeLow > LOG_MAX)) {
        _snwprintf(old, MAX_PATH - 1, L"%ls.old", p);
        old[MAX_PATH - 1] = 0;
        MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_logf = _wfopen(p, L"a");
}

static void init_recv_dir(void) {
    _snwprintf(g_recv_dir, MAX_PATH - 1, L"%ls\\received", g_base);
    g_recv_dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(g_recv_dir, NULL);
}

/* ---------- crypto: random, HMAC-SHA256 (Windows CNG), pairing code ---------- */

static int rand_bytes(void *p, ULONG n) {
    return BCryptGenRandom(NULL, (PUCHAR)p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

static int hmac256(const uint8_t *key, ULONG klen, const uint8_t *data, ULONG dlen, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, (PUCHAR)key, klen, 0) >= 0 &&
        BCryptHashData(h, (PUCHAR)data, dlen, 0) >= 0 &&
        BCryptFinishHash(h, out, 32, 0) >= 0) ok = 1;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int ct_eq(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void new_otp(void) {
    static const char CH[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* 32 symbols, no 0/O/1/I */
    uint8_t r[4];
    if (!rand_bytes(r, 4)) {
        logmsg("random generator failed");
        MessageBoxW(NULL, L"Secure random generator failed.", L"mwb", MB_ICONERROR);
        ExitProcess(1);
    }
    for (int i = 0; i < 4; i++) { g_otp[i] = CH[r[i] & 31]; g_otp_w[i] = (wchar_t)g_otp[i]; }
    g_otp[4] = 0; g_otp_w[4] = 0;
}

static void mk_data(uint8_t d[33], char tag, const uint8_t *x, const uint8_t *y) {
    d[0] = (uint8_t)tag; memcpy(d + 1, x, 16); memcpy(d + 17, y, 16);
}

/* returns 1 ok, 0 wrong code, -1 network/other error */
static int auth_server(SOCKET c) {
    uint8_t ns[16], in[48], d[33], ex[32], out[33];
    char otp[8]; memcpy(otp, g_otp, 8);
    if (!rand_bytes(ns, 16) || !send_all(c, ns, 16) || !recv_all(c, in, 48)) return -1;
    mk_data(d, 'C', ns, in);                               /* in[0..15] = client nonce */
    if (!hmac256((uint8_t *)otp, 4, d, 33, ex)) return -1;
    if (!ct_eq(ex, in + 16, 32)) { memset(out, 0, sizeof out); send_all(c, out, 33); return 0; }
    out[0] = 1;
    mk_data(d, 'S', in, ns);
    if (!hmac256((uint8_t *)otp, 4, d, 33, out + 1)) return -1;
    mk_data(d, 'K', ns, in);
    if (!hmac256((uint8_t *)otp, 4, d, 33, g_key)) return -1;
    if (!send_all(c, out, 33)) return -1;
    g_have_key = 1;
    return 1;
}

static int auth_client(SOCKET s, const char *otp) {
    uint8_t ns[16], nc[16], out[48], d[33], resp[33], ex[32];
    if (!recv_all(s, ns, 16) || !rand_bytes(nc, 16)) return -1;
    memcpy(out, nc, 16);
    mk_data(d, 'C', ns, nc);
    if (!hmac256((const uint8_t *)otp, 4, d, 33, out + 16)) return -1;
    if (!send_all(s, out, 48) || !recv_all(s, resp, 33)) return -1;
    if (resp[0] != 1) return 0;
    mk_data(d, 'S', nc, ns);
    if (!hmac256((const uint8_t *)otp, 4, d, 33, ex)) return -1;
    if (!ct_eq(ex, resp + 1, 32)) { logmsg("server could not prove the pairing code"); return 0; }
    mk_data(d, 'K', ns, nc);
    if (!hmac256((const uint8_t *)otp, 4, d, 33, g_key)) return -1;
    g_have_key = 1;
    return 1;
}

/* file channel: acceptor sends a nonce, initiator answers HMAC(session key, 'F'||nonce) */
static int file_auth_acceptor(SOCKET c) {
    uint8_t nn[16], mac[32], ex[32], d[17];
    uint8_t ok = 0;
    if (!g_have_key || !rand_bytes(nn, 16) || !send_all(c, nn, 16) || !recv_all(c, mac, 32)) return 0;
    d[0] = 'F'; memcpy(d + 1, nn, 16);
    if (hmac256(g_key, 32, d, 17, ex) && ct_eq(ex, mac, 32)) ok = 1;
    send_all(c, &ok, 1);
    return ok;
}

static int file_auth_initiator(SOCKET s) {
    uint8_t nn[16], mac[32], d[17], ok = 0;
    if (!g_have_key || !recv_all(s, nn, 16)) return 0;
    d[0] = 'F'; memcpy(d + 1, nn, 16);
    if (!hmac256(g_key, 32, d, 17, mac) || !send_all(s, mac, 32) || !recv_all(s, &ok, 1)) return 0;
    return ok == 1;
}

/* ---------- pairing code prompt (client) ---------- */

static volatile LONG g_prompting;
static HWND g_otp_edit;
static int  g_otp_done, g_otp_ok;
static char g_otp_buf[8];

static LRESULT CALLBACK OtpProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) {
            wchar_t t[16];
            GetWindowTextW(g_otp_edit, t, 16);
            int good = (wcslen(t) == 4);
            for (int i = 0; good && i < 4; i++) if (t[i] < 33 || t[i] > 126) good = 0;
            if (good) {
                for (int i = 0; i < 4; i++) g_otp_buf[i] = (char)towupper(t[i]);
                g_otp_buf[4] = 0; g_otp_ok = 1; g_otp_done = 1;
                DestroyWindow(h);
            } else MessageBeep(MB_ICONWARNING);
        } else if (LOWORD(w) == IDCANCEL) { g_otp_done = 1; DestroyWindow(h); }
        return 0;
    }
    if (m == WM_CLOSE) { g_otp_done = 1; DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static int prompt_otp(char *out) {
    if (InterlockedExchange(&g_prompting, 1)) return 0;
    static int reg;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = OtpProc; wc.hInstance = hi; wc.lpszClassName = L"mwb_otp_wnd";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        reg = 1;
    }
    g_otp_done = 0; g_otp_ok = 0;
    int W = 340, H = 150;
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"mwb_otp_wnd", L"mwb - pairing",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, (g_sw - W) / 2, (g_sh - H) / 2, W, H,
                               NULL, NULL, hi, NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND lbl = CreateWindowExW(0, L"STATIC", L"Enter the 4-character code shown on the server PC:",
                               WS_CHILD | WS_VISIBLE, 16, 14, 300, 20, wnd, NULL, hi, NULL);
    g_otp_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_UPPERCASE | ES_CENTER,
                                 110, 44, 100, 26, wnd, NULL, hi, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              120, 82, 80, 26, wnd, (HMENU)(INT_PTR)IDOK, hi, NULL);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_otp_edit, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_otp_edit, EM_SETLIMITTEXT, 4, 0);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(g_otp_edit);

    MSG msg;
    while (!g_otp_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            PostMessageW(wnd, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int r = g_otp_ok;
    if (r) memcpy(out, g_otp_buf, 5);
    InterlockedExchange(&g_prompting, 0);
    return r;
}

/* ---------- name sanitising ---------- */

static int is_reserved(const wchar_t *s) {
    wchar_t b[8]; size_t i = 0;
    while (s[i] && s[i] != L'.' && i < 7) { b[i] = (wchar_t)towupper(s[i]); i++; }
    b[i] = 0;
    if (s[i] && s[i] != L'.') return 0;
    if (!wcscmp(b, L"CON") || !wcscmp(b, L"PRN") || !wcscmp(b, L"AUX") || !wcscmp(b, L"NUL")) return 1;
    if ((!wcsncmp(b, L"COM", 3) || !wcsncmp(b, L"LPT", 3)) && b[3] >= L'1' && b[3] <= L'9' && !b[4]) return 1;
    return 0;
}

/* make ONE path component safe */
static void fix_component(wchar_t *s) {
    wchar_t *p;
    for (p = s; *p; p++)
        if (*p < 32 || wcschr(L"<>:\"|?*\\/", *p)) *p = L'_';
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L'.' || s[n - 1] == L' ')) s[--n] = 0;
    if (!n) { wcscpy(s, L"file"); n = 4; }
    if (is_reserved(s) && n < MAX_PATH - 2) { memmove(s + 1, s, (n + 1) * sizeof(wchar_t)); s[0] = L'_'; }
}

/* a plain file name: strip any path, then fix */
static void sanitize_name(wchar_t *s) {
    const wchar_t *base = last_comp(s);
    if (base != s) memmove(s, base, (wcslen(base) + 1) * sizeof(wchar_t));
    fix_component(s);
}

/* relative path (dir\sub\file): drop empty/./.. parts, fix each part */
static void sanitize_rel(const wchar_t *in, wchar_t *out) {
    size_t o = 0;
    wchar_t comp[MAX_PATH];
    const wchar_t *p = in;
    out[0] = 0;
    while (*p) {
        size_t k = 0;
        while (*p && *p != L'\\' && *p != L'/') { if (k < MAX_PATH - 2) comp[k++] = *p; p++; }
        comp[k] = 0;
        while (*p == L'\\' || *p == L'/') p++;
        if (!k || !wcscmp(comp, L".") || !wcscmp(comp, L"..")) continue;
        fix_component(comp);
        size_t cl = wcslen(comp);
        if (o + cl + 2 > MAX_PATH) break;
        if (o) out[o++] = L'\\';
        wcscpy(out + o, comp);
        o += cl;
    }
    if (!o) wcscpy(out, L"file");
}

/* client entry-edge code carried in M_ENTER c bits 1-2:
 * 0 = client LEFT edge, 1 = client RIGHT, 2 = client TOP, 3 = client BOTTOM */
static int side_to_code(int side) {
    switch (side) {
    case MWB_SIDE_RIGHT: return 0;
    case MWB_SIDE_LEFT:  return 1;
    case MWB_SIDE_UP:    return 3;      /* peer above -> client enters at its bottom */
    default:             return 2;      /* peer below -> client enters at its top    */
    }
}

/* ---------- SERVER: hooks ---------- */

static int fg_is_explorer(void) {
    wchar_t cls[64];
    HWND fg = GetForegroundWindow();
    if (!fg || !GetClassNameW(fg, cls, 64)) return 0;
    return !wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"Progman") ||
           !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"ExploreWClass");
}

static LRESULT CALLBACK MouseProc(int code, WPARAM w, LPARAM l) {
    if (code < 0) return CallNextHookEx(NULL, code, w, l);
    MSLLHOOKSTRUCT *h = (MSLLHOOKSTRUCT *)l;
    if (h->flags & LLMHF_INJECTED) return CallNextHookEx(NULL, code, w, l);

    if (!g_remote) {
        int edge = 0;
        if (w == WM_MOUSEMOVE && g_sock != INVALID_SOCKET) {
            switch (g_side) {
            case MWB_SIDE_LEFT:  edge = h->pt.x <= 0;        break;
            case MWB_SIDE_UP:    edge = h->pt.y <= 0;        break;
            case MWB_SIDE_DOWN:  edge = h->pt.y >= g_sh - 1; break;
            default:             edge = h->pt.x >= g_sw - 1; break;
            }
        }
        if (edge) {
            int pos = (g_side == MWB_SIDE_UP || g_side == MWB_SIDE_DOWN)
                    ? h->pt.x * 10000 / (g_sw - 1)    /* edge runs horizontally */
                    : h->pt.y * 10000 / (g_sh - 1);
            int drag = g_sync_files && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && fg_is_explorer();
            g_remote = 1;
            g_skip_move = 1;
            send_msg(M_ENTER, pos, 0, (drag ? 1 : 0) | (side_to_code(g_side) << 1));
            SetCursorPos(g_cx, g_cy);   /* park cursor at center; we swallow all moves */
            status_idle();
            if (drag) PostMessageW(g_hwnd, WM_DRAGCROSS, 0, 0);
            return 1;
        }
        return CallNextHookEx(NULL, code, w, l);
    }

    /* remote mode: forward and swallow */
    switch (w) {
    case WM_MOUSEMOVE: {
        SetCursorPos(g_cx, g_cy);       /* re-park every move: keeps deltas = true mouse deltas
                                         * and stops the pointer from running to the screen edge,
                                         * where a parked dx of half a screen would misbehave */
        if (InterlockedExchange(&g_skip_move, 0)) break;    /* first event may still carry stale
                                                             * edge coordinates from before the park */
        int dx = h->pt.x - g_cx, dy = h->pt.y - g_cy;
        if (dx || dy) send_move_try(dx, dy);
        break;
    }
    case WM_LBUTTONDOWN: send_msg(M_BUTTON, 0, 1, 0); break;
    case WM_LBUTTONUP:   send_msg(M_BUTTON, 0, 0, 0); break;
    case WM_RBUTTONDOWN: send_msg(M_BUTTON, 1, 1, 0); break;
    case WM_RBUTTONUP:   send_msg(M_BUTTON, 1, 0, 0); break;
    case WM_MBUTTONDOWN: send_msg(M_BUTTON, 2, 1, 0); break;
    case WM_MBUTTONUP:   send_msg(M_BUTTON, 2, 0, 0); break;
    case WM_XBUTTONDOWN: send_msg(M_BUTTON, HIWORD(h->mouseData) == XBUTTON1 ? 3 : 4, 1, 0); break;
    case WM_XBUTTONUP:   send_msg(M_BUTTON, HIWORD(h->mouseData) == XBUTTON1 ? 3 : 4, 0, 0); break;
    case WM_MOUSEWHEEL:  send_msg(M_WHEEL, (short)HIWORD(h->mouseData), 0, 0); break;
    case WM_MOUSEHWHEEL: send_msg(M_WHEEL, (short)HIWORD(h->mouseData), 1, 0); break;
    }
    return 1;
}

static LRESULT CALLBACK KeyProc(int code, WPARAM w, LPARAM l) {
    if (code < 0 || !g_remote) return CallNextHookEx(NULL, code, w, l);
    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)l;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(NULL, code, w, l);

    int up = (w == WM_KEYUP || w == WM_SYSKEYUP);

    if (k->vkCode == VK_SCROLL) {       /* emergency exit */
        if (!up) { g_remote = 0; send_msg(M_LEAVE, 0, 0, 0); status_idle(); }
        return 1;
    }
    if (!g_sync_keyboard) return 1;     /* keyboard off: keys stay parked with the remote cursor */
    send_msg(M_KEY, (int)k->vkCode, (int)k->scanCode,
             (up ? 1 : 0) | ((k->flags & LLKHF_EXTENDED) ? 2 : 0));
    return 1;
}

/* ---------- input injection (client side, and drag helper) ---------- */

static void inject_abs(int x, int y) {
    INPUT in; memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = x * 65535 / (g_sw - 1);
    in.mi.dy = y * 65535 / (g_sh - 1);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof in);
}

static void inject_button(int btn, int down) {
    INPUT in; memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    switch (btn) {
    case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
    case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
    case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 3: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
    case 4: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
    default: return;
    }
    SendInput(1, &in, sizeof in);
}

static void inject_wheel(int delta, int horiz) {
    INPUT in; memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = horiz ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)delta;
    SendInput(1, &in, sizeof in);
}

static void inject_key(int vk, int scan, int flags) {
    INPUT in; memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)scan;
    if (flags & 1) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (flags & 2) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}

/* make sure no injected button survives a leave/disconnect (up on an up button is a no-op) */
static void release_all_buttons(void) {
    for (int b = 0; b < 5; b++) inject_button(b, 0);
}

/* server side of a file drag: cancel the local drag (Esc), then Ctrl+C the selection so the
 * clipboard listener can offer it to the peer (flagged as a drag). */
static DWORD WINAPI DragSeq(LPVOID unused) {
    (void)unused;
    InterlockedExchange(&g_drag_offer, 1);
    inject_key(VK_ESCAPE, 0, 0);
    inject_key(VK_ESCAPE, 0, 1);
    Sleep(120);
    inject_key(VK_LCONTROL, 0, 0);
    inject_key('C', 0, 0);
    inject_key('C', 0, 1);
    inject_key(VK_LCONTROL, 0, 1);
    Sleep(2000);
    InterlockedExchange(&g_drag_offer, 0);
    return 0;
}

/* ---------- message reading, shared by server and client ---------- */

static int read_msg(SOCKET s, Msg *m, uint8_t **payload) {
    *payload = NULL;
    if (!recv_all(s, m, (int)sizeof *m)) return 0;
    if (m->type == M_CLIP_TEXT || m->type == M_CLIP_FILES) {
        int n = m->c;
        if (n < 0 || n > MAX_PAYLOAD) return 0;
        if (n > 0) {
            *payload = (uint8_t *)malloc((size_t)n + 1);
            if (!*payload || !recv_all(s, *payload, n)) { free(*payload); *payload = NULL; return 0; }
            (*payload)[n] = 0;
        }
    }
    return 1;
}

typedef struct { uint64_t size; int isdir; wchar_t name[MAX_PATH]; } REnt;
typedef struct { uint32_t gen; int drag, n; REnt *e; } RemoteOffer;

static void ro_free(RemoteOffer *ro) {
    if (!ro) return;
    free(ro->e);
    free(ro);
}

static RemoteOffer *offer_parse(uint32_t gen, int drag, const uint8_t *p, int len) {
    if (!p || len < 4) return NULL;
    uint32_t n; memcpy(&n, p, 4);
    if (n == 0 || n > MAX_OFFER) return NULL;
    RemoteOffer *ro = (RemoteOffer *)calloc(1, sizeof *ro);
    if (!ro) return NULL;
    ro->e = (REnt *)calloc(n, sizeof(REnt));
    if (!ro->e) { free(ro); return NULL; }
    int off = 4;
    for (uint32_t i = 0; i < n; i++) {
        if (len - off < 16) { ro_free(ro); return NULL; }
        uint64_t size; uint32_t isdir, nl;
        memcpy(&size, p + off, 8); memcpy(&isdir, p + off + 8, 4); memcpy(&nl, p + off + 12, 4);
        off += 16;
        if (nl == 0 || nl > 1024 || (int)nl > len - off) { ro_free(ro); return NULL; }
        wchar_t w[1100];
        int wl = MultiByteToWideChar(CP_UTF8, 0, (const char *)p + off, (int)nl, w, 1099);
        off += (int)nl;
        if (wl <= 0) { ro_free(ro); return NULL; }
        w[wl] = 0;
        sanitize_rel(w, ro->e[i].name);
        ro->e[i].size = size;
        ro->e[i].isdir = isdir ? 1 : 0;
    }
    ro->gen = gen; ro->drag = drag; ro->n = (int)n;
    return ro;
}

/* takes ownership of payload */
static void handle_clip(const Msg *m, uint8_t *payload) {
    if (!g_clipwnd) { free(payload); return; }
    if (m->type == M_CLIP_TEXT) {
        if (!g_clip_sync) { free(payload); return; }        /* clipboard channel off */
        if (!payload || m->c <= 0 || m->c > MAX_TEXT ||
            !PostMessageW(g_clipwnd, WM_APP_TEXT, (WPARAM)m->c, (LPARAM)payload)) free(payload);
    } else if (m->type == M_CLIP_FILES) {
        if (!g_sync_files) { free(payload); return; }       /* files channel off */
        RemoteOffer *ro = offer_parse((uint32_t)m->a, m->b & 1, payload, m->c);
        free(payload);
        if (ro && !PostMessageW(g_clipwnd, WM_APP_OFFER, 0, (LPARAM)ro)) ro_free(ro);
    } else free(payload);
}

/* ---------- SERVER: network thread ---------- */

static DWORD WINAPI ServerNet(LPVOID unused) {
    (void)unused;
    BOOL on = 1;

    SOCKET us = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(us, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof on);
    struct sockaddr_in bc; memset(&bc, 0, sizeof bc);
    bc.sin_family = AF_INET; bc.sin_port = htons(UDP_PORT);
    bc.sin_addr.s_addr = INADDR_BROADCAST;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(TCP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) {
        logmsg("bind/listen %d failed: %d", TCP_PORT, WSAGetLastError());
        set_status(L"mwb server: port %d unavailable", TCP_PORT);
        return 1;
    }
    logmsg("server: waiting for client (beaconing on UDP %d)", UDP_PORT);

    for (;;) {
        sendto(us, "MWB1", 4, 0, (struct sockaddr *)&bc, sizeof bc);
        fd_set fs; FD_ZERO(&fs); FD_SET(ls, &fs);
        struct timeval tv = { 2, 0 };
        if (select(0, &fs, NULL, NULL, &tv) <= 0) continue;

        struct sockaddr_in pa; int pl = (int)sizeof pa;
        SOCKET c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c == INVALID_SOCKET) continue;
        tune_sock(c, 10000, 5000);                 /* handshake must finish within 10 s */

        int r = auth_server(c);
        if (r != 1) {
            wchar_t ipw[48]; InetNtopW(AF_INET, &pa.sin_addr, ipw, 48);
            if (r == 0) {
                g_fail++;
                logmsg("pairing failed from %ls (%d/3)", ipw, g_fail);
                Sleep(1500);
                if (g_fail >= 3) {
                    new_otp(); g_fail = 0;
                    logmsg("pairing code regenerated after repeated failures");
                    show_code();
                    status_idle();
                }
            } else logmsg("handshake error from %ls", ipw);
            closesocket(c);
            continue;
        }
        g_fail = 0;
        tune_sock(c, 0, 30000);                    /* session: no recv timeout, long send budget */
        keep_alive(c);                             /* detect a vanished peer in seconds */
        g_sock = c;
        set_peer(pa.sin_addr);
        send_msg(M_HELLO, MWB_OS_WIN32, g_sw, g_sh);   /* announce our platform after auth */

        Msg m; uint8_t *pl2;
        while (read_msg(c, &m, &pl2)) {
            if (m.type == M_CLIP_TEXT || m.type == M_CLIP_FILES) { handle_clip(&m, pl2); continue; }
            free(pl2);
            if (m.type == M_HELLO) {
                g_peer_os = m.a;
                logmsg("peer OS: %s", m.a == MWB_OS_MAC ? "macOS" : (m.a == MWB_OS_WIN32 ? "win32" : "unknown"));
                continue;
            }
            if (m.type == M_LEAVE) {
                int f = m.a;
                if (f < 0) f = 0; else if (f > 10000) f = 10000;
                if (g_side == MWB_SIDE_LEFT)      SetCursorPos(1, f * (g_sh - 1) / 10000);
                else if (g_side == MWB_SIDE_UP)   SetCursorPos(f * (g_sw - 1) / 10000, 1);
                else if (g_side == MWB_SIDE_DOWN) SetCursorPos(f * (g_sw - 1) / 10000, g_sh - 2);
                else                              SetCursorPos(g_sw - 2, f * (g_sh - 1) / 10000);
                g_remote = 0;
                status_idle();
            }
        }
        g_remote = 0;
        g_sock = INVALID_SOCKET;
        closesocket(c);
        clear_peer();
    }
}

/* ---------- CLIENT: loop ---------- */

static void client_loop(SOCKET s) {
    Msg m; uint8_t *pl;
    int px = 0, py = 0, active = 0, enter_edge = 0;
    while (read_msg(s, &m, &pl)) {
        if (m.type == M_CLIP_TEXT || m.type == M_CLIP_FILES) { handle_clip(&m, pl); continue; }
        free(pl);
        switch (m.type) {
        case M_HELLO:
            g_peer_os = m.a;
            logmsg("peer OS: %s", m.a == MWB_OS_MAC ? "macOS" : (m.a == MWB_OS_WIN32 ? "win32" : "unknown"));
            break;
        case M_ENTER: {
            int edge = (m.c >> 1) & 3;   /* entry edge on OUR screen: 0=left 1=right 2=top 3=bottom */
            switch (edge) {
            case 1: px = g_sw - 1; py = m.a * (g_sh - 1) / 10000; break;
            case 2: px = m.a * (g_sw - 1) / 10000; py = 0; break;
            case 3: px = m.a * (g_sw - 1) / 10000; py = g_sh - 1; break;
            default: px = 0; py = m.a * (g_sh - 1) / 10000; break;
            }
            enter_edge = edge;
            active = 1;
            g_drag_expect = (m.c & 1) ? 1 : 0;
            g_drag_cancel = 0;
            if (g_sync_mouse) inject_abs(px, py);
            break;
        }
        case M_LEAVE:
            active = 0; g_drag_expect = 0; g_drag_cancel = 1;
            release_all_buttons();
            break;
        case M_MOVE: {
            if (!active) break;
            int nx = px + m.a, ny = py + m.b;
            int out = 0, pos = 0;               /* leaving back through the entry edge */
            switch (enter_edge) {
            case 1: if (nx > g_sw - 1) { out = 1; pos = py * 10000 / (g_sh - 1); } break;
            case 2: if (ny < 0)        { out = 1; pos = px * 10000 / (g_sw - 1); } break;
            case 3: if (ny > g_sh - 1) { out = 1; pos = px * 10000 / (g_sw - 1); } break;
            default: if (nx < 0)       { out = 1; pos = py * 10000 / (g_sh - 1); } break;
            }
            if (out) {
                active = 0; g_drag_expect = 0; g_drag_cancel = 1;
                release_all_buttons();
                send_msg(M_LEAVE, pos, 0, 0);
                break;
            }
            px = nx; py = ny;
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (py > g_sh - 1) py = g_sh - 1;
            if (px > g_sw - 1) px = g_sw - 1;
            if (g_sync_mouse) inject_abs(px, py);
            break;
        }
        case M_BUTTON:
            if (!active) break;
            if (m.a == 0 && !m.b) {             /* left button released */
                if (g_drag_expect && !g_drag_active) { g_drag_expect = 0; break; }  /* drag never started */
                g_drag_expect = 0;
            }
            if (g_sync_mouse) inject_button(m.a, m.b);
            break;
        case M_WHEEL:  if (active && g_sync_mouse)    inject_wheel(m.a, m.b);  break;
        case M_KEY:    if (active && g_sync_keyboard) inject_key(m.a, m.b, m.c); break;
        }
    }
}

static int discover(struct sockaddr_in *out) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    BOOL on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(UDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) {
        logmsg("udp bind failed: %d", WSAGetLastError());
        closesocket(s);
        return 0;
    }
    for (;;) {
        char buf[8];
        int len = (int)sizeof *out;
        int n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)out, &len);
        if (n == 4 && !memcmp(buf, "MWB1", 4)) {
            out->sin_port = htons(TCP_PORT);
            closesocket(s);
            return 1;
        }
    }
}

static DWORD WINAPI ClientNet(LPVOID p) {
    const char *ip = (const char *)p;
    for (;;) {
        if (!g_otp[0]) {
            char b[8];
            set_status(L"mwb client: enter the pairing code");
            if (prompt_otp(b)) memcpy(g_otp, b, 5);
            else {
                set_status(L"mwb client: not paired (tray menu > Enter pairing code)");
                WaitForSingleObject(g_pair_event, INFINITE);
                continue;
            }
        }
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        if (ip) {
            a.sin_family = AF_INET; a.sin_port = htons(TCP_PORT);
            if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
                logmsg("bad server ip: %s", ip);
                set_status(L"mwb client: bad server IP");
                return 1;
            }
        } else {
            status_idle();
            logmsg("client: listening for server beacon");
            if (!discover(&a)) { Sleep(2000); continue; }
        }
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
            tune_sock(s, 10000, 5000);
            int r = auth_client(s, g_otp);
            if (r == 1) {
                tune_sock(s, 0, 30000);            /* session: no recv timeout, long send budget */
                keep_alive(s);                     /* detect a vanished peer in seconds */
                g_sock = s;
                set_peer(a.sin_addr);
                send_msg(M_HELLO, MWB_OS_WIN32, g_sw, g_sh);
                client_loop(s);
                g_sock = INVALID_SOCKET;
                g_drag_expect = 0;
                release_all_buttons();
                clear_peer();
            } else if (r == 0) {
                logmsg("pairing rejected: wrong code");
                g_otp[0] = 0;
                notify(L"mwb", L"Wrong pairing code");
            } else logmsg("handshake error");
        }
        closesocket(s);
        Sleep(1000);
    }
}

/* ---------- FILES: receive (push) and serve (pull) ---------- */

typedef struct { wchar_t *path, *rel; uint64_t size; int isdir; } OEnt;
typedef struct { uint32_t gen; int n, cap; OEnt *e; } Offer;
static Offer g_offer;                   /* what WE are offering (guarded by g_offer_lock) */

static void offer_free(Offer *o) {
    for (int i = 0; i < o->n; i++) { free(o->e[i].path); free(o->e[i].rel); }
    free(o->e);
    memset(o, 0, sizeof *o);
}

static HANDLE create_unique(const wchar_t *dir, const wchar_t *name, wchar_t *out) {
    const wchar_t *dot = wcsrchr(name, L'.');
    if (dot == name) dot = NULL;
    size_t stemlen = dot ? (size_t)(dot - name) : wcslen(name);
    wchar_t stem[260];
    if (stemlen > 259) stemlen = 259;
    wcsncpy(stem, name, stemlen); stem[stemlen] = 0;
    for (int i = 0; i < 1000; i++) {
        if (i == 0) _snwprintf(out, MAX_PATH - 1, L"%ls\\%ls", dir, name);
        else        _snwprintf(out, MAX_PATH - 1, L"%ls\\%ls (%d)%ls", dir, stem, i, dot ? dot : L"");
        out[MAX_PATH - 1] = 0;
        HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) break;
    }
    return INVALID_HANDLE_VALUE;
}

/* answer a pull request: u64 size (all-ones = refused), then the bytes. 1 = keep connection */
static int handle_pull(SOCKET c, uint32_t gen, uint64_t idx) {
    wchar_t path[MAX_PATH];
    int ok = 0;
    uint64_t bad = ~(uint64_t)0;
    EnterCriticalSection(&g_offer_lock);
    if (g_offer.gen == gen && idx < (uint64_t)g_offer.n && !g_offer.e[idx].isdir) {
        wcsncpy(path, g_offer.e[idx].path, MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        ok = 1;
    }
    LeaveCriticalSection(&g_offer_lock);
    HANDLE f = ok ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
    LARGE_INTEGER sz;
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &sz)) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        send_all(c, &bad, 8);
        logmsg("file: pull refused (stale or missing)");
        return 0;
    }
    uint64_t size = (uint64_t)sz.QuadPart, left = size;
    char *buf = (char *)malloc(CHUNK);
    int good = buf && send_all(c, &size, 8);
    while (good && left > 0) {
        DWORD want = left > CHUNK ? CHUNK : (DWORD)left, got = 0;
        if (!ReadFile(f, buf, want, &got, NULL) || got == 0 || !send_all(c, buf, (int)got)) good = 0;
        else left -= got;
    }
    CloseHandle(f);
    free(buf);
    logmsg(good ? "file: served %llu bytes to peer" : "file: serve aborted (%llu bytes)", (unsigned long long)size);
    return good;
}

static DWORD WINAPI RecvConn(LPVOID arg) {
    SOCKET c = (SOCKET)(UINT_PTR)arg;
    int count = 0, failed = 0;
    wchar_t last[128] = L"";
    if (!file_auth_acceptor(c)) { logmsg("file: peer failed authentication"); closesocket(c); return 0; }
    char *buf = (char *)malloc(CHUNK);
    if (!buf) { closesocket(c); return 0; }
    CreateDirectoryW(g_recv_dir, NULL);

    for (;;) {
        FileHdr h;
        if (!recv_all(c, &h, (int)sizeof h)) break;         /* clean end between requests */
        if (h.magic == FILE_MAGIC_PULL) { if (!handle_pull(c, h.namelen, h.size)) break; continue; }
        if (h.magic != FILE_MAGIC || h.namelen == 0 || h.namelen > 1024) { logmsg("file: bad header"); failed = 1; break; }
        char name8[1025];
        if (!recv_all(c, name8, (int)h.namelen)) { failed = 1; break; }
        name8[h.namelen] = 0;
        wchar_t wname[1100];
        int wl = MultiByteToWideChar(CP_UTF8, 0, name8, (int)h.namelen, wname, 1099);
        if (wl <= 0 || wl > 255) { logmsg("file: bad name"); failed = 1; break; }   /* MAX_PATH component */
        wname[wl] = 0;
        sanitize_name(wname);

        wchar_t path[MAX_PATH];
        HANDLE f = create_unique(g_recv_dir, wname, path);
        if (f == INVALID_HANDLE_VALUE) { logmsg("file: cannot create %s (err %lu)", name8, GetLastError()); failed = 1; break; }

        uint64_t left = h.size;
        int ok = 1;
        DWORD lastp = 0;
        while (left > 0) {
            int want = left > CHUNK ? CHUNK : (int)left;
            int r = recv(c, buf, want, 0);
            DWORD w = 0;
            if (r <= 0 || !WriteFile(f, buf, (DWORD)r, &w, NULL) || w != (DWORD)r) { ok = 0; break; }
            left -= (uint64_t)r;
            DWORD now = GetTickCount();
            if (now - lastp > 500) {
                lastp = now;
                set_status(L"mwb: receiving %ls %d%%", wname, (int)((h.size - left) * 100 / h.size));
            }
        }
        CloseHandle(f);
        if (!ok) { DeleteFileW(path); logmsg("file: receive failed: %s", name8); failed = 1; break; }
        send_all(c, "K", 1);
        count++;
        wcsncpy(last, wname, 127); last[127] = 0;
        logmsg("file: received %s (%llu bytes)", name8, (unsigned long long)h.size);
    }
    free(buf);
    closesocket(c);
    if (failed)          notify(L"mwb", L"Receive failed (%d file(s) completed)", count);
    else if (count == 1) notify(L"mwb: file received", L"%ls", last);
    else if (count > 1)  notify(L"mwb: files received", L"%d files saved to the received folder", count);
    status_idle();
    return 0;
}

static DWORD WINAPI FileListen(LPVOID unused) {
    (void)unused;
    BOOL on = 1;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(FILE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) {
        logmsg("file listen %d failed: %d", FILE_PORT, WSAGetLastError());
        return 1;
    }
    for (;;) {
        struct sockaddr_in pa; int pl = (int)sizeof pa;
        SOCKET c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        ULONG peer = g_peer_ip;
        if (!peer || pa.sin_addr.s_addr != peer) {      /* only the connected peer may connect */
            logmsg("file: rejected connection from non-peer");
            closesocket(c);
            continue;
        }
        DWORD to = 30000, sto = 60000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&sto, sizeof sto);   /* never wedge a thread on a stalled peer */
        spawn(RecvConn, (LPVOID)(UINT_PTR)c);
    }
}

/* ---------- FILES: send (tray menu) ---------- */

typedef struct { int n; wchar_t **paths; } SendJob;

static int send_one(SOCKET s, const wchar_t *path, int idx, int total) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { logmsg("file: cannot open for send (err %lu)", GetLastError()); return 0; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz)) { CloseHandle(f); return 0; }

    const wchar_t *base = last_comp(path);
    char name8[1100];
    int nl = WideCharToMultiByte(CP_UTF8, 0, base, -1, name8, sizeof name8, NULL, NULL);
    if (nl <= 1) { CloseHandle(f); return 0; }
    nl--;                                           /* without terminator */
    if (nl > 1024) { CloseHandle(f); return 0; }    /* receiver rejects names over 1024 bytes */

    FileHdr h; h.magic = FILE_MAGIC; h.namelen = (uint32_t)nl; h.size = (uint64_t)sz.QuadPart;
    char *buf = (char *)malloc(CHUNK);
    int ok = buf && send_all(s, &h, (int)sizeof h) && send_all(s, name8, nl);

    uint64_t left = h.size;
    DWORD lastp = 0;
    while (ok && left > 0) {
        DWORD want = left > CHUNK ? CHUNK : (DWORD)left, got = 0;
        if (!ReadFile(f, buf, want, &got, NULL) || got == 0) { ok = 0; break; }
        if (!send_all(s, buf, (int)got)) { ok = 0; break; }
        left -= got;
        DWORD now = GetTickCount();
        if (now - lastp > 500) {
            lastp = now;
            set_status(L"mwb: sending %d/%d %ls %d%%", idx, total, base, (int)((h.size - left) * 100 / h.size));
        }
    }
    CloseHandle(f);
    free(buf);
    if (ok) { char ack = 0; ok = (recv(s, &ack, 1, 0) == 1 && ack == 'K'); }
    logmsg(ok ? "file: sent %s (%llu bytes)" : "file: send failed: %s", name8, (unsigned long long)h.size);
    return ok;
}

static DWORD WINAPI SendThread(LPVOID arg) {
    SendJob *j = (SendJob *)arg;
    int sent = 0;
    ULONG peer = g_peer_ip;
    if (!peer) {
        notify(L"mwb", L"Not connected to the other PC");
    } else {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(FILE_PORT); a.sin_addr.s_addr = peer;
        DWORD to = 60000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0 && file_auth_initiator(s)) {
            for (int i = 0; i < j->n; i++) {
                if (!send_one(s, j->paths[i], i + 1, j->n)) break;
                sent++;
            }
        } else {
            logmsg("file: connect/auth to peer failed: %d", WSAGetLastError());
        }
        closesocket(s);
        notify(L"mwb", L"Sent %d of %d file(s)", sent, j->n);
    }
    for (int i = 0; i < j->n; i++) free(j->paths[i]);
    free(j->paths);
    free(j);
    status_idle();
    return 0;
}

static wchar_t *dup_join(const wchar_t *dir, const wchar_t *name) {
    size_t dl = dir ? wcslen(dir) : 0, nl = wcslen(name);
    wchar_t *r = (wchar_t *)malloc((dl + nl + 2) * sizeof(wchar_t));
    if (!r) return NULL;
    if (dir) { wcscpy(r, dir); wcscat(r, L"\\"); wcscat(r, name); }
    else wcscpy(r, name);
    return r;
}

static void do_send_dialog(HWND owner) {
    static wchar_t buf[65536];
    buf[0] = 0;
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.lpstrTitle = L"Send to the other PC";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    SendJob *j = (SendJob *)calloc(1, sizeof *j);
    wchar_t *first = buf;
    size_t l1 = wcslen(first);
    if (first[l1 + 1] == 0) {                       /* single file: full path */
        j->paths = (wchar_t **)calloc(1, sizeof(wchar_t *));
        j->paths[0] = dup_join(NULL, first);
        j->n = 1;
    } else {                                        /* dir\0file1\0file2\0\0 */
        int cnt = 0; wchar_t *p;
        for (p = first + l1 + 1; *p; p += wcslen(p) + 1) cnt++;
        j->paths = (wchar_t **)calloc((size_t)cnt, sizeof(wchar_t *));
        for (p = first + l1 + 1; *p; p += wcslen(p) + 1) j->paths[j->n++] = dup_join(first, p);
    }
    spawn(SendThread, j);
}

/* ---------- CLIPBOARD: local side (we offer what is copied here) ---------- */

static int open_clip(HWND h) {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(h)) return 1;
        Sleep(15);
    }
    return 0;
}

static int offer_add(Offer *o, const wchar_t *path, const wchar_t *rel, uint64_t size, int isdir) {
    if (o->n >= MAX_OFFER) return 0;
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 64;
        OEnt *ne = (OEnt *)realloc(o->e, (size_t)nc * sizeof(OEnt));
        if (!ne) return 0;
        o->e = ne; o->cap = nc;
    }
    OEnt *e = &o->e[o->n];
    e->path = _wcsdup(path); e->rel = _wcsdup(rel); e->size = size; e->isdir = isdir;
    if (!e->path || !e->rel) { free(e->path); free(e->rel); return 0; }
    o->n++;
    return 1;
}

static void offer_walk(Offer *o, const wchar_t *path, const wchar_t *rel, int depth) {
    DWORD at = GetFileAttributesW(path);
    if (at == INVALID_FILE_ATTRIBUTES) return;
    if (at & FILE_ATTRIBUTE_DIRECTORY) {
        if (!offer_add(o, path, rel, 0, 1) || depth > 32) return;
        wchar_t pat[MAX_PATH];
        _snwprintf(pat, MAX_PATH - 1, L"%ls\\*", path);
        pat[MAX_PATH - 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* no symlink loops */
            wchar_t cp[MAX_PATH], cr[MAX_PATH];
            _snwprintf(cp, MAX_PATH - 1, L"%ls\\%ls", path, fd.cFileName); cp[MAX_PATH - 1] = 0;
            _snwprintf(cr, MAX_PATH - 1, L"%ls\\%ls", rel, fd.cFileName);  cr[MAX_PATH - 1] = 0;
            offer_walk(o, cp, cr, depth + 1);
        } while (o->n < MAX_OFFER && FindNextFileW(h, &fd));
        FindClose(h);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA d;
        uint64_t size = 0;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &d))
            size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        offer_add(o, path, rel, size, 0);
    }
}

static uint8_t *offer_serialize(const Offer *o, int *outlen) {
    size_t total = 4;
    for (int i = 0; i < o->n; i++) {
        int nl = WideCharToMultiByte(CP_UTF8, 0, o->e[i].rel, -1, NULL, 0, NULL, NULL);
        if (nl <= 1 || nl - 1 > 1024) return NULL;
        total += 16 + (size_t)(nl - 1);
    }
    if (total > MAX_PAYLOAD) return NULL;
    uint8_t *b = (uint8_t *)malloc(total + 1);   /* +1: per-entry NUL write must not run past the block */
    if (!b) return NULL;
    uint32_t cnt = (uint32_t)o->n;
    memcpy(b, &cnt, 4);
    size_t off = 4;
    for (int i = 0; i < o->n; i++) {
        uint64_t size = o->e[i].size;
        uint32_t isdir = (uint32_t)o->e[i].isdir;
        int nl = WideCharToMultiByte(CP_UTF8, 0, o->e[i].rel, -1, (char *)b + off + 16, 1100, NULL, NULL);
        if (nl <= 1) { free(b); return NULL; }
        nl--;                                   /* without terminator; matches offer_parse's read length */
        uint32_t unl = (uint32_t)nl;
        memcpy(b + off, &size, 8); memcpy(b + off + 8, &isdir, 4); memcpy(b + off + 12, &unl, 4);
        off += 16 + (size_t)nl;
    }
    *outlen = (int)total;
    return b;
}

static void offer_text(HWND h) {
    if (!open_clip(h)) return;
    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    char *u8 = NULL; int n = 0;
    if (hd) {
        const wchar_t *w = (const wchar_t *)GlobalLock(hd);
        if (w) {
            n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 1 && n - 1 <= MAX_TEXT) {
                u8 = (char *)malloc((size_t)n);
                if (u8) WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, n, NULL, NULL);
            }
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    if (!u8) return;
    send_var(M_CLIP_TEXT, 0, 0, u8, n - 1);
    logmsg("clipboard: sent text (%d bytes)", n - 1);
    free(u8);
}

static void offer_files(HWND h) {
    Offer o; memset(&o, 0, sizeof o);
    if (!open_clip(h)) return;
    HDROP dr = (HDROP)GetClipboardData(CF_HDROP);
    if (dr) {
        UINT cnt = DragQueryFileW(dr, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < cnt; i++) {
            wchar_t p[MAX_PATH];
            if (!DragQueryFileW(dr, i, p, MAX_PATH)) continue;
            offer_walk(&o, p, last_comp(p), 0);
        }
    }
    CloseClipboard();
    int len = 0;
    uint8_t *buf = o.n ? offer_serialize(&o, &len) : NULL;
    if (!buf) { offer_free(&o); return; }
    int n = o.n;
    EnterCriticalSection(&g_offer_lock);
    offer_free(&g_offer);
    g_offer = o;
    g_offer.gen = ++g_gen;
    uint32_t gen = g_offer.gen;
    LeaveCriticalSection(&g_offer_lock);
    int drag = InterlockedExchange(&g_drag_offer, 0) ? 1 : 0;
    send_var(M_CLIP_FILES, (int)gen, drag, buf, len);
    logmsg("clipboard: offered %d file entries%s", n, drag ? " (drag)" : "");
    free(buf);
}

static void on_clip_update(HWND h) {
    if ((!g_clip_sync && !g_sync_files) || g_sock == INVALID_SOCKET || !g_peer_ip) return;
    if (IsClipboardFormatAvailable(g_cfOrigin) || IsClipboardFormatAvailable(g_cfExclude)) return;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (g_sync_files) offer_files(h);                   /* files channel */
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (g_clip_sync) offer_text(h);                     /* clipboard text channel */
    }
}

/* ---------- CLIPBOARD: remote side (lazy files via IDataObject / IStream) ---------- */

typedef struct { IStream iface; LONG ref; uint32_t gen, index; uint64_t size, pos; SOCKET s; int st; } RStream;

static int rs_open(RStream *r) {
    if (r->st == 1) return 1;
    if (r->st == 2) return 0;
    r->st = 2;
    ULONG peer = g_peer_ip;
    if (!peer || !g_have_key) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(FILE_PORT); a.sin_addr.s_addr = peer;
    DWORD to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof to);
    FileHdr h; h.magic = FILE_MAGIC_PULL; h.namelen = r->gen; h.size = r->index;
    uint64_t sz = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0 || !file_auth_initiator(s) ||
        !send_all(s, &h, (int)sizeof h) || !recv_all(s, &sz, 8) || sz != r->size) {
        logmsg("clipboard: could not pull file %lu from peer", (unsigned long)r->index);
        closesocket(s);
        return 0;
    }
    r->s = s; r->st = 1;
    return 1;
}

static HRESULT STDMETHODCALLTYPE RS_QI(IStream *t, REFIID riid, void **pp) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISequentialStream) || IsEqualIID(riid, &IID_IStream)) {
        *pp = t; InterlockedIncrement(&((RStream *)t)->ref); return S_OK;
    }
    *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE RS_AddRef(IStream *t) { return (ULONG)InterlockedIncrement(&((RStream *)t)->ref); }
static ULONG STDMETHODCALLTYPE RS_Release(IStream *t) {
    RStream *r = (RStream *)t;
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) { if (r->s != INVALID_SOCKET) closesocket(r->s); free(r); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE RS_Read(IStream *t, void *pv, ULONG cb, ULONG *pcb) {
    RStream *r = (RStream *)t;
    ULONG got = 0;
    if (pcb) *pcb = 0;
    if (r->pos >= r->size) return S_FALSE;
    if (!rs_open(r)) return STG_E_READFAULT;
    uint64_t left = r->size - r->pos;
    ULONG want = left < cb ? (ULONG)left : cb;
    while (got < want) {                              /* fill the buffer if we can */
        int n = recv(r->s, (char *)pv + got, (int)(want - got), 0);
        if (n <= 0) { r->st = 2; return STG_E_READFAULT; }
        got += (ULONG)n;
    }
    r->pos += got;
    if (pcb) *pcb = got;
    return got == cb ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE RS_Write(IStream *t, const void *pv, ULONG cb, ULONG *pcb) {
    (void)t; (void)pv; (void)cb; if (pcb) *pcb = 0; return STG_E_ACCESSDENIED;
}
static int rs_skip(RStream *r, uint64_t n) {
    char tmp[16384];
    if (!rs_open(r)) return 0;
    while (n > 0) {
        int want = n > sizeof tmp ? (int)sizeof tmp : (int)n;
        int g = recv(r->s, tmp, want, 0);
        if (g <= 0) { r->st = 2; return 0; }
        n -= (uint64_t)g; r->pos += (uint64_t)g;
    }
    return 1;
}
static HRESULT STDMETHODCALLTYPE RS_Seek(IStream *t, LARGE_INTEGER mv, DWORD org, ULARGE_INTEGER *np) {
    RStream *r = (RStream *)t;
    int64_t target;
    if (org == STREAM_SEEK_END && mv.QuadPart == 0) {   /* size query: answer without consuming */
        if (np) np->QuadPart = r->size;
        return S_OK;
    }
    if (org == STREAM_SEEK_SET) target = mv.QuadPart;
    else if (org == STREAM_SEEK_CUR) target = (int64_t)r->pos + mv.QuadPart;
    else if (org == STREAM_SEEK_END) target = (int64_t)r->size + mv.QuadPart;
    else return STG_E_INVALIDFUNCTION;
    if (target < 0 || (uint64_t)target > r->size || (uint64_t)target < r->pos) return STG_E_INVALIDFUNCTION;
    if ((uint64_t)target > r->pos && !rs_skip(r, (uint64_t)target - r->pos)) return STG_E_READFAULT;
    if (np) np->QuadPart = r->pos;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE RS_SetSize(IStream *t, ULARGE_INTEGER n) { (void)t; (void)n; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE RS_CopyTo(IStream *t, IStream *dst, ULARGE_INTEGER cb, ULARGE_INTEGER *pr, ULARGE_INTEGER *pw) {
    uint8_t buf[65536];
    uint64_t left = cb.QuadPart, tr = 0, tw = 0;
    while (left > 0) {
        ULONG want = left > sizeof buf ? (ULONG)sizeof buf : (ULONG)left, got = 0, wr = 0;
        HRESULT hr = RS_Read(t, buf, want, &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        tr += got;
        hr = dst->lpVtbl->Write(dst, buf, got, &wr);
        if (FAILED(hr)) return hr;
        tw += wr; left -= got;
    }
    if (pr) pr->QuadPart = tr;
    if (pw) pw->QuadPart = tw;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE RS_Commit(IStream *t, DWORD f) { (void)t; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE RS_Revert(IStream *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE RS_Lock(IStream *t, ULARGE_INTEGER o, ULARGE_INTEGER c, DWORD ty) { (void)t; (void)o; (void)c; (void)ty; return STG_E_INVALIDFUNCTION; }
static HRESULT STDMETHODCALLTYPE RS_Stat(IStream *t, STATSTG *ps, DWORD f) {
    (void)f;
    if (!ps) return STG_E_INVALIDPOINTER;
    memset(ps, 0, sizeof *ps);
    ps->type = STGTY_STREAM;
    ps->cbSize.QuadPart = ((RStream *)t)->size;
    ps->grfMode = STGM_READ;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE RS_Clone(IStream *t, IStream **pp) { (void)t; *pp = NULL; return E_NOTIMPL; }

static const IStreamVtbl g_rs_vtbl = {
    .QueryInterface = RS_QI, .AddRef = RS_AddRef, .Release = RS_Release,
    .Read = RS_Read, .Write = RS_Write, .Seek = RS_Seek, .SetSize = RS_SetSize,
    .CopyTo = RS_CopyTo, .Commit = RS_Commit, .Revert = RS_Revert,
    .LockRegion = RS_Lock, .UnlockRegion = RS_Lock, .Stat = RS_Stat, .Clone = RS_Clone
};

typedef struct { IDataObject iface; LONG ref; RemoteOffer *ro; } MwbData;

static HRESULT STDMETHODCALLTYPE DO_QI(IDataObject *t, REFIID riid, void **pp) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *pp = t; InterlockedIncrement(&((MwbData *)t)->ref); return S_OK;
    }
    *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE DO_AddRef(IDataObject *t) { return (ULONG)InterlockedIncrement(&((MwbData *)t)->ref); }
static ULONG STDMETHODCALLTYPE DO_Release(IDataObject *t) {
    MwbData *d = (MwbData *)t;
    LONG n = InterlockedDecrement(&d->ref);
    if (!n) { ro_free(d->ro); free(d); }
    return (ULONG)n;
}

static HGLOBAL build_descriptor(const RemoteOffer *ro) {
    SIZE_T sz = sizeof(UINT) + (SIZE_T)ro->n * sizeof(FILEDESCRIPTORW);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sz);
    if (!h) return NULL;
    FILEGROUPDESCRIPTORW *g = (FILEGROUPDESCRIPTORW *)GlobalLock(h);
    g->cItems = (UINT)ro->n;
    for (int i = 0; i < ro->n; i++) {
        FILEDESCRIPTORW *d = &g->fgd[i];
        d->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | (ro->e[i].isdir ? 0 : FD_FILESIZE);
        d->dwFileAttributes = ro->e[i].isdir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        d->nFileSizeHigh = (DWORD)(ro->e[i].size >> 32);
        d->nFileSizeLow = (DWORD)(ro->e[i].size & 0xFFFFFFFFu);
        wcsncpy(d->cFileName, ro->e[i].name, MAX_PATH - 1);
    }
    GlobalUnlock(h);
    return h;
}

static HRESULT STDMETHODCALLTYPE DO_GetData(IDataObject *t, FORMATETC *f, STGMEDIUM *m) {
    MwbData *d = (MwbData *)t;
    memset(m, 0, sizeof *m);
    if (f->cfFormat == g_cfDesc && (f->tymed & TYMED_HGLOBAL)) {
        HGLOBAL h = build_descriptor(d->ro);
        if (!h) return E_OUTOFMEMORY;
        m->tymed = TYMED_HGLOBAL; m->hGlobal = h;
        return S_OK;
    }
    if (f->cfFormat == g_cfContents && (f->tymed & TYMED_ISTREAM)) {
        LONG i = f->lindex;
        if (i < 0 || i >= d->ro->n || d->ro->e[i].isdir) return DV_E_LINDEX;
        RStream *r = (RStream *)calloc(1, sizeof *r);
        if (!r) return E_OUTOFMEMORY;
        r->iface.lpVtbl = (IStreamVtbl *)&g_rs_vtbl;
        r->ref = 1; r->gen = d->ro->gen; r->index = (uint32_t)i; r->size = d->ro->e[i].size; r->s = INVALID_SOCKET;
        m->tymed = TYMED_ISTREAM; m->pstm = &r->iface;
        return S_OK;
    }
    if (f->cfFormat == g_cfOrigin && (f->tymed & TYMED_HGLOBAL)) {
        m->tymed = TYMED_HGLOBAL; m->hGlobal = GlobalAlloc(GMEM_MOVEABLE, 1);
        return m->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    return DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE DO_GetDataHere(IDataObject *t, FORMATETC *f, STGMEDIUM *m) { (void)t; (void)f; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DO_QueryGetData(IDataObject *t, FORMATETC *f) {
    (void)t;
    if (f->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (f->cfFormat == g_cfDesc && (f->tymed & TYMED_HGLOBAL)) return S_OK;
    if (f->cfFormat == g_cfContents && (f->tymed & TYMED_ISTREAM)) return S_OK;
    if (f->cfFormat == g_cfOrigin && (f->tymed & TYMED_HGLOBAL)) return S_OK;
    return DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE DO_Canon(IDataObject *t, FORMATETC *i, FORMATETC *o) { (void)t; (void)i; o->ptd = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DO_SetData(IDataObject *t, FORMATETC *f, STGMEDIUM *m, WINBOOL rel) { (void)t; (void)f; (void)m; (void)rel; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DO_Enum(IDataObject *t, DWORD dir, IEnumFORMATETC **pp) {
    (void)t;
    if (dir != DATADIR_GET) { *pp = NULL; return E_NOTIMPL; }
    FORMATETC fe[3] = {
        { (CLIPFORMAT)g_cfDesc,     NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        { (CLIPFORMAT)g_cfContents, NULL, DVASPECT_CONTENT,  0, TYMED_ISTREAM },
        { (CLIPFORMAT)g_cfOrigin,   NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL }
    };
    return SHCreateStdEnumFmtEtc(3, fe, pp);
}
static HRESULT STDMETHODCALLTYPE DO_DAdvise(IDataObject *t, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) { (void)t; (void)f; (void)a; (void)s; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE DO_DUnadvise(IDataObject *t, DWORD c) { (void)t; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE DO_EnumDAdvise(IDataObject *t, IEnumSTATDATA **pp) { (void)t; *pp = NULL; return OLE_E_ADVISENOTSUPPORTED; }

static const IDataObjectVtbl g_do_vtbl = {
    .QueryInterface = DO_QI, .AddRef = DO_AddRef, .Release = DO_Release,
    .GetData = DO_GetData, .GetDataHere = DO_GetDataHere, .QueryGetData = DO_QueryGetData,
    .GetCanonicalFormatEtc = DO_Canon, .SetData = DO_SetData, .EnumFormatEtc = DO_Enum,
    .DAdvise = DO_DAdvise, .DUnadvise = DO_DUnadvise, .EnumDAdvise = DO_EnumDAdvise
};

/* ---------- DRAG (receiving side): invisible catcher window + OLE DoDragDrop ---------- */

static HRESULT STDMETHODCALLTYPE DS_QI(IDropSource *t, REFIID riid, void **pp) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) { *pp = t; return S_OK; }
    *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE DS_AddRef(IDropSource *t) { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE DS_Release(IDropSource *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE DS_Query(IDropSource *t, WINBOOL esc, DWORD keys) {
    (void)t;
    if (esc || g_drag_cancel || !g_peer_ip) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE DS_Feedback(IDropSource *t, DWORD eff) { (void)t; (void)eff; return DRAGDROP_S_USEDEFAULTCURSORS; }
static const IDropSourceVtbl g_ds_vtbl = {
    .QueryInterface = DS_QI, .AddRef = DS_AddRef, .Release = DS_Release,
    .QueryContinueDrag = DS_Query, .GiveFeedback = DS_Feedback
};
static IDropSource g_dropsrc = { (IDropSourceVtbl *)&g_ds_vtbl };

static IDataObject *g_drag_data;
static HWND g_catcher;

static LRESULT CALLBACK CatchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN: {
        KillTimer(h, 1);
        IDataObject *d = g_drag_data;
        g_drag_data = NULL;
        if (d) {
            DWORD eff = 0;
            g_drag_active = 1; g_drag_cancel = 0;
            logmsg("drag: started");
            HRESULT hr = DoDragDrop(d, &g_dropsrc, DROPEFFECT_COPY, &eff);
            g_drag_active = 0; g_drag_expect = 0;
            logmsg("drag: finished (hr=%08lx effect=%lu)", (unsigned long)hr, (unsigned long)eff);
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) inject_button(0, 0);   /* never leave it held */
            d->lpVtbl->Release(d);
        }
        DestroyWindow(h);
        return 0;
    }
    case WM_TIMER:                                  /* the injected click never arrived */
        KillTimer(h, 1);
        if (g_drag_data) { g_drag_data->lpVtbl->Release(g_drag_data); g_drag_data = NULL; }
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) inject_button(0, 0);
        logmsg("drag: timed out before start");
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        g_catcher = NULL;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void start_drag(IDataObject *d) {
    if (g_drag_active || g_catcher) return;
    POINT pt; GetCursorPos(&pt);
    d->lpVtbl->AddRef(d);
    g_drag_data = d;
    g_catcher = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                L"mwb_catcher", L"", WS_POPUP, pt.x - 32, pt.y - 32, 64, 64,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_catcher) { g_drag_data = NULL; d->lpVtbl->Release(d); return; }
    SetLayeredWindowAttributes(g_catcher, 0, 1, LWA_ALPHA);     /* nearly invisible, still hit-testable */
    ShowWindow(g_catcher, SW_SHOWNOACTIVATE);
    SetTimer(g_catcher, 1, 1000, NULL);
    inject_button(0, 1);                                        /* press lands on the catcher */
}

/* ---------- CLIPBOARD thread (STA, own message loop) ---------- */

static void apply_text(char *u8, int len) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, u8, len, NULL, 0);
    HGLOBAL hg = wl > 0 ? GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)wl + 1) * sizeof(wchar_t)) : NULL;
    if (hg) {
        wchar_t *w = (wchar_t *)GlobalLock(hg);
        MultiByteToWideChar(CP_UTF8, 0, u8, len, w, wl);
        w[wl] = 0;
        GlobalUnlock(hg);
        if (open_clip(g_clipwnd)) {
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, hg);
            SetClipboardData(g_cfOrigin, GlobalAlloc(GMEM_MOVEABLE, 1));   /* marks it as ours */
            CloseClipboard();
            logmsg("clipboard: received text (%d bytes)", len);
        } else GlobalFree(hg);
    }
    free(u8);
}

static void apply_offer(RemoteOffer *ro) {
    if (!ro) return;
    if (!g_sync_files) { ro_free(ro); return; }             /* files channel off */
    int n = ro->n, drag = ro->drag;
    MwbData *d = (MwbData *)calloc(1, sizeof *d);
    if (!d) { ro_free(ro); return; }
    d->iface.lpVtbl = (IDataObjectVtbl *)&g_do_vtbl;
    d->ref = 1; d->ro = ro;
    IDataObject *obj = &d->iface;
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 5 && FAILED(hr); i++) { hr = OleSetClipboard(obj); if (FAILED(hr)) Sleep(20); }
    logmsg("clipboard: peer offered %d file entries%s (hr=%08lx)", n, drag ? " (drag)" : "", (unsigned long)hr);
    if (drag && g_drag_expect) start_drag(obj);
    obj->lpVtbl->Release(obj);
}

static LRESULT CALLBACK ClipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLIPBOARDUPDATE: on_clip_update(h); return 0;
    case WM_APP_TEXT:  apply_text((char *)l, (int)w); return 0;
    case WM_APP_OFFER: apply_offer((RemoteOffer *)l); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI ClipThread(LPVOID unused) {
    (void)unused;
    OleInitialize(NULL);
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ClipProc; wc.hInstance = hi; wc.lpszClassName = L"mwb_clip_wnd";
    RegisterClassW(&wc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = CatchProc; wc.hInstance = hi; wc.lpszClassName = L"mwb_catcher";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"mwb_clip_wnd", L"mwbclip", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!w) { logmsg("clipboard window failed"); return 1; }
    AddClipboardFormatListener(w);
    g_clipwnd = w;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    RemoveClipboardFormatListener(w);
    OleUninitialize();
    return 0;
}

/* ---------- tray window ---------- */

static void show_menu(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SEND, L"Send file(s)...");
    AppendMenuW(m, MF_STRING | (g_clip_sync ? MF_CHECKED : 0), ID_CLIP, L"Sync clipboard text");
    AppendMenuW(m, MF_STRING, ID_CODE, g_is_server ? L"Show pairing code" : L"Enter pairing code...");
    HMENU side = CreatePopupMenu();
    AppendMenuW(side, MF_STRING | (g_side == MWB_SIDE_RIGHT ? MF_CHECKED : 0), ID_SIDE_R, L"Right of this PC\tdefault");
    AppendMenuW(side, MF_STRING | (g_side == MWB_SIDE_LEFT  ? MF_CHECKED : 0), ID_SIDE_L, L"Left of this PC");
    AppendMenuW(side, MF_STRING | (g_side == MWB_SIDE_UP    ? MF_CHECKED : 0), ID_SIDE_U, L"Above this PC");
    AppendMenuW(side, MF_STRING | (g_side == MWB_SIDE_DOWN  ? MF_CHECKED : 0), ID_SIDE_D, L"Below this PC");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)side, L"Peer is on...");
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Open received folder");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case ID_SEND:
        if (!g_sync_files) { logmsg("files channel disabled by config: send skipped"); break; }
        do_send_dialog(h);
        break;
    case ID_CLIP:
        g_clip_sync = !g_clip_sync;
        logmsg("clipboard text sync %s", g_clip_sync ? "on" : "off");
        break;
    case ID_SIDE_R: g_side = MWB_SIDE_RIGHT; logmsg("peer side: right of this PC"); break;
    case ID_SIDE_L: g_side = MWB_SIDE_LEFT;  logmsg("peer side: left of this PC");  break;
    case ID_SIDE_U: g_side = MWB_SIDE_UP;    logmsg("peer side: above this PC");    break;
    case ID_SIDE_D: g_side = MWB_SIDE_DOWN;  logmsg("peer side: below this PC");    break;
    case ID_CODE:
        if (g_is_server) show_code();
        else {
            char b[8];
            if (prompt_otp(b)) { memcpy(g_otp, b, 5); SetEvent(g_pair_event); }
        }
        break;
    case ID_OPEN:
        CreateDirectoryW(g_recv_dir, NULL);
        ShellExecuteW(NULL, L"open", g_recv_dir, NULL, NULL, SW_SHOWNORMAL);
        break;
    case ID_EXIT: PostQuitMessage(0); break;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbar_created) { tray_add(); return 0; }   /* explorer restarted */
    switch (m) {
    case WM_TRAY:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP) show_menu(h);
        return 0;
    case WM_STATUS:    tray_tip(); return 0;
    case WM_BALLOON:   tray_balloon(); return 0;
    case WM_DRAGCROSS: spawn(DragSeq, NULL); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- main ---------- */

static int run_ui(int server, const char *ip) {
    g_is_server = server;
    init_screen();
    init_recv_dir();
    g_gen = GetTickCount();
    g_cfDesc     = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    g_cfContents = RegisterClipboardFormatW(L"FileContents");
    g_cfOrigin   = RegisterClipboardFormatW(L"MWB.Origin");
    g_cfExclude  = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (server) new_otp();
    status_idle();

    HINSTANCE hi = GetModuleHandleW(NULL);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"mwb_tray_wnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"mwb", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;
    tray_add();
    logmsg("started as %s; data folder: %ls", server ? "server" : "client", g_base);

    spawn(FileListen, NULL);
    spawn(ClipThread, NULL);

    HHOOK hm = NULL, hk = NULL;
    if (server) {
        show_code();
        spawn(ServerNet, NULL);
        if (g_sync_mouse || g_sync_keyboard) {
            if (g_sync_mouse)    hm = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hi, 0);
            if (g_sync_keyboard) hk = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, hi, 0);
            if (!hm && !hk) {
                logmsg("hook install failed");
                MessageBoxW(NULL, L"Could not install input hooks.", L"mwb", MB_ICONERROR);
                tray_del();
                return 1;
            }
        } else {
            logmsg("input hooks skipped (mouse + keyboard disabled by config)");
        }
    } else {
        spawn(ClientNet, (LPVOID)ip);
    }

    /* MsgWait instead of a bare GetMessage loop: the TinyIPC toggle event joins the
     * wait, so a second launch cleanly stops this instance (repo IPC contract). */
    MSG msg;
    for (;;) {
        DWORD wr = MsgWaitForMultipleObjectsEx(1, &g_toggle_event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wr == WAIT_OBJECT_0) {
            logmsg("second launch detected: stopping (toggle-off)");
            break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
out:
    if (hm) UnhookWindowsHookEx(hm);
    if (hk) UnhookWindowsHookEx(hk);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);   /* unblock/notify the peer */
    tray_del();
    return 0;
}

static void to_acp(const wchar_t *w, char *out, int cap) {
    if (!w || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

/* optional read-only config file (default: mwb.txt beside the exe, or --config PATH).
 * Parser shared with the macOS peer via common/tiny_config.h; this side only maps
 * the known keys. The tool NEVER writes to the file; CLI args always override it.
 * There is deliberately no OTP key: the code rotates at runtime. */
static int cfg_server = -1, cfg_side = -1, cfg_clip = -1;
static int cfg_mouse = -1, cfg_keyboard = -1, cfg_files = -1;

static void cfg_accept(void *user, const char *key, const char *value) {
    int *used = (int *)user;
    if (!strcmp(key, "mode")) {
        if      (!strcmp(value, "server")) { cfg_server = 1; (*used)++; }
        else if (!strcmp(value, "client")) { cfg_server = 0; (*used)++; }
        else logmsg("config: bad mode '%s'", value);
    } else if (!strcmp(key, "side")) {
        if      (!strcmp(value, "right")) { cfg_side = MWB_SIDE_RIGHT; (*used)++; }
        else if (!strcmp(value, "left"))  { cfg_side = MWB_SIDE_LEFT;  (*used)++; }
        else if (!strcmp(value, "up"))    { cfg_side = MWB_SIDE_UP;    (*used)++; }
        else if (!strcmp(value, "down"))  { cfg_side = MWB_SIDE_DOWN;  (*used)++; }
        else logmsg("config: bad side '%s'", value);
    } else if (!strcmp(key, "clipboard")) {
        if      (!strcmp(value, "on"))  { cfg_clip = 1; (*used)++; }
        else if (!strcmp(value, "off")) { cfg_clip = 0; (*used)++; }
        else logmsg("config: bad clipboard '%s'", value);
    } else if (!strcmp(key, "mouse")) {
        if      (!strcmp(value, "on"))  { cfg_mouse = 1; (*used)++; }
        else if (!strcmp(value, "off")) { cfg_mouse = 0; (*used)++; }
        else logmsg("config: bad mouse '%s'", value);
    } else if (!strcmp(key, "keyboard")) {
        if      (!strcmp(value, "on"))  { cfg_keyboard = 1; (*used)++; }
        else if (!strcmp(value, "off")) { cfg_keyboard = 0; (*used)++; }
        else logmsg("config: bad keyboard '%s'", value);
    } else if (!strcmp(key, "files")) {
        if      (!strcmp(value, "on"))  { cfg_files = 1; (*used)++; }
        else if (!strcmp(value, "off")) { cfg_files = 0; (*used)++; }
        else logmsg("config: bad files '%s'", value);
    } else {
        logmsg("config: unknown key '%s'", key);
    }
}

static void load_config(const char *path) {
    if (!path || !*path) return;
    int existed = 0, used = 0;
    TinyConfig_Load(path, cfg_accept, &used, &existed);
    if (!existed) { logmsg("config: %s not readable", path); return; }
    if (used) logmsg("config: %d key(s) applied from %s", used, path);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hi; (void)hp; (void)cmd; (void)show;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_send_lock);
    InitializeCriticalSection(&g_offer_lock);
    g_pair_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {     /* when started from a terminal, print there too */
        freopen("CONOUT$", "w", stdout);
        g_console = 1;
    }

    /* TinyCLI: tokenize the command line once; --otp/--log go through the option
     * table, the two positionals (mode, ip) are collected in one matching walk */
    wchar_t wbuf[2048];
    wcsncpy_s(wbuf, _countof(wbuf), GetCommandLineW(), _TRUNCATE);
    wchar_t *wargs[TINY_CLI_MAX_ARGS];
    int wargc = TinyCLI_Tokenize(wbuf, wargs, TINY_CLI_MAX_ARGS);

    const wchar_t *otp_w = NULL, *log_w = NULL, *cfg_w = NULL;
    CliOption opts[] = {
        { L"--otp", CLI_OPT_STRING, (void *)&otp_w, 0, 0, L"<code>", L"4-char pairing code" },
        { L"--log", CLI_OPT_STRING, (void *)&log_w, 0, 0, L"<file>", L"append-mode log file" },
        { L"--config", CLI_OPT_STRING, (void *)&cfg_w, 0, 0, L"<file>", L"read-only config (default: mwb.txt beside the exe)" },
    };
    if (TinyCLI_CheckHelp(wargc, wargs, L"mwb",
                          L"Mouse With Borders style LAN peer: the server shares its mouse/keyboard, the client injects",
                          L"mwb [server | client [server-ip]] [--otp CODE] [--log file] [--config file] [--help]", opts, 3))
        return 0;
    TinyCLI_Parse(wargc, wargs, opts, 2);

    char mode_buf[16] = {0}, ip_buf[64] = {0}, log_buf[MAX_PATH] = {0}, otp_buf[8] = {0};
    int npos = 0;
    for (int i = 1; i < wargc; i++) {
        if (i + 1 < wargc && (!wcscmp(wargs[i], L"--otp") || !wcscmp(wargs[i], L"--log") ||
                              !wcscmp(wargs[i], L"--config"))) { i++; continue; }
        if (wargs[i][0] == L'-') continue;              /* unknown flag: ignore */
        if (npos == 0)      to_acp(wargs[i], mode_buf, sizeof mode_buf);
        else if (npos == 1) to_acp(wargs[i], ip_buf,  sizeof ip_buf);
        npos++;
    }
    const char *mode    = mode_buf[0] ? mode_buf : NULL;
    const char *ip      = ip_buf[0]   ? ip_buf   : NULL;
    const char *otp     = otp_buf[0]  ? otp_buf  : NULL;
    const char *logpath = log_buf[0]  ? log_buf  : NULL;

    TinyDPI_EnablePerMonitorAwareness();
    if (!TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &g_toggle_event))
        return 0;                          /* second launch: toggle sent, exit 0 */

    init_base_dir();
    if (logpath) g_logf = fopen(logpath, "a");
    else open_default_log();
    if (otp && strlen(otp) == 4) {
        for (int i = 0; i < 4; i++) g_otp[i] = (char)toupper((unsigned char)otp[i]);
        g_otp[4] = 0;
    }

    /* optional read-only config applies only where the CLI stayed silent */
    char cfg_path_a[MAX_PATH];
    if (cfg_w) to_acp(cfg_w, cfg_path_a, sizeof cfg_path_a);
    else {
        wchar_t cfg_path_w[MAX_PATH];
        _snwprintf(cfg_path_w, MAX_PATH - 1, L"%ls\\mwb.txt", g_base);
        cfg_path_w[MAX_PATH - 1] = 0;
        to_acp(cfg_path_w, cfg_path_a, sizeof cfg_path_a);
    }
    load_config(cfg_path_a);
    if (cfg_side >= 0) g_side = cfg_side;
    if (cfg_clip >= 0) InterlockedExchange(&g_clip_sync, cfg_clip);
    if (cfg_mouse >= 0)     InterlockedExchange(&g_sync_mouse, cfg_mouse);
    if (cfg_keyboard >= 0)  InterlockedExchange(&g_sync_keyboard, cfg_keyboard);
    if (cfg_files >= 0)     InterlockedExchange(&g_sync_files, cfg_files);
    logmsg("channels: mouse=%s keyboard=%s clipboard=%s files=%s "
           "(a channel is live only if the peer enables it too)",
           g_sync_mouse ? "on" : "off", g_sync_keyboard ? "on" : "off",
           g_clip_sync ? "on" : "off", g_sync_files ? "on" : "off");

    int server;
    if (!mode) {
        if (cfg_server >= 0) {
            server = cfg_server;                    /* config decided the role */
        } else {                                    /* no CLI and no config: ask */
            int r = MessageBoxW(NULL,
                L"Is this PC the SERVER (the one with the physical keyboard and mouse)?\n\n"
                L"Yes = Server\nNo = Client\nCancel = Quit",
                L"mwb", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDYES) server = 1;
            else if (r == IDNO) server = 0;
            else return 0;
        }
    } else if (tolower((unsigned char)mode[0]) == 's') {
        server = 1;
    } else if (tolower((unsigned char)mode[0]) == 'c') {
        server = 0;
    } else {
        MessageBoxW(NULL, L"usage: mwb [server | client [server-ip]] [--otp CODE] [--log file]\n(run with --help for details)", L"mwb", MB_ICONINFORMATION);
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(NULL, L"Winsock initialization failed.", L"mwb", MB_ICONERROR);
        return 1;
    }
    return run_ui(server, ip);
}
