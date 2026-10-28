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

---

---

## Verified

- [-] Status: **Experimental**
- [x] Toggle `Global\TinyKyperMutex/Event`; Ctrl+C exits clean (port freed, netstat verified)
- [x] `--user/--pass` (Basic, memory only), `--port` 8080, `--match` `opencode,claude,codex` (first token = spawn binary)
- [x] API only: `GET /` JSON status; `POST /spawn|/steer|/kill` → 200/400; else 404; bad auth → 401 + `WWW-Authenticate`
- [x] Listing: Toolhelp32 scan vs `--match` tokens; mode `pipe`/`console`; ≤16 slots, pruned
- [x] Spawn `CreateProcessW` (no `system()`); steer = pipe write or console inject + Enter; kill = `TerminateProcess`
- [x] mDNS `kyper.local` (A+PTR+SRV, TTL 60), mirrors whisper-cast; degrades if 5353 busy; thread joins on toggle-off
- [x] e2e verified 2026-10-26: 401 / status / spawn / pipe steer / kill / bogus kill 400 / 404 / toggle-off
- [x] `.\build.ps1 kyper` clean

---

## Spec — original design brief

<details>
<summary>Original 2026-10-26 brief — historical, superseded where it conflicts with the implementation above</summary>

> Original user design brief (2026-10-26), kept verbatim for history. Superseded where it conflicts with the implemented behavior above — notably the HTML dashboard and the Linux `agent_hub.c` source, both dropped by user decision (see `docs/JOURNEY.md`).

A minimal, single-binary C controller that auto-detects, monitors, steers, and spawns CLI agents across disparate terminals (Zed, CMD, Bash, PowerShell) over LAN or secure SSH tunnels.

* **Disk footprint:** ~100–150 KB (compiled binary)
* **RAM footprint:** ~1.2 MB RSS
* **CPU:** 0.0% idle (native blocking sockets)
* **Dependencies:** None (Pure C standard library + OS APIs)

---

## Architecture Overview

```text
[ Zed Terminal / Bash / CMD / PWSH ]
        │
        ▼ (runs ad-hoc anywhere)
   [ ai-cli ] <── (steer via FIFO / Win32 AppActivate)
        ▲
        │ (monitored via /proc or Toolhelp32)
┌───────┴────────────────────────┐
│   agent_hub (Native C / Clang) │ <─── Port 8080 (HTTP Basic Auth)
└───────┬────────────────────────┘
        │
        ├── LAN Browser: http://192.168.1.X:8080
        └── Remote Tunnel: ssh -N -R 0.0.0.0:8080:localhost:8080 user@vps

```

---

## 1. Source Code (`agent_hub.c`)

Save the following source as `agent_hub.c`:

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <tlhelp32.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define CLOSE_SOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <signal.h>
  #include <dirent.h>
  #include <ctype.h>
  #define CLOSE_SOCK close
  #define SOCKET int
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
#endif

#define PORT 8080
#define CLI_BINARY_NAME "ai-cli"

/* 
 * HTTP Basic Authentication Credentials
 * Base64 string for "admin:changeme" is "YWRtaW46Y2hhbmdlbWU="
 * To change: echo -n "user:pass" | base64
 */
#define EXPECTED_AUTH "Basic YWRtaW46Y2hhbmdlbWU="

typedef struct {
    int pid;
    char path[256];
} AgentInfo;

