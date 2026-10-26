# AGENTS.md

Spec: `PRD.md` · Status: `docs/CHECKLIST.md` · Per-tool specs: `docs/<tool>.md` (source in `src/<tool>.c`)

Tools: `pin-to-top` `find-my-mouse` `mouse-spotlight` `color-picker` `capture` `pixel-view` `ip-send` `context-menu` `ocr` `window-switcher` `mwb` `kyper` `whisper-cast`
Script tool: `src/cmdx.ps1` → `docs/cmdx.md`

## Hard rules

**Runtime**
- One process per invocation. No services, config files (exception: mwb's optional **read-only** `mwb.txt` / `--config` per PRD §21), env vars, disk persistence, or global hooks.
- Registry is off-limits except: per-user Explorer context menus (`context-menu`) and User PATH in `HKCU\Environment` (`cmdx`).
- No GUI windows except click-through overlays and native dialogs.
- Any failure → exit cleanly. Toggle-off → exit cleanly.

**Code**
- Plain C99, Win32 + GDI only (no DirectX). Wide (`W`) APIs.
- Entry: `WinMain` (GUI) or `wmain` (console).
- Style: `static` helpers, `g_` globals, `snake_case`.
- Compile with Clang only (`clang-cl` or MinGW-w64 `clang`). Never gcc.

**Shared headers (`src/common/`): always reuse, never reimplement**

| Header | Use for |
|---|---|
| `tiny_cli.h` | CLI arg parsing |
| `tiny_ipc.h` | Single-instance toggle (`TinyIPC_AcquireOrToggle`) |
| `tiny_dpi.h` | DPI awareness |
| `tiny_gui.h` | Overlays, multi-monitor bounds, dimming, resize hit-testing |
| `font.h` | Fonts + DPI scaling |
| `clipboard.h` | Text / bitmap / file-drop clipboard |
| `tiny_config.h` | Read-only `key = value` config loading (`TinyConfig_Load`) — the tool never writes to the file (mwb / PRD §21 carve-out) |
| `tiny_http.h` | HTTP API for console server tools |
| `mdns.h` | mDNS announce (`mdns_string_extract` output is NOT NUL-terminated, keeps trailing dot) |
| `color-thief-algorithm.h` | Dominant wallpaper color |

**Workflow**
- Build only the target you changed: `pwsh -File .\build.ps1 [-Mode release|debug] <target>` (`<target>` = a tool name above, or `all`). Output goes to `dist/release/` or `dist/debug/`. (`mwb` has a macOS sibling `src/mwb_mac.c`, built only on a Mac via `build_mac.sh`.)
- Git: never run `git add`, `commit`, or `push` unless the user asks.
- After any file edit, tell the user what changed and why.

## IPC toggle

Names: `Global\Tiny<Tool>Mutex` and `Global\Tiny<Tool>Event` (manual-reset = FALSE).
First launch creates both. A second launch sees `ERROR_ALREADY_EXISTS`, signals the event, and exits `0`.

## Done checklist

1. Build the target.
2. Smoke test: double-launch toggle (overlay tools) or run the CLI (console tools).
3. Docs: append behavior changes to `docs/CHECKLIST.md`, and decisions/experiments/trade-offs to `docs/JOURNEY.md`.
