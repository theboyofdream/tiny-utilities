/*
 * mwb_mac.c - macOS peer of src/mwb.c (mouse-without-borders, win32<->mac<->win32).
 *              Pure C: CoreGraphics CGEventTap capture/injection, CoreFoundation
 *              pasteboard, CommonCrypto HMAC. Same wire protocol, same ports.
 *
 * Build (macOS):
 *   clang -O2 -o mwb-mac src/mwb_mac.c \
 *       -framework CoreFoundation -framework CoreGraphics \
 *       -framework CoreServices -framework Carbon -framework IOKit
 *
 * Run (terminal):
 *   mwb-mac server                this Mac shares its mouse/keyboard
 *   mwb-mac client [server-ip]    this Mac receives the other PC's input
 *   options: --side right|left|up|down  which screen edge leads to the peer
 *              (server only; default right; the client mirrors it automatically)
 *            --otp CODE (skip the code prompt)   --log FILE   --config FILE
 *            (config default: mwb.txt beside this binary; mode/side/clipboard keys;
 *             read-only - the tool never writes to it; CLI arguments always win)
 *
 * Permissions: System Settings > Privacy & Security > Accessibility -> add the
 * mwb binary (required for the event tap; without it the program exits with a
 * hint). Cross-machine drag-and-drop is a Windows-OLE feature; on macOS the
 * peer's file offers materialise as real files in ~/mwb-received and are placed
 * on the pasteboard (Cmd+V pastes into Finder). Emergency return to local
 * control: double-tap Caps Lock while controlling the remote PC.
 *
 * Ports: UDP 24800 discovery | TCP 24801 input+clipboard | TCP 24802 files
 * Logs:   mwb.log next to the binary (or ~/Library/Logs/mwb.log), events only.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ctype.h>                      /* toupper (code prompt) */
#include <limits.h>                     /* NAME_MAX, PATH_MAX */
#include <mach-o/dyld.h>                /* _NSGetExecutablePath (log path) */
#include <stdbool.h>
#include <sys/file.h>                   /* flock (single instance) */

#include "common/tiny_config.h"         /* read-only key=value config loader */

#include <Carbon/Carbon.h>              /* Pasteboard C API, kVK_* keycodes */
#include <CommonCrypto/CommonHMAC.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/hidsystem/ev_keymap.h>

#define UDP_PORT   24800
#define TCP_PORT   24801
#define FILE_PORT  24802
#define FILE_MAGIC      0x3146574Du     /* "MWF1" push: {magic, namelen, size} + name + bytes */
#define FILE_MAGIC_PULL 0x3150574Du     /* "MWP1" pull: {magic, gen, index} -> size64 + bytes */
#define CHUNK       (256 * 1024)
#define MAX_PAYLOAD (8 * 1024 * 1024)
#define MAX_TEXT    (4 * 1024 * 1024)
#define MAX_OFFER   20000
#define LOG_MAX     (5 * 1024 * 1024)
#define MWB_OS_WIN32 1
#define MWB_OS_MAC   2

/* tunnel side: which edge of OUR screen leads to the peer (session-only setting) */
#define MWB_SIDE_RIGHT 0
#define MWB_SIDE_LEFT  1
#define MWB_SIDE_UP    2       /* peer above: tunnel at our TOP edge    */
#define MWB_SIDE_DOWN  3       /* peer below: tunnel at our BOTTOM edge */

enum { M_MOVE = 1, M_BUTTON, M_WHEEL, M_KEY, M_ENTER, M_LEAVE, M_CLIP_TEXT, M_CLIP_FILES, M_HELLO };
/*
 * M_MOVE   a=dx        b=dy          (screen-space deltas, y top-down positive)
 * M_BUTTON a=button    b=down        (0=L 1=R 2=M 3=X1 4=X2)
 * M_WHEEL  a=delta     b=horizontal  (120-based, positive = up/right)
 * M_KEY    a=win32-vk  b=scan        c=flags (bit0=up, bit1=extended)
 * M_ENTER  a=pos 0..10000 along the entry edge   c bit0 = left button held (drag)
 *            c bits1-2 = client entry edge: 0=left 1=right 2=top 3=bottom  server -> client
 * M_LEAVE  a=pos 0..10000 along the tunnel edge  client -> server: give control back;
 *            server -> client: stop remote mode
 * M_CLIP_TEXT  c=len, then len bytes UTF-8
 * M_CLIP_FILES a=generation b=flags    c=len, then payload:
 *              u32 count, then per entry {u64 size, u32 isdir, u32 namelen, utf8 relative name}
 * M_HELLO  a=os (1 = win32, 2 = mac) b=screen-width c=screen-height, post-auth, both ways
 */
typedef struct { uint32_t type; int32_t a, b, c; } Msg;
typedef struct { uint32_t magic; uint32_t namelen; uint64_t size; } FileHdr;

typedef int SOCKET;
#define INVALID_SOCKET (-1)

static const char *g_progname = "mwb-mac";

static int   g_is_server = 0;
static int   g_peer_os = 0;
static int   g_sw, g_sh, g_cx, g_cy;    /* main display size, its center */
static int   g_mx, g_my;                /* main display origin in mac global space */
static int   g_side = MWB_SIDE_RIGHT;   /* tunnel edge: peer is on this side */
static volatile int g_sock = INVALID_SOCKET;
static volatile int g_remote = 0;       /* server: 1 = input is being forwarded */
static volatile uint32_t g_peer_ip = 0; /* IPv4 (network order) of the peer, 0 = none */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_offer_lock = PTHREAD_MUTEX_INITIALIZER;

static char    g_otp[8];
static int     g_fail;
static uint8_t g_key[32];
static volatile int g_have_key = 0;

static FILE *g_logf;
static int   g_stdout_log;              /* mirror log lines to stdout when a tty */

static PasteboardRef g_pb;              /* the system clipboard */
static uint64_t g_self_change_ms;       /* suppress watcher once after our own change */
static volatile int g_clip_sync = 1;
static volatile int g_sync_mouse = 1;        /* config: mouse = on|off (tunnel handover + pointer events) */
static volatile int g_sync_keyboard = 1;     /* config: keyboard = on|off */
static volatile int g_sync_files = 1;        /* config: files = on|off (offers + pulls) */
static volatile int g_drag_expect = 0, g_drag_cancel = 0;
static uint32_t g_gen;

static char g_received[MAXPATHLEN];     /* ~/mwb-received */

/* ---------- time / log ---------- */

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logmsg(const char *fmt, ...) {
    char line[700];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    if (g_logf) { fputs(line, g_logf); fputc('\n', g_logf); fflush(g_logf); }
    if (g_stdout_log) { fputs(line, stdout); fputc('\n', stdout); fflush(stdout); }
    pthread_mutex_unlock(&g_lock);
}

static void open_default_log(void) {
    char base[PATH_MAX] = {0};
    uint32_t sz = (uint32_t)sizeof base;
    if (_NSGetExecutablePath(base, &sz) != 0) strcpy(base, ".");
    char *sl = strrchr(base, '/');
    if (sl && sl != base) *sl = 0; else strcpy(base, ".");
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/mwb.log", base);
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size >= LOG_MAX) {
        char old[PATH_MAX];
        snprintf(old, sizeof old, "%s.old", p);
        rename(p, old);
    }
    if (access(base, W_OK) != 0) {
        const char *home = getenv("HOME");
        snprintf(p, sizeof p, "%s/Library/Logs/mwb.log", home ? home : "/tmp");
    }
    g_logf = fopen(p, "a");
}

/* ---------- socket helpers ---------- */

