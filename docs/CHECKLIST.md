# CHECKLIST.md — Implementation Status

Behavior spec: **PRD.md** (authoritative). Guide: **AGENTS.md**.
Updated whenever behavior changes; verified steps are marked with dates/evidence.

Legend: `[x]` done & verified, `[ ]` known gap, `[-]` not applicable.

## 1. HARD RULES (PRD §1)
- [x] No background services — both tools are one-shot process + bounded loop
- [x] No registry usage — none anywhere
- [x] No disk persistence — no file I/O at all
- [x] No configuration files — config passed via CLI args only (Pin2Top)
- [x] No environment variables
- [x] No global system hooks
- [x] One process per invocation — single process model, no workers/threads
- [x] No multi-process coordination except IPC (§5)
- [x] Overlays click-through — `WS_EX_LAYERED | WS_EX_TRANSPARENT` + color-keyed pixels
- [x] No UI/GUI windows except overlays — no main/tool windows
- [x] Clean exit on toggle-off — verified by smoke test (both tools)
- [x] Clang only — both compile with clang-cl (A) and clang MinGW (B); never gcc

## 2. COMPILER / BUILD (PRD §2)
- [x] Builds via clang-cl: `clang-cl src\pin-to-top.c src\find-my-mouse.c user32.lib gdi32.lib -O2`
  (verified `build.ps1` Option A, 2026-08-13)
- [x] Builds via clang MinGW fallback: `--target=x86_64-w64-windows-gnu -mwindows`
  (verified Option B at development time)
- [x] `build.ps1` wrapper works: `pwsh -File .\build.ps1 all` (or target arguments `pin-to-top`/`find-my-mouse` / interactive menu) → `pin-to-top.exe` and `find-my-mouse.exe` OK
- [x] Only user32 + gdi32 linked (no shell32, no extra .lib)

## 3. PIN2TOP CLI (PRD §3)
- [x] Parser handles `--border-width N` (range 1–20)
- [x] Parser handles `--max-windows N` (range 1–10)
- [x] Unknown flags / missing values / non-integers → fall back to defaults
- [x] Out-of-range values → clamped to range
- [x] `Config { int borderWidth; int maxWindows; }` in code
- [x] No config files or env vars used
- [x] Defaults are tunable `#define`s at the top of pin-to-top.c: `DEFAULT_BORDER_WIDTH` (2),
      `DEFAULT_MAX_WINDOWS` (1), `DEFAULT_BORDER_COLOR` (RGB(0,120,215)), plus range
      bounds `BORDER_MIN/MAX` and `MAXWINDOWS_MIN/MAX` — updated 2026-08-13

## 4. STATE MODEL (PRD §4)
- [x] Pin2Top: per-window `{ target, overlay, lastRect }`, bounded array `g_pinned`
- [x] CursorFocus: active state = process lifetime + event loop

## 5. IPC CONTRACT (PRD §5)
- [x] Pin2Top mutex/event names match: `Global\TinyPin2TopMutex` / `Global\TinyPin2TopEvent`
- [x] CursorFocus names match: `Global\TinyCursorFocusMutex` / `Global\TinyCursorFocusEvent`
- [x] Second instance → `OpenEvent` + `SetEvent` (toggle) → exit `0`
- [x] Event is auto-reset, toggle-only
- [x] Verified: double-launch → second exits, first exits cleanly (2026-08-12)

## 6. PIN2TOP BEHAVIOR (PRD §6)
- [x] First launch: parse → mutex → event → pin foreground → overlay → loop
- [x] Toggle off tracked window → unpin + destroy overlay; exit when last unpinned
- [x] maxWindows limit: new pin ignored (existing windows unchanged) at cap
- [x] Pin/unpin via `SetWindowPos(HWND_TOP*NOTOPMOST)`
- [x] Overlay border thickness == `config.borderWidth`
- [x] Border color = dominant wallpaper accent color computed via `GetWallpaperDominantColorThief()`
      (`src/common/color-thief-algorithm.h` included directly in `pin-to-top.c`),
      falls back to `DEFAULT_BORDER_COLOR` — updated 2026-08-13
- [x] 16 ms tracking loop: drop removed windows, hide on `IsIconic`, move overlay
      only when `GetFrameRect` changes (DWM `DWMWA_EXTENDED_FRAME_BOUNDS`, loaded
      dynamically so no extra .lib; falls back to `GetWindowRect`) — updated 2026-08-13
- [x] Window destroyed while pinned → silently removed, overlay destroyed

## 7. CURSORFOCUS BEHAVIOR (PRD §7)
- [x] First launch: mutex → event → dim+glow overlay → loop
- [x] Second launch: signal toggle → exit; first deactivates → destroys overlay → exits
- [x] 16 ms loop: `GetCursorPos` → move glow window (only on cursor change, no redraw)
- [x] Spotlight = two layers: full-screen uniform-alpha dim (LWA_ALPHA, `DIM_ALPHA`=160)
      + tiny per-pixel flat white disc (`SPOT_ALPHA`=160, no border/gradient)
- [x] Focus-in animation: big faint disc (`PULSE_RADIUS_MAX`=170, alpha 40) centered
      on cursor shrinks to the steady disc (`SPOT_RADIUS`=100, `SPOT_ALPHA`=160) over
      400 ms (ease-in quadratic) — updated 2026-08-12
- [x] Focus-out animation: disc grows back to `PULSE_RADIUS_MAX` while fading to 0
      over 350 ms (ease-out quadratic), then process exits (click/key after grace, or toggle-off) — updated
      2026-08-12
- [x] Any mouse click or key press dismisses spotlight + exits (after 250 ms activation
      grace; polled via `GetAsyncKeyState`, no system hook) — verified 2026-08-12
- [x] Verified with smoke test (2026-08-12)