static int get_running_agents(AgentInfo *list, int max_agents) {
    int count = 0;
#ifdef _WIN32
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe)) {
            do {
                if (strstr(pe.szExeFile, CLI_BINARY_NAME) && count < max_agents) {
                    list[count].pid = (int)pe.th32ProcessID;
                    snprintf(list[count].path, sizeof(list[count].path), "%s", pe.szExeFile);
                    count++;
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
#else
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_agents) {
        if (!isdigit(ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        if (pid == getpid()) continue;

        char comm_path[64], comm[128] = {0};
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *f = fopen(comm_path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f)) {
                comm[strcspn(comm, "\r\n")] = 0;
                if (strstr(comm, CLI_BINARY_NAME)) {
                    list[count].pid = pid;
                    char cwd_symlink[64];
                    snprintf(cwd_symlink, sizeof(cwd_symlink), "/proc/%d/cwd", pid);
                    ssize_t len = readlink(cwd_symlink, list[count].path, sizeof(list[count].path) - 1);
                    if (len != -1) list[count].path[len] = '\0';
                    else snprintf(list[count].path, sizeof(list[count].path), "cwd unavailable");
                    count++;
                }
            }
            fclose(f);
        }
    }
    closedir(dir);
#endif
    return count;
}

static void send_unauthorized(SOCKET client) {
    const char *resp = 
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"AgentHub\"\r\n"
        "Content-Length: 12\r\n"
        "Connection: close\r\n\r\n"
        "Unauthorized";
    send(client, resp, strlen(resp), 0);
}

static void send_redirect(SOCKET client) {
    const char *resp = "HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(client, resp, strlen(resp), 0);
}

static void parse_field(const char *body, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    char *start = strstr(body, pattern);
    if (!start) return;
    start += strlen(pattern);
    char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= out_sz) len = out_sz - 1;
    strncpy(out, start, len);
    out[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        if (out[i] == '+') out[i] = ' ';
    }
}