static int recv_all(SOCKET s, void *buf, int n) {
    char *p = (char *)buf;
    while (n > 0) {
        int r = (int)recv(s, p, (size_t)n, MSG_NOSIGNAL);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

static int send_all(SOCKET s, const void *buf, int n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        int r = (int)send(s, p, (size_t)n, MSG_NOSIGNAL);
        if (r <= 0) return 0;
        p += r; n -= r;
    }
    return 1;
}

static void send_locked(const Msg *m, const void *data, int len) {
    pthread_mutex_lock(&g_send_lock);
    SOCKET s = g_sock;
    int ok = s != INVALID_SOCKET && send_all(s, m, (int)sizeof *m) && (len == 0 || send_all(s, data, len));
    pthread_mutex_unlock(&g_send_lock);
    if (s != INVALID_SOCKET && !ok && g_is_server) g_remote = 0;
}

static void send_msg(uint32_t t, int a, int b, int c) {
    Msg m; m.type = t; m.a = a; m.b = b; m.c = c;
    send_locked(&m, NULL, 0);
}

static void send_var(uint32_t t, int a, int b, const void *data, int len) {
    Msg m; m.type = t; m.a = a; m.b = b; m.c = len;
    send_locked(&m, data, len);
}

static void tune_sock(SOCKET s, int rcv_ms, int snd_ms) {
    int on = 1;
    struct timeval tv;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    if (rcv_ms > 0) { tv.tv_sec = rcv_ms / 1000; tv.tv_usec = (rcv_ms % 1000) * 1000;
                      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
    if (snd_ms > 0) { tv.tv_sec = snd_ms / 1000; tv.tv_usec = (snd_ms % 1000) * 1000;
                      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
}

/* keepalives: idle 5 s (TCP_KEEPALIVE), interval 1 s (TCP_KEEPINTVL) */
static void keep_alive(SOCKET s) {
    int on = 1, idle = 5, itvl = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &itvl, sizeof itvl);
}

static void init_screen(void) {
    CGDirectDisplayID main = CGMainDisplayID();
    CGRect mb = CGDisplayBounds(main);        /* global space, bottom-left origin */
    g_sw = (int)mb.size.width;
    g_sh = (int)mb.size.height;
    g_mx = (int)mb.origin.x;
    g_my = (int)mb.origin.y;
    g_cx = g_sw >> 1;
    g_cy = g_sh >> 1;
}

static void spawn(void *(*fn)(void *), void *arg) {
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg) == 0) pthread_detach(th);
}

/* ---------- crypto ---------- */

static int rand_bytes(void *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)p + got, n - got);
        if (r <= 0) { close(fd); return 0; }
        got += (size_t)r;
    }
    close(fd);
    return 1;
}

static int hmac256(const uint8_t *key, size_t klen, const uint8_t *data, size_t dlen, uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256, key, klen, data, dlen, out);
    return 1;
}

static int ct_eq(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void new_otp(void) {
    static const char CH[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t r[4];
    if (!rand_bytes(r, 4)) {
        logmsg("random generator failed");
        exit(1);
    }
    for (int i = 0; i < 4; i++) g_otp[i] = CH[r[i] & 31];
    g_otp[4] = 0;
}

static void mk_data(uint8_t d[33], char tag, const uint8_t *x, const uint8_t *y) {
    d[0] = (uint8_t)tag; memcpy(d + 1, x, 16); memcpy(d + 17, y, 16);
}

static int auth_server(SOCKET c) {
    uint8_t ns[16], in[48], d[33], ex[32], out[33];
    char otp[8]; memcpy(otp, g_otp, 8);
    if (!rand_bytes(ns, 16) || !send_all(c, ns, 16) || !recv_all(c, in, 48)) return -1;
    mk_data(d, 'C', ns, in);
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

/* ---------- message framing ---------- */

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

static void set_peer(uint32_t ip_net) {
    struct in_addr a; a.s_addr = ip_net;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    g_peer_ip = ip_net;
    logmsg("connected: %s", ip);
}

static void clear_peer(void) {
    g_peer_ip = 0;
    g_peer_os = 0;
    g_have_key = 0;
    g_remote = 0;
    logmsg("disconnected");
}

/* ---------- pairing code prompt (client, tty) ---------- */

static int prompt_otp(char *out) {
    printf("Enter the 4-character code shown on the other machine:\n> ");
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof line, stdin)) return 0;
    int n = 0;
    for (int i = 0; line[i]; i++) {
        unsigned char ch = (unsigned char)toupper((unsigned char)line[i]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '9')) {
            if (n >= 4) return 0;
            out[n++] = (char)ch;
        } else if (ch == '\n' || ch == '\r') break;
        else return 0;
    }
    out[n] = 0;
    return n == 4;
}

/* ---------- input: key mapping (wire = win32 VK) ---------- */

typedef struct { uint8_t vk, mk; } VKPair;

static const VKPair g_keymap[] = {
    { 0x08, kVK_Delete },             { 0x09, kVK_Tab },               { 0x0D, kVK_Return },
    { 0x10, kVK_Shift },              { 0x11, kVK_Control },           { 0x12, kVK_Option },
    { 0x14, kVK_CapsLock },           { 0x1B, kVK_Escape },            { 0x20, kVK_Space },
    { 0x21, kVK_PageUp },             { 0x22, kVK_PageDown },          { 0x23, kVK_End },
    { 0x24, kVK_Home },               { 0x25, kVK_LeftArrow },         { 0x26, kVK_UpArrow },
    { 0x27, kVK_RightArrow },         { 0x28, kVK_DownArrow },         { 0x2E, kVK_ForwardDelete },
    { 0x30, kVK_ANSI_0 }, { 0x31, kVK_ANSI_1 }, { 0x32, kVK_ANSI_2 }, { 0x33, kVK_ANSI_3 },
    { 0x34, kVK_ANSI_4 }, { 0x35, kVK_ANSI_5 }, { 0x36, kVK_ANSI_6 }, { 0x37, kVK_ANSI_7 },
    { 0x38, kVK_ANSI_8 }, { 0x39, kVK_ANSI_9 },
    { 0x41, kVK_ANSI_A }, { 0x42, kVK_ANSI_B }, { 0x43, kVK_ANSI_C }, { 0x44, kVK_ANSI_D },
    { 0x45, kVK_ANSI_E }, { 0x46, kVK_ANSI_F }, { 0x47, kVK_ANSI_G }, { 0x48, kVK_ANSI_H },
    { 0x49, kVK_ANSI_I }, { 0x4A, kVK_ANSI_J }, { 0x4B, kVK_ANSI_K }, { 0x4C, kVK_ANSI_L },
    { 0x4D, kVK_ANSI_M }, { 0x4E, kVK_ANSI_N }, { 0x4F, kVK_ANSI_O }, { 0x50, kVK_ANSI_P },
    { 0x51, kVK_ANSI_Q }, { 0x52, kVK_ANSI_R }, { 0x53, kVK_ANSI_S }, { 0x54, kVK_ANSI_T },
    { 0x55, kVK_ANSI_U }, { 0x56, kVK_ANSI_V }, { 0x57, kVK_ANSI_W }, { 0x58, kVK_ANSI_X },
    { 0x59, kVK_ANSI_Y }, { 0x5A, kVK_ANSI_Z },
    { 0x5B, kVK_Command },            { 0x5C, kVK_RightCommand },
    { 0x60, kVK_ANSI_Keypad0 }, { 0x61, kVK_ANSI_Keypad1 }, { 0x62, kVK_ANSI_Keypad2 },
    { 0x63, kVK_ANSI_Keypad3 }, { 0x64, kVK_ANSI_Keypad4 }, { 0x65, kVK_ANSI_Keypad5 },
    { 0x66, kVK_ANSI_Keypad6 }, { 0x67, kVK_ANSI_Keypad7 }, { 0x68, kVK_ANSI_Keypad8 },
    { 0x69, kVK_ANSI_Keypad9 },
    { 0x6A, kVK_ANSI_KeypadMultiply },{ 0x6B, kVK_ANSI_KeypadPlus },
    { 0x6D, kVK_ANSI_KeypadMinus },   { 0x6E, kVK_ANSI_KeypadDecimal },
    { 0x6F, kVK_ANSI_KeypadDivide },
    { 0x70, kVK_F1 }, { 0x71, kVK_F2 }, { 0x72, kVK_F3 }, { 0x73, kVK_F4 },
    { 0x74, kVK_F5 }, { 0x75, kVK_F6 }, { 0x76, kVK_F7 }, { 0x77, kVK_F8 },
    { 0x78, kVK_F9 }, { 0x79, kVK_F10 },{ 0x7A, kVK_F11 }, { 0x7B, kVK_F12 },
    { 0x7C, kVK_F13 }, { 0x7D, kVK_F14 }, { 0x7E, kVK_F15 }, { 0xA0, kVK_Shift },
    { 0xA1, kVK_RightShift }, { 0xA2, kVK_Control },  { 0xA3, kVK_RightControl },
    { 0xA4, kVK_Option },     { 0xA5, kVK_RightOption },
    { 0xBA, kVK_ANSI_Semicolon }, { 0xBB, kVK_ANSI_Equal },
    { 0xBC, kVK_ANSI_Comma },     { 0xBD, kVK_ANSI_Minus },
    { 0xBE, kVK_ANSI_Period },    { 0xBF, kVK_ANSI_Slash },
    { 0xC0, kVK_ANSI_Grave },
    { 0xDB, kVK_ANSI_LeftBracket },{ 0xDC, kVK_ANSI_Backslash },
    { 0xDD, kVK_ANSI_RightBracket },{ 0xDE, kVK_ANSI_Quote },
};
#define KEYMAP_N ((int)(sizeof g_keymap / sizeof g_keymap[0]))

