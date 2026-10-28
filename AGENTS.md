# AGENTS.md

Global rules: `docs/PRD.md` (§1–§13 + §21 carve-out registry) · Principles: `docs/PRINCIPLES.md` · Cross-cutting status: `docs/CHECKLIST.md` · Per-tool spec **+ verification**: `docs/tools/<tool>.md` (source in `src/<tool>.c`)

Tools: `pin-to-top` `find-my-mouse` `mouse-spotlight` `color-picker` `capture` `pixel-view` `ip-send` `context-menu` `ocr` `window-switcher` `mwb` `kyper` `whisper-cast`
Script tool: `src/cmdx.ps1` → `docs/tools/cmdx.md`

## Docs

| Path | Contents |
|---|---|
| `docs/PRD.md` | Global contract: hard rules, compiler, IPC toggle, overlay/perf contract, **carve-out registry (§21)** — per-tool specs live in the tool docs |
| `docs/CHECKLIST.md` | Cross-cutting status only (hard-rule compliance, build/release pipeline) + tool index table |
| `docs/PRINCIPLES.md` | Design principles & taste (inferred from JOURNEY) — tie-breakers when a spec doesn't answer |
| `docs/JOURNEY.md` | Append-only decisions, experiments, trade-offs — **read-only, never edit** |
| `docs/DEVELOPMENT.md` | Build + dev setup |
| `docs/HOW_I_USE_IT.md` | Author's daily-driver workflow |
| `docs/tools/<tool>.md` | Per-tool spec **+ its "Verified" section** — the single owner of per-tool truth |
| `docs/common/<header>.md` | One per `src/common/<header>.h` (e.g. `tiny_gui.h` → `tiny_gui.md`) |
| `shortcuts.ahk` | AutoHotkey v2 script (repo root, not a doc) |

## Hard rules

**Runtime**
- One process per invocation. No services, config files, env vars, disk persistence, or global hooks — exceptions exist **only as granted in the PRD §21 carve-out registry** (e.g. mwb's read-only `mwb.txt` / `--config`).
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
| `tiny_tray.h` | Notification-area icon host for console server tools (`kyper`, `whisper-cast`) |
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
3. Docs (each fact exactly once, at its owner):
   - per-tool behavior / verification → that tool's `docs/tools/<tool>.md` ("Verified" section),
   - cross-cutting (hard rules, build/release pipeline, shared headers, new tools, carve-outs) → `docs/PRD.md` / `docs/CHECKLIST.md`,
   - decisions / experiments / trade-offs → append-only `docs/JOURNEY.md`; `JOURNEY.md` itself is never edited.
