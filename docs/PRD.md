
# Tiny Windows Productivity Utilities (Agent-Proof + Clang + CLI Args)

---

# 0. Purpose

This document defines the **global deterministic contract** for the `tiny-utilities` Windows tools: the hard rules, the compiler requirement, the IPC toggle contract, and the regime of granted exceptions (§21).

* ✅ **Command-line arguments support with deterministic parsing**
* ✅ **Clang as the required compiler (NOT GCC)**
* ✅ Strict Win32 C implementation rules
* ✅ Fully deterministic agent instructions

---

# 0.1 HOW TO READ THIS REPO

Ownership is layered by fact type — each fact lives in exactly one place:

- **Global contract** (this file, §1–§13 + §21): hard rules, compiler, IPC toggle, overlay/perf contract, carve-out registry.
- **Per-tool truth**: `docs/tools/<tool>.md` holds each tool's behavior spec, CLI reference **and its "Verified" section** (implementation status). One doc per `src/<tool>.c`.
- **Shared headers**: `docs/common/<header>.md`, one per `src/common/<header>.h`.
- **Cross-cutting status**: `docs/CHECKLIST.md` (hard-rule compliance, build/release pipeline only).
- **Design principles / taste**: `docs/PRINCIPLES.md`. **Decision history (append-only)**: `docs/JOURNEY.md`.

| Tool | Spec |
|---|---|
| pin-to-top | `docs/tools/pin-to-top.md` |
| find-my-mouse | `docs/tools/find-my-mouse.md` |
| mouse-spotlight | `docs/tools/mouse-spotlight.md` |
| color-picker | `docs/tools/color-picker.md` |
| capture | `docs/tools/capture.md` |
| pixel-view | `docs/tools/pixel-view.md` |
| ip-send | `docs/tools/ip-send.md` |
| context-menu | `docs/tools/context-menu.md` |
| ocr | `docs/tools/ocr.md` |
| window-switcher | `docs/tools/window-switcher.md` |
| mwb (win32 + macOS peer) | `docs/tools/mwb.md` |
| kyper | `docs/tools/kyper.md` |
| whisper-cast | `docs/tools/whisper-cast.md` |
| cmdx (script tool) | `docs/tools/cmdx.md` |

---

# 1. HARD RULES (NON-NEGOTIABLE)

Agents MUST follow all rules:

```text
- No background services
- No registry usage
- No disk persistence
- No configuration files
- No global system hooks
- One process per executable invocation
- No multi-process coordination except defined IPC
- All overlays must be click-through
- No UI/GUI windows except overlays
- Must exit cleanly on toggle-off
- Must compile with Clang (MSVC-compatible or MinGW-clang)
```

Exceptions exist **only** where granted explicitly in the §21 carve-out registry. An unlisted exception is a spec violation.

---

# 2. COMPILER REQUIREMENT

## 2.1 REQUIRED TOOLCHAIN

```text
clang (LLVM)
```

### Allowed:

* clang-cl (MSVC-style)
* clang via MinGW-w64
* LLVM clang on Windows

### NOT allowed:

* gcc
* g++
* non-clang toolchains

---

## 2.2 BUILD COMMAND

One declarative entry point; only the changed target is rebuilt:

```bash
pwsh -File .\build.ps1 [-Mode release|debug] <target|all>
```

Output goes to `dist/release/` or `dist/debug/`.

---

# 3. CLI ARGUMENT RULES (GLOBAL)

Full per-tool flag references live in each `docs/tools/<tool>.md`. The global contract:

```text
- Arguments are optional; missing → defaults
- Missing values / unknown flags / non-integers → fallback to defaults
- Out-of-range values → clamp (never fail)
- No config files, no environment variables
```

Parsing is declarative via `TinyCLI_ParseCommandLine` / `TinyCLI_Parse` (`src/common/tiny_cli.h`): each utility defines its `CliOption` options table **once**, reused for both argument parsing and `--help` / `-h` / `/?` output (dual-mode output: redirected stdout → parent console → `MessageBoxW`).

---

# 4. GLOBAL DESIGN MODEL

## 4.1 Execution Model

```text
AHK hotkey (shortcuts.ahk) → exe → toggle → exit OR stay active
```

No daemon processes.

---

# 5. IPC TOGGLE CONTRACT (STRICT, ALL TOOLS)

## 5.1 Named Objects

```text
Mutex: Global\Tiny<Tool>Mutex
Event: Global\Tiny<Tool>Event   (manual-reset = FALSE)
```

## 5.2 IPC RULES

```text
- First launch creates both, then runs
- Second launch sees ERROR_ALREADY_EXISTS → SetEvent → exit 0
- Event = TOGGLE only
```

---

# 6. & 7. PER-TOOL BEHAVIOR