static uint8_t wire_to_mac(int vk) {
    for (int i = 0; i < KEYMAP_N; i++) if (g_keymap[i].vk == (uint8_t)vk) return g_keymap[i].mk;
    return 0xFF;
}

static uint8_t mac_to_wire(int mk) {
    for (int i = 0; i < KEYMAP_N; i++) if (g_keymap[i].mk == (uint8_t)mk) return g_keymap[i].vk;
    return 0xFF;
}

#define VKMOD_SHIFT   0x01
#define VKMOD_CONTROL 0x02
#define VKMOD_ALT     0x04
#define VKMOD_WIN     0x08

static uint32_t wire_modmask(int vk) {
    switch (vk) {
    case 0x10: case 0xA0: case 0xA1: return VKMOD_SHIFT;
    case 0x11: case 0xA2: case 0xA3: return VKMOD_CONTROL;
    case 0x12: case 0xA4: case 0xA5: return VKMOD_ALT;
    case 0x5B: case 0x5C:            return VKMOD_WIN;
    }
    return 0;
}

static uint32_t mac_modmask(int mk) {
    switch (mk) {
    case kVK_Shift: case kVK_RightShift:     return VKMOD_SHIFT;
    case kVK_Control: case kVK_RightControl: return VKMOD_CONTROL;
    case kVK_Option: case kVK_RightOption:   return VKMOD_ALT;
    case kVK_Command: case kVK_RightCommand: return VKMOD_WIN;
    }
    return 0;
}

static CGEventFlags mod_to_cg(int mask) {
    CGEventFlags f = 0;
    if (mask & VKMOD_SHIFT)   f |= kCGEventFlagMaskShift;
    if (mask & VKMOD_CONTROL) f |= kCGEventFlagMaskControl;
    if (mask & VKMOD_ALT)     f |= kCGEventFlagMaskAlternate;
    if (mask & VKMOD_WIN)     f |= kCGEventFlagMaskCommand;
    return f;
}

/* ---------- CLIENT: input injection state ---------- */

static CGEventSourceRef g_inj_src;
static int g_px, g_py;
static int g_held[5];                    /* injected button state 0..4 */
static int g_moddown;                    /* currently held modifier bitmask */

static void inj_abs(int x, int y) {
    if (x < 0) x = 0;
    if (x >= g_sw) x = g_sw - 1;
    if (y < 0) y = 0;
    if (y >= g_sh) y = g_sh - 1;
    g_px = x; g_py = y;
    CGPoint pt = CGPointMake((CGFloat)g_mx + (CGFloat)x, (CGFloat)g_my + (CGFloat)(g_sh - 1 - y));
    CGEventType type = kCGEventMouseMoved;
    CGMouseButton mb = kCGMouseButtonLeft;
    if (g_held[0])      { type = kCGEventLeftMouseDragged;  mb = kCGMouseButtonLeft; }
    else if (g_held[1]) { type = kCGEventRightMouseDragged; mb = kCGMouseButtonRight; }
    else if (g_held[2]) { type = kCGEventOtherMouseDragged; mb = kCGMouseButtonCenter; }
    CGEventRef ev = CGEventCreateMouseEvent(g_inj_src, type, pt, mb);
    if (ev) { CGEventPost(kCGSessionEventTap, ev); CFRelease(ev); }
}

static void inj_button(int btn, int down) {
    if (btn < 0 || btn > 4) return;
    if (g_held[btn] == down) return;
    g_held[btn] = down;
    CGPoint pt = CGPointMake((CGFloat)g_mx + (CGFloat)g_px, (CGFloat)g_my + (CGFloat)(g_sh - 1 - g_py));
    CGEventType t; CGMouseButton mb;
    switch (btn) {
    case 0: t = down ? kCGEventLeftMouseDown  : kCGEventLeftUp;    mb = kCGMouseButtonLeft;    break;
    case 1: t = down ? kCGEventRightMouseDown : kCGEventRightUp;   mb = kCGMouseButtonRight;   break;
    case 2: t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; mb = kCGMouseButtonCenter; break;
    case 3: t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; mb = (CGMouseButton)3; break;
    default: t = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; mb = (CGMouseButton)4; break;
    }
    CGEventRef ev = CGEventCreateMouseEvent(g_inj_src, t, pt, mb);
    if (ev) { CGEventPost(kCGSessionEventTap, ev); CFRelease(ev); }
}

static void inj_release_all(void) {
    for (int b = 0; b < 5; b++) if (g_held[b]) inj_button(b, 0);
    g_moddown = 0;                         /* modifiers can stick when control is dropped */
}

static void inj_wheel(int delta, int horiz) {
    /* win delta 120-based, positive up/right; mac axis1 line delta positive up */
    int lines = delta / 120;
    if (lines == 0 && delta != 0) lines = delta > 0 ? 1 : -1;
    if (lines > 3) lines = 3; else if (lines < -3) lines = -3;
    CGEventRef ev = CGEventCreateScrollWheelEvent(g_inj_src, kCGScrollEventUnitLine, 2, 0, 0);
    if (!ev) return;
    if (horiz) CGEventSetIntegerValueField(ev, kCGScrollWheelEventDeltaAxis2, lines);
    else       CGEventSetIntegerValueField(ev, kCGScrollWheelEventDeltaAxis1, lines);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
}

static void inj_key(int vk, int down) {
    uint32_t mask = wire_modmask(vk);
    if (mask) {                            /* literal mapping: Ctrl -> Control, Win -> Command */
        if (down) g_moddown |= mask;
        else      g_moddown &= ~mask;
    }
    uint8_t mk = wire_to_mac(vk);
    if (mk == 0xFF) return;
    CGEventRef ev = CGEventCreateKeyboardEvent(g_inj_src, (CGKeyCode)mk, down ? true : false);
    if (!ev) return;
    CGEventFlags f = mod_to_cg(g_moddown);
    if (f) CGEventSetFlags(ev, f);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
}

/* ---------- SERVER: capture (CGEventTap) ---------- */

static CGEventTapRef g_tap;
static int g_have_last;
static uint64_t g_caps_last;

/* client entry-edge code carried in M_ENTER c bits 1-2:
 * 0 = client LEFT edge, 1 = client RIGHT, 2 = client TOP, 3 = client BOTTOM */
static int side_to_code(int side) {
    switch (side) {
    case MWB_SIDE_RIGHT: return 0;
    case MWB_SIDE_LEFT:  return 1;
    case MWB_SIDE_UP:    return 3;     /* peer above -> client enters at its bottom */
    default:             return 2;     /* peer below -> client enters at its top    */
    }
}

/* lx/ly: event location in main-display-local coordinates (bottom-left origin) */
static void capture_on(CGFloat lx, CGFloat ly) {
    int pos;
    if (g_side == MWB_SIDE_UP || g_side == MWB_SIDE_DOWN)
        pos = (int)(lx * 10000.0 / (CGFloat)(g_sw - 1));   /* edge runs horizontally */
    else
        pos = (int)((CGFloat)(g_sh - 1 - ly) * 10000.0 / (CGFloat)(g_sh - 1));
    if (pos < 0) pos = 0; else if (pos > 10000) pos = 10000;
    g_remote = 1;
    g_have_last = 0;
    send_msg(M_ENTER, pos, 0, side_to_code(g_side) << 1);
    logmsg("remote control handed to peer (tunnel at our %s edge)",
           g_side == MWB_SIDE_RIGHT ? "right" : g_side == MWB_SIDE_LEFT ? "left" :
           g_side == MWB_SIDE_UP ? "top" : "bottom");
}

/* park the local cursor back at our tunnel edge (frac = 0..10000 along it) */
static void capture_off(int frac) {
    CGFloat lx, ly;
    if (g_side == MWB_SIDE_UP || g_side == MWB_SIDE_DOWN) {
        lx = (CGFloat)((int64_t)frac * (g_sw - 1) / 10000);
        ly = g_side == MWB_SIDE_UP ? 1 : (CGFloat)(g_sh - 2);
    } else {
        ly = (CGFloat)(g_sh - 1 - (int)((int64_t)frac * (g_sh - 1) / 10000));
        lx = g_side == MWB_SIDE_LEFT ? 1 : (CGFloat)(g_sw - 2);
    }
    g_remote = 0;
    g_have_last = 0;
    CGWarpMouseCursorPosition(CGPointMake((CGFloat)g_mx + lx, (CGFloat)g_my + ly));
    CGAssociateMouseAndMouseCursorPosition(1);
    send_msg(M_LEAVE, frac, 0, 0);
    logmsg("back to local control");
}

