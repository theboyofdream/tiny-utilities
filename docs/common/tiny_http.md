# TinyHTTP Common Module (`src/common/tiny_http.h`)

Zero-heap HTTP/1.x server helpers and Winsock utilities shared by the long-running console tools (`kyper`, `whisper-cast`).

## Overview
- **No heap, no threads**: every buffer is stack/static; the tool owns its accept loop and any worker threads (mDNS, tray).
- **One request per connection**: `TinyHTTP_LoadRequest` reads headers + body once, replies are fixed-form, and the caller closes the socket.
- **Fixed caps**: 8 KB headers, 8 KB body, 5 s send/recv timeouts, 256-byte target and auth buffers.
- **Blocking accepted sockets**: `TinyHTTP_Accept` deregisters the inherited `WSAEventSelect` event, switches the socket back to blocking mode, and applies the timeouts.

## API Reference

### Types
- `TinyHTTPRequest`: `method`, `target`, `auth` (raw `Authorization` header value, `""` when absent), `bodyLen`, `body` (NUL-terminated).

### Lifecycle / sockets
- `bool TinyHTTP_Startup(void)` / `void TinyHTTP_Cleanup(void)` — `WSAStartup` / `WSACleanup`.
- `SOCKET TinyHTTP_Listen(int port)` — binds `0.0.0.0:<port>` and listens; `INVALID_SOCKET` on failure.
- `WSAEVENT TinyHTTP_WireAcceptEvent(SOCKET server)` — attaches an `FD_ACCEPT` event for `WaitForMultipleObjects` loops.
- `SOCKET TinyHTTP_Accept(SOCKET server)` — accepts one connection, resets the listener's network event, and prepares the client socket for blocking use.
- `void TinyHTTP_SetSocketTimeouts(SOCKET s)` — applies `TINYHTTP_TIMEOUT_MS` (5000) to send and receive.

### Request parsing / replies
- `int TinyHTTP_LoadRequest(SOCKET sock, TinyHTTPRequest *req)` — `0` on success, `-1` on closed, malformed, or oversized input. Handles `Content-Length` and `Expect: 100-continue`.
- `bool TinyHTTP_ReplyUtf8(SOCKET s, int code, const char *reason, const char *contentType, const char *body)` — status line + body with `Content-Length` and `Connection: close`.
- `void TinyHTTP_ReplyUnauthorized(SOCKET s, const char *realm)` — `401` with `WWW-Authenticate: Basic realm="..."`.
- `void TinyHTTP_ReplyNoContent(SOCKET s, const char *reason)` / `void TinyHTTP_ReplyNotFound(SOCKET s)`.

### Encoding / form data
- `TinyHTTP_WideToUtf8` / `TinyHTTP_Utf8ToWide` — conversions into caller-owned buffers.
- `TinyHTTP_UrlDecode` / `TinyHTTP_FormFieldWide` — `application/x-www-form-urlencoded` field extraction.
- `TinyHTTP_Base64Encode` — builds the expected Basic-auth token from `user:pass`.

### Network discovery
- `TinyHTTP_GetFirstLanSockaddr(struct sockaddr_in *out)` / `TinyHTTP_GetFirstLanIPv4(wchar_t *out, size_t cap)` — first non-loopback IPv4, used to bind mDNS and print the LAN URL.

## Usage Notes
- Basic auth is a string compare against a precomputed Base64 token; the header does not parse credentials itself.
- Bodies larger than `TINYHTTP_MAX_BODY` are rejected before any copy, so the fixed `body` buffer can never overflow.
- `TinyHTTP_Accept` is the only place that must clear the listener's event registration; a raw `ioctlsocket(FIONBIO)` alone is not enough for an accepted socket.

---

## Verified

- [x] Added 2026-10-26 for kyper + whisper-cast: 8 KB parse caps, 100-continue skip, form/urldecode, Basic auth token, UTF-8/16, `TinyHTTP_GetFirstLanSockaddr`
- [x] `ReplyRedirectHome` deleted — redirect strips Authorization (fake 401); POSTs reply 200/400 text
- [x] Fixes: `SendAll` declared before use; accept must deregister events before `FIONBIO=0` (else WSAEWOULDBLOCK 10035)