static void handle_client(SOCKET client) {
    char buf[4096] = {0};
    int received = recv(client, buf, sizeof(buf) - 1, 0);
    if (received <= 0) return;

    char *auth = strstr(buf, "Authorization: ");
    if (!auth || strncmp(auth + 15, EXPECTED_AUTH, strlen(EXPECTED_AUTH)) != 0) {
        send_unauthorized(client);
        return;
    }

    if (strncmp(buf, "GET / ", 6) == 0 || strncmp(buf, "GET /?", 6) == 0) {
        AgentInfo agents[64];
        int n_agents = get_running_agents(agents, 64);

        char body[16384];
        int offset = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Agent Hub</title>"
            "<style>body{font-family:monospace;background:#0f111a;color:#e6edf3;padding:20px;}"
            "table{width:100%%;border-collapse:collapse;margin-top:15px;}"
            "th,td{border:1px solid #30363d;padding:8px 12px;}"
            "th{background:#161b22;}input,button{background:#161b22;color:#fff;border:1px solid #30363d;padding:6px;border-radius:4px;}"
            ".card{background:#161b22;padding:16px;border:1px solid #30363d;border-radius:6px;margin-bottom:20px;}</style></head><body>"
            "<h2>AI-CLI Control Hub (Native C)</h2>"
            "<div class='card'><h3>Spawn Agent</h3>"
            "<form method='POST' action='/spawn'>"
            "<input type='text' name='dir' placeholder='Working Directory' style='width:300px;' required> "
            "<input type='text' name='args' placeholder='Flags/Args'> "
            "<button type='submit'>Launch</button></form></div>"
            "<div class='card'><h3>Active Agents (%d)</h3>"
            "<table><thead><tr><th>PID</th><th>Location / Binary</th><th>Actions</th></tr></thead><tbody>", n_agents);

        for (int i = 0; i < n_agents; i++) {
            offset += snprintf(body + offset, sizeof(body) - offset,
                "<tr><td><code>%d</code></td><td><code>%s</code></td><td>"
                "<form method='POST' action='/steer' style='display:inline;'>"
                "<input type='hidden' name='pid' value='%d'>"
                "<input type='text' name='msg' placeholder='Send input...'> "
                "<button type='submit'>Steer</button></form> "
                "<form method='POST' action='/kill' style='display:inline;'>"
                "<input type='hidden' name='pid' value='%d'>"
                "<button type='submit' style='color:#f85149;'>Kill</button></form>"
                "</td></tr>",
                agents[i].pid, agents[i].path, agents[i].pid, agents[i].pid);
        }

        if (n_agents == 0) {
            offset += snprintf(body + offset, sizeof(body) - offset,
                "<tr><td colspan='3'>No active agents found matching '%s'.</td></tr>", CLI_BINARY_NAME);
        }

        snprintf(body + offset, sizeof(body) - offset, "</tbody></table></div></body></html>");

        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
            strlen(body));
        send(client, header, hlen, 0);
        send(client, body, strlen(body), 0);
        return;
    }

    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) { send_redirect(client); return; }
    body_start += 4;

    if (strncmp(buf, "POST /steer", 11) == 0) {
        char pid_str[16], msg[256];
        parse_field(body_start, "pid", pid_str, sizeof(pid_str));
        parse_field(body_start, "msg", msg, sizeof(msg));

#ifndef _WIN32
        char fifo[128];
        snprintf(fifo, sizeof(fifo), "%s/.agent-registry/%s.in", getenv("HOME"), pid_str);
        FILE *fp = fopen(fifo, "w");
        if (fp) {
            fprintf(fp, "%s\n", msg);
            fclose(fp);
        }
#endif
    } else if (strncmp(buf, "POST /kill", 10) == 0) {
        char pid_str[16];
        parse_field(body_start, "pid", pid_str, sizeof(pid_str));
        int pid = atoi(pid_str);
        if (pid > 0) {
#ifdef _WIN32
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
#else
            kill(pid, SIGKILL);
#endif
        }
    } else if (strncmp(buf, "POST /spawn", 11) == 0) {
        char dir[256], args[256];
        parse_field(body_start, "dir", dir, sizeof(dir));
        parse_field(body_start, "args", args, sizeof(args));

        char sys_cmd[1024];
#ifdef _WIN32
        snprintf(sys_cmd, sizeof(sys_cmd), "start /d \"%s\" cmd /k \"%s %s\"", dir, CLI_BINARY_NAME, args);
#else
        snprintf(sys_cmd, sizeof(sys_cmd), "cd \"%s\" && nohup %s %s > agent.log 2>&1 &", dir, CLI_BINARY_NAME, args);
#endif
        system(sys_cmd);
    }

    send_redirect(client);
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) return 1;

    int opt = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        CLOSE_SOCK(server);
        return 1;
    }

    listen(server, 16);
    printf("[*] Agent Hub listening on 0.0.0.0:%d (RAM: ~1.2MB)\n", PORT);

    while (1) {
        struct sockaddr_in c_addr;
        socklen_t c_len = sizeof(c_addr);
        SOCKET client = accept(server, (struct sockaddr *)&c_addr, &c_len);
        if (client != INVALID_SOCKET) {
            handle_client(client);
            CLOSE_SOCK(client);
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

```

---

## 2. Compilation Instructions

Compile with Clang to strip all unnecessary overhead:

### Linux / macOS / WSL

```bash
clang -O3 -s agent_hub.c -o agent_hub

```

### Windows (Developer CMD or PowerShell with Clang installed)

```cmd
clang -O3 agent_hub.c -o agent_hub.exe -lws2_32

```

---

## 3. Shell Hook for Seamless Steering (Linux / macOS / WSL)

To allow the web interface to steer agents launched manually from Zed or bash, add this hook to your `~/.bashrc` or `~/.zshrc`:

```bash
ai-agent() {
  local reg_dir="$HOME/.agent-registry"
  mkdir -p "$reg_dir"

  local fifo="$reg_dir/$$.in"
  mkfifo "$fifo" 2>/dev/null || true

  cleanup() {
    rm -f "$fifo"
  }
  trap cleanup EXIT INT TERM

  # Pipes background steering input directly to stdin while keeping your terminal active
  cat "$fifo" 2>/dev/null | ai-cli "$@"
}

```

Now, launch your agents using `ai-agent` inside any workspace folder or terminal.

---

## 4. Running & Network Access

### Start the Service Locally

```bash
./agent_hub &

```

### Accessing on LAN

Open any browser on the local network:

```text
http://<HOST-LOCAL-IP>:8080

```

* **Username:** `admin`
* **Password:** `changeme`

### Accessing via Secure SSH Reverse Tunnel (Zero Third-Party Dependency)

On the machine running `agent_hub`:

```bash
ssh -N -R 0.0.0.0:8080:localhost:8080 user@your-vps-ip

```

Access the dashboard securely at `

</details>