static CGEventRef tap_cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ud) {
    (void)proxy; (void)ud;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(g_tap, true);
        return NULL;
    }

    /* emergency return: double-tap Caps Lock within 400 ms while remote */
    if (type == kCGEventFlagsChanged && g_remote) {
        int64_t kc = CGEventGetIntegerValueField(ev, kCGKeyboardEventKeycode);
        if (kc == kVK_CapsLock) {
            uint64_t now = ms_now();
            if (now - g_caps_last < 400) {
                g_caps_last = 0;
                capture_off(5000);
                return NULL;
            }
            g_caps_last = now;
        }
        /* forward the modifier change (bit compare handled below in KEY case) */
    }

    if (!g_remote) {
        if (type != kCGEventMouseMoved) return ev;
        if (!g_sync_mouse) return ev;                 /* mouse off: no tunnel handover */
        CGPoint pt = CGEventGetLocation(ev);
        CGFloat lx = pt.x - (CGFloat)g_mx, ly = pt.y - (CGFloat)g_my;
        int edge;
        switch (g_side) {
        case MWB_SIDE_LEFT:  edge = lx <= 0;                   break;
        case MWB_SIDE_UP:    edge = ly <= 0;                   break;
        case MWB_SIDE_DOWN:  edge = ly >= (CGFloat)(g_sh - 1); break;
        default:             edge = lx >= (CGFloat)(g_sw - 1); break;
        }
        if (!edge) return ev;
        capture_on(lx, ly);
        return NULL;                                  /* swallow the edge event */
    }

    /* remote mode: forward + swallow */
    switch (type) {
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged: case kCGEventRightMouseDragged: case kCGEventOtherMouseDragged: {
        if (!g_sync_mouse) break;                     /* mouse off: swallow */
        double dx = CGEventGetDoubleValueField(ev, kCGMouseEventDeltaX);
        double dy = CGEventGetDoubleValueField(ev, kCGMouseEventDeltaY);
        if (!g_have_last) { g_have_last = 1; break; }
        if (dx || dy) send_msg(M_MOVE, (int)dx, (int)-dy, 0);
        break;
    }
    case kCGEventLeftMouseDown:  case kCGEventLeftUp:
    case kCGEventRightMouseDown: case kCGEventRightUp:
    case kCGEventOtherMouseDown: case kCGEventOtherMouseUp: {
        if (!g_sync_mouse) break;                     /* mouse off: swallow */
        int bn = (int)CGEventGetIntegerValueField(ev, kCGMouseEventButtonNumber);
        int btn = bn <= 0 ? 0 : bn == 1 ? 1 : bn == 2 ? 2 : bn == 3 ? 3 : 4;
        int down = (type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown);
        if (bn > 4) break;                    /* buttons beyond X2 are not forwarded */
        send_msg(M_BUTTON, btn, down, 0);
        break;
    }
    case kCGEventScrollWheel: {
        if (!g_sync_mouse) break;                     /* mouse off: swallow */
        int64_t cont = CGEventGetIntegerValueField(ev, kCGScrollWheelEventIsContinuous);
        if (cont) {
            int64_t p1 = CGEventGetIntegerValueField(ev, kCGScrollWheelEventPointDeltaAxis1);
            int64_t p2 = CGEventGetIntegerValueField(ev, kCGScrollWheelEventPointDeltaAxis2);
            if (p1) send_msg(M_WHEEL, (int)(p1 / 10), 0, 0);
            if (p2) send_msg(M_WHEEL, (int)(p2 / 10), 1, 0);
        } else {
            int64_t a1 = CGEventGetIntegerValueField(ev, kCGScrollWheelEventDeltaAxis1);
            int64_t a2 = CGEventGetIntegerValueField(ev, kCGScrollWheelEventDeltaAxis2);
            if (a1) send_msg(M_WHEEL, (int)(a1 * 120), 0, 0);
            if (a2) send_msg(M_WHEEL, (int)(a2 * 120), 1, 0);
        }
        break;
    }
    case kCGEventKeyDown: case kCGEventKeyUp: case kCGEventFlagsChanged: {
        if (!g_sync_keyboard) break;                  /* keyboard off: swallow (emergency Caps double-tap above) */
        int64_t kc = CGEventGetIntegerValueField(ev, kCGKeyboardEventKeycode);
        int down = type != kCGEventKeyUp;
        uint32_t mm = mac_modmask((int)kc);
        if (mm) {
            /* tracked via CGEvent flags so we only report real press/release edges */
            static uint32_t s_prev;
            uint32_t wg = (uint32_t)CGEventGetFlags(ev);
            uint32_t bit;
            switch (mm) {
            case VKMOD_SHIFT:   bit = kCGEventFlagMaskShift;     break;
            case VKMOD_CONTROL: bit = kCGEventFlagMaskControl;   break;
            case VKMOD_ALT:     bit = kCGEventFlagMaskAlternate; break;
            default:            bit = kCGEventFlagMaskCommand;   break;
            }
            uint32_t now_down = (wg & bit) ? 1 : 0;
            uint32_t was_down = (s_prev & bit) ? 1 : 0;
            s_prev = wg;
            if (now_down == was_down) break;      /* ignore repeats */
            if (type == kCGEventFlagsChanged) {   /* sanity: flags and key event agree */
                down = now_down ? 1 : 0;
            }
        }
        int vk = mac_to_wire((int)kc);
        if (vk == 0xFF) break;                    /* no win32 equivalent: swallow */
        send_msg(M_KEY, vk, (int)kc, down ? 0 : 1);
        break;
    }
    default:
        break;
    }
    return NULL;
}

/* ---------- FILES: offer struct, sanitising, disk helpers ---------- */

typedef struct { uint64_t size; int isdir; char rel[1025]; char *path; } OEnt;
typedef struct { uint32_t gen; int n, cap; OEnt *e; } Offer;
static Offer g_offer;

static void offer_free(Offer *o) {
    for (int i = 0; i < o->n; i++) free(o->e[i].path);
    free(o->e);
    memset(o, 0, sizeof *o);
}

static void offer_add(Offer *o, const char *path, const char *rel, uint64_t size, int isdir) {
    if (o->n >= MAX_OFFER) return;
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 64;
        OEnt *ne = (OEnt *)realloc(o->e, (size_t)nc * sizeof(OEnt));
        if (!ne) return;
        o->e = ne; o->cap = nc;
    }
    OEnt *e = &o->e[o->n];
    e->path = strdup(path);
    e->size = size; e->isdir = isdir;
    snprintf(e->rel, sizeof e->rel, "%s", rel);
    if (!e->path) return;
    o->n++;
}

static void offer_walk(Offer *o, const char *path, const char *rel, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (depth > 16) return;
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *de;
        int before = o->n;
        char child[2 * MAXPATHLEN], cr[2 * 1025];
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (o->n >= MAX_OFFER) break;
            snprintf(child, sizeof child, "%s/%s", path, de->d_name);
            struct stat st2;
            if (lstat(child, &st2) != 0 || S_ISLNK(st2.st_mode)) continue;   /* no symlink loops */
            snprintf(cr, sizeof cr, "%s/%s", rel, de->d_name);
            offer_walk(o, child, cr, depth + 1);
        }
        closedir(d);
        if (o->n == before) offer_add(o, path, rel, 0, 1);   /* keep empty dirs */
    } else {
        offer_add(o, path, rel, (uint64_t)st.st_size, 0);
    }
}

/* make ONE mac path component safe: no '/', no control chars, no '..', non-empty */
static void sanitize_component(char *s) {
    size_t n = 0;
    for (; s[n]; n++) if (s[n] < 32 || s[n] == ':' ) s[n] = '_';
    while (n && s[n - 1] == '.') s[--n] = 0;
    if (!n || !strcmp(s, "..")) { strcpy(s, "file"); n = 4; }
    if (s[0] == '/') { memmove(s, s + 1, n); s[n - 1] = 0; }
}

/* build a mac destination path in g_received while joining sanitized components */
static int build_received_path(const char *rel, char *out, size_t outsz) {
    /* drop any absolute part; only keep components */
    char tmp[4 * 1025];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *comps[64]; int nc = 0;
    char *p = tmp;
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        if (*p && strcmp(p, ".") && strcmp(p, "..") && nc < 64) {
            sanitize_component(p);
            comps[nc++] = p;
        }
        p = slash ? slash + 1 : p + strlen(p);
    }
    if (!nc) return 0;
    /* mkdir -p prefixes (skip the file name) */
    char cur[MAXPATHLEN];
    snprintf(cur, sizeof cur, "%s", g_received);
    for (int i = 0; i < nc - 1; i++) {
        size_t l = strlen(cur);
        snprintf(cur + l, sizeof cur - l - 1, "/%s", comps[i]);
        if (strlen(comps[i]) > NAME_MAX) return 0;
        mkdir(cur, 0755);
    }
    snprintf(out, outsz, "%s/%s", cur, comps[nc - 1]);
    return 1;
}

