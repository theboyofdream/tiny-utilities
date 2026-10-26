#ifndef TINY_HTTP_H
#define TINY_HTTP_H

/*
 * TinyHTTP — zero-heap blocking HTTP/1.x server helpers + network utilities.
 * Shared by long-running console tools (kyper, whisper-cast).
 *
 * Design:
 *   - Win32 / Winsock only, fixed stack/static buffers, no heap, no threads here.
 *   - Tools own their accept loops; this header provides listening, event wiring,
 *     single-request parsing, static-form responses, URL form decoding,
 *     UTF <-> UTF-8 conversion, LAN IPv4 discovery and Base64.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define TINYHTTP_TIMEOUT_MS   5000
#define TINYHTTP_MAX_HEADERS  8192
#define TINYHTTP_MAX_BODY     8192

typedef struct {
    char   method[8];
    char   target[256];
    char   auth[256]; /* raw Authorization header value ("" when absent) */
    size_t bodyLen;
    char   body[TINYHTTP_MAX_BODY + 1];
} TinyHTTPRequest;

/* ------------------------------------------------------------------ */
/* Startup / sockets                                                    */
/* ------------------------------------------------------------------ */

static inline bool TinyHTTP_Startup(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static inline void TinyHTTP_Cleanup(void) {
    WSACleanup();
}

/* Blocking TCP listener bound to 0.0.0.0:<port>. Returns INVALID_SOCKET on failure. */
static inline SOCKET TinyHTTP_Listen(int port) {
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) return INVALID_SOCKET;

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(server, 16) == SOCKET_ERROR) {
        closesocket(server);
        return INVALID_SOCKET;
    }
    return server;
}

/*
 * Pattern: WSAEventSelect(FD_ACCEPT) -> WaitForMultipleObjects([toggle, wake, accept]).
 * The WSA event is NOT auto-reset by plain WaitForMultipleObjects, so callers must
 * WSAResetEvent() after draining accepts.
 */
static inline WSAEVENT TinyHTTP_WireAcceptEvent(SOCKET server) {
    WSAEVENT ev = WSACreateEvent();
    if (!ev) return NULL;
    if (WSAEventSelect(server, ev, FD_ACCEPT) == SOCKET_ERROR) {
        WSACloseEvent(ev);
        return NULL;
    }
    return ev;
}

static inline void TinyHTTP_SetSocketTimeouts(SOCKET s) {
    DWORD ms = TINYHTTP_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
}

