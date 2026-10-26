# Kyper Tool Specification (kyper.c)

## Overview

> [!WARNING]
> **Status: Experimental** — core hub is implemented and smoke tested; the API surface and steering behavior are still under evaluation and may change.

`kyper` is a zero-overhead, HTTP API-based **AI-CLI agent control plane** for Windows. It lets a LAN client (phone, laptop, another terminal) list running AI-CLI agent processes (`opencode`, `claude`, `codex`, or a custom match list), spawn new agents into a chosen working directory, steer them with text lines, and kill them — all from a tiny, dependency-free Win32/+Clang binary.

The hub also runs an embedded **mDNS responder** (shared `src/common/mdns.h`): LAN clients can reach it as `kyper.local` without knowing the raw IP — `http://kyper.local:<port>/` — plus DNS-SD discovery via `kyper._http._tcp.local` (see §5a).

It serves a stateless HTTP **API only** (no HTML by design; the UI is expected to be built by the user separately).

> [!NOTE]
> Credentials are passed **in memory only** via CLI flags (`--user` / `--pass`); nothing is persisted to disk. They are visible in the local process list while the hub runs — accepted by design.

---

## 1. CLI Arguments

```text
kyper.exe --user <name> --pass <secret>
          [--port <1-65535>]      (default: 8080)
          [--match <a,b,c>]       (default: opencode,claude,codex)
          [--help | -h | /?]
```

- **`--user` / -u** and **`--pass`** (required to start): HTTP Basic auth credentials, held only in process memory.
- **`--port` / -p**: HTTP API port (default `8080`).
- **`--match` / -m**: comma-separated process-name match list; the first token also selects the spawn binary resolved via `SearchPathW`. Matching is case-insensitive substring against executable names.
- **Single instance toggle**: launching `kyper.exe` a second time (e.g. bare, without flags) signals the running hub over `Global\TinyKyperEvent` and the hub exits cleanly with code `0`. `Ctrl+C` also exits cleanly.

---

## 2. HTTP API (No HTML)

All endpoints live on `0.0.0.0:<port>`; bad/missing credentials yield `401` with `WWW-Authenticate: Basic realm="Kyper"`.

| Method | Path | Body (form-urlencoded) | Success reply | Failure reply |
|--------|------|------------------------|---------------|---------------|
| `GET` | `/` | — | `200` JSON status | — |
| `POST` | `/spawn` | `dir=<path>&args=<flags>` | `200` status text | `400` error text |
| `POST` | `/steer` | `pid=<pid>&msg=<line>` | `200` status text | `400` error text |
| `POST` | `/kill` | `pid=<pid>` | `200` status text | `400` error text |
| any | else | — | — | `404 Not Found` |

### `GET /` — JSON status

```json
{
  "port": 8080,
  "statusError": false,
  "status": "Spawned agent PID 1234 in C:\\proj (pipe steering).",
  "agents": [
    { "pid": 1234, "name": "opencode.exe", "path": "C:\\tools\\opencode.exe", "mode": "pipe" }
  ]
}
```

- `status` / `statusError`: result of the last action (empty until the first action).
- `agents[]`: currently running processes whose executable name matches the `--match` list (case-insensitive token substring), scanned via `CreateToolhelp32Snapshot` on every request.
  - `mode` is `"pipe"` when the hub owns that PID's stdin (hub-spawned slot), otherwise `"console"` (externally launched; steering via console injection).
- `path` is resolved with `QueryFullProcessImageNameW` when accessible, otherwise empty.

Text action replies are the same strings as `status` (UTF-8, `text/plain`).

---

## 3. Spawning (hub-owned stdin pipe)

