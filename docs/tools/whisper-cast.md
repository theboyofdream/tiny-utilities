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

---

## Spec — original design brief

<details>
<summary>Original 2026-10-26 brief — historical, superseded where it conflicts with the implementation above</summary>

> Original user implementation brief (2026-10-26), kept verbatim for history. Superseded where it conflicts with the implemented behavior above — notably the built-in HTML page and the `ghosttype` default name, both dropped by user decision (see `docs/JOURNEY.md`).

Follow these concise steps to build, compile, and test the utility.

---

### Step 1: Project Setup & Dependencies

1. Create a clean project directory `ghosttype/`.
2. Choose **C (with Clang)** or **Rust**.
3. Include a minimal, non-blocking mDNS responder:
* **If C:** Download the single-header [`mdns.h`](https://github.com/mjansson/mdns) into the source folder.
* **If Rust:** Add `mdns-sd` (or `simple-dns`), `tokio`, and `enigo` (or `windows-sys`).



---

### Step 2: Implement CLI Argument Parsing

Implement flags with fallback defaults:

* `-p, --port <number>`: Default `7447`.
* `-n, --name <string>`: Default `ghosttype`.
* `-h, --help`: Print usage instructions.

---

### Step 3: Implement Win32 Keystroke Injection

Write a function `inject_text(text)` using native Windows APIs:

* Convert input text into UTF-16 wide characters.
* Call `SendInput()` with `INPUT_KEYBOARD` and `KEYEVENTF_UNICODE` for each character (both `KEY_DOWN` and `KEYUP`).
* Ensure it types directly into the currently focused window without shifting focus.

---

### Step 4: Implement the Minimal HTTP Server

Listen on `0.0.0.0:<PORT>`:

* **`GET /`**: Serve an inline, mobile-friendly dark-mode HTML page containing:
* A single `<textarea>` for dictation.
* A button that runs `fetch('/type', { method: 'POST', body: textarea.value })`.
* Auto-clear the textarea after sending.


* **`POST /type`**: Read the payload string, pass it to `inject_text()`, and return HTTP `200 OK`.

---

### Step 5: Implement mDNS Announcement

* Broadcast an mDNS responder thread using the service name `_http._tcp.local` and hostname `<name>.local`.
* Map it to the selected `<PORT>` and the machine's local LAN IPv4 address.

---

### Step 6: Compile

Ensure zero GUI overhead and no third-party runtime installs:

* **For C (via Clang on Windows):**
```bash
clang -O2 main.c -o ghosttype.exe -lws2_32 -luser32

```


* **For Rust:**
```bash
cargo build --release

```



---

### Step 7: Verify & Test

1. Run `./ghosttype.exe -p 7447 -n ghosttype`.
2. Confirm the console prints:
```text
GhostType running!
Open on phone: http://ghosttype.local:7447
Press Ctrl+C to stop.

```


3. Focus cursor into Notepad on the PC.
4. On phone browser, open `[http://ghosttype.local:7447](http://ghosttype.local:7447)`, dictate using the keyboard microphone, and tap **Send**. Confirm text appears in Notepad.

</details>

---

## Verified

- [-] Status: **Experimental**
- [x] Toggle `Global\TinyWhisperCastMutex/Event`; 2nd launch exits 1st (exit 0 verified)
- [x] `--port` 7447, `--name`, `--help`; API only: `POST /type` raw UTF-8 ≤ 2000 chars (else 404/400); no HTML
- [x] Injection: `SendInput` `KEYEVENTF_UNICODE` 32 pairs + 2 ms pause; `\n`→Enter, `\t`→Tab; final Enter always (verified: file created by posted echo)
- [x] mDNS (shared `mdns.h`): A/PTR/SRV for `<name>.local` + `_http._tcp`, TTL 60; degrades if 5353 busy
- [x] Debug-verified: reply looped back to own socket (A + PTR + SRV)
- [x] Fix: `mdns_string_extract` unterminated → `ExtractMdnsName` (strip dot, NUL-terminate)
- [x] `.\build.ps1 whisper-cast` clean
- [ ] Real-phone mDNS reachability (loopback can't see replies on Windows)