/* Accept one client with request timeouts applied. INVALID_SOCKET when none pending. */
static inline SOCKET TinyHTTP_Accept(SOCKET server) {
    SOCKET client = accept(server, NULL, NULL);
    if (client != INVALID_SOCKET) {
        /* An accepted socket inherits the listener's WSAEventSelect registration.
         * Deregister it first, otherwise the socket stays non-blocking and
         * recv() fails with WSAEWOULDBLOCK before request bytes land. */
        WSAEventSelect(client, NULL, 0);
        u_long blockMode = 0; /* 0 = blocking */
        ioctlsocket(client, FIONBIO, &blockMode);
        TinyHTTP_SetSocketTimeouts(client);
    }
    return client;
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                      */
/* ------------------------------------------------------------------ */

/* Case-insensitive scan for "<name>: " among header lines (request line is skipped once).
 * Returns pointer to the value start or NULL. */
static inline const char *TinyHTTP_FindHeader(const char *buf, size_t len, const char *name) {
    size_t nameLen = strlen(name);
    const char *end = buf + len;

    /* Skip the request line. */
    const char *p = buf;
    while (p < end && *p != '\n') p++;
    if (p >= end) return NULL;
    p++;

    while (p < end) {
        const char *colon = p;
        while (colon < end && *colon != ':' && *colon != '\r' && *colon != '\n') colon++;
        if (colon >= end || *colon != ':') break;
        if ((size_t)(colon - p) == nameLen && _strnicmp(p, name, nameLen) == 0) {
            const char *v = colon + 1;
            while (v < end && *v == ' ') v++;
            return v;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return NULL;
}

static inline size_t TinyHTTP_ParseContentLength(const char *start, const char *end) {
    const char *v = TinyHTTP_FindHeader(start, (size_t)(end - start), "Content-Length");
    if (!v) return 0;
    return (size_t)strtoul(v, NULL, 10);
}

/* Write all bytes to a socket. Returns false on error/remote close. */
static inline bool TinyHTTP_SendAll(SOCKET s, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/*
 * Read one request (headers + body). Returns 0 on success, <0 on malformed/oversized/disconnect.
 * Only the first request per connection is supported (Connection: close).
 */
static inline int TinyHTTP_LoadRequest(SOCKET sock, TinyHTTPRequest *req) {
    ZeroMemory(req, sizeof(*req));
    char tmp[TINYHTTP_MAX_HEADERS];
    size_t filled = 0;
    size_t headerEnd = 0;
    size_t contentLen = 0;

    while (1) {
        int n = recv(sock, tmp + filled, (int)(sizeof(tmp) - 1 - filled), 0);
        if (n <= 0) return -1;
        filled += (size_t)n;
        tmp[filled] = '\0';

        char *sep = strstr(tmp, "\r\n\r\n");
        if (sep) {
            headerEnd = (size_t)(sep - tmp) + 4;
            contentLen = TinyHTTP_ParseContentLength(tmp, sep);
            break;
        }
        if (filled >= sizeof(tmp) - 1) return -1; /* headers too large */
    }

    /* Request line: METHOD SP TARGET SP VERSION */
    const char *lineEnd = strchr(tmp, '\n');
    if (!lineEnd || lineEnd >= tmp + headerEnd - 4) return -1;
    {
        char line[512];
        size_t lineLen = (size_t)(lineEnd - tmp);
        if (lineLen >= sizeof(line)) return -1;
        memcpy(line, tmp, lineLen);
        line[lineLen] = '\0';

        char *sp1 = strchr(line, ' ');
        if (!sp1) return -1;
        size_t mLen = (size_t)(sp1 - line);
        if (mLen == 0 || mLen >= sizeof(req->method)) return -1;
        memcpy(req->method, line, mLen);
        req->method[mLen] = '\0';

        char *sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) sp2 = line + lineLen; /* version may be missing */
        size_t tLen = (size_t)(sp2 - (sp1 + 1));
        if (tLen == 0) return -1;
        if (tLen >= sizeof(req->target)) tLen = sizeof(req->target) - 1;
        memcpy(req->target, sp1 + 1, tLen);
        req->target[tLen] = '\0';
    }

    if (contentLen == 0) {
        req->bodyLen = 0;
        req->body[0] = '\0';
    }
    else {
        if (contentLen > TINYHTTP_MAX_BODY) return -1;

        /* Clients that send "Expect: 100-continue" hold the body until the server
         * acknowledges; ack once, then read the body normally. */
        size_t have = filled - headerEnd;
        if (have < contentLen) {
            const char *expect = TinyHTTP_FindHeader(tmp, headerEnd, "Expect");
            if (expect && _strnicmp(expect, "100-continue", 12) == 0) {
                TinyHTTP_SendAll(sock, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            }
        }

        if (have > contentLen) have = contentLen;
        memcpy(req->body, tmp + headerEnd, have);
        req->bodyLen = have;

        while (req->bodyLen < contentLen) {
            int n = recv(sock, req->body + req->bodyLen, (int)(contentLen - req->bodyLen), 0);
            if (n <= 0) return -1;
            req->bodyLen += (size_t)n;
        }
        req->body[req->bodyLen] = '\0';
    }

    /* Capture Authorization header value (trimmed). */
    {
        const char *authV = TinyHTTP_FindHeader(tmp, headerEnd, "Authorization");
        req->auth[0] = '\0';
        if (authV) {
            size_t i = 0;
            while (authV[i] && authV[i] != '\r' && authV[i] != '\n' &&
                   i < sizeof(req->auth) - 1) {
                req->auth[i] = authV[i];
                i++;
            }
            while (i > 0 && req->auth[i - 1] == ' ') i--;
            req->auth[i] = '\0';
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Responses                                                            */
/* ------------------------------------------------------------------ */

static inline bool TinyHTTP_ReplyUtf8(SOCKET s, int code, const char *reason,
                                      const char *contentType, const char *body) {
    char header[512];
    int h = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, contentType, strlen(body));
    if (h <= 0 || h >= (int)sizeof(header)) return false;
    return TinyHTTP_SendAll(s, header, (size_t)h) && TinyHTTP_SendAll(s, body, strlen(body));
}

static inline void TinyHTTP_ReplyUnauthorized(SOCKET s, const char *realm) {
    char header[512];
    int h = snprintf(header, sizeof(header),
                     "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"%s\"\r\n"
                     "Content-Length: 12\r\nConnection: close\r\n\r\nUnauthorized",
                     realm);
    if (h > 0 && h < (int)sizeof(header)) TinyHTTP_SendAll(s, header, (size_t)h);
}

static inline void TinyHTTP_ReplyNoContent(SOCKET s, const char *reason) {
    char header[256];
    int h = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     strlen(reason), reason);
    if (h > 0 && h < (int)sizeof(header)) TinyHTTP_SendAll(s, header, (size_t)h);
}

static inline void TinyHTTP_ReplyNotFound(SOCKET s) {
    TinyHTTP_ReplyUtf8(s, 404, "Not Found", "text/plain", "Not Found");
}

/* ------------------------------------------------------------------ */
/* Encoding helpers                                                     */
/* ------------------------------------------------------------------ */

static inline int TinyHTTP_WideToUtf8(const wchar_t *w, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return (n > 0) ? n - 1 : -1; /* exclude NUL */
}

static inline int TinyHTTP_Utf8ToWide(const char *u, size_t len, wchar_t *out, size_t cap) {
    if (!out || cap == 0) return -1;
    int n = MultiByteToWideChar(CP_UTF8, 0, u, (int)len, out, (int)cap);
    if (n > 0) out[n < (int)cap ? n : (int)cap - 1] = L'\0';
    return n;
}

static inline int TinyHTTP_HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* application/x-www-form-urlencoded byte decode (+ -> space, %XX -> byte). UTF-8 bytes. */
static inline int TinyHTTP_UrlDecode(const char *src, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '+') {
            if (o + 1 >= cap) return -1;
            out[o++] = ' ';
        } else if (c == '%') {
            if (i + 2 >= len) return -1;
            int hi = TinyHTTP_HexNibble(src[++i]);
            int lo = TinyHTTP_HexNibble(src[++i]);
            if (hi < 0 || lo < 0) return -1;
            char b = (char)((hi << 4) | lo);
            if (b == 0) return -1;
            if (o + 1 >= cap) return -1;
            out[o++] = b;
        } else {
            if (c == 0) return -1;
            if (o + 1 >= cap) return -1;
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* Extract key=value from a x-www-form-urlencoded body; value decoded to wide (UTF-8 aware).
 * Returns length of decoded wide string (>=0) or -1 if key missing / malformed. */
static inline int TinyHTTP_FormFieldWide(const char *body, const char *key,
                                         wchar_t *out, size_t cap) {
    if (!body) return -1;
    size_t keyLen = strlen(key);
    const char *p = body;
    while (p) {
        /* field = [p, sep) */
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq || (amp && eq > amp)) { p = amp ? amp + 1 : NULL; continue; }
        size_t fLen = (size_t)(eq - p);
        if (fLen == keyLen && strncmp(p, key, keyLen) == 0) {
            const char *v = eq + 1;
            size_t vLen = amp ? (size_t)(amp - v) : strlen(v);
            char decoded[2048];
            int dn = TinyHTTP_UrlDecode(v, vLen, decoded, sizeof(decoded));
            if (dn < 0) return -1;
            int wn = MultiByteToWideChar(CP_UTF8, 0, decoded, dn, out, (int)cap);
            if (wn < 0) return -1;
            out[wn < (int)cap ? wn : (int)cap - 1] = L'\0';
            return wn;
        }
        p = amp ? amp + 1 : NULL;
    }
    return -1;
}

/* HTTP-safe base64 (RFC 4648). outLen includes NUL. Small inputs only. */
static inline bool TinyHTTP_Base64Encode(const char *in, size_t len, char *out, size_t cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4 + 1;
    if (cap < need) return false;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned b0 = (unsigned char)in[i];
        unsigned b1 = (i + 1 < len) ? (unsigned char)in[i + 1] : 0;
        unsigned b2 = (i + 2 < len) ? (unsigned char)in[i + 2] : 0;
        unsigned trip = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = tbl[(trip >> 18) & 63];
        out[o++] = tbl[(trip >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(trip >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[trip & 63] : '=';
    }
    out[o] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Network discovery                                                    */
/* ------------------------------------------------------------------ */

/* First non-loopback, non-APIPA, up IPv4 address. Returns false if none. */
static inline bool TinyHTTP_GetFirstLanSockaddr(struct sockaddr_in *out) {
    static char buf[16 * 1024];
    ULONG sz = sizeof(buf);
    PIP_ADAPTER_ADDRESSES aa = (PIP_ADAPTER_ADDRESSES)buf;
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, aa, &sz) != NO_ERROR) {
        return false;
    }
    for (PIP_ADAPTER_ADDRESSES a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            unsigned long ip = ntohl(sin->sin_addr.S_un.S_addr);
            if ((ip >> 16) == 0xA9FE) continue; /* 169.254.0.0/16 APIPA */
            ZeroMemory(out, sizeof(*out));
            out->sin_family = AF_INET;
            out->sin_port = 0;
            out->sin_addr = sin->sin_addr;
            return true;
        }
    }
    return false;
}

static inline bool TinyHTTP_GetFirstLanIPv4(wchar_t *out, size_t cap) {
    struct sockaddr_in sa;
    if (!TinyHTTP_GetFirstLanSockaddr(&sa)) return false;
    return InetNtopW(AF_INET, &sa.sin_addr, out, cap) != NULL;
}

#endif /* TINY_HTTP_H */