## 8. OVERLAY SYSTEM (PRD §8)
- [x] Styles: `WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW`
- [x] Pin2Top transparency: `SetLayeredWindowAttributes(KEY_COLOR, LWA_COLORKEY)` keying
- [x] CursorFocus transparency: uniform `LWA_ALPHA` dim layer + per-pixel alpha glow
      layer (`UpdateLayeredWindow` + `AC_SRC_ALPHA`); no color-keying
- [x] GDI only (no DirectX) — `FillRect` border for Pin2Top; 32-bpp DIB disc for CursorFocus
- [x] Redraw/move only on change; loop ≤ 60 Hz (16 ms)

## 9. ERROR HANDLING (PRD §9)
- [x] Failure → clean exit (no loops, no recovery) — handles released on all paths
- [x] Overlay creation failure non-fatal (pin without overlay where reasonable)

## 10. PERFORMANCE LIMITS (PRD §10)
- [x] Loop ≤ 60 Hz (16 ms wait in both main loops)
- [ ] CPU idle < 0.2% — design satisfies (event-wait sleeps); not re-measured post-change
- [x] Memory < 10 MB — measured: Pin2Top WS ≈ 5.82 MiB / priv ≈ 1.07 MiB,
      CursorFocus WS ≈ 7.31 MiB / priv ≈ 1.62 MiB (2026-08-13)

## 11. AUTOHOTKEY CONTRACT (PRD §11)
- [x] `#^t::Run("pin.exe --border-width 3 --max-windows 2")` — matches CLI
- [x] `#^c::Run("cursor.exe")` — no args required

## 12. FSM (PRD §12)
- [x] Pin2Top first/second-launch + event state machine matches §12.1
- [x] CursorFocus FSM matches §12.2 (unchanged)

## 13. FINAL GUARANTEE (PRD §13)
- [x] Deterministic CLI parsing, clang-only build, Win32-only, no undefined IPC,
      no hidden state, predictable overlays, multi-window limit enforced

## 14. CAPTURE TOOL (PRD §14)
- [x] Modes: `--fullscreen` (virtual desktop), `--window` (interactive window-picker overlay with zero-jank hover highlight, DWM frame bounds detection, title/size info badge, click/Enter selection, and Esc/right-click cancel), `--snip` (interactive selection overlay with rich ~49% dark translucent background dimming) — updated 2026-08-24
- [x] `--draw` annotation overlay: frameless top-level overlay (`WS_POPUP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW`) rendering a translucent dimmed desktop backdrop around the captured snip/window (replacing solid pitch-black background) with 100% crisp focus on the active drawing canvas; hidden by default while annotating, `Space` toggles toolbar visibility cleanly; `--toolbar` / `--draw-toolbar` / `--show-toolbar` CLI flag to show/hide toolbar on startup (defaults to `false`); subtext shortcut key badges rendered below every control (`R`, `Y`, `B`, `G`, `K`, `W`, `2`, `4`, `8`, `1`, `Z`, `X`, `C`, `S`, `Enter`, `Esc`, `Space`); direct single-key shortcut handlers; `--font-size N` CLI arg for proportional toolbar scaling (range 8..48, default 13); per-monitor toolbar centering (never straddling monitor seams) and screen-coordinate preservation for snip & window capture modes across multi-monitor setups (updated 2026-08-24)
- [x] Output: `--clipboard` (copy CF_BITMAP to clipboard) and/or `--save <path_or_dir>` (file/directory output) with mode-specific filenames (`snip_YYYYMMDD_HHMMSS.<ext>`, `capture_YYYYMMDD_HHMMSS.<ext>`, or `<appname>-<title>_YYYYMMDD_HHMMSS.<ext>`)
- [x] `--format png|bmp` CLI argument: defaults to `png` (e.g. `capture_YYYYMMDD_HHMMSS.png`), supports optional `bmp`
- [x] Composable flags: all modes and output/annotation options are independent (e.g. `capture.exe --snip --draw --clipboard --save "D:\Screenshots"`)
- [x] Single instance IPC: `Global\TinyCaptureMutex` and `Global\TinyCaptureEvent` toggle cancel support
- [x] Build rule: agents build only changed file target (`pwsh -File .\build.ps1 capture`)

## 15. COLORPICKER MULTI-FORMAT TOOLTIP & SHORTCUT KEYS
- [x] Multi-format floating tooltip card: displays clean color representations (`#023B32`, `2, 59, 50`, `169, 94%, 10%`, `96, 0, 4, 80`) simultaneously in an aligned 3-column layout below a top horizontal color swatch bar.
- [x] Dynamic auto-sizing: `RecalculateTooltipSize` measures exact text extents on each cursor update before calling `SetWindowPos`, eliminating any window boundary clipping on any monitor or font size.
- [x] Direct shortcut keys & CSS copied values: pressing keys `1` through `4` (or Numpad `1`-`4`) copies CSS-compatible color strings (`HEX` `#023B32` 1, `RGB` `rgb(2, 59, 50)` 2, `HSL` `hsl(169, 94%, 10%)` 3, `CMYK` `cmyk(96%, 0%, 4%, 80%)` 4) to the Windows Clipboard (`CF_UNICODETEXT`) and exits immediately. Shortcuts are rendered without brackets in dim secondary text color.
- [x] Click & default output: Left-click copies configured format (or default `HEX`), Right-click / ESC / toggle event cancels without copying.
- [x] Build rule verified: `pwsh -File .\build.ps1 color-picker` succeeds cleanly.

