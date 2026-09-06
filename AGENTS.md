# AGENTS.md — Agent Guide

Authoritative spec: **PRD.md**. Implementation status: **CHECKLIST.md**. Tool specs: `src/<tool>.c` -> `docs/<tool>.md` (`pin-to-top` | `find-my-mouse` | `color-picker` | `capture` | `pixel-view` | `ip-send` | `context-menu` | `ocr`) + `src/cmdx.ps1` -> `docs/cmdx.md`.

## HARD RULES

- No background services, registry usage (except per-user Explorer context menus managed by `context-menu` and User PATH in `HKCU\Environment` managed by `cmdx`), disk persistence, config files, env vars, or global system hooks.
- One process per invocation; click-through overlays only; no GUI windows except overlays and native dialogs.
- Must exit cleanly on toggle-off; compile with Clang (MSVC `clang-cl` or MinGW-w64 `clang`), never gcc. Strict Win32 C & GDI only (no DirectX).
- Always explain what changes you made and why to the user after editing or creating any file.
- **GIT RULE**: Agents must NOT run `git add`, `git commit`, or `git push` unless explicitly requested by the user.
- **BUILD RULE**: Agents must ONLY build the target corresponding to the changed file: `pwsh -File .\build.ps1 [-Mode release|debug] <target>` (`pin-to-top` | `find-my-mouse` | `color-picker` | `capture` | `pixel-view` | `ip-send` | `context-menu` | `ocr` | `all`). Outputs to `dist/release/` (default optimized) or `dist/debug/` (PDB/DWARF symbols). Supports target aliases (e.g. `pin`, `cursor`, `snip`, `pixelview`, `ip`, `contextmenu`).

## Shared Common Headers (`src/common/`)

- [`tiny_cli.h`](src/common/tiny_cli.h): Zero-heap, wide-character declarative command-line argument tokenizer & parser (`TinyCLI_ParseCommandLine`, `TinyCLI_Tokenize`, `TinyCLI_Parse`).
- [`tiny_ipc.h`](src/common/tiny_ipc.h): Single-instance mutex & event toggle pattern helper (`TinyIPC_AcquireOrToggle`).
- [`tiny_dpi.h`](src/common/tiny_dpi.h): Per-Monitor v2 DPI awareness initializer with legacy fallbacks.
- [`tiny_gui.h`](src/common/tiny_gui.h): Win32 GUI & overlay helpers (multi-monitor bounds `TinyGUI_GetVirtualScreenBounds`, alpha dimming `TinyGUI_ApplyDimOverlay`, border resize hit-testing `TinyGUI_HitTestResizeBorders`, scaled fonts).
- [`color-thief-algorithm.h`](src/common/color-thief-algorithm.h): Dynamic dominant wallpaper accent color extractor.

## IPC Contract Overview

Named objects follow `Global\Tiny<Tool>Mutex` and `Global\Tiny<Tool>Event` (see PRD §5 / `docs/<tool>.md`).
- First launch creates Mutex & Event (`manualReset=FALSE`).
- Second launch detects Mutex (`ERROR_ALREADY_EXISTS`), signals Event (TOGGLE), and exits immediately (`0`).

## Coding Conventions & Verification

- Plain C (C99), Win32 API (`W`-suffixed wide functions), entry point `WinMain` (GUI) or `wmain`/`main` (Console). Static helpers, `g_`-prefixed globals, `snake_case`.
- Error handling: any failure → clean exit.
- **Verification steps**:
  1. Build target via `pwsh -File .\build.ps1 <target>`.
  2. Smoke test double-launch toggle pattern (for overlay tools) or CLI execution (for console tools).
  3. Update docs:
    a. append [docs/CHECKLIST.md](docs/CHECKLIST.md) for behavior changes.
    b. append [docs/JOURNEY.md](docs/JOURNEY.md) for journey, experiments, descisions made, trade-offs.