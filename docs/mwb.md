# mwb — LAN Mouse Without Borders Peer Specification (`src/mwb.c` -> `mwb.exe`), macOS peer `src/mwb_mac.c`

Windows ↔ macOS (and Windows ↔ Windows) mouse/keyboard/file/clipboard sharing over the LAN,
in the style of Mouse Without Borders. One physical console controls both machines.

## 1. Overview & Core Contract

- **Source Files**: [`src/mwb.c`](../src/mwb.c) (win32) and [`src/mwb_mac.c`](../src/mwb_mac.c) (macOS peer, identical wire protocol).
- **Binary Targets**: `dist/release/mwb.exe` / `dist/debug/mwb.exe` (via `pwsh -File .\build.ps1 mwb`); macOS: `./build_mac.sh` -> `./mwb-mac` (Mac required; never compiled on Windows).
- **Subsystem**: `windows` (`WinMain`); attaches to the parent console for log output when started from a terminal.
- **Language**: strict Win32 C (Winsock2, GDI, OLE only for cross-machine drag-and-drop), Clang-only build.
- **Common headers**: `TinyCLI` (tokenize/help/option table), `TinyIPC` (`Global\TinyMwbMutex` / `Global\TinyMwbEvent` toggle), `TinyDPI` (Per-Monitor v2).
  No custom fonts/overlays are created, so `font.h`/`tiny_gui.h` are not used.
- **Screen geometry**: primary-monitor-only by design (`GetSystemMetrics(SM_CXSCREEN)`, center park `g_cx/g_cy`). Per-Monitor v2 awareness makes these real device pixels, which is required for edge math on scaled displays.

### PRD carve-outs (deterministic exceptions to the hard rules)

`docs/PRD.md` §21 authorizes:

