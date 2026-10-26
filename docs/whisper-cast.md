# WhisperCast Tool Specification (whisper-cast.c)

## Overview

`whisper-cast` is a phone-to-PC text typer for Windows: a LAN client resolves the PC by name over mDNS, POSTs text to a tiny HTTP endpoint, and the PC **types** that text into whatever window currently has focus via `SendInput`. Zero dependencies, zero GUI, single binary — the UI lives in the client you bring (the built-in HTML page and kyper-style dashboards were intentionally removed by user decision).

> [!WARNING]
> **Status: Experimental** — core typer is implemented; real-phone mDNS verification on the LAN is still outstanding. **Default mDNS name:** `whisper-cast.local` (Service discovery: PTR/SRV for `whisper-cast._http._tcp.local`, i.e. `_http._tcp.local`) — reachable at `http://whisper-cast.local:7447/type`; if the name does not resolve from the phone, fall back to the raw LAN IP (see §3 for the 5353-port-busy degrade case).

---

## 1. CLI Arguments

```text
whisper-cast.exe [--port <1-65535>]     (default: 7447)
                 [--name <label>]       (default: whisper-cast)
                 [--help | -h | /?]
```

- **`--port` / -p**: HTTP endpoint port (default `7447`).
- **`--name` / -n**: single DNS label (ASCII letters/digits/`-`/`_`, no dots). Advertised as `<name>.local` and as service instance `<name>._http._tcp.local`.
- **Single instance toggle**: a second launch signals `Global\TinyWhisperCastEvent`; the running instance exits cleanly with reply `toggle signal sent` exit code `0`. `Ctrl+C` also exits cleanly.

---

## 2. HTTP API (No HTML)

Served on `0.0.0.0:<port>`:

| Method | Path | Body | Reply |
|--------|------|------|-------|
| `POST` | `/type` | raw UTF-8 text (≤ 2000 wide chars) | `200 OK` / `200 OK (empty)` |
| `POST` | `/type` | body > 2000 chars or undecodable | `400 Bad Request` |
| any | else | — | `404 Not Found` |

`SendInput` injection details:

- Text is converted UTF-8 → UTF-16 (`MultiByteToWideChar`).
- Sent in **chunks of 32 characters** per `SendInput` call with a 2 ms pause between chunks, each character as a `KEYEVENTF_UNICODE` key-**DOWN** + key-**UP** pair (`wVk=0`, `wScan=char`).
- `\r` skipped; `\n` becomes a real **VK_RETURN** pair (scan `0x1C`), `\t` a **VK_TAB** pair.
- A line-**submit Enter is always appended**, so any POST behaves exactly like a phone keyboard's Send: the focused app receives the full line plus Enter. There is no focus stealing — whatever the user has focused receives the keystrokes.
- Needing `SendInput` rejection (system-injected blocked) is reported as `500 SendInput Rejected`.

Client example:

```powershell
Invoke-WebRequest -Uri 'http://whisper-cast.local:7447/type' -Method POST -Body 'hello world'
```

---

## 3. mDNS Responder (shared `mdns.h`)

A dedicated thread opens one IPv4 mDNS socket bound to `192.<LAN>:5353` on the first non-loopback IPv4 interface (multicast group `224.0.0.251` joined, `IP_MULTICAST_IF` pinned, TTL 1, `SO_REUSEADDR`), and answers *question* entries only:

| Query | Answer | Additional |
|-------|--------|------------|
| A/ANY `whisper-cast.local` | `A <LAN IPv4>` | PTR + SRV for the service |
| PTR `_http._tcp.local` | PTR → `whisper-cast._http._tcp.local` | SRV + A |

- **SRV**: port = configured HTTP port, target = `<name>.local`.
- TTL 60 (via `mdns_query_answer_multicast`); replies go out multicast (`mdns_multicast_send` → `224.0.0.251:5353`) — `IP_MULTICAST_LOOP` on Windows only feeds the sender's own socket, so loopback probes from the same host will not see replies, but LAN devices will.
- Name matching is byte-exact case-insensitive against the extracted question name; `mdns_string_extract`
 output (trailing dot, no NUL) is normalized by a local helper.
- If 5353 is unavailable (another mDNS stack holds it), the thread logs a degradation notice and the HTTP port still works with the raw IP.

---

## 4. Implementation Notes

- **Zero heap / fixed buffers**: request parsing (8 KB), text conversion capsule (4 KB char / 2 K wchar cap), and the 8 KB mDNS TX/RX buffers are all static/stack.
- HTTP serving, UTF-8/UTF-16 conversion + URL handling, and LAN IPv4/`sockaddr` discovery come from the shared header `src/common/tiny_http.h`; CLI parsing/help from `tiny_cli.h`; the single-instance toggle from `tiny_ipc.h`; the mDNS protocol is the vendored public-domain single header `src/common/mdns.h` (one `recvfrom` per listener poll, non-blocking on Win32, `Sleep(20)` cadence).
- stdout is unbuffered so redirected logs are reliable; Ctrl handler flips the same `g_running` flag the main accept loop and the mDNS thread both poll, ensuring clean UDP/mutex teardown on both shutdown paths (toggle event or Ctrl+C).
- Hard rules honored: no background services, no registry, no persistence, no config files, no global hooks (SendInput injects within the caller's own desktop session), clean exit on toggle-off/Ctrl+C.