static FILE *create_unique_received(const char *name, char *out, size_t outsz) {
    char stem[1025], ext[300] = "";
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        size_t sl = (size_t)(dot - name);
        if (sl > 1024) sl = 1024;
        memcpy(stem, name, sl); stem[sl] = 0;
        snprintf(ext, sizeof ext, "%s", dot);
    } else {
        snprintf(stem, sizeof stem, "%s", name);
    }
    for (int i = 0; i < 1000; i++) {
        if (i == 0) snprintf(out, outsz, "%s/%s%s", g_received, stem, ext);
        else        snprintf(out, outsz, "%s/%s (%d)%s", g_received, stem, i, ext);
        int fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "wb");
            return f;
        }
        if (errno != EEXIST && errno != EACCES) break;
    }
    return NULL;
}

/* ---------- FILES: pull + push (server side of the file channel) ---------- */

/* answer a pull request: u64 size (all-ones = refused), then the bytes. 1 = keep connection */
static int handle_pull(SOCKET c, uint32_t gen, uint64_t idx) {
    char path[MAXPATHLEN];
    int ok = 0;
    uint64_t bad = ~(uint64_t)0;
    pthread_mutex_lock(&g_offer_lock);
    if (g_offer.gen == gen && idx < (uint64_t)g_offer.n && !g_offer.e[idx].isdir) {
        snprintf(path, sizeof path, "%s", g_offer.e[idx].path);
        ok = 1;
    }
    pthread_mutex_unlock(&g_offer_lock);
    uint64_t size = 0;
    if (ok) {
        struct stat st;
        if (stat(path, &st) == 0) size = (uint64_t)st.st_size;
        else ok = 0;
    }
    if (!ok) { send_all(c, &bad, 8); logmsg("file: pull refused (stale or missing)"); return 0; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { send_all(c, &bad, 8); return 0; }
    if (!send_all(c, &size, 8)) { close(fd); return 0; }
    char *buf = (char *)malloc(CHUNK);
    uint64_t left = size;
    int good = buf != NULL;
    while (good && left > 0) {
        size_t want = left > CHUNK ? CHUNK : left;
        ssize_t got = read(fd, buf, want);
        if (got <= 0) { good = 0; break; }
        if (!send_all(c, buf, (int)got)) { good = 0; break; }
        left -= (uint64_t)got;
    }
    free(buf);
    close(fd);
    logmsg("file: served %llu bytes to peer", (unsigned long long)size);
    return good;
}

typedef struct { SOCKET s; int failed; int count; char last[512]; } RecvCtx;

static int recv_one_push(RecvCtx *rx, const FileHdr *h) {
    SOCKET c = rx->s;
    char name8[1025];
    if (!recv_all(c, name8, (int)h->namelen)) { rx->failed = 1; return 0; }
    name8[h->namelen] = 0;
    /* UTF-8 name -> plain char path (utf-8 is native on mac) */
    char base[1025];
    snprintf(base, sizeof base, "%s", name8);
    char *last = strrchr(base, '/');
    if (!last) last = base; else last++;
    sanitize_component(last);
    char outpath[MAXPATHLEN];
    FILE *f = create_unique_received(last, outpath, sizeof outpath);
    if (!f) { logmsg("file: cannot create %s", base); rx->failed = 1; return 0; }
    uint64_t left = h->size;
    char *buf = (char *)malloc(CHUNK);
    int ok = buf != NULL;
    while (ok && left > 0) {
        size_t want = left > CHUNK ? CHUNK : left;
        ssize_t got = recv(c, buf, want, MSG_NOSIGNAL);
        if (got <= 0 || fwrite(buf, 1, (size_t)got, f) != (size_t)got) { ok = 0; break; }
        left -= (uint64_t)got;
    }
    free(buf);
    fclose(f);
    if (!ok) { unlink(outpath); logmsg("file: receive failed: %s", base); rx->failed = 1; return 0; }
    send_all(c, "K", 1);
    rx->count++;
    snprintf(rx->last, sizeof rx->last, "%s", outpath);
    logmsg("file: received %s (%llu bytes)", base, (unsigned long long)h->size);
    return 1;
}

static void *RecvConn(void *arg) {
    SOCKET c = (SOCKET)(intptr_t)arg;
    RecvCtx rx = { c, 0, 0, "" };
    int count_local = 0, failed = 0;
    char lastlocal[512] = "";
    if (!file_auth_acceptor(c)) { logmsg("file: peer failed authentication"); close(c); return NULL; }
    mkdir(g_received, 0755);
    for (;;) {
        FileHdr h;
        if (!recv_all(c, &h, (int)sizeof h)) break;
        if (h.magic == FILE_MAGIC_PULL) {
            if (!handle_pull(c, h.namelen, h.size)) break;
            continue;
        }
        if (h.magic != FILE_MAGIC || h.namelen == 0 || h.namelen > 1024) { logmsg("file: bad header"); failed = 1; break; }
        if (!recv_one_push(&rx, &h)) { failed = 1; break; }
        count_local = rx.count;
        snprintf(lastlocal, sizeof lastlocal, "%s", rx.last);
    }
    close(c);
    if (failed)      logmsg("file: receive failed (%d file(s) completed)", count_local);
    else if (count_local == 1) logmsg("file: saved %s", lastlocal);
    else if (count_local > 1)  logmsg("file: %d files saved to %s", count_local, g_received);
    return NULL;
}

static void *FileListen(void *unused) {
    (void)unused;
    int on = 1;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return NULL;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(FILE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) {
        logmsg("file listen %d failed: %s", FILE_PORT, strerror(errno));
        return NULL;
    }
    for (;;) {
        struct sockaddr_in pa; socklen_t pl = sizeof pa;
        SOCKET c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c == INVALID_SOCKET) { usleep(100000); continue; }
        if (!g_peer_ip || pa.sin_addr.s_addr != g_peer_ip) {
            logmsg("file: rejected connection from non-peer");
            close(c);
            continue;
        }
        int on2 = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on2, sizeof on2);
        tune_sock(c, 30000, 60000);
        spawn(RecvConn, (void *)(intptr_t)c);
    }
    return NULL;
}

/* ---------- CLIPBOARD: harvest our pasteboard -> wire (both roles) ---------- */

/* flavor UTIs are created at startup: CFSTR() is not a valid C file-scope
 * initialiser, so the constants are assigned in main() before threads start */
static CFStringRef kUf8Flavor;
static CFStringRef kFileUrlFlavor;

static void offer_text_out(const char *utf8, int len) {
    if (!g_clip_sync || g_sock == INVALID_SOCKET || !g_peer_ip) return;
    send_var(M_CLIP_TEXT, 0, 0, utf8, len);
    logmsg("clipboard: sent text (%d bytes)", len);
}

/* serialize the entry list and hand it to the peer; ownership of o's entries
 * moves into g_offer so FileListen's pull handler can serve the files */
static void offer_publish(Offer *o) {
    if (!g_sync_files || g_sock == INVALID_SOCKET || !g_peer_ip) { offer_free(o); return; }
    size_t total = 4;
    for (int i = 0; i < o->n; i++) {
        int nl = (int)strlen(o->e[i].rel);
        if (nl <= 0 || nl > 1024) { offer_free(o); return; }
        total += 16 + (size_t)nl;
    }
    if (total > MAX_PAYLOAD) { offer_free(o); return; }
    uint8_t *b = (uint8_t *)malloc(total);
    if (!b) { offer_free(o); return; }
    uint32_t cnt = (uint32_t)o->n;
    memcpy(b, &cnt, 4);
    size_t off = 4;
    for (int i = 0; i < o->n; i++) {
        uint64_t size = o->e[i].size;
        uint32_t isdir = (uint32_t)o->e[i].isdir;
        uint32_t nl = (uint32_t)strlen(o->e[i].rel);
        memcpy(b + off, &size, 8); memcpy(b + off + 8, &isdir, 4); memcpy(b + off + 12, &nl, 4);
        memcpy(b + off + 16, o->e[i].rel, nl);
        off += 16 + nl;
    }
    pthread_mutex_lock(&g_offer_lock);
    offer_free(&g_offer);                          /* drop the previous pullable list */
    g_offer.e = o->e; g_offer.n = o->n; g_offer.cap = o->cap;
    o->e = NULL; o->n = 0; o->cap = 0;
    g_offer.gen = ++g_gen;
    o->gen = g_offer.gen;
    pthread_mutex_unlock(&g_offer_lock);
    send_var(M_CLIP_FILES, (int)g_offer.gen, 0, b, (int)total);
    free(b);
    logmsg("clipboard: offered %u item(s) to peer", cnt);
}