## 16. PIXEL-VIEW TOOL
- [x] Win32 / C native frameless overlay window with zero external web runtimes.
- [x] Supports PNG, JPG, BMP, and animated GIF decoding via Windows Imaging Component (WIC).
- [x] Retro Game Detail Preservation Engine: constant pixel scale (`pixelScale`, default **2px**) ensures expanding the window preserves & reveals more image details with sharp 1:1 chunky pixel block edges.
- [x] 100% nearest-neighbor point sampling with hard pixel edges — zero bilinear, bicubic, or anti-aliasing interpolation.
- [x] Initial window width defaults to **400px** (aspect ratio preserved).
- [x] Preserves source image/GIF aspect ratio by default across window resizing.
- [x] Animated GIF playback: per-frame disposal methods, frame offsets, and frame delays.
- [x] Background modes: Pixelated Gradient (default grid blocks), Linear Gradient, Solid Color, and Transparent.
- [x] Left-click + drag moves window (`WM_NCHITTEST` `HTCAPTION`). Resizable borders enabled (`HTTOPLEFT`, `HTRIGHT`, etc.).
- [x] Right-click context menu: Open File (`Ctrl+O`), Background Submenu, Pixel Scale / Detail Submenu (1px, 2px Default, 3px, 4px, 8px), Close (`Esc`).
- [x] Keyboard shortcuts: `Ctrl + O` opens file dialog.
- [x] CLI arguments: `pixel-view.exe --file <image_or_gif> [--pixel-size N] [--ratio 400x400]` (also positional, `--bg`, `--grid`).
- [x] Single instance IPC: `Global\TinyPixelViewMutex` and `Global\TinyPixelViewEvent` toggle support.
- [x] Build rule verified: `pwsh -File .\build.ps1 pixel-view` builds `dist/release/pixel-view.exe` cleanly.

## 17. SEND VIA IPMSG TOOL
- [x] Win32 / C native frameless dark TUI composer window restyled to ASCII-drawn terminal aesthetic — updated 2026-08-21:
      near-black canvas `#121212`, dashed hairline outer box (`+ - - +` corners/edges drawn with GDI pens) replacing solid borders,
      centered bracketed title `[ SEND VIA IPMSG ]` in accent blue `#4A90E2` sitting on the top border line,
      dim gray text `#777777`, active/bright text `#EEEEEE`, monospace font with fallback chain
      (JetBrains Mono → Fira Code → Cascadia Code → Cascadia Mono → Consolas), DPI-scaled via `SetProcessDPIAware`.
- [x] Layout follows ASCII TUI structure: payloads as `@ name` + 50%-dimmed path below, message line (`> msg_` insert mode),
      `/ query_` search line, recipient rows indented under search as `> ✓ PC-01` (U+2713 checkmark for selected; no `[x]`
      checkbox, no IP/host details), empty states "No message. Press 'i' to insert" / "No users found. Press 'r' to refresh".
      Highlighted row = full-width inverted accent-blue fill with cursor/check/name repainted in void black for high contrast.
- [x] Resizable window — updated 2026-08-21: default size reduced to 500x400 (was 640x520), `WS_POPUP | WS_THICKFRAME`
      with `WM_NCCALCSIZE` returning 0 (frameless look retained), 8px edge hit-test zones for drag-resize (corners + edges).
      No min/max size constraints (`WM_GETMINMAXINFO` removed); `WM_SIZE` forces repaint; recipient list rows scale
      dynamically (1..16) with client height via `AppState.visibleRows`.
- [x] Header key hints removed from the title bar; `/` key now opens a native Windows message box (`MessageBoxW`,
      "IPMsg - Keyboard Shortcuts") listing all shortcuts — updated 2026-08-21.
- [x] Message loop toggle responsiveness fix — updated 2026-08-21: replaced blocking `GetMessage` loop with
      `MsgWaitForMultipleObjects` (100 ms timeout) + `PeekMessageW` pump so a second launch's TOGGLE signal exits the
      first instance within ~100 ms even when idle (verified: double-launch smoke test first-running=True, after-toggle-running=False).
- [x] Explorer context menu trigger & CLI commands: `ip-send.exe --context-menu register` / `--context-menu true` (adds `Send via IP-send` context-menu entries under HKCU `*`, `Directory`, and `Directory\Background`, automatically purging legacy `Send via IP`, `IP Send`, & `Send via IPMsg` entries) and `--context-menu unregister` / `--context-menu false`.
- [x] Capture: positional CLI args or `--file <path>` pre-populate selected files or text as initial payload content (`> report.pdf`, `C:\Work\report.pdf`).
- [x] Add content without visible "Add" UI: `Ctrl+O` Open File dialog, Drag & Drop (`WM_DROPFILES`), and Paste (`Ctrl+V` for `CF_HDROP` files or `CF_UNICODETEXT`).
- [x] Message section: hidden by default when empty; `Insert` key enters edit mode (`> Fixed this issue._`), `Esc` exits insert mode. `Enter` in message mode inserts a new line. Supports automatic word wrapping (`DrawTextW` with `DT_WORDBREAK | DT_EDITCONTROL`) and dynamic window height auto-resizing (`SetWindowPos`) as text grows/shrinks. Typing names with 'i' (e.g. 'iris') in search mode works cleanly without accidentally triggering message mode. Space key toggles recipient selection without appending ' ' to the search query.
- [x] Composer: Single small native frameless dark TUI window (no `WS_EX_TOPMOST`), handling `WM_NCACTIVATE` to prevent white border on focus loss, top-left "Send" title in dim text (`textDim`) positioned inside the unbroken top border, minimum top container height (`44px`) for payloads/message, and dynamic auto-sizing (`SetWindowPos`) to eliminate extra empty space below the `[SEND]` button. Unified 12pt font size (`-MulDiv(12, dpi, 96)`) across all UI text elements.
- [x] Recipients: active search field by default (`/ pc_`), clean multi-select recipient list displaying strictly 4 users at a time with smooth arrow-key scrolling (`↑`/`↓`), cursor (`>`), checkmark (`✓`), user display names with symmetrical 20px top/bottom padding and 26px row spacing, `r` key shortcut to refresh online recipients (displaying `Refreshing... Please wait` state and removing it when done), and `/` key shortcut to trigger a clean popup with all keyboard hints (without icon). Highlighted user text and cursor (`>`) display in vibrant accent blue (`#4A90E2`) without background color fill.
- [x] Footer & Send: `To: PC-01, LAPTOP-03` summary line (`To: none` when empty) and `[SEND]` button. When no recipients are selected, the `[SEND]` button is dimmed and auto-selection is disabled; attempting to send displays a warning status (`Warning: No recipients selected. Press Space to select.`).
- [x] Backend: detects IP Messenger (`ipcmd.exe` / `ipmsg.exe`) via `SearchPathW`, Registry App Paths, and standard installation directories (`Program Files`, `LocalAppData`). Displays native Windows error dialog (`MessageBoxW` with `MB_ICONERROR`) if not installed. If installed but background process is disabled/not-running, automatically starts the IP Messenger process and displays a warning status (`Warning: IP Messenger process was not running. Started background process.`). Dispatches via `<path> send /to:<recipients> [/msg:<message>] [files...]`.
- [x] CLI automation: `--to`, `--message`/`--msg`, `--file`, `--send` pre-populate fields and support direct automated sending.
- [x] Single instance IPC: `Global\TinyIPSendMutex` and `Global\TinyIPSendEvent` toggle support. When multiple files are selected in Explorer (spawning concurrent instances), subsequent instances forward payload arguments via `WM_COPYDATA` to the active window instead of triggering toggle-off.
- [x] Build rule verified: `pwsh -File .\build.ps1 ip-send` builds `dist/release/ip-send.exe` cleanly.

