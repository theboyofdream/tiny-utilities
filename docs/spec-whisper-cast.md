# WhisperCast: Implementation Steps for Agent

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