/* files on the pasteboard -> M_CLIP_FILES offer (win32 peer pulls on paste,
 * mac peer pulls eagerly when applying the offer) */
static int pb_harvest_files(void) {                /* 1 = offer sent, 0 = no file urls */
    ItemCount n = 0;
    Offer o; memset(&o, 0, sizeof o);
    if (PasteboardGetItemCount(g_pb, &n) != noErr) return 0;
    for (ItemCount i = 1; i <= n && o.n < MAX_OFFER; i++) {
        PasteboardItemID id = 0;
        if (PasteboardGetItemIdentifier(g_pb, i, &id) != noErr) continue;
        CFArrayRef types = NULL;
        if (PasteboardCopyItemFlavorTypeList(g_pb, id, &types) != noErr || !types) continue;
        for (CFIndex t = 0; t < CFArrayGetCount(types); t++) {
            CFStringRef ty = (CFStringRef)CFArrayGetValueAtIndex(types, t);
            if (CFStringCompare(ty, kFileUrlFlavor, 0) != kCFCompareEqualTo) continue;
            CFDataRef d = NULL;
            if (PasteboardCopyItemFlavorData(g_pb, id, ty, &d) != noErr || !d) continue;
            CFIndex bl = CFDataGetLength(d);
            char *buf = (char *)malloc((size_t)bl + 1);
            if (!buf) { CFRelease(d); continue; }
            CFDataGetBytes(d, CFRangeMake(0, bl), (UInt8 *)buf);
            buf[bl] = 0;
            CFStringRef cs = CFStringCreateWithBytes(NULL, (const UInt8 *)buf, bl, kCFStringEncodingUTF8, false);
            free(buf);
            CFRelease(d);
            if (!cs) continue;
            CFURLRef url = CFURLCreateWithString(NULL, cs, NULL);
            CFRelease(cs);
            if (!url) continue;
            CFStringRef pathcs = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
            if (pathcs) {
                char path[MAXPATHLEN], rel[1025];
                if (CFStringGetFileSystemRepresentation(pathcs, path, sizeof path)) {
                    const char *base = strrchr(path, '/');
                    snprintf(rel, sizeof rel, "%s", base ? base + 1 : path);
                    sanitize_component(rel);
                    offer_walk(&o, path, rel, 0);  /* rels may collide when two pasted
                                                    * items share a basename */
                }
                CFRelease(pathcs);
            }
            CFRelease(url);
        }
        CFRelease(types);
    }
    if (!o.n) { offer_free(&o); return 0; }
    offer_publish(&o);                             /* takes ownership; frees on failure */
    return 1;
}

/* plain text on the pasteboard -> M_CLIP_TEXT */
static void pb_send_text(void) {
    ItemCount n = 0;
    if (PasteboardGetItemCount(g_pb, &n) != noErr) return;
    for (ItemCount i = 1; i <= n; i++) {
        PasteboardItemID id = 0;
        if (PasteboardGetItemIdentifier(g_pb, i, &id) != noErr) continue;
        CFDataRef d = NULL;
        if (PasteboardCopyItemFlavorData(g_pb, id, kUf8Flavor, &d) != noErr || !d) continue;
        CFIndex bl = CFDataGetLength(d);
        if (bl > 0 && bl <= MAX_TEXT) {
            char *txt = (char *)malloc((size_t)bl + 1);
            if (txt) {
                CFDataGetBytes(d, CFRangeMake(0, bl), (UInt8 *)txt);
                txt[bl] = 0;
                offer_text_out(txt, (int)bl);
                free(txt);
                CFRelease(d);
                return;
            }
        }
        CFRelease(d);
    }
}

static void *ClipWatch(void *unused) {
    (void)unused;
    for (;;) {
        usleep(300000);
        if (!g_pb || (!g_clip_sync && !g_sync_files) || g_sock == INVALID_SOCKET || !g_peer_ip) continue;
        PasteboardSyncFlags flags = PasteboardSynchronize(g_pb);
        if (!(flags & kPasteboardModified)) continue;
        if (ms_now() - g_self_change_ms < 600) continue;      /* our own change, skip */
        if (g_sync_files && pb_harvest_files()) continue;     /* files win over text */
        if (g_clip_sync) pb_send_text();
    }
    return NULL;
}

/* ---------- CLIPBOARD: apply remote offers ---------- */

static void pb_put_file_url(const char *path, PasteboardItemID id) {
    CFStringRef csp = CFStringCreateWithFileSystemRepresentation(NULL, path);
    if (!csp) return;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, csp, kCFURLPOSIXPathStyle, false);
    CFRelease(csp);
    if (!url) return;
    CFStringRef us = CFURLGetString(url);
    CFIndex bl = CFStringGetMaximumSizeForEncoding(CFStringGetLength(us), kCFStringEncodingUTF8) + 1;
    char *u8 = (char *)malloc((size_t)bl);
    if (u8 && CFStringGetCString(us, u8, bl, kCFStringEncodingUTF8)) {
        CFDataRef d = CFDataCreate(NULL, (const UInt8 *)u8, (CFIndex)strlen(u8));
        if (d) { PasteboardPutItemFlavor(g_pb, id, kFileUrlFlavor, d, 0); CFRelease(d); }
    }
    free(u8);
    CFRelease(url);
}

typedef struct { uint64_t size; int isdir; char rel[1025]; } REnt;

/* remote apply: parse offer, materialise files under ~/mwb-received, push to pasteboard */
static void apply_offer(uint8_t *payload, int len, int drag, uint32_t gen) {
    (void)drag;    /* remote drag-and-drop is a Windows-only feature; ignored on macOS */
    if (!payload) return;
    uint32_t n; memcpy(&n, payload, 4);
    if (n == 0 || n > MAX_OFFER) return;
    REnt *ents = (REnt *)calloc(n, sizeof(REnt));
    if (!ents) return;
    int off = 4, ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        if (len - off < 16) { ok = 0; break; }
        uint64_t size; uint32_t isdir, nl;
        memcpy(&size, payload + off, 8); memcpy(&isdir, payload + off + 8, 4); memcpy(&nl, payload + off + 12, 4);
        off += 16;
        if (nl == 0 || nl > 1024 || off + (int)nl > len) { ok = 0; break; }
        ents[i].size = size;
        ents[i].isdir = isdir ? 1 : 0;
        memcpy(ents[i].rel, payload + off, nl);
        ents[i].rel[nl] = 0;
        off += (int)nl;
    }
    free(payload);
    if (!ok) { free(ents); return; }

    mkdir(g_received, 0755);
    PasteboardClear(g_pb);
    PasteboardSyncFlags sf = PasteboardSynchronize(g_pb);
    (void)sf;
    PasteboardItemID item = 1;
    int file_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        char rel[1026];
        /* strip leading path separators so we never escape the received folder */
        const char *rl = ents[i].rel;
        while (*rl == '/') rl++;
        snprintf(rel, sizeof rel, "%s", rl);
        /* sanitize component-wise by rebuilding through the same helper used on pull */
        char dst[MAXPATHLEN];
        if (!build_received_path(rel, dst, sizeof dst)) continue;
        if (ents[i].isdir) {
            if (mkdir(dst, 0755) == 0 || errno == EEXIST) pb_put_file_url(dst, item++);
        } else {
            /* eager pull: identical wire handshake to the win32 client's lazy stream */
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a; memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_port = htons(FILE_PORT); a.sin_addr.s_addr = g_peer_ip;
            uint64_t rsize = 0;
            int gotit = 0;
            if (s != INVALID_SOCKET) {
                tune_sock(s, 30000, 30000);
                if (g_have_key && connect(s, (struct sockaddr *)&a, sizeof a) == 0 && file_auth_initiator(s)) {
                FileHdr h; h.magic = FILE_MAGIC_PULL; h.namelen = gen; h.size = i;
                if (send_all(s, &h, (int)sizeof h) && recv_all(s, &rsize, 8) && rsize == ents[i].size) {
                    /* stream to disk then place on the pasteboard */
                    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    gotit = 1;
                    if (fd >= 0) {
                        char *buf = (char *)malloc(CHUNK);
                        uint64_t left = rsize;
                        if (!buf) gotit = 0;
                        while (gotit && left > 0) {
                            size_t want = left > CHUNK ? CHUNK : left;
                            ssize_t got = recv(s, buf, want, MSG_NOSIGNAL);
                            if (got <= 0) { gotit = 0; break; }
                            if (write(fd, buf, (size_t)got) != got) { gotit = 0; break; }
                            left -= (uint64_t)got;
                        }
                        free(buf);
                        close(fd);
                        if (gotit && left == 0) pb_put_file_url(dst, item++);
                        else unlink(dst);
                    } else gotit = 0;
                } else logmsg("file: pull refused (stale or missing)");
                }
                close(s);
            }
            if (gotit) file_count++;
            else logmsg("file: could not pull entry %u", i);
        }
    }
    free(ents);
    if (file_count) logmsg("clipboard: peer offered %u file(s) (%d pulled to %s)", n, file_count, g_received);
    g_self_change_ms = ms_now();
}