## 18. CONTEXT MENU UTILITY
- [x] Binary target: `context-menu.exe` native C / Win32 application.
- [x] CLI interface: `context-menu.exe --update <config.xml>` (applies XML config immediately) and `context-menu.exe --backup <backup.xml>` (exports managed entries to XML).
- [x] Double-click flow: double-clicking without CLI flags opens a native Windows choice dialog with options: **Update Context Menu**, **Backup Context Menu**, and **Cancel**.
- [x] Update GUI flow: select `.xml` via native `GetOpenFileNameW` → complete pre-flight XML validation → apply to Registry immediately.
- [x] Backup GUI flow: choose destination via native `GetSaveFileNameW` → export managed registry entries to XML immediately.
- [x] No confirmation dialogs: executes immediately upon file selection.
- [x] Unlimited nested submenus: `<item>` can contain child `<item>` elements recursively via Windows native `SubCommands = ""` cascading registry keys.
- [x] Actions: supports `command`, `arguments`, and optional `icon` path.
- [x] Windows placeholders: supports `%1` (file path) and `%V` (directory path).
- [x] Registry hive: per-user `HKCU\Software\Classes` default storage.
- [x] Safety & validation: complete XML loading, parsing, and structure validation before touching Registry. Invalid XML aborts with `stderr` error message and non-zero exit code (CLI) or error dialog (GUI).
- [x] Idempotency: re-running `--update` produces identical registry state.
- [x] Isolation: marks managed top-level keys with `ManagedBy = "context-menu"`. Non-managed registry entries are left untouched.
- [x] Single instance IPC: `Global\TinyContextMenuMutex` and `Global\TinyContextMenuEvent`.
- [x] Build rule verified: `pwsh -File .\build.ps1 context-menu` builds `dist/release/context-menu.exe` cleanly.

## 19. SIZE REDUCTION & BUILD OPTIMIZATION AUDIT (2026-08-21)
- [x] Comprehensive size audit performed across all 7 utilities: documented baseline binary sizes in `utility_size_reduction_analysis.md`.
- [x] Updated `build.ps1` release flags: added `-flto -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables` for Clang MinGW release target to strip unused SEH unwind tables and perform link-time optimization.
- [x] Shared header optimization: updated `src/common/color-thief-algorithm.h` to allocate `hist[32768]` histogram buffer on `GetProcessHeap()` instead of static BSS stack memory, saving 64 KB of static uninitialized memory bloat across binary targets.
- [x] Phase 2 Clean Win32 C Source Refactoring: replaced CRT `swprintf`/`swprintf_s` calls across `color-picker.c`, `capture.c`, `ip-send.c`, `context-menu.c`, and `pixel-view.c` with native Win32 `wsprintfW`, `lstrlenW`, and `lstrcatW` while preserving 100% human-readable C code. Reduced `pixel-view.exe` from 174.5 KB to **144.0 KB** (26.5 KB reduction).
- [x] Verified double-launch IPC toggle contract across executables.

## 20. CMDX TOOL (`src/cmdx.ps1` -> `cmds/`)
- [x] Single-file dynamic global command registry and dispatcher in `src/cmdx.ps1`.
- [x] Dual-shim generation (`.cmd` and `.ps1`) in dedicated `cmds/` folder.
- [x] User PATH environment variable registration (`HKCU\Environment`, `Path`), broadcasting `WM_SETTINGCHANGE` and refreshing session `$env:Path` in-place.
- [x] Dynamic regex-key expansion into all candidate alias names with capture variables `$1`, `$2`, etc. passed to ScriptBlocks.
- [x] Native in-session environment refreshing for `refreshenv.cmd` and `refreshenv.ps1`.
- [x] CLI commands: `cmdx setup` (register PATH & generate shims), `cmdx clean` (remove shims & unregister PATH), `cmdx help` / no arguments (show help manual and list aliases).
- [x] Dynamic dispatch: editing `src/cmdx.ps1` changes command behaviors immediately without regeneration.

---