- Spawn binary = the first `--match` token resolved through `SearchPathW` at startup (e.g. `opencode` → `opencode.exe`); banner reports it.
- New agents are created with `CreateProcessW` in `dir` with a **owned anonymous stdin pipe**: the hub keeps the write end and the agent runs reading from the pipe (batch, non-interactive mode).
- Agent stdout/stderr are duplicated to the hub console (visOutput visible; harmless when hidden).
- Args are validated: no embedded control characters; length ≤ 512 chars; working dir must exist.
- Up to 16 concurrent hub-spawned slots; dead slots are pruned automatically on status scans and spawns.
- Steering a spawned agent writes one UTF-8 line (`msg` + `\n`) into the pipe; a broken pipe (agent gone) is reported and the slot cleared.

## 4. Steering (two mechanisms, per design decision)

1. **Pipe steering (hub-spawned agents)**: direct `WriteFile` into the owned pipe (agents run in non-interactive batch mode).
2. **Console injection (any externally launched console agent)**: the hub briefly detaches from its own console (`FreeConsole`), attaches to the target (`AttachConsole(pid)`), opens `CONIN$`, and writes synthesized `INPUT_RECORD` key events via `WriteConsoleInputW` — the full text plus a final **Enter**, so a typed line is submitted like real typing. Its console-mates are re-attached afterwards. Fallback for pipe agents whose pipe broke.

## 5. Killing

`TerminateProcess` on the supplied PID; the slot registry is cleared for hub-spawned agents. Error text is returned for PIDs that cannot be opened.

---

## 5a. mDNS Responder (shared `mdns.h` — added 2026-10-26)

A dedicated thread (`MdnsThread`) opens one IPv4 mDNS socket bound to the first LAN IPv4 on UDP 5353 and answers mDNS questions in a 20 ms poll loop, so LAN clients resolve the hub without a raw IP. Default / fixed identity: **`kyper.local`** (constant `MDNS_NAME`), i.e. the hub is reachable at `http://kyper.local:<port>/` when `--port` is left at 8080.

| Question | Answer | Additional records |
|----------|--------|--------------------|
| A / ANY `kyper.local` | `A <LAN IPv4>` | PTR + SRV |
| PTR `_http._tcp.local` | PTR → `kyper._http._tcp.local` | SRV + A |

- **SRV**: port = configured HTTP port, target = `kyper.local`, TTL 60 — mirrors the whisper-cast responder (`MdnsCallback` + `ExtractMdnsName`; `mdns_string_extract` output is not NUL-terminated and keeps a trailing dot).
- No CLI flag: the name is fixed `kyper`, matching the binary name (documented decision "kyper as default mDNS").
- Degrade, not fatal: if 5353 is busy (another mDNS stack on the LAN box), the hub logs `mDNS responder unavailable (bind 5353 busy?)` and stays reachable by raw IP.
- Same-host resolution limitation applies (identical to whisper-cast): Windows multicast loopback only feeds the sending socket, so verifying `kyper.local` from a phone/second machine is the remaining external check.

---

## 6. Implementation Notes

- **Zero heap / fixed buffers**: request parsing, JSON builders and process scans use static/stack buffers only (8 KB headers, 8 KB body cap).
- **Accept pattern**: `WSAEventSelect(FD_ACCEPT)` listener + `WaitForMultipleObjects` over `{toggle event, Ctrl+C wake event, accept event}`; every FD_ACCEPT uses the atomic `WSAEnumNetworkEvents` reset and a late-accept catch to never strand a backlog entry. Accepted sockets are deregistered and forced back to block mode (a raw `ioctlsocket` alone is refused for sockets that inherit the listener's event registration), with 5 s `SO_RCVTIMEO`/`SO_SNDTIMEO`.
- HTTP helpers, basic auth token (`Base64("user:pass")`), form decode, UTF-8/UTF-16 conversion and LAN IPv4 discovery come from the shared header `src/common/tiny_http.h`; CLI parsing/help from `tiny_cli.h`; single-instance toggle from `tiny_ipc.h`; mDNS (§5a) from `common/mdns.h`.
- Console writes are unbuffered (`setvbuf`) so redirected hub logs stay truthful for diagnosis.
- Hard rules honored: no background services, no registry, no persistence, no config files, no global hooks (console attach is per-process state, not a hook), clean exit on toggle-off/Ctrl+C.