/* ---------- CLIPBOARD: apply remote text ---------- */

static void pb_apply_text(const char *utf8, int len) {
    if (!g_pb || len <= 0) return;
    CFDataRef d = CFDataCreate(NULL, (const UInt8 *)utf8, len);
    if (!d) return;
    PasteboardClear(g_pb);
    PasteboardPutItemFlavor(g_pb, (PasteboardItemID)1, kUf8Flavor, d, 0);
    CFRelease(d);
    PasteboardSynchronize(g_pb);
    g_self_change_ms = ms_now();           /* do not echo our own change back */
    logmsg("clipboard: applied %d bytes of text from peer", len);
}

static void handle_clip(Msg *m, uint8_t *pl) {
    if (m->type == M_CLIP_TEXT) {
        if (!g_clip_sync) { free(pl); return; }             /* clipboard channel off */
        if (m->c > 0 && m->c <= MAX_TEXT && pl) pb_apply_text((const char *)pl, m->c);
        free(pl);
    } else {
        if (!g_sync_files) { free(pl); return; }            /* files channel off */
        apply_offer(pl, m->c, m->b, (uint32_t)m->a);        /* apply_offer frees pl */
    }
}

/* ---------- SERVER: network thread ---------- */

static void *ServerNet(void *arg) {
    (void)arg;
    int on = 1;

    SOCKET us = socket(AF_INET, SOCK_DGRAM, 0);          /* discovery beacon */
    setsockopt(us, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in bc; memset(&bc, 0, sizeof bc);
    bc.sin_family = AF_INET;
    bc.sin_port = htons(UDP_PORT);
    bc.sin_addr.s_addr = INADDR_BROADCAST;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(TCP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        logmsg("server: bind/listen %d failed: %s", TCP_PORT, strerror(errno));
        return NULL;
    }
    logmsg("server: waiting for a peer (beacon on UDP %d)", UDP_PORT);
    logmsg("server: tunnel on our %s edge",
           g_side == MWB_SIDE_RIGHT ? "right" : g_side == MWB_SIDE_LEFT ? "left" :
           g_side == MWB_SIDE_UP ? "top" : "bottom");

    for (;;) {
        sendto(us, "MWB1", 4, 0, (struct sockaddr *)&bc, sizeof bc);
        fd_set fs; FD_ZERO(&fs); FD_SET(ls, &fs);
        struct timeval tv = { 2, 0 };
        if (select(ls + 1, &fs, NULL, NULL, &tv) <= 0) continue;

        struct sockaddr_in pa; socklen_t pl = sizeof pa;
        SOCKET c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c == INVALID_SOCKET) continue;
        int on2 = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on2, sizeof on2);
        tune_sock(c, 10000, 5000);                       /* handshake must finish within 10 s */

        int r = auth_server(c);
        if (r != 1) {
            struct in_addr ia; ia.s_addr = pa.sin_addr.s_addr;
            char ipw[64]; inet_ntop(AF_INET, &ia, ipw, sizeof ipw);
            if (r == 0) {
                g_fail++;
                logmsg("pairing failed from %s (%d/3)", ipw, g_fail);
                sleep(1);
                if (g_fail >= 3) {
                    new_otp();
                    g_fail = 0;
                    logmsg("pairing code regenerated after repeated failures: %s", g_otp);
                }
            } else logmsg("handshake error from %s", ipw);
            close(c);
            continue;
        }
        g_fail = 0;
        tune_sock(c, 0, 30000);                          /* session: long send budget */
        keep_alive(c);                                   /* notice a vanished peer fast */
        g_sock = c;
        set_peer(pa.sin_addr.s_addr);
        send_msg(M_HELLO, MWB_OS_MAC, g_sw, g_sh);       /* announce our platform */

        Msg m; uint8_t *pl2;
        while (read_msg(c, &m, &pl2)) {
            if (m.type == M_CLIP_TEXT || m.type == M_CLIP_FILES) { handle_clip(&m, pl2); continue; }
            free(pl2);
            if (m.type == M_HELLO) {
                g_peer_os = m.a;
                logmsg("peer OS: %s", m.a == MWB_OS_MAC ? "macOS" : "win32");
                continue;
            }
            if (m.type == M_LEAVE) {                     /* client crossed back to us */
                if (g_remote) {
                    int f = m.a;
                    if (f < 0) f = 0; else if (f > 10000) f = 10000;
                    capture_off(f);
                }
            }
        }
        g_sock = INVALID_SOCKET;
        g_remote = 0;
        close(c);
        clear_peer();
    }
    return NULL;
}

/* ---------- CLIENT: input loop ---------- */

static void client_loop(SOCKET s) {
    Msg m; uint8_t *pl;
    int enter_code = 0;                    /* our tunnel edge: 0=left 1=right 2=top 3=bottom */
    while (read_msg(s, &m, &pl)) {
        if (m.type == M_CLIP_TEXT || m.type == M_CLIP_FILES) { handle_clip(&m, pl); continue; }
        free(pl);
        switch (m.type) {
        case M_HELLO:
            g_peer_os = m.a;
            logmsg("peer OS: %s", m.a == MWB_OS_MAC ? "macOS" : "win32");
            break;
        case M_ENTER: {                                /* remote cursor enters via the tunnel */
            enter_code = (m.c >> 1) & 3;
            switch (enter_code) {
            case 1: g_px = g_sw - 1; g_py = m.a * (g_sh - 1) / 10000; break;
            case 2: g_px = m.a * (g_sw - 1) / 10000; g_py = 0; break;
            case 3: g_px = m.a * (g_sw - 1) / 10000; g_py = g_sh - 1; break;
            default: g_px = 0; g_py = m.a * (g_sh - 1) / 10000; break;
            }
            g_remote = 1; g_drag_expect = (m.c & 1) ? 1 : 0; g_drag_cancel = 0;
            if (g_sync_mouse) inj_abs(g_px, g_py);
            break;
        }
        case M_LEAVE:
            g_remote = 0; g_drag_expect = 0; g_drag_cancel = 1;
            inj_release_all();
            break;
        case M_MOVE: {
            if (!g_remote) break;
            int nx = g_px + m.a, ny = g_py + m.b;
            int out = 0, pos = 0;                      /* leaving back through the tunnel */
            switch (enter_code) {
            case 1: if (nx > g_sw - 1) { out = 1; pos = g_py * 10000 / (g_sh - 1); } break;
            case 2: if (ny < 0)        { out = 1; pos = g_px * 10000 / (g_sw - 1); } break;
            case 3: if (ny > g_sh - 1) { out = 1; pos = g_px * 10000 / (g_sw - 1); } break;
            default: if (nx < 0)       { out = 1; pos = g_py * 10000 / (g_sh - 1); } break;
            }
            if (out) {
                g_remote = 0; g_drag_expect = 0; g_drag_cancel = 1;
                inj_release_all();
                send_msg(M_LEAVE, pos, 0, 0);
                break;
            }
            g_px = nx; g_py = ny;
            if (g_px < 0) g_px = 0;
            if (g_py < 0) g_py = 0;
            if (g_px > g_sw - 1) g_px = g_sw - 1;
            if (g_py > g_sh - 1) g_py = g_sh - 1;
            if (g_sync_mouse) inj_abs(g_px, g_py);
            break;
        }
        case M_BUTTON:
            if (!g_remote) break;
            if (m.a == 0 && !m.b) {                    /* left button released */
                if (g_drag_expect && !g_held[0]) {     /* drag offered but never started */
                    g_drag_expect = 0;
                    break;
                }
                g_drag_expect = 0;
            }
            if (g_sync_mouse) inj_button(m.a, m.b);
            break;
        case M_WHEEL: if (g_remote && g_sync_mouse)    inj_wheel(m.a, m.b); break;
        case M_KEY:   if (g_remote && g_sync_keyboard) inj_key(m.a, !(m.c & 1)); break;
        default: break;
        }
    }
    g_remote = 0;
    g_drag_expect = 0;
    inj_release_all();
}

static int discover(uint32_t *out_ip) {                /* 1 = server beacon seen */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(UDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        logmsg("client: udp bind failed: %s", strerror(errno));
        close(s);
        return 0;
    }
    for (;;) {
        char buf[8];
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int got = (int)recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (got == 4 && !memcmp(buf, "MWB1", 4)) {
            *out_ip = from.sin_addr.s_addr;
            close(s);
            return 1;
        }
    }
}