## 21. OCR TOOL (src/ocr.c -> ocr.exe)
- [x] CLI & Interactive Picker: `ocr.exe [--image] <png|jpg|jpeg|bmp> [--lang eng|auto] [--psm 6] [--oem 1]`; if no image flag or positional path is passed, opens native Win32 File Explorer picker (`GetOpenFileNameW` via `comdlg32`) and exits cleanly on cancel — verified `ocr.exe --help` exit 0, missing file exit 1, invalid format exit 2
- [x] Always portable: `ocr.exe` + `ocr-resources/tesseract/tesseract.exe` + `tessdata/*.traineddata` beside exe via `GetModuleFileNameW`, no `%APPDATA%`/`HKLM`, delete folder to uninstall
- [x] Smart provisioning: `ensure_tesseract_runtime` validates `>10KB`, downloads `zstrathe` portable `main.zip` (4MB) via `urlmon`→`curl`→`powershell` fallback, `Expand-Archive`/`tar`, `copy_dir_contents`, no elevation; `ensure_lang_file` lazy `tessdata_fast` for `hin`/`tam`/`tel`/`osd`
- [x] OSD auto: `--lang auto` (default) scans available language models with fallback to `eng+hin+tam+tel`
- [x] Tight letters: WIC `2-3x` upscale (`Fant`), grayscale conversion, WIC PNG encoding
- [x] Output: copies recognized text to Windows Clipboard (`CF_UNICODETEXT`), prints formatted UTF-8 text to `stdout`, and displays native alert / notification dialog (`MessageBoxW`) if launched interactively
- [x] Build: `pwsh -File .\build.ps1 ocr` → `dist/release/ocr.exe` GUI subsystem `-mwindows` (no black terminal window on double-click), libs `user32/shell32/ole32/urlmon/shlwapi/windowscodecs/gdi32/comdlg32` — verified 2026-08-25

## 22. DECLARATIVE BUILD PIPELINE & SHARED HEADERS (2026-08-24)
- [x] Declarative build manifest: refactored `build.ps1` to use an ordered hashtable `$TARGET_CONFIGS` replacing procedural `if-else` cascades with structured subsystem, library, and extra flags mappings.
- [x] Compiler flag standardization: standardized MinGW Clang (`-Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-s`) and MSVC `clang-cl` (`/O1 /Gy /Gw /link /OPT:REF /OPT:ICF /MANIFEST:NO`) flags across all 9 utilities.
- [x] Removed unsafe compilation hacks: removed `-fno-unwind-tables` and `/GS-` hacks, eliminating SEH exception crash risks on 64-bit Windows while preserving identical binary release sizes.
- [x] Shared common headers in `src/common/`:
  - `tiny_cli.h`: Zero-heap wide-string command line tokenizer and declarative argument parser (`TinyCLI_ParseCommandLine`, `TinyCLI_Tokenize`, `TinyCLI_Parse`).
  - `tiny_ipc.h`: Single-instance mutex and toggle event IPC helper (`TinyIPC_AcquireOrToggle`).
  - `tiny_dpi.h`: Per-Monitor v2 DPI awareness initializer with fallback chain.
  - `tiny_gui.h`: Win32 GUI & overlay helper library providing `TinyGUI_GetVirtualScreenBounds`, `TinyGUI_ApplyDimOverlay`, `TinyGUI_HitTestResizeBorders`, and `TinyGUI_CreateScaledFont`.
- [x] Integrated `tiny_cli.h` into `src/pin-to-top.c`: reduced 60+ lines of custom tokenizer boilerplate to a concise declarative option array while maintaining exact binary size (23,552 bytes).
- [x] Integrated `tiny_gui.h` & `tiny_dpi.h` into GUI utilities: modernized `src/capture.c`, `src/pixel-view.c`, `src/find-my-mouse.c`, and `src/color-picker.c`, eliminating duplicated virtual screen metrics, alpha blending blitters, DPI initializers, and border hit-testing routines with 0 KB bloat.
- [x] Target aliases & build validation: verified `pwsh -File .\build.ps1 all` in release and debug modes, and validated aliases (`ip`, `globalcmd`, `pin`, `cursor`, `snip`, `pixelview`, `contextmenu`).

## 23. CAPTURE INTERACTIVE MODE SELECTOR & SAVE DIALOG (2026-08-25)
- [x] Zero-Flicker Unified Overlay Engine: single top-level overlay window (`TinyUnifiedCaptureClass`) manages the entire lifecycle across states (`STAGE_SELECTOR`, `STAGE_SNIP`, `STAGE_WINDOW_PICK`, `STAGE_ANNOTATE`) with zero window destruction/re-creation between steps for 100% flicker-free transitions.
- [x] Centered Group Overlay Selection (ip-send Style, No Window): renders centered as a group both vertically and horizontally on screen with prominent scaled 22px text and clear ~14px hint text (without an enclosed window card). Text lines are left-aligned in fixed sub-rectangles with zero horizontal text shift when hovered (`> 1. Snip (Region)`, `  2. Window`, `  3. Full Screen`) with vibrant `accentBlue` (`#60A5FA`) highlight on the active choice.
- [x] Keyboard & Mouse Navigation: supports click, hover feedback with hand cursor (`IDC_HAND`), mouse leave tracking, arrow key focus switching (`Up`/`Down`/`Left`/`Right`/`Tab`), `Enter`/`Space` activation, direct shortcut keys (`1`/`S`, `2`/`W`, `3`/`F`), and `Esc` / right-click clean cancellation.
- [x] Seamless State Transitions: selecting a mode immediately morphs the overlay in-place into region rubberbanding (reusing desktop backdrop), window picking, or doodle annotation canvas with floating toolbar visible.
- [x] Save File Dialog on Enter/Save: in draw mode, pressing `Enter` ("Done"), clicking `Save`, or pressing `S` opens the native Windows Save File Dialog (`GetSaveFileNameW` via `comdlg32`) to save the PNG image whenever no explicit `--save <path>` CLI option was passed (and saves directly without prompt if `--save` was passed), in addition to placing the image on the Windows Clipboard.
- [x] Single-Instance Toggle: respects `Global\TinyCaptureMutex` and `Global\TinyCaptureEvent` toggle cancellation across all interactive overlay states.
- [x] Build rule verified: `pwsh -File .\build.ps1 capture` builds `dist/release/capture.exe` cleanly.

