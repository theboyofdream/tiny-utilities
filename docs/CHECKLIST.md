# CHECKLIST.md — Cross-Cutting Implementation Status

Global spec: **PRD.md** (hard rules + carve-out registry §21). Per-tool behavior **and** verification: `docs/tools/<tool>.md` ("Verified" section — the single owner of per-tool status). Design principles: `docs/PRINCIPLES.md`. Guide: `AGENTS.md`.

This file tracks **cross-cutting status only**: hard-rule compliance, build/release pipeline, shared infra. Per-tool verified state lives in the tool's own doc and is not duplicated here.

Legend: `[x]` done & verified, `[ ]` known gap, `[-]` not applicable.

## Tool index

| Tool | Status | Last verified | Spec |
|---|---|---|---|
| pin-to-top | stable | 2026-09-11 | `docs/tools/pin-to-top.md` |
| find-my-mouse | stable | 2026-08-25 | `docs/tools/find-my-mouse.md` |
| mouse-spotlight | stable | 2026-09-07 | `docs/tools/mouse-spotlight.md` |
| color-picker | stable | 2026-08-21 | `docs/tools/color-picker.md` |
| capture | stable | 2026-08-25 | `docs/tools/capture.md` |
| pixel-view | stable | 2026-08-21 | `docs/tools/pixel-view.md` |
| ip-send | stable* | 2026-10-17 | `docs/tools/ip-send.md` |
| context-menu | **Preview / Experimental** | 2026-08-24 | `docs/tools/context-menu.md` |
| ocr | stable | 2026-08-25 | `docs/tools/ocr.md` |
| window-switcher | stable | 2026-10-01 | `docs/tools/window-switcher.md` |
| mwb | stable (hardware matrix open) | 2026-10-17 | `docs/tools/mwb.md` |
| kyper | **Experimental** | 2026-10-26 | `docs/tools/kyper.md` |
| whisper-cast | **Experimental** | 2026-10-26 | `docs/tools/whisper-cast.md` |
| cmdx (script tool) | stable | 2026-09-04 | `docs/tools/cmdx.md` |

\* ip-send: last-known-good restored from `src/ip-send.c.bak` on 2026-10-17 after an uncommitted rewrite broke the composer (discarded rewrite preserved as `src/ip-send.c.rewrite`; details in ip-send.md).

## 1. HARD RULES (PRD §1)
- [x] No background services — one-shot process + bounded loop (PRD §21 exceptions: mwb tray, kyper/whisper-cast servers)
- [x] No registry — except PRD §21 (`context-menu`, `ip-send` menus; `cmdx` User PATH)
- [x] No disk persistence — no config files (exceptions: mwb read-only `mwb.txt`; ocr provisioning beside exe), no env vars
- [x] No global hooks — except PRD §21 (mwb server capture; window-switcher session hook)
- [x] One process per invocation
- [x] Overlays click-through — `WS_EX_LAYERED | WS_EX_TRANSPARENT`
- [x] No GUI windows except overlays + native dialogs (exceptions: ip-send composer, pixel-view viewer)
- [x] Clean exit on toggle-off — smoke-tested across tools
- [x] Clang only — clang-cl (A) and MinGW (B); never gcc

## 2. COMPILER / BUILD (PRD §2)
- [x] Both toolchains verified; `build.ps1 [-Mode release|debug] <target|all>` → `dist/release/` / `dist/debug/`
- [x] `mwb` macOS sibling built only on a Mac (`build_mac.sh`)

## 3. RUNTIME CONTRACT (PRD §8–§10)
- [x] GDI only, no DirectX; redraw on change only; loops ≤ 60 Hz (16 ms)
- [x] Failure → clean exit; overlay-creation failure non-fatal where reasonable
- [ ] CPU idle < 0.2% — design satisfies (event-wait sleeps); not re-measured
- [x] Memory < 10 MB per tool — per-tool values in each tool doc

## 4. SIZE REDUCTION & BUILD AUDIT (2026-08-21)
- [x] Size audit across all then-7 utilities (baseline in `utility_size_reduction_analysis.md`)
- [x] Release flags: `-flto -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables` (MinGW)
- [x] `color-thief-algorithm.h`: 64 KB `hist[]` → `GetProcessHeap()`
- [x] CRT string funcs → `wsprintfW`/`lstrlenW`/`lstrcatW` across 5 tools (per-tool results in tool docs)
- [x] Double-launch IPC contract re-verified

## 5. DECLARATIVE BUILD PIPELINE & SHARED HEADERS (2026-08-24)
- [x] `build.ps1` → declarative `$TARGET_CONFIGS` manifest; flags standardized (MinGW `-Os -flto ... -Wl,--gc-sections -Wl,-s`; clang-cl `/O1 /Gy /Gw /link /OPT:REF /OPT:ICF /MANIFEST:NO`)
- [x] Unsafe hacks removed (`-fno-unwind-tables`, `/GS-`) — 0 KB size cost
- [x] `src/common/` headers: `tiny_cli`, `tiny_ipc`, `tiny_dpi`, `tiny_gui`, `font`, `clipboard`, `tiny_config`, `tiny_http`, `tiny_tray`, `mdns`, `color-thief-algorithm` — each with a `docs/common/` doc
- [x] Target aliases + `build.ps1 all` green in release and debug

## 6. UNIFIED DECLARATIVE CLI HELP (2026-10-01)
- [x] `tiny_cli.h`: `valHint`/`description`, `--key=val`, boolean negation, `TinyCLI_HasHelp`/`FormatHelp`/`OutputHelp`/`CheckHelp*`
- [x] GUI output: redirected stdout → `AttachConsole` + `CONOUT$` → `MessageBoxW`
- [x] Options table defined once per tool (help + parsing share it); ip-send deduped ~100 lines
- [x] `--help` exit 0 across all 10 tools (line counts per tool doc)

## 7. RELEASE PIPELINE & VERSIONING (2026-10-02)
- [x] `release.yml`: `release` branch, `v*` tags, manual dispatch; root `VERSION` = source of truth
- [x] Assets: ZIP bundles (`tiny-utilities-{x64,arm64}.zip` + `shortcuts.ahk`) + standalone exes
- [x] `/MD` dynamic CRT: exes −70–89% (16–52 KB), bundle ~896 → ~180 KB
- [x] Packaging in `build.ps1 -Package`; `softprops/action-gh-release@v3`; markdown body
- [x] Version `1.0.1`

## Open items / notes
Cross-cutting only — per-tool open items live in that tool's "Verified" section.

- [ ] CPU idle % not re-measured (event-wait loop, expected < 0.2%)
- [ ] Overlay visuals (border/disc, click-through) verified only manually so far