Per-tool behavior (launch flows, toggle logic, overlay animation, tracking loops, edge cases) is specified **and verified** in each tool's own doc — see the table in §0.1. The docs tools mirror `src/` and are the single owner of tool truth; this PRD does not duplicate them.

---

# 8. OVERLAY SYSTEM (SHARED CONTRACT)

## 8.1 WINDOW STYLE

```c
WS_EX_LAYERED
WS_EX_TRANSPARENT
WS_EX_NOACTIVATE
WS_EX_TOOLWINDOW
```

Per-tool transparency technique (color-key vs uniform-alpha + per-pixel DIB) is documented in each tool doc.

---

## 8.2 RENDERING

GDI ONLY. No DirectX.

---

## 8.3 UPDATE RULE

```text
max 60 FPS
only redraw on change
```

---

# 9. ERROR HANDLING

```text
- fail → exit cleanly
- no infinite loops
- no crash recovery systems
```

---

# 10. PERFORMANCE LIMITS

```text
CPU idle < 0.2%
Memory < 10MB per tool
Loop ≤ 60Hz
```

Measured values are recorded per tool doc.

---

# 11. AUTOHOTKEY CONTRACT

Hotkey wiring lives in `shortcuts.ahk` (repo root); example shape:

```ahk
#^t::Run(".\pin-to-top.exe --border-width 2")
```

---

# 12. STATE MACHINES

Per-tool launch/toggle state machines are documented in each tool doc (they supersede any historical FSM text in old PRD revisions).

---

# 13. FINAL GUARANTEE

* deterministic CLI parsing
* clang-only build compatibility
* strict Win32-only implementation
* no undefined IPC behavior
* no hidden state
* predictable overlay behavior

---

# 20. GLOBAL-CMD — SUPERSEDED

`src/global-cmd.c` was removed 2026-09-04 and superseded by the dynamic PowerShell script tool `src/cmdx.ps1` (`docs/tools/cmdx.md`), which owns the PATH registration and alias-shim behavior.

---

# 21. CARVE-OUT REGISTRY (EXCEPTIONS TO §1)

Every deviation from the hard rules is registered here — granted explicitly, scoped narrowly. An exception not listed below is a spec violation.

| Tool | Granted exception | Scope |
|---|---|---|
| `context-menu` | Registry writes | Per-user Explorer menus under `HKCU\Software\Classes`; managed keys marked `ManagedBy = "context-menu"`; non-managed entries untouched |
| `ip-send` | Registry + native composer window + third-party process | HKCU context-menu registration (`*`, `Directory`, `Directory\Background`) + App Paths lookup for backend detection; one resizable composer window (not an overlay); auto-starts the `ipmsg.exe` background process with a visible warning |
| `pixel-view` | Native viewer window | One resizable main viewer window (not an overlay); zero web runtimes |
| `cmdx` (script tool) | User PATH | `HKCU\Environment` `Path` registration with `WM_SETTINGCHANGE` broadcast |
| `ocr` | Disk provisioning + dialogs | Downloads portable Tesseract + `tessdata` beside the exe on first run (user-managed folder; delete = uninstall); native file-picker and message dialogs |
| `mwb` | Full §21.1 grant below | Low-level hooks (server mode), tray + native dialogs, log file + received-files folder, long-running tray process, optional read-only config |
| `kyper` | Resident + network + child processes | Long-running console server until toggle-off; TCP listener on `0.0.0.0:<port>` (HTTP Basic auth); spawns and steers child agent processes |
| `whisper-cast` | Resident + network + input injection | Long-running console server; TCP listener; `SendInput` keystroke injection into the currently focused window (no focus stealing) |

### 21.1 mwb (full grant)

```text
- Global system hooks ARE permitted in SERVER mode only (WH_MOUSE_LL / WH_KEYBOARD_LL are
  the capture mechanism; unhooked on every exit path; client mode installs none).
- UI: tray icon, popup menus, native MessageBox/OTP dialogs and ShellExecute folder-open.
- Disk output: mwb.log beside the exe (5 MiB cap) and files received into <exe>\received
  (win32) / ~/mwb-received (mac) are USER DATA, not persistence: NO configuration,
  pairing codes, paths or flags are ever saved BY the tool; runtime settings remain
  session-only. EXCEPTION - an optional READ-ONLY config file may be READ at startup:
  default `mwb.txt` beside the exe/binary, or `--config PATH`; keys
  `mode = server|client`, `side = right|left|up|down`, plus channel keys
  `clipboard`/`mouse`/`keyboard`/`files = on|off`;
  '#' comments; unknown keys logged and ignored; CLI arguments always win;
  the tool never writes to the file.
- Long-running process: stays resident in the tray until toggle-off (second launch) or Exit.
```

Full behavior spec: `docs/tools/mwb.md`.

---

# END OF PRD