## 24. SHARED FONT & CLIPBOARD COMMON MODULES (2026-08-25)
- [x] Simplified single-header common modules (`src/common/font.h`, `src/common/clipboard.h`):
  - Consolidated implementation entirely inside header files using `static inline` functions; deleted all `.c` files in `src/common/`.
  - Primary font face: `JetBrains Mono` (`TINY_FONT_FACE_JETBRAINS`) with fallback chain (`Consolas`, `Fira Code`, `Cascadia Code`, `Cascadia Mono`, `Courier New`, system monospace).
  - Standard UI font face: `Segoe UI` (`TINY_FONT_FACE_SEGOE`) with fallback chain (`Tahoma`, `Arial`).
  - Probes system font installation via `TinyFont_IsAvailable` using re-entrant thread-safe `EnumFontFamiliesExW` callbacks.
  - Physical size typography consistency across monitors (`TinyFont_ScaleSize`): scales font height dynamically via `MulDiv(fontSize, dpi, 96)` so typography maintains the exact same real-world physical size (in inches / millimeters) between high-DPI laptop displays and desktop monitors.
  - OS Default Font Size Query (`TinyFont_GetSystemDefaultSize`): queries `SPI_GETNONCLIENTMETRICS` (`ncm.lfMessageFont.lfHeight`) to match OS native font defaults.
  - Runtime Font Zoom Controls (`TinyFont_HandleZoomKey`): supports `Ctrl +` (increase font size), `Ctrl -` (decrease font size, clamped to 8..48px), and `Ctrl 0` (reset to OS default font size) consistently across interactive overlay utilities.
  - Renamed parameter `pointSize` -> `fontSize` across all `TinyFont_Create*` and `TinyGUI_CreateScaledFont` routines for design transparency.
  - Centralized font size constants (`TINY_FONT_SIZE_MIN`..`MAX`, `XS`, `SMALL`, `DEFAULT`, `NORMAL`, `MEDIUM`, `LARGE`, `XLARGE`).
  - Centralized font weight constants (`TINY_FONT_WEIGHT_LIGHT`, `NORMAL`, `MEDIUM`, `SEMIBOLD`, `BOLD`).
  - Centralized UI color constants (`TINY_COLOR_BG_DARK`, `TEXT_BRIGHT`, `TEXT_MUTED`, `ACCENT`, `BORDER`, `SELECTION`).
  - DPI-scaled GDI font constructors: `TinyFont_Create`, `TinyFont_CreateMonospace`, `TinyFont_CreateUI`, `TinyFont_ScaleSize`.
  - Font text extent measurement helpers: `TinyFont_MeasureText`, `TinyFont_GetFontHeight`.
  - Robust Unicode text helpers: `TinyClipboard_SetText` (`CF_UNICODETEXT` with retry loop and clipboard locking protection), `TinyClipboard_GetText`, `TinyClipboard_GetTextAlloc`, `TinyClipboard_HasText`.
  - Bitmap clipboard helper: `TinyClipboard_SetBitmap` (`CF_BITMAP` with `CopyImage`).
  - File drop clipboard query: `TinyClipboard_HasFiles` (`CF_HDROP`).
- [x] Capture Draw Toolbar UX Upgrades (`src/capture.c`):
  - Segmented floating pill container (`RGB(22, 24, 28)`) with subtle vertical dividers between Title, Colors, Pen Widths, History, and Actions.
  - Interactive mouse hover tracking (`GetToolbarHoverItem`) with responsive hover backgrounds, glowing borders, and hand cursor (`IDC_HAND`).
  - Active color dot double-ring glow (`RGB(37, 99, 235)` outer ring, `RGB(255, 255, 255)` inner ring) and active pen width filled chips.
  - Centered monospace shortcut badges (`Z`, `X`, `C`, `S`, `Enter`, `Esc`, `1`-`4`, `R`, `Y`, `B`, `G`, `K`, `W`).
  - Sleek bottom-right floating shortcut pill when toolbar is hidden.
- [x] Find My Mouse Dynamic Monitor DPI Scaling (`src/find-my-mouse.c`, `src/common/tiny_dpi.h`):
  - Dynamic monitor DPI query via `TinyDPI_GetDpiForPoint(cur)` and `TinyDPI_GetDpiForMonitor(hMon)`.
  - Calculates radius dynamically as `MulDiv(BASE_SPOT_RADIUS, dpi, 96)` so that physical diameter on screen remains constant (~1.56 inches / ~40 mm) across 100% desktop monitors and 150%/200% high-density laptop displays.
  - Dynamically redraws and scales when the cursor crosses displays of different DPIs.
- [x] IP Send Active Monitor Positioning & Light Theme (`src/ip-send.c`):
  - Windows position dynamically centers on active monitor containing cursor/focused window (`MonitorFromPoint` + `rcWork`).
  - Added runtime Light Theme toggle with `Ctrl + D` with clean high-contrast light palette and updated `/` keyboard shortcut help.
- [x] Pin to Top Interactive Window Selection Picker (`src/pin-to-top.c`):
  - Added `--window-selection true|false` CLI argument (defaults to `true`).
  - First launch / double click triggers interactive fullscreen selection overlay (`capture --window` style).
  - Hovering top-level windows highlights frame with dominant wallpaper accent color and title badge (`[📌 Pin] App — Title`).
  - Left click pins/unpins target window, creating/destroying tracking border overlay.