static void *ClientNet(void *arg) {
    const char *ip = (const char *)arg;
    char otp[8];
    for (;;) {
        if (!g_otp[0]) {
            if (prompt_otp(otp)) snprintf(g_otp, sizeof g_otp, "%s", otp);
            else { logmsg("client: no code entered, retrying in 10 s"); sleep(10); continue; }
        }
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(TCP_PORT);
        if (ip) {
            if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
                logmsg("client: bad server ip: %s", ip);
                return NULL;
            }
        } else {
            logmsg("client: listening for server beacon");
            if (!discover(&a.sin_addr.s_addr)) { sleep(2); continue; }
        }
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) { sleep(1); continue; }
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
            tune_sock(s, 10000, 5000);
            int r = auth_client(s, g_otp);
            if (r == 1) {
                tune_sock(s, 0, 30000);
                keep_alive(s);
                g_sock = s;
                set_peer(a.sin_addr.s_addr);
                send_msg(M_HELLO, MWB_OS_MAC, g_sw, g_sh);
                client_loop(s);
                g_sock = INVALID_SOCKET;
                clear_peer();
            } else if (r == 0) {
                logmsg("client: pairing rejected: wrong code");
                g_otp[0] = 0;
            } else logmsg("client: handshake error");
        } else {
            logmsg("client: connect failed: %s", strerror(errno));
        }
        close(s);
        sleep(1);
    }
    return NULL;
}

/* ---------- app plumbing ---------- */

static void install_tap(void) {
    CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved)
                     | CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDragged)
                     | CGEventMaskBit(kCGEventOtherMouseDragged)
                     | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftUp)
                     | CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightUp)
                     | CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp)
                     | CGEventMaskBit(kCGEventScrollWheel)
                     | CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
                     | CGEventMaskBit(kCGEventFlagsChanged);
    g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                             mask, tap_cb, NULL);
    if (!g_tap) {
        fprintf(stderr, "mwb-mac: cannot create the event tap.\n"
                "System Settings > Privacy & Security > Accessibility: add this binary, then retry.\n");
        extern void CGRequestListenEventAccess(void) __attribute__((weak_import));
        if (&CGRequestListenEventAccess) CGRequestListenEventAccess();   /* open the prompt */
        exit(1);
    }
    CGEventTapEnable(g_tap, true);
    CFRunLoopSourceRef src = CGEventTapGetRunLoopSource(g_tap);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopDefaultMode);
}

static void check_post_permission(void) {
    extern bool CGPreflightPostEventAccess(void) __attribute__((weak_import));
    if (&CGPreflightPostEventAccess && !CGPreflightPostEventAccess()) {
        fprintf(stderr, "mwb-mac: without the Accessibility permission injection will not work.\n"
                "System Settings > Privacy & Security > Accessibility: add this binary.\n");
        logmsg("accessibility permission missing: injected input will be ignored");
    }
}

static void single_instance(void) {                    /* lock file lock, released on exit */
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.mwb-mac.lock", home ? home : "/tmp");
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "mwb-mac: already running\n");
        exit(1);
    }
    /* fd deliberately stays open for the process lifetime to hold the lock */
}

/* optional read-only config file: default mwb.txt beside this binary; --config
 * PATH overrides. Parser shared with the Windows peer via common/tiny_config.h;
 * this side only maps the known keys. The tool never writes to the file; CLI
 * arguments always win; no OTP key (the code rotates). */
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

static void help(void) {
    fprintf(stderr, "usage: mwb-mac server | mwb-mac client [server-ip]\n"
                    "       [--side right|left|up|down] [--otp CODE] [--log FILE]\n"
                    "       [--config FILE]\n");
    exit(2);
}

static void on_signal(int sig) {
    (void)sig;
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SHUT_RDWR);
    if (g_logf) fflush(g_logf);
    exit(0);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const char *ip = NULL, *logpath = NULL, *otp = NULL, *cfgp = NULL;
    int mode_seen = 0, side_given = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "server")) { g_is_server = 1; mode_seen = 1; }
        else if (!strcmp(argv[i], "client")) { g_is_server = 0; mode_seen = 1; }
        else if (!strcmp(argv[i], "--otp") && i + 1 < argc)  otp = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)  logpath = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgp = argv[++i];
        else if (!strcmp(argv[i], "--side") && i + 1 < argc) {
            const char *s = argv[++i];
            if      (!strcmp(s, "right")) g_side = MWB_SIDE_RIGHT;
            else if (!strcmp(s, "left"))  g_side = MWB_SIDE_LEFT;
            else if (!strcmp(s, "up"))    g_side = MWB_SIDE_UP;
            else if (!strcmp(s, "down"))  g_side = MWB_SIDE_DOWN;
            else { fprintf(stderr, "mwb-mac: bad --side '%s' (use right|left|up|down)\n", s); return 2; }
            side_given = 1;
        }
        else if (argv[i][0] != '-') {
            if (g_is_server) help();
            else ip = argv[i];
        }
        else help();
    }

    single_instance();
    if (logpath) g_logf = fopen(logpath, "a");
    if (!g_logf) open_default_log();
    if (isatty(STDOUT_FILENO)) g_stdout_log = 1;

    /* optional read-only config: default mwb.txt beside this binary; CLI wins */
    {
        char cfg_default[PATH_MAX];
        if (!cfgp) {
            char exe[PATH_MAX]; uint32_t es = (uint32_t)sizeof exe;
            if (_NSGetExecutablePath(exe, &es) == 0) {
                char *slash = strrchr(exe, '/');
                snprintf(cfg_default, sizeof cfg_default, "%.*s/mwb.txt",
                         (int)(slash ? slash - exe : 1), exe);
            } else cfg_default[0] = 0;
        }
        load_config(cfgp ? cfgp : cfg_default);
        if (!side_given && cfg_side >= 0) g_side = cfg_side;
        if (cfg_clip >= 0) g_clip_sync = cfg_clip;
        if (cfg_mouse >= 0)    g_sync_mouse = cfg_mouse;
        if (cfg_keyboard >= 0) g_sync_keyboard = cfg_keyboard;
        if (cfg_files >= 0)    g_sync_files = cfg_files;
        logmsg("channels: mouse=%s keyboard=%s clipboard=%s files=%s "
               "(a channel is live only if the peer enables it too)",
               g_sync_mouse ? "on" : "off", g_sync_keyboard ? "on" : "off",
               g_clip_sync ? "on" : "off", g_sync_files ? "on" : "off");
    }
    if (!mode_seen && cfg_server >= 0) g_is_server = cfg_server;   /* config decided the role */
    else if (!mode_seen) {
        fprintf(stderr, "mwb-mac: need 'server' or 'client' on the command line (or a config 'mode' key)\n");
        help();
    }

    kUf8Flavor     = CFSTR("public.utf8-plain-text");
    kFileUrlFlavor = CFSTR("public.file-url");
    init_screen();
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *home = getenv("HOME");
    snprintf(g_received, sizeof g_received, "%s/mwb-received", home ? home : "/tmp");
    mkdir(g_received, 0755);

    if (PasteboardCreate(kPasteboardClipboard, &g_pb) != noErr || !g_pb) {
        fprintf(stderr, "mwb-mac: system clipboard unavailable\n");
        return 1;
    }
    g_inj_src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

    if (g_is_server) {
        if (otp) snprintf(g_otp, sizeof g_otp, "%s", otp);
        else new_otp();
        fprintf(stderr, "mwb-mac: pairing code: %s\n", g_otp);
        logmsg("pairing code: %s", g_otp);
        logmsg("server mode (tunnel on our %s edge)",
               g_side == MWB_SIDE_RIGHT ? "right" : g_side == MWB_SIDE_LEFT ? "left" :
               g_side == MWB_SIDE_UP ? "top" : "bottom");
        spawn(FileListen, NULL);
        spawn(ServerNet, NULL);
        spawn(ClipWatch, NULL);
        if (g_sync_mouse || g_sync_keyboard) install_tap();
        else logmsg("input tap skipped (mouse + keyboard disabled by config)");
        CFRunLoopRun();                    /* serves the event tap forever */
        return 0;
    }

    if (otp) snprintf(g_otp, sizeof g_otp, "%s", otp);
    check_post_permission();
    spawn(FileListen, NULL);
    spawn(ClipWatch, NULL);                /* mac clients share their clipboard too */
    spawn(ClientNet, (void *)ip);
    for (;;) sleep(3600);                  /* client ticks entirely on its own threads */
    return 0;
}