1. **Global LL hooks** (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`) — installed only in server mode, the core capture mechanism; unhooked on every exit path.
2. **UI**: tray icon + popup menu, native `MessageBoxW` prompts, the 4-char OTP edit dialog, and Explorer's "open folder" shell call.
3. **Disk output**: log file beside the exe (`mwb.log`, capped via `LOG_MAX` 5 MiB) and files received into `<exe dir>\received` (user data only — **no settings/config are ever persisted**; side selection and clipboard-sync flag are session-only).
4. **Long-running tray process** that shares input until toggle-off / Exit; single instance per machine.

## 2. CLI Syntax & Options

```text
mwb.exe [server | client [server-ip]] [--otp CODE] [--log FILE] [--config FILE] [--help]
```

| Argument        | Meaning                                                                       |
|-----------------|-------------------------------------------------------------------------------|
| `server`        | This PC has the physical keyboard/mouse; its input is shared / captured.      |
| `client`        | This PC receives and injects the peer's input. `server-ip` optional (else UDP discovery). |
| `--otp CODE`    | Supply the 4-char pairing code on the command line (skips the prompt/balloon flow). |
| `--log FILE`    | Append-mode log file; default `mwb.log` beside the exe.                       |
| `--config FILE` | Read-only config file; default `mwb.txt` beside the exe (mac: beside the binary). |
| `--help` / `-h` | Declarative help box (`TinyCLI`).                                             |

- No CLI mode: a Yes/No/Cancel dialog asks Server/Client/Quit.
- Exit codes: `0` = clean (also: help shown, second launch toggle-signal sent, toggle-off), `1` = error (usage, winsock, hook install, window create).
- macOS: `mwb-mac server | mwb-mac client [server-ip] [--side right|left|up|down] [--otp CODE] [--log FILE] [--config FILE]`.

### Optional read-only config file

Settings you don't want to pass every time live in a plain text file next to the binary
(win32: `<exe dir>\mwb.txt`, macOS: `mwb.txt` beside `mwb-mac`; `--config FILE` picks any path).
The tool only ever **reads** it and never writes anything back — this keeps the "no disk
persistence" rule intact (no settings are saved *by the tool*; the file is user-managed):

```ini
# mwb.txt - one "key = value" per line, '#' comments
mode      = server          # server | client (config mode also skips the role dialog)
side      = left            # right | left | up | down (same as the "Peer is on..." tray menu)
mouse     = on              # on | off (cursor handover + pointer events)
keyboard  = on              # on | off (key events)
clipboard = on              # on | off (clipboard text sync)
files     = on              # on | off (file offers, pulls, cross-PC drag)
```

- Precedence: **CLI arguments always win**; config fills only what the CLI left silent.
- Applies at startup; `side`/`clipboard text` can still be changed live afterwards (tray menu on win32).
- Unknown keys and bad values are logged and ignored.
- **Channel gates** (win32 log line at startup: `channels: mouse= keyboard= clipboard= files=`):
  - A channel is **live only if both peers enable it** — each peer gates its own send path and its own injection path, so one side turning a channel off kills it in both directions. No wire changes; nothing is negotiated.
  - `mouse = off` on a side ⇒ that side installs **no mouse hook / no event tap** ⇒ no cursor handover at all ⇒ keyboard sync (which piggybacks on the handover) is also moot even if `keyboard = on`.
  - `keyboard = off` on the controller ⇒ keys are **parked** while the cursor is remote (swallowed, not typed into either machine); the Scroll Lock (win) / double-Caps-Lock (mac) emergency return still works — it is control-plane, not a channel.
  - `files = off` ⇒ no automatic file offers, no cross-machine drag, and the tray "Send file" action is skipped. `clipboard = off` + `files = on` is valid: file-sync without text sync.
- There is deliberately **no `otp` key**: the pairing code rotates at runtime and a permanent shared-secret in a file buys nothing.
- Parser is the shared loader [`src/common/tiny_config.h`](../src/common/tiny_config.h) (`TinyConfig_Load`) — the same code runs on the win32 and macOS peers.

## 3. Single Instance & IPC Toggle

- Mutex: `Global\TinyMwbMutex`, Event: `Global\TinyMwbEvent` (`TinyIPC_AcquireOrToggle`).
- Second launch signals the event and exits **0**. The running instance waits on the event inside its message loop (`MsgWaitForMultipleObjectsEx`) and **stops cleanly** on toggle: unhook LL hooks, `shutdown()` the session socket, remove tray.
- The peer notices the session loss within seconds via TCP keepalive.

## 4. Modes & Topology

- Exactly **two peers** per session, one `server` (capturer) and one `client` (receiver/injector); roles are fixed for the session.
- Discovery: the server binds UDP **24800** and broadcasts `MWB1` every 2 s; a client without `server-ip` binds the same port and adopts the beacon's source address.
- Session: TCP **24801** config — keepalive 5 s idle / 1 s interval, handshake recv/send budget 10 s/5 s, session send timeout 30 s.
- System requirements/discovery semantics: both machines on one LAN/subnet (broadcast-dependent); platform parity announced via `M_HELLO` after auth (`a = os in {1 = win32, 2 = mac}`, `b/c` = screen size; both sides log the peer OS).

## 5. Pairing & Authentication

- OTP: 4 chars from a 32-symbol alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no `0/O/1/I`).
- Flow over TCP 24801 (`hmac256` = HMAC-SHA256 via CNG, `ct_eq` = constant-time compare):

```text
S -> C: ns (16 random bytes)
C -> S: nc (16 random bytes) || HMAC(otp, 'C' || ns || nc)      (48 bytes)
   wrong code -> S sends a 33-byte zero blob, closes, +1 failure; after 3 failures S regenerates the code
   right code -> S -> C: 0x01 || HMAC(otp, 'S' || nc || ns)      (33 bytes, client verifies the server proof)
   both sides:    session key = HMAC(otp, 'K' || ns || nc)       (32-byte g_key)
```

- **The session key authenticates the file channel only; input/clipboard/file payload is not encrypted.** The 4-char code defeats casual/accidental joins, not an active LAN attacker — trusted networks only.

## 6. Wire Protocol Message Table

16-byte header `{u32 type; i32 a, b, c}` little-endian; `M_CLIP_*` messages carry `c`-byte payloads. Keys are **always Win32 VK codes** on the wire (macOS maps `kVK <-> VK` in both directions).

| Type          | Dir            | Fields |
|---------------|----------------|--------|
| `M_MOVE` (1)  | S→C            | `a=dx b=dy` (raw deltas) |
| `M_BUTTON` (2)| S→C            | `a=button(0=L 1=R 2=M 3=X1 4=X2) b=down` |
| `M_WHEEL` (3) | S→C            | `a=delta b=horizontal` |
| `M_KEY` (4)   | S→C            | `a=vk b=scancode c=flags(bit0=up, bit1=extended)` |
| `M_ENTER` (5) | S→C            | remote cursor enters: `a=pos 0..10000 along the entry edge`; `c` bit0 = left button held (drag announced), bits 1-2 = **client entry edge** `0=left 1=right 2=top 3=bottom` |
| `M_LEAVE` (6) | C→S / S→C      | `a=pos 0..10000 along the tunnel edge`; C→S = "control back", S→C = "stop remote" |
| `M_CLIP_TEXT` (7) | both       | `c=len`, then `len` bytes UTF-8 |
| `M_CLIP_FILES` (8)| both       | `a=offer generation b=flags(bit0=drag) c=len`, payload: `u32 count`, per entry `{u64 size, u32 isdir, u32 namelen, utf8 relative name}` |
| `M_HELLO` (9) | both (post-auth)| `a=OS`, `b=screenW`, `c=screenH` |

## 7. Tunnel-Side Model

The server decides which of ITS edges leads to the peer; the client mirrors it automatically (server Right ↔ client Left, Above ↔ client Bottom, etc.). Client entry/exit edge code travels inside `M_ENTER.c`; `M_LEAVE.a` carries the position along the tunnel edge so the server re-parks the cursor at the exact spot.

| Server config              | Handover edge (server) | Entry & exit edge (client) | `M_ENTER.c` bits 1-2 |
|----------------------------|------------------------|----------------------------|----------------------|
| Peer **right** of this PC  | right                  | client LEFT                | `0`                  |
| Peer **left** of this PC   | left                   | client RIGHT               | `1`                  |
| Peer **above** this PC     | top                    | client BOTTOM              | `3`                  |
| Peer **below** this PC     | bottom                 | client TOP                 | `2`                  |

- Win32 config: tray menu "Peer is on..." submenu (Right default / Left / Above / Below), session-only.
- macOS config: `--side right|left|up|down`, default right (no menu bar in v1).
- Position fractions: `0..10000` mapped to `(dim-1)` pixels; on macOS Y is bottom-up internally, converted at capture/inject.
- Emergency return: double-tap Caps Lock within 400 ms while remote (macOS); Win32 uses the client's edge cross.

## 8. Clipboard & File Transfer

- **Text**: `M_CLIP_TEXT`, UTF-8, up to 4 MiB (`MAX_TEXT`); clipboard watcher on both sides (win32 `ClipboardFormatListener`, mac pasteboard poll 300 ms + 600 ms self-echo suppression); "Sync clipboard + drag-and-drop" toggles the whole sync pipeline off/on.
- **File offers** (`M_CLIP_FILES`): snapshot of the current selection, identified by an ever-increasing **generation**; up to 20000 entries per offer (`MAX_OFFER`), names ≤ 1024 UTF-8 bytes.
- **Lazy pull** (win32 & mac): on paste/drop the receiver opens TCP **24802**, authenticates, then sends the pull header `{u32 magic 0x3150574D "MWP1", u32 = generation, u32 = entry index}`, gets `{u64 size}` + streamed bytes (256 KiB chunks). Stale generations are refused.
- **Eager push** ("Send file(s)..." dialog): header `{u32 magic 0x3146574D "MWF1", u32 namelen, u64 size}` + name bytes + file bytes; recipient stores unique names in its received folder.
- **Channel auth**: acceptor sends 16 random bytes; initiator answers `HMAC(session_key, 'F' || nonce)`; accepted = `0x01`.
- **Received files**: win32 `<exe dir>\received`, macOS `~/mwb-received` (mac additionally puts `public.file-url` items on the pasteboard for Cmd+V in Finder).
- **Cross-machine drag-and-drop (Windows↔Windows only)**: on edge-cross with a held left button in Explorer, the server cancels the local drag (Esc), Ctrl+C's the selection, and announces it via `M_ENTER.c` bit0/M_CLIP_FILES `b` bit0; the injected `M_BUTTON` on the client performs a real OLE drag (`FileGroupDescriptorW` + `FileContents` + `MWB.Origin` format). macOS receives those offers as pasted files instead (no OLE).

## 9. Tray UI (win32)

- Menu: `Send file(s)...`, `Sync clipboard + drag-and-drop` (check toggle), `Show pairing code` / `Enter pairing code...`, `Peer is on...` (Right/Left/Above/Below radio), `Open received folder`, `Exit`.
- `ID_EXIT` and toggle-off both route through the same clean stop (message loop exit).

## 10. Diagnostics

- Logs: append-mode next to the exe (`mwb.log`, `--log` overrides), also echoed to an attached console; ~5 MiB cap re-created on overflow. Starts/binds, pairing attempts (IP + strike count), peer OS, clipboard/file transfer sizes, toggle-off are all logged.
- Failure surfaces: hook-install error box (server), winsock error box, wrong-code balloon on the server (with regeneration after 3 strikes), stale-offer refusals on pull.

## 11. macOS Peer Parity

Identical: ports, auth, message framing, VK-canonical keys, keepalive, discovery, file offer format, clipboard text.

macOS-specific:
- Capture = `CGEventTap` (`kCGSessionEventTap`, tap-disable re-enable on timeout), injection = `CGEventPost` + event source; requires the **Accessibility** permission (hint + `CGRequestListenEventAccess` weak call on failure).
- `kVK <-> VK` keymap, Command ≡ Win key, Control/Option literal; modifier state tracked via flag edges.
- Single instance via `flock ~/.mwb-mac.lock`; no tray (v1), logs to `mwb.log` beside the binary (fallback `~/Library/Logs`), `SIGINT/SIGTERM` = clean stop.
- Side config via `--side`; no cross-machine OLE drag (received instead as files + pasteboard URLs).
- **Hardware-verify checklist remains open until run on a real Mac**: delta-Y sign, trackpad/wheel scaling, accessibility prompt flow, end-to-end pairing both directions (see `docs/CHECKLIST.md` §30).

## 12. Verification

- `pwsh -File .\build.ps1 -Mode release mwb` (Clang, `ws2_32 user32 gdi32 shell32 comdlg32 ole32 uuid bcrypt`).
- Smoke: first launch binds/starts; second launch signals toggle and exits 0; first instance stops cleanly (0 processes remain).
- Session behaviors (edge crossing, pairing, clipboard/files) are exercised between two machines; see CHECKLIST entries for the tested matrix and the open mac hardware list.