- [x] IP Send Button Overlap & Resizing Height Fixes (2026-09-04):
  - Fixed Send button visibility when multiple recipients are selected by constraining `To:` summary rendering with `DT_END_ELLIPSIS` and giving `Send` a dedicated styled button rect.
  - Eliminated window height "flinch back" on resize by removing redundant `SetWindowPos` from `RenderTUIWindow`, dynamically calculating visible list rows from available vertical space, adding `WM_GETMINMAXINFO` (min 360x260), `WM_MOUSEWHEEL` scrolling, and row click selection.
  - Added offline recipient selection guard: prevented spacebar and click selection of offline users (`!r->active`), kept clean dim appearance without extra label clutter, and added auto-dismissing toast-style warning messages (cleared via 3s timer).
- [x] Tiny Window Switcher (`src/window-switcher.c`, `docs/window-switcher.md` — updated 2026-09-08):
  - Created native Win32/DWM keyboard-driven window switcher utility.
  - Real UWP / Store App & Process Metadata Resolution: resolves actual application name via `GetExeMetadataName` (`FileDescription` / `ProductName` from Win32 `GetFileVersionInfoW` / `VerQueryValueW`) and `GetUWPRealProcessId` (child `Windows.UI.Core.CoreWindow` PID query + title parsing for `ApplicationFrameHost.exe` host process), accurately distinguishing Chromium/Electron apps (e.g. `Helium -> H`, `Calculator -> C`, `Clock -> CL`).
  - Top header text baseline alignment precisely matching grid container margins (`gridLeft`), resolving `LAYOUT_FULL` alignment offset.
  - Shortest Unique Prefix/Subsequence hint generation (`Chrome -> C`, `Code -> CO`, `Chromium -> CH`, `Zen -> ZN`, `Zed -> ZD`) with candidate prioritization (base + extension e.g. `Notepad -> N`, `Notepad+ -> N+`, word initials e.g. `File Pilot -> FP`, shared 2-letter prefix distinguishing characters e.g. `Zen vs Zed -> ZN vs ZD`, 1-letter hints, 2-letter prefixes), window sub-indexing (`C1`, `C2`), and hint prefix cycling.
  - Dynamic Search Filter, Session Low-Level Hook & Tiered Priority Auto-Activation: typing characters into search filter (`g_typedBuf`) uses tiered matching (`GetFilteredIndices`) prioritizing App Name and Hint prefix/word matches (Tier 1 & 2) over arbitrary title substring matches (Tier 3). Installs a session-lifetime low-level keyboard hook (`WH_KEYBOARD_LL`, unhooked cleanly on exit) to reliably intercept system keys (`Alt`, `Tab`, `Shift+Tab`, arrows, prefix hints, `Esc`, `Enter`). Holding `Alt` pauses the auto-activation timer; releasing `Alt` starts/restarts the 300 ms delay countdown (default `300` ms, configurable via `-d, --delay <ms>`). Every keystroke resets the 300 ms commit timer back to zero. Subsequent invocations from AHK while `Alt` is held advance window selection (`Tab` cycle) via IPC event handling instead of closing. Unmatched DWM live window thumbnails are hidden explicitly (`fVisible = FALSE`).
  - Default sort order set to `name` (`SORT_NAME`); grouping logic (`--group`), `dim`, and `none` background modes removed (`bgMode` supports `blur` or `tint`).
  - Transparent Tint & Acrylic Blur: replaced legacy tint opacity with hexadecimal `--tint-color #RRGGBBAA` parsing (`ParseHexColorWithAlpha`), supporting values like `#00000005` or `#FFFFFF50` for true translucent GDI tinting; `--blur` (1..100) maps 1..30 to Aero light glass blur and 31..100 to Acrylic frosted blur.
  - Centered Grid Container Layout (`--layout center` default, floating cards directly on screen backdrop without outer container box/border), or `--layout full` (100% display edge-to-edge expansion with status header text aligned precisely to column 0 left boundary). Unified proportional physical scaling (configurable at the top of `src/window-switcher.c` via `UI_CARD_BASE_WIDTH`, `UI_CARD_ASPECT_RATIO_W`/`H`, `UI_CARD_GAP`, `UI_CONTAINER_PADDING`, `UI_HEADER_HEIGHT`, `UI_FONT_SIZE_HINT`, `UI_FONT_SIZE_TITLE`) scales typography and card dimensions synchronously across display DPIs (`scale = dpi / 96.0f`), guaranteeing identical physical size and proportions between high-DPI laptop displays and desktop monitors, while dynamic column calculation in `--layout full` caps columns (3–8) so cards maintain target physical sizing without stretching on large monitors.
  - Robust single-window rendering architecture (`g_hwndOverlay`), reusing `font.h` header functions (`TinyFont_GetBestUIFace`), positioning the borderless standard-size shortcut hint badge (`g_hFontHint` 15pt bold) on the RIGHT side of card headers, followed by an on-hover / on-focus red cross close icon (`✕`) with no background, allowing instant graceful window closing (`WM_CLOSE`) or `Shift + Click` force termination (`TerminateProcess` / `EndTask`) while keeping the switcher overlay open, dynamically refreshing remaining cards, hints, and thumbnails, with foreground deactivation protection (`g_lastCloseTime`).
  - Verified `pwsh -File .\build.ps1 window-switcher` compiles cleanly in release and debug modes with zero errors. Smoke tested double-launch IPC toggle contract.

## 25. MOUSE SPOTLIGHT TOOL (`src/mouse-spotlight.c` -> `mouse-spotlight.exe` — updated 2026-09-07)
- [x] Native Win32 presentation overlay tool: dims screen except for a customizable spotlight hole centered on mouse cursor.
- [x] Real-time tracking: 60 FPS loop (16 ms step) updates spotlight position and size smoothly as cursor moves across displays.
- [x] Customization CLI flags (`tiny_cli.h`): `--size N` / `-s N` (base radius at 96 DPI, default 150, range 20..1000) and `--dim N` / `-d N` (background dimming percentage 0..100% or raw alpha 0..255, default 65% / 165 alpha).
- [x] Interactive hotkeys & dismissal: `Ctrl +` / `Numpad +` (enlarge spotlight), `Ctrl -` / `Numpad -` (shrink spotlight), `Ctrl 0` / `Numpad 0` (reset size to startup value). Pressing `Esc`, any non-hotkey key, or mouse click exits cleanly (following `find-my-mouse` UX conventions after 250ms activation grace period).
- [x] Multi-monitor DPI awareness (`tiny_dpi.h`): `TinyDPI_EnablePerMonitorAwareness()` + `TinyDPI_GetDpiForPoint(cur)` dynamically scales spotlight radius (`MulDiv(baseSize, dpi, 96)`) so physical size remains identical on high-DPI and standard displays.
- [x] Zero-Lag Per-Pixel Alpha DIB Renderer (`UpdateLayeredWindow`): full virtual desktop layered window (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`) with 32-bpp top-down DIB section, partial bounding-box clears, and smooth 6px anti-aliased soft edge feathering. Eliminates `SetWindowRgn` DWM region recalculation lag for silky smooth 60+ FPS tracking.
- [x] Click-through overlay: `WS_EX_TRANSPARENT` allows mouse clicks, drags, typing, and app interaction to pass through seamlessly during presentations.
- [x] Single-Instance IPC Toggle (`tiny_ipc.h`): `Global\TinyMouseSpotlightMutex` and `Global\TinyMouseSpotlightEvent`. Second launch signals toggle event and exits 0; running instance receives signal and exits cleanly.
- [x] Build rule verified: `pwsh -File .\build.ps1 mouse-spotlight` compiles cleanly in both release (`dist/release/mouse-spotlight.exe`) and debug (`dist/debug/mouse-spotlight.exe`) modes with exit code 0.

- [x] IP Send Native EDIT Control, Ctrl+Backspace & Dynamic Resizing (`src/ip-send.c` — updated 2026-09-07):
  - Converted the message input box from a raw GDI text append buffer to a native Win32 `EDIT` control (`ES_MULTILINE | ES_AUTOVSCROLL`).
  - Added native `Ctrl+Backspace` backward word deletion in `EditSubclassProc`, suppressing the standard Win32 `0x7F` DEL control character.
  - Implemented dynamic multiline message box height expansion (`DrawTextW` with `DT_CALCRECT`) in real-time as text is typed or multiline wrapped, updating child edit control bounds via `SetWindowPos` and repainting smoothly on `EN_CHANGE`.
  - Provides full standard text box behavior: caret positioning, mouse text selection, drag-and-drop, `Ctrl+A`, `Ctrl+C`, `Ctrl+V`, `Ctrl+X`, `Ctrl+Z`, `Backspace`, `Delete`, arrow key navigation, `Home`, `End`, and right-click context menus.
  - Dedicates the `<Insert>` key (and mouse clicking the message area) exclusively for entering/exiting message edit mode, removing `<Tab>` from toggling into the message box.
  - Seamlessly styled child edit control matching dark/light themes via `WM_CTLCOLOREDIT` with matching background and text colors.
  - Synchronizes typed message buffer on change, focus loss, and send dispatch.
- [x] IP Send Recipient Selection Preservation Across Refreshes (`src/ip-send.c` — updated 2026-09-11):
  - Preserves selected checks (`selected == true`) across live recipient refreshes (`r` key or `RefreshRecipientList`) by introducing `AreRecipientsEqual` matching by UID, Name/Hostname, and IP.
  - Parses incoming `ipcmd list /all` output into a temporary buffer (`s_tempRecipients`) before committing, safely retaining checked status for existing users and preserving selected custom/CLI recipients.
  - Stabilizes active cursor highlighting (`highlightedFilteredIdx` and `scrollOffset`) across list refreshes so cursor focus does not jump to a different user.
  - Updated selection toggling in both mouse click (`WM_LBUTTONDOWN`) and keyboard (`VK_SPACE`): users can always deselect an already selected contact even if their active status changes to offline.
- [x] Pin to Top Interactive Window Selection & IPC Lifecycle Fixes (`src/pin-to-top.c` — updated 2026-09-11):
  - Fixed premature process termination bug: removed `PostQuitMessage(0)` on picker overlay destruction (`WM_DESTROY`), resolving the issue where selecting any window immediately caused `WinMain` to receive `WM_QUIT`, unpin the window, destroy its border overlay, and terminate in 16 ms.
  - Migrated IPC single-instance toggle mechanism strictly to `tiny_ipc.h` (`TinyIPC_AcquireOrToggle`), eliminating manual mutex creation and invalid `ReleaseMutex` calls on unowned mutexes.
  - Refactored CLI argument parsing to declarative `TinyCLI_ParseCommandLine` (`tiny_cli.h`) with fallback support for `--key=val` formats.
  - Added cloaked window filtering (`DWMWA_CLOAKED`) and tool window filtering (`WS_EX_TOOLWINDOW`) to avoid selecting invisible/suspended background UWP windows or utility overlays.
  - Added FIFO replacement when pinning past `maxWindows` limit: unpins oldest pinned window so users can pin a new window without manually unpinning first.
  - Automatically terminates tracking loop and cleans up process when all pinned windows are destroyed by the user.
  - Added `CS_HREDRAW | CS_VREDRAW` and explicit repaint invalidation to border overlay window class so resized windows maintain clean, artifact-free borders.
  - Centered interactive picker prompt dynamically on active monitor containing cursor.
  - Verified `pwsh -File .\build.ps1 pin-to-top` compiles cleanly in release and debug modes with 0 errors/warnings and verified double-launch toggle IPC pattern.

## Open items / notes
- CPU idle % not re-measured; loop is event-wait based (sleeps 16 ms) so expected < 0.2%.
- Manual visual verification of overlay rendering (border/disc appearance, click-through)
  still recommended in a real desktop session.