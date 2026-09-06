# JOURNEY.md — Engineering Log & Architecture Journey

This file is an **append-only** log documenting the exact development history, architectural decisions, thought processes, technical challenges, and implementation milestones for `tiny-windows-utilities`.

> [!IMPORTANT]
> **Maintainer Guidelines**:
> - Append new entries at the bottom of this file in chronological order.
> - Do not alter or delete previous entries.
> - Each entry must include: Date/Timestamp, Context & Motivation, Thought Process & Design Trade-offs, Implementation Details, and Verification Results.

---

## [2026-08-10] Core Philosophy & Architectural Foundations

### Context & Goal
Modern desktop productivity tools often degrade into resource-heavy background daemons, Electron apps, or registry-heavy system tray applications that consume hundreds of megabytes of RAM when idle. The goal of `tiny-windows-utilities` is to build instant-launch, hyper-focused productivity utilities for Windows with **zero background footprint when idle**.

### Design Decisions & Hard Rules
1. **Zero Background Footprint**: No persistent background services, system tray daemons, environment variables, or registry keys.
2. **Single Process Per Invocation**: Each utility process runs strictly on demand and exits cleanly upon completing its task or when toggled off.
3. **Pure Win32 C99 & GDI**: Standard C99 compiled with Clang (`clang-cl` or LLVM `clang`). No DirectX, framework dependencies, or heavy runtime libraries.
4. **IPC Double-Launch Toggle Contract**:
   - First launch creates a named Mutex (`Global\Tiny<Tool>Mutex`) and a named manual-reset Event (`Global\Tiny<Tool>Event`).
   - Second launch detects the Mutex (`ERROR_ALREADY_EXISTS`), signals the Event to toggle off the active instance, and exits immediately (`0`).

---

## [2026-08-12] Pin2Top & CursorFocus (Find My Mouse) Architecture

### 1. Pin2Top (`pin-to-top.c`)
- **Thought Process**: Pinning a foreground window to stay topmost requires clear visual feedback (a custom border overlay) so users always know which window is pinned.
- **DWM Extended Frame Bounds**: Standard `GetWindowRect` includes invisible Aero glass drop-shadow margins on Windows 10/11. Dynamic-loaded `DwmGetWindowAttribute` with `DWMWA_EXTENDED_FRAME_BOUNDS` to align the border overlay with the physical window frame.
- **CLI Argument System**: Added optional `--border-width N` and `--max-windows N` using a custom, zero-dependency `GetCommandLineW` tokenizer (`SplitCommandLine`) to avoid linking `shell32.dll` (`CommandLineToArgvW`).

### 2. CursorFocus / Find My Mouse (`find-my-mouse.c`)
- **Thought Process**: Locate a lost mouse cursor instantly across multi-monitor setups without intrusive GUI windows.
- **Two-Layer Overlay Architecture**:
  - `g_dim`: Full virtual-screen dimming window using uniform alpha (`DIM_ALPHA`=160).
  - `g_glow`: Per-pixel 32-bpp top-down DIB section (`UpdateLayeredWindow`) creating a spotlight disc around the cursor position.
- **Animation & Dismissal**: Quadratic ease-in focus-in pulse (400 ms) shrinking to steady spotlight, and ease-out focus-out dismiss (350 ms). Polled `GetAsyncKeyState` with a 250 ms grace period to exit cleanly on key press or mouse click.

---

## [2026-08-13] Dominant Wallpaper Accent Color Sampling

### Color Thief Algorithm (`src/common/color-thief-algorithm.h`)
- **Thought Process**: Hardcoded border colors for `Pin2Top` felt disconnected from user desktop themes.
- **Solution**: Developed a header-only Win32 GDI implementation of the MMCQ (Modified Median Cut Quantization) Color Thief algorithm. Pin2Top samples the desktop wallpaper accent color at launch to match the system theme dynamically, falling back to `#0078D7` if sampling fails.
- **Memory Footprint**: Verified working set footprint remains minimal:
  - Pin2Top WS ≈ 5.82 MiB (Private ≈ 1.07 MiB)
  - CursorFocus WS ≈ 7.31 MiB (Private ≈ 1.62 MiB)

---

## [2026-08-14] OCR Experimentation & Philosophy Alignment

### Context & Exploration
Investigated adding text extraction capabilities (`ocr.exe`) using native Windows OCR APIs (`Windows.Media.Ocr`).

### Thought Process & Decision
- **Evaluation**: While native Windows OCR requires no bundled models, introducing OCR code paths added architectural complexity and potential regional language runtime failures.
- **Decision**: Reverted `ocr.exe` and clipboard text extraction experiments to preserve the project's strict scope: single-purpose, instant Win32 visual tools without peripheral scope creep.

---

## [2026-08-16] ColorPicker UI/UX Redesign & Multi-Format Precision

### Context & User Feedback
The initial `color-picker` tooltip displayed a single format at a time and lacked quick keyboard shortcuts for clipboard copying.

### Thought Process & Key Refinements
1. **Multi-Format Display**: Expanded tooltip layout to show HEX, RGB, HSL, and CMYK formats simultaneously in a clean 3-column format below a top color swatch bar.
2. **Shortcuts & Styling**: Assigned numeric keys `1`–`4` for instant format copying. Styled shortcut indicators as dimmed secondary text and moved the color swatch to the top header bar.
3. **CSS Format Compatibility**: Maintained plain visual numbers in the UI while formatting clipboard output strings in valid CSS syntax (e.g. `#023B32`, `rgb(2, 59, 50)`, `hsl(169, 94%, 10%)`, `cmyk(96%, 0%, 4%, 80%)`).
4. **Screen Edge Clipping Fix**: Addressed bug where tooltips were cut off near monitor boundaries by implementing dynamic text measurement (`RecalculateTooltipSize`) and screen boundary clamping (`MonitorFromPoint`).

---

## [2026-08-16] Capture Tool Annotation Toolbar & File Naming Conventions

### Context & User Requests
1. **Doodle Toolbar Visibility**: Users needed control over the doodle annotation toolbar visibility in `--draw` mode. Added `--draw-toolbar` flag (defaulting to `false`, toggleable via `Space`).
2. **Snip Mode Dimming**: Reduced dark overlay dimming in `--snip` mode from heavy opacity to a subtle ~27% dimming overlay for better background legibility.
3. **Systematic File Naming**: Established structured naming conventions for captured outputs:
   - Region snip: `snip_YYYYMMDD_HHMMSS.<ext>`
   - Fullscreen capture: `capture_YYYYMMDD_HHMMSS.<ext>`
   - Active window capture: `<appname>-<title>_YYYYMMDD_HHMMSS.<ext>`

---

## [2026-08-16] Documentation Streamlining & AGENTS.md Refactoring

### Context & Motivation
`AGENTS.md` had accumulated extensive documentation snippets, making it overly verbose for LLM agent prompt contexts.

### Architectural Decisions
1. **Targeted Documentation Model**: Split tool documentation into dedicated specs under `docs/<tool>.md` (`pin-to-top.md`, `find-my-mouse.md`, `color-picker.md`, `capture.md`).
2. **Lean Guide**: Streamlined `AGENTS.md` down to non-negotiable hard rules, the build command contract (`pwsh -File .\build.ps1 <target>`), IPC object contracts, and basic verification steps.

---

## [2026-08-17] AI Social Post Specification Experiment

### Context
Added `docs/social-posts.md` as an internal specification for generating factual social media announcement posts across Twitter/X, LinkedIn, Peerlist, and Bluesky.

---

## [2026-08-18] Language Evaluation (Zig) & Build Pipeline Optimization

### 1. Language Binary Size Experiment
- **Context**: Explored writing `find-my-mouse` in Zig (`src-zig/`) to evaluate binary size and memory efficiency compared to Win32 C.
- **Findings & Decision**: Benchmarking demonstrated that pure C99 compiled with Clang (`clang-cl -O2`) yields the smallest binary footprint without language runtime overhead. Reverted `src-zig/` and reaffirmed C99 + Win32 GDI as the standard.

### 2. Dual Build Pipeline (`build.ps1`)
- **Thought Process**: Developers needed quick PDB symbol generation during debugging without sacrificing optimized binary sizes in production releases.
- **Implementation**: Updated `build.ps1` with separate output directories:
  - `dist/release/`: Fully optimized release binaries (`-O2`, stripped).
  - `dist/debug/`: Debug builds with embedded PDB symbols (`-Z7` / `-gcodeview`).

---

## [2026-08-18] Capture Tool Multi-Monitor & DPI Re-attachment Alignment Fix

### Root Cause Analysis
Users reported that the doodle annotation toolbar became misaligned when reattaching secondary monitors or using monitors with mismatched DPI scaling factors.
- **Cause**: Absolute screen coordinates were failing to account for multi-monitor virtual screen offsets (`SM_CXVIRTUALSCREEN`, `SM_CYVIRTUALSCREEN`) when monitors were dynamically added/removed.
- **Solution**: Refactored positioning logic in `capture.c` to dynamically recalculate monitor work areas and DPI scales via `MonitorFromPoint` and `GetDpiForMonitor` on layout updates.

---

## [2026-08-18] Transition to Append-Only Journey Log

### Context & Rationale
User requested removing `docs/social-posts.md` and maintaining `docs/JOURNEY.md` as an append-only log capturing the full history, thought process, design trade-offs, and decisions behind `tiny-windows-utilities`.

### Changes Executed
1. **Removed `docs/social-posts.md`**: Removed marketing spec from repository docs.
2. **Created `docs/JOURNEY.md`**: Prefilled this document with complete, chronological history retrieved from past transcript logs and codebase analysis.
3. **Core Spec Structure**: Maintained `PRD.md` as authoritative behavioral spec, `CHECKLIST.md` as verification tracker, and `docs/<tool>.md` as per-tool technical design docs.

---

## [2026-08-19] Interactive Window-Picker Overlay for `capture --window`

### Context & Goal
Previously, running `capture --window` instantly snapped the active foreground window without allowing the user to select or switch windows interactively. The goal was to build an interactive **window-picker overlay** (similar to ShareX) where hovering over any top-level window highlights it with a selection border, and clicking captures that window, while maintaining zero jank and clean ESC / IPC cancel handling.

### Technical Challenges & Anti-Jank Engineering
1. **High-Frequency Mouse Event Overhead**: Modern mice send 500–1000 `WM_MOUSEMOVE` events per second. Calling `InvalidateRect` on every pixel move caused CPU spikes and flickering.
   - **Solution**: Implemented hover caching in `UpdatePickerHover`—if the hovered window `HWND` is unchanged, repaints are skipped, ensuring silky-smooth 60+ FPS performance.
2. **Accurate Window Hit-Testing**: `WindowFromPoint` initially hit our own full-screen overlay window, falling back to desktop worker windows (`explorer.exe`).
   - **Solution**: Replaced `WindowFromPoint` with system-wide Z-order traversal via `EnumWindows`. It ignores the overlay window, filters out desktop shell windows (`Progman`, `WorkerW`, `Shell_TrayWnd`), and tests `PtInRect` against DWM frame bounds (`DWMWA_EXTENDED_FRAME_BOUNDS`).
3. **Focus & ESC Cancel Fix**: Added `ForceForeground(hwnd)` and explicit `GetAsyncKeyState(VK_ESCAPE)` polling in the main message loop to ensure pressing `ESC` instantly cancels the picker at any time regardless of focus state.
4. **UI Styling & Border Line Experimentation**:
   - Experimented with dashed selection borders (testing 1px dashed GDI pens as well as 4px dash / 3px gap cosmetic style pens via `ExtCreatePen`) across both `--snip` and `--window` picker overlays.
   - **User Evaluation & Decision**: Dashed borders felt visually cluttered and less clean during active window selection compared to a sharp outline. Reverted both overlays back to a clean, solid 2px blue selection border (`CreatePen(PS_SOLID, 2, RGB(0, 122, 255))`) for consistent visual polish.

### Verification
- Compiled cleanly via `pwsh -File .\build.ps1 capture`.
- Verified double-launch IPC toggle cancel, ESC key cancellation, and accurate window cropping across multi-monitor setups.

---

## [2026-08-19] Pixel View Tool Architecture & Evolution (`pixel-view.c`)

### Context & Goal
User requested a new standalone Win32 C overlay utility named `pixel-view` to display images (PNG, JPG, BMP) and multi-frame animated GIFs using strict nearest-neighbor point sampling with hard pixel edges, zero anti-aliasing interpolation, frameless draggable window controls, customizable background modes, and a retro game feel.

### Engineering Milestones & Architectural Refinements
1. **WIC Decoder & Frame Compositing**:
   - Decodes single and multi-frame images using Windows Imaging Component (WIC).
   - Standardized WIC factory initialization using system `<wincodec.h>` CLSIDs (`CLSID_WICImagingFactory`, `CLSID_WICImagingFactory2`, `CLSID_WICImagingFactory1`) for universal Windows version compatibility.
   - Handles GIF frame delay timing (`TIMER_GIF_ANIM`), per-frame bounding box offsets (`/imgdesc/Left`, `/imgdesc/Top`), and frame disposal methods (restore to background, restore to previous frame).
2. **Initial Sizing & `--ratio` CLI Flag**:
   - Initial window width defaults to **400px** while preserving the source image aspect ratio (`initHeight = 400 * imgH / imgW`).
   - Added CLI parsing for `--ratio <WIDTH>x<HEIGHT>` (e.g. `--ratio 400x400` or `--ratio 800x600`).
3. **DOOM-Style Pixel Grid & 1:1 Square Pixel Aspect Ratio Correction**:
   - Modeled retro game pixelation by rendering background and image content to a low-resolution internal pixel grid upscaled to full window resolution via nearest-neighbor point sampling.
   - Fixed an initial aspect ratio bug where non-square window dimensions stretched 1:1 square retro pixels. Implemented dynamic grid sizing (`gridW = winW / pixelSize`, `gridH = winH / pixelSize`) to guarantee every chunky pixel block remains a 100% 1:1 square block of screen pixels.
4. **Retro Game Detail Preservation Engine (`2px` Default)**:
   - Initial fixed grid downsampling caused detail loss when maximizing or expanding the window.
   - Built a game-engine pixelation scale model: maintaining a constant chunky pixel scale (`pixelScale = 2px` default) relative to screen pixels ensures that enlarging the window **preserves & reveals more image details** while keeping sharp 1:1 retro pixel art edges.
   - Added CLI argument (`--pixel-size N`) and right-click context menu submenu (*Pixel Scale / Detail*: 1px max detail, 2px default, 3px, 4px, 8px).
5. **Frameless Right-Click Hit Testing & Shortcuts**:
   - Fixed right-click context menu failure on frameless windows caused by `WM_NCHITTEST` returning `HTCAPTION`. Added explicit handling for `WM_NCRBUTTONUP` alongside `WM_RBUTTONUP` and `WM_CONTEXTMENU`.
   - Removed `Esc` key auto-close keybinding per user request (closing performed via context menu or IPC toggle).

### Verification
- Compiled cleanly via `pwsh -File .\build.ps1 pixel-view` to `dist/release/pixel-view.exe`.
- Verified `--file`, `--ratio`, `--pixel-size`, `--grid`, `--bg` CLI flags, right-click context menu, and IPC mutex/event toggle.

---

## [2026-08-21] Comprehensive Binary Size Audit & Win32 C Build/Source Refactoring

### Context & Motivation
User requested a comprehensive audit across all 7 utilities in `tiny-windows-utilities` to analyze binary executable sizes and identify opportunities for size reduction while ensuring zero impact on feature functionality, Win32 compatibility, and long-term codebase maintainability.

### Thought Process & Key Refinements
1. **Phase 1 (Build System Optimization in `build.ps1`)**:
   - Added `-flto -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables` to Clang MinGW release build flags.
   - **Rationale**: Standard Win32 C code does not throw/catch C++ exceptions; stripping `.pdata`/`.xdata` SEH unwind tables removes unused PE metadata without changing a single runtime instruction. Enabling Link-Time Optimization (LTO) performs cross-unit dead-code elimination.
2. **Phase 2 (Shared Header & CRT Source Refactoring)**:
   - Converted 64 KB static stack array `hist[32768]` in `src/common/color-thief-algorithm.h` to dynamic heap allocation (`HeapAlloc`/`HeapFree`) on demand.
   - Replaced CRT string formatting (`swprintf`, `swprintf_s`) across `color-picker.c`, `capture.c`, `ip-send.c`, `context-menu.c`, and `pixel-view.c` with native Win32 `wsprintfW`, `lstrlenW`, and `lstrcatW`.
   - **Maintainability & Windows 8+ Compatibility**: Maintained 100% human-readable Win32 C code, standard API signatures, comments, and docstrings. Confirmed 100% compatibility across Windows 8, 8.1, 10, and 11 x64.
3. **Executable Size Results**:
   - `pixel-view.exe` reduced from 174.5 KB to **144.0 KB** (26.5 KB / -15.5% reduction).
   - Project-wide executable binary total dropped from **347.5 KB** down to **318.5 KB** (-29.7 KB overall).

### Verification
- Rebuilt all 7 targets cleanly via `pwsh -File .\build.ps1 -Mode release all`.
- Verified double-launch IPC toggle contracts across target executables.
- Updated `docs/CHECKLIST.md` (Section 19) and maintained `utility_size_reduction_analysis.md` artifact.

---

## [2026-08-21] IP Messenger Payload Dispatcher Overhaul & Utility Naming Alignment (`ip-send.c`)

### Context & Goal
The utility originally named `ipmsg` (and later `ip`) was designed to provide an instant, terminal/TUI-inspired payload composer and dispatcher for IP Messenger (`ipcmd.exe`), eliminating content-selection overhead when sending files, text snippets, and LAN messages. The goals for this cycle were:
1. Complete UI/UX polish and layout alignment based on user feedback.
2. Standardize naming across repository specs, source files, build scripts, and Explorer context menu triggers to **`ip-send`** (`Send via IP-send`).
3. Build automatic runtime checks for IP Messenger installation and background daemon state.

### Engineering Milestones & Key Refinements
1. **Window Framing & Title Layout Alignment**:
   - Drawn a full, unbroken outer border frame with corner crosses.
   - Positioned the top-left **`"Send"`** caption inside the top border margin in dim text (`textDim`), ensuring clean visual separation.
   - Enforced a minimum height (`44px`) for the top payload/message container and implemented dynamic content auto-sizing via `SetWindowPos` to eliminate empty space below the `[SEND]` button.
   - Handled `WM_NCACTIVATE` (`return TRUE;`) and `WM_NCPAINT` (`return 0;`) to prevent Windows from rendering a white non-client frame on focus loss.
2. **Typography & Keyboard Interaction Overhaul**:
   - Unified font rendering across all UI sections (title, body, search, list, `To:` summary, `[SEND]`) to a 100% consistent **12pt** font height (`-MulDiv(12, dpi, 96)`).
   - Replaced the `'i'` key shortcut for message edit mode with the **`Insert` key** (`VK_INSERT`). This allows recipient names containing `'i'` (e.g. `iris`, `admin`, `charvi`) to be searched without accidentally triggering message mode.
   - Intercepted `VK_SPACE` in recipient search mode to toggle selection while setting `g_skipNextCharForSpace` to consume `' '` in `WM_CHAR`, preventing unwanted space characters in `searchQuery`.
3. **Backend Detection & Daemon Auto-Start**:
   - **Missing Installation**: If `ipcmd.exe` is not found on disk, displays a native Windows Error Dialog (`MessageBoxW` with `MB_ICONERROR`) explaining that IP Messenger needs to be installed.
   - **Disabled / Stopped Daemon Auto-Start**: Implemented process snapshot scanning (`IsIPMsgProcessRunning` via `CreateToolhelp32Snapshot`). If `ipcmd.exe` exists on disk but `ipmsg.exe` background daemon is not running, `EnsureIPMsgProcessRunning` automatically starts the daemon in the background and displays a yellow warning status banner (`Warning: IP Messenger process was not running. Started background process.`).
4. **Standardized Tool Naming & Explorer Context Menu (`ip-send`)**:
   - Renamed source `src/ip.c` $\rightarrow$ `src/ip-send.c`, documentation `docs/ip.md` $\rightarrow$ `docs/ip-send.md`, build target `ip-send` (`dist/release/ip-send.exe`), IPC objects `Global\TinyIPSendMutex` / `Global\TinyIPSendEvent`, and window class `TinyIPSendComposerWindowClass`.
   - Updated Explorer context menu registration (`--context-menu register`) under `HKCU\Software\Classes\*\shell\Send via IP-send`, `Directory\shell\Send via IP-send`, and `Directory\Background\shell\Send via IP-send`, with automatic purging of legacy key paths (`Send via IP`, `IP Send`, `Send via IPMsg`).

### Verification
- Compiled release binary cleanly via `pwsh -File .\build.ps1 -Mode release ip-send`.
- Verified double-launch IPC toggle contract (`Global\TinyIPSendMutex`, `Global\TinyIPSendEvent`).
- Updated `docs/CHECKLIST.md` (Section 17), `AGENTS.md`, `docs/ip-send.md`, and `docs/JOURNEY.md`.

---

## [2026-08-21] Context Menu Utility Implementation & XML Registry Synchronization (`context-menu.c`)

### Context & Goal
User requested a new native Win32/C utility binary `context-menu.exe` to manage per-user Explorer context menu entries (`HKCU\Software\Classes`) using XML-based configuration files. The utility supports:
1. CLI operations (`--update <config.xml>` / `--backup <backup.xml>`) and double-click GUI flow (native Windows choice dialog for Update, Backup, or Cancel).
2. Unlimited nested submenus (`<item>` elements containing child `<item>` elements) using Windows native `SubCommands = ""` cascading registry key structure.
3. Action commands with parameters, icons, and Windows shell placeholders (`%1`, `%V`).
4. Strict pre-flight XML validation, entry isolation (`ManagedBy = "context-menu"`), idempotency, and silent exit on success without confirmation popups.

### Engineering Milestones & Key Refinements
1. **Pre-Flight XML Parser & Schema Validation Engine**:
   - Built a lightweight recursive descent XML parser handling UTF-8 (BOM or no BOM) and UTF-16 encoding conversion, entity escaping/unescaping (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`), attributes (`title`, `icon`, `command`, `arguments`), and nested `<item>` tags.
   - Implemented strict pre-flight validation: verifies non-empty target names, valid titles, non-empty commands for leaf action items, and valid child nodes for container items. Any validation failure halts execution immediately before modifying the Registry.
2. **Registry Sync, Isolation & Unlimited Cascading Submenus**:
   - Creates managed keys under `HKCU\Software\Classes\<Target>\shell\<KeyName>` tagged with `ManagedBy = "context-menu"` (REG_SZ).
   - Leaf items store action command strings in subkey `command`. Container items store `SubCommands = ""` and create a child `shell` subkey containing child items, enabling unlimited recursive submenu depth in Explorer.
   - Non-managed context menu entries in `HKCU\Software\Classes` created by other tools or Windows defaults are preserved and left untouched. Re-running `--update` produces the exact same registry state (idempotent).
   - Notifies shell via `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL)` on update.
3. **Registry Enumeration Buffer Reset & Two-Pass Backup Engine**:
   - **Buffer Reset Fix**: `RegEnumKeyExW` modifies its buffer length argument in-place. Added explicit `target_len = 256` reset in loop evaluation conditions to prevent enumeration loops from prematurely aborting after short key names.
   - **Two-Pass Backup**: Pass 1 scans for entries tagged with `ManagedBy = "context-menu"`. Pass 2 provides a fallback scan for custom user context menu shell entries if no `ManagedBy` entries have been tagged yet.
   - **Binary UTF-8 File Writing**: Switched file writing from `_wfopen_s` `L"w, ccs=UTF-8"` (unsupported by MinGW CRT) to binary mode `L"wb"` with direct `WideCharToMultiByte` buffer conversion.
4. **No Confirmation Dialogs & Path Resolution**:
   - Enforced silent exit code `0` on success across both CLI and GUI flows (no confirmation message boxes or stdout logs). Error cases output to `stderr` (CLI) or display a `MessageBoxW` error icon (GUI).
   - Resolved relative paths via `GetFullPathNameW` in `do_update` and `do_backup`.

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 -Mode release context-menu` to `dist/release/context-menu.exe`.
- Verified `--update`, `--backup`, invalid XML validation rejection (`stderr` + exit code 1), silent exit on success, and single-instance IPC contract (`Global\TinyContextMenuMutex`, `Global\TinyContextMenuEvent`).
- Updated `docs/CHECKLIST.md` (Section 18), `AGENTS.md`, `docs/context-menu.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] Global-Cmd Tool Implementation & Debugging Journey (`global-cmd.c`)

### Context & Goal
User requested a tiny native Windows CLI executable (`global-cmd.exe`) to generate `.cmd` and `.ps1` alias wrapper scripts from a JSON configuration file containing regex-keyed command aliases, and automatically register the destination folder in the User PATH (`HKCU\Environment`, `Path`).

### Engineering Milestones & Key Refinements

1. **Permission & Direct Generation Simplification**:
   - **Directive**: Removed disk executable path verification and file existence blocking checks per user request, allowing direct file generation into the specified `--dest` directory without requiring administrator elevation or disk scanning permissions.
   - **CLI Flags**: Supported `--dest`, `--dist`, `--destination`, `-d` for folder path, and `--config`, `-c` for JSON config path.

2. **Subsystem Build Configuration & Thread Stack Overflow Fix**:
   - **Subsystem Mismatch**: In `build.ps1`, MinGW Clang array splatting `@subsystemMinGW = @('-mconsole')` caused Clang to fall back to `-mwindows` (GUI application mode). Standard output handles (`stdout`) were unattached. Fixed `build.ps1` to pass scalar `$subsystemMinGW = '-mconsole'`.
   - **Stack Overflow Fix (Root Cause)**: `main()` declared `CandidateAlias candidates[MAX_CANDIDATES]`, allocating **~1.44 MB** directly on the thread stack. Because the default Windows thread stack size is 1 MB, entering `main()` triggered an immediate Windows stack overflow guard page crash (`0xC00000FD STACK_OVERFLOW`) before instructions executed. Fixed by heap-allocating `candidates` via `malloc()` and `free()`.

3. **Disk Wildcard Version Directory Resolution (`v16*` → `v16.20.2`)**:
   - **Problem**: PowerShell's call operator (`&`) fails when executing path templates containing unexpanded wildcards (e.g. `& "$env:LOCALAPPDATA\nvm\v16*\node.cmd"`).
   - **Solution**: Implemented `ResolveWildcardInTemplate` using Win32 `FindFirstFileW`. `global-cmd.exe` queries wildcard path segments on disk at generation time and replaces them with exact versioned folder names (e.g. `v16*` → `v16.20.2`).

4. **Target Executable Extension Auto-Detection & Fallback (`node.exe` vs `node.cmd`)**:
   - **Problem**: NVM installs Node.js as a native binary **`node.exe`** inside `v16.20.2\`, whereas `npm` and `npx` are script wrappers **`npm.cmd`**. Generating `node.cmd` produced a missing file error in PowerShell.
   - **Solution**: Implemented `CheckAndFixExecutableExtension`. Probes target files on disk (`GetFileAttributesW`) and automatically corrects extensions (e.g. `.cmd` → `.exe` for `node.exe`, preserving `.cmd` for `npm.cmd`).

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 -Mode release global-cmd` to `dist/release/global-cmd.exe`.
- Tested `node16 --version` (`v16.20.2`) and `node24 --version` (`v24.13.0`) in PowerShell.
- Updated `docs/global-cmd.md`, `docs/PRD.md`, `docs/CHECKLIST.md`, `AGENTS.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] IP Messenger Message Multiline Text Wrapping & Auto-Resizing (`ip-send.c`)

### Context & Goal
In `ip-send.c`, typing long message strings in message edit mode previously rendered text on a single line via `TextOutW`, causing text to cut off horizontally beyond the right window border. The goal was to support automatic word wrapping (`DrawTextW` with `DT_WORDBREAK`) and dynamic window height auto-resizing (`SetWindowPos`) as text is typed or deleted.

### Implementation & Refinements
1. **Multiline Word Wrapping**:
   - Calculated available horizontal text width `maxTextWidth = (width - 9) - marginX - 16`.
   - Measured multiline text height using `DrawTextW(memDC, msgBuf, -1, &calcRect, DT_WORDBREAK | DT_CALCRECT)`.
   - Rendered multiline wrapped text via `DrawTextW(memDC, msgBuf, -1, &drawRect, DT_WORDBREAK)` and advanced `y` by `msgH + 6`.
2. **Dynamic Height Auto-Resizing**:
   - As `y` flows downwards into search prompt, recipient list, and footer `footY`, `neededH` updates dynamically.
   - Calling `SetWindowPos(g_hWnd, NULL, 0, 0, width, neededH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)` automatically expands or shrinks window height as message lines are typed or deleted.

3. **Enter Key Mode-Aware Dispatch vs Newline**:
   - `Enter` in recipient search / browsing mode triggers `ExecuteSend`.
   - `Enter` in insert/message mode appends a newline (`\n`) to `messageText`, growing the message height and dynamically resizing the window.
   - User exits insert mode via `Esc` or `Tab` (or clicks `[ SEND ]` directly).

### Verification
- Compiled release binary cleanly via `pwsh -File .\build.ps1 -Mode release ip-send` to `dist/release/ip-send.exe`.
- Updated `docs/ip-send.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] OCR Utility — Portable Tesseract Wrapper (`ocr.exe`)

### Context & Goal
User requested a tiny Windows-native OCR CLI: `ocr.exe --image <png|jpg|bmp> [--lang eng] [--psm 6] [--oem 1]` → clean formatted UTF-8 to `stdout`, `stderr` diagnostics only, exit `0` success / non-zero on invalid input / missing resources / OCR failure. Must be **always portable** (`ocr.exe` + `ocr-resources/` beside exe, no `%APPDATA%`/`HKLM`/`%LOCALAPPDATA%`), auto-download Tesseract runtime + `*.traineddata` lazily on first run, cache & validate/repair, keep Tesseract as internal detail. Design goal: Copyfish-like smart provisioning but standalone tiny CLI, referencing Helium extension `eenjdnjldapjajjofmldgmkjaienebbj` (Copyfish `6.3.16_0`).

### Engineering Milestones
1. **OmaSnap Clone & Windows Port (temp)**:
   - Cloned `https://github.com/tobi/omasnap.git` to `temp/omasnap` and `C:\Users\abhishek.c\AppData\Local\Temp\opencode\omasnap`, explored `src/capture.c`, `pin-to-top.c`, Win32 `W`-wide, `WinMain`, `g_`.
   - Created `src/omasnap.c` (1000 lines, `53248` bytes) `Global\TinyOmasnapMutex/Event`, CLI `--capture-region/window/fullscreen/scroll` + `--file/--clipboard/--copy/--save/--draw/--toolbar/--font-size/--format`, `BitBlt` virtual desktop, `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`, `Gdiplus` dynamic PNG fallback BMP, `Stroke[256]` annotation `CompositeStrokesToBitmap` `ExtCreatePen`, built `dist/release/omasnap.exe` via `build.ps1` `shell32/ole32`.
   - Added `RunOcrOnBitmap` (`SearchPathW tesseract.exe`, `GetTempFileNameW` PNG, `OMASNAP_OCR_LANGS`/`OMARCHY_OCR_LANGS` `eng`, `CreatePipe` `15s`, `MultiByteToWideChar(CP_UTF8)`), `O` key, toolbar `O:OCR`, then removed `src/omasnap.c`/`docs/omasnap.md` per pivot.

2. **Standalone `ocr.exe` Initial (`src/ocr.c` 379 lines)**:
   - `wmain` CLI `--image`/`--lang` (`+` multi) / `--psm`/`--oem`/`--help`/`--version`, validates `png`/`bmp` (warn `jpg/gif`), `OMASNAP_OCR_LANGS` default `eng`, `find_tesseract` probes `.\tesseract\`, `%LOCALAPPDATA%\ocr\`, `Program Files`, `PATH`, `ensure_tesseract_with_langs` downloads `UB-Mannheim` zip + `tessdata_fast` via `urlmon!URLDownloadToFileW` + `powershell Expand-Archive`/`tar`, `run_tesseract` `CreatePipe` `30s` `stdout -l <lang> --oem --psm --tessdata-dir`, `WriteConsoleW`/`wprintf` + `CF_UNICODETEXT` clipboard, built `dist/release/ocr.exe:153088` (fixed `WriteConsoleW` → `wprintf` + `MessageBoxW`).
   - Copied Helium extension `eenjdnjldapjajjofmldgmkjaienebbj\6.3.16_0` to `temp/copyfish-6.3.16_0` (15 entries), parsed `manifest.json` (`copyfish@a9t9.com`, `desktop-text-capture-3s-delay`), `config/config.json` (`visualCopyOCRLang eng`, `ocrEngine OcrSpaceSecond`, `apipro1/2` `copyfishonly_26`, `ocr_languages` `eng/ara/chs/cht/...`), `background.js` `getLocalOcrPath()` → `%APPDATA%\UI.Vision\XModules\ocr\ocrexe\ocrcl1.exe`, updated `find_tesseract` to check `XModule` first.

3. **Spec-Driven Portable Rewrite (2026-08-24)**:
   - **Always portable**: `get_resource_dir()` `GetModuleFileNameW` → `ocr-resources/tesseract/tesseract.exe` + `tessdata/` + `tmp/` beside exe (`src/ocr.c:59`), no `CSIDL_APPDATA`/`CSIDL_LOCAL_APPDATA`, `is_tesseract_valid` `>10KB`, `is_lang_valid` `>10KB` `!HTML`.
   - **Provisioning**: `ensure_tesseract_runtime` `>10KB` check, `TESS_ZIP_URL`/`TESS_PORTABLE_URL` `zstrathe/tesseract_portable_windows` `main.zip` (4MB) via `urlmon` → `curl -L` → `powershell IWR` fallback (`src/ocr.c:64`), extracts via `powershell Expand-Archive` fallback `tar -xf`, copies `tesseract.exe` + `tessdata` via `copy_dir_contents`. Fallback `UB-Mannheim` `v5.4.0.20240606` `50MB` NSIS `highestAvailable` (requires elevation → `RunAsInvoker` fails) → portable fallback. `ensure_lang_file` lazy `tessdata_fast` `eng/hin/tam/tel/osd` (`4113088`/`1122751`/`3237963`/`2769654`/`10562727`).
   - **Preprocessing** (`WIC` `COBJMACROS`): `CreateDecoderFromFilename` → `GetSize` → aggressive upscale `2.0x` default (`2.5x` `<600`, `3.0x` `<400`, `1.5x` `>=2000`), `Fant` cubic, `GUID_WICPixelFormat8bppGray`, `CopyPixels` → Otsu `90-160` soft `+30` preserves thin gaps + `20px` white border `CreateBitmapFromMemory` `fwb=fh+40`, `WIC PNG encoder` to `%TEMP%\ocr-pre-<tick>.png`. Fixes `System Utilities` tight kerning `SET` → `System Utilities`.
   - **PSM/DPI**: `choose_psm` `6` default (uniform block, `7` if `h<60&&w>h*4`), `run_tesseract` adds `-c preserve_interword_spaces=1 -c user_defined_dpi=300`, heap `262KB` buf, `format_text` `CRLF→LF` trim, collapse `>1` blank, strip `\f`.
   - **Auto language**: default `auto` (no `--lang` → `auto`), `detect_lang_auto` (`src/ocr.c:386`) ensures `osd.traineddata`, runs `tesseract <img> stdout --psm 1 -l osd` piped, parses `Script: Devanagari→hin Tamil→tam Telugu→tel Latin→eng` (`+eng`), fallback `build_auto_lang` scans `tessdata/*.traineddata` joined `+` plus `hin/tam/tel` for short `Too few characters` fallback, explicit `--lang` overrides. Verified `hindi.png` `auto` → `eng+hin+tam+tel` → `नमस्ते` hex `E0A4...`, `tamil.png` `வணக்கம்`, `eng.png` `System Utilities` (all `exit 0`, second run silent).

### Verification
- `pwsh -File .\build.ps1 ocr` → `dist/release/ocr.exe:162816` (console `-mconsole`, libs `shell32/ole32/urlmon/shlwapi/windowscodecs/gdi32`, `78` warnings).
- `ocr.exe --help` `stderr` exit 0, `notfound.png` exit 1, `test.gif` exit 2.
- First `ocr.exe --image sample.png` (`400x100` `Hello 123`) → `[*] tesseract runtime missing... [*] extracting ... [*] copying ... [*] tesseract ready` `Hello 123` `stdout`, second run silent `Hello 123`.
- Tight `9pt` `System Utilities` + `hindi.png --lang auto` + `tamil.png --lang auto` all `exit 0`, `Format-Hex` UTF-8 correct.
- Updated `docs/ocr.md` (portable, OSD, tight, auto), `docs/CHECKLIST.md` `##21`, `build.ps1` `ocr` `windowscodecs`.

---

## [2026-08-24] Build System Modernization, Flag Standardization, & Shared Common Headers

### Context & Motivation
As the project expanded to 9 standalone Win32 utilities, `build.ps1` had become an unwieldy procedural script with cascading `if-else` branches, ad-hoc library definitions, subsystem overrides, and inconsistent compilation flag hacks (such as `-fno-unwind-tables`, `-fno-stack-protector`, and `/GS-`). Furthermore, while single-file architecture kept utilities isolated, common Win32 patterns (CLI argument tokenization, single-instance IPC toggles, and Per-Monitor DPI awareness) were duplicated across source files.

### Thought Process & Design Decisions

1. **Size Optimization vs Safety Analysis**:
   - Analyzed binary disk sizes across all 9 utilities. Due to Windows PE 512-byte section alignment, removing aggressive size hacks (`-fno-unwind-tables`, `-fno-stack-protector`, `/GS-`) produced a net size difference of **0 KB** on 6 tools and **+0.5 KB** (a single 512-byte disk sector) on 3 larger tools.
   - Preserving 64-bit Structured Exception Handling (SEH) unwind tables and stack guards eliminated crash risks on tools like `global-cmd.exe` while preserving core size-saving optimizations (`-Os` / `/O1`, `-flto`, `-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections`, `/OPT:REF,ICF`, `/MANIFEST:NO`).

2. **Declarative Build Pipeline (`build.ps1`)**:
   - Replaced procedural branching with an ordered hashtable manifest (`$TARGET_CONFIGS`) defining `Subsystem`, `Libs`, `ExtraMinGW`/`ExtraMSVC`, and `Aliases`.
   - Used explicit array argument splatting (`$cmdArgs`) to prevent argument-splitting bugs in PowerShell.
   - Retained full CLI backward compatibility (`pwsh -File .\build.ps1 [-Mode release|debug] <target|all>` and interactive numeric menu).

3. **Zero-Overhead Shared Common Headers (`src/common/`)**:
   - **`tiny_cli.h`**: In-place, zero-allocation wide-character command-line tokenizer (`TinyCLI_Tokenize`) and declarative argument parser (`TinyCLI_ParseCommandLine`, `TinyCLI_Parse`). Avoids `shell32.dll` / `CommandLineToArgvW` dependency, keeping minimal GUI tools bound strictly to `user32` and `gdi32`.
   - **`tiny_ipc.h`**: Standardized single-instance mutex and auto-reset event toggle helper (`TinyIPC_AcquireOrToggle`).
   - **`tiny_dpi.h`**: Standardized Per-Monitor v2 DPI awareness initialization (`TinyDPI_EnablePerMonitorAwareness`).

4. **Integration & Size Verification**:
   - Integrated `tiny_cli.h` into `src/pin-to-top.c`, eliminating ~60 lines of repetitive tokenizer and range-validation boilerplate.
   - Release binary size for `pin-to-top.exe` remained **23,552 bytes (23.00 KB)** (0 bytes added).

### Verification
- `pwsh -File .\build.ps1 all` builds all 9 targets in Release mode (`dist/release/`) with exit code 0.
- `pwsh -File .\build.ps1 all -Mode debug` builds all 9 targets in Debug mode (`dist/debug/`) with exit code 0.
- Verified target aliases (`pwsh -File .\build.ps1 ip globalcmd pin`).
- Verified double-launch IPC toggle and CLI argument parsing (`pin-to-top.exe --border-width 3 --max-windows 2`).
- Updated `AGENTS.md`, `docs/CHECKLIST.md`, `docs/pin-to-top.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] Capture Tool Visual Refinement: Darker Dimming & Translucent Annotation Backdrop

### Context & Goal
Previously in `capture.c`, entering annotation mode (`--draw`) rendered a solid, opaque black backdrop (`RGB(15, 15, 18)`) across the virtual desktop around the active snip/window, creating an abrupt visual disconnect from the user's workspace. Additionally, the snip/window selection dimming was overly light (`SNIP_DIM_ALPHA = 70`, ~27% opacity). The goal was to replace the solid black annotation canvas background with a translucent dimmed desktop backdrop (matching snip mode) and darken the dim overlay across all capture modes.

### Implementation Details
1. **Unified Dim Overlay Helper (`ApplyDimOverlay`)**:
   - Extracted shared dimming blit logic into a reusable helper function using `AlphaBlend` from `msimg32.dll`.
   - Increased `SNIP_DIM_ALPHA` from `70` (~27%) to `125` (~49% opacity) for a richer, more focused, high-contrast dark translucent overlay that maintains clear visibility of underlying windows without visual distraction.
2. **Translucent Desktop Backdrop in Annotation Mode**:
   - Preserved `g_hbmDesktop` across `RunSnipOverlay` and `RunWindowPickerOverlay` when `--draw` is enabled (falling back to on-demand virtual desktop capture if needed).
   - In `AnnotationWndProc`'s `WM_PAINT`, rendered `ApplyDimOverlay(hdcMem, hdcDesktop, g_vw, g_vh, SNIP_DIM_ALPHA)` across the full virtual screen, followed by blitting `g_hbmCaptured` at `(g_capX, g_capY)` with 100% crisp sharpness and vibrant contrast.
   - Ensured proper memory management and bitmap handle deallocation in `WinMain` cleanup.

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 capture` (`dist/release/capture.exe`, 40.0 KB).
- Updated `docs/capture.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] Common GUI Header (`tiny_gui.h`) & Multi-Utility Integration

### Context & Goal
Several GUI and overlay utilities (`capture.c`, `find-my-mouse.c`, `color-picker.c`, `pixel-view.c`) had duplicate Win32 boilerplate:
- Multi-monitor virtual screen bounding box calculation (`EnumDisplayMonitors` + `MONITORINFO` bounds accumulation).
- Alpha-blended dark translucent dim overlay rendering (`msimg32.dll` / `AlphaBlend`).
- Border resize hit-testing for frameless layered windows (`WM_NCHITTEST`).
- Per-Monitor v2 DPI awareness initialization.

The goal was to create a zero-overhead shared header [`src/common/tiny_gui.h`](src/common/tiny_gui.h) and integrate it across GUI tools without adding binary bloat.

### Implementation Details
1. **Shared GUI Library (`src/common/tiny_gui.h`)**:
   - `TinyGUI_GetVirtualScreenBounds(int *outX, int *outY, int *outW, int *outH)`: Reliable multi-monitor screen bounds probing with automatic fallback to `SM_XVIRTUALSCREEN`.
   - `TinyGUI_ApplyDimOverlay(HDC hdcDest, HDC hdcDesktop, int vw, int vh, BYTE alpha)`: Robust GDI double-buffered dark overlay blending using dynamic `AlphaBlend`.
   - `TinyGUI_HitTestResizeBorders(HWND hwnd, LPARAM lParam, int borderThickness)`: 8-direction border resize hit-tester for frameless windows.
   - `TinyGUI_CreateScaledFont(int pointSize, int weight, const wchar_t *faceName)`: Cleartype font constructor.
2. **Tool Integrations**:
   - **`capture.c`**: Replaced 60+ lines of custom monitor callbacks and dimming code with `TinyGUI_GetVirtualScreenBounds`, `TinyGUI_ApplyDimOverlay`, and `TinyDPI_EnablePerMonitorAwareness`.
   - **`pixel-view.c`**: Replaced custom 25-line `WM_NCHITTEST` branch with `TinyGUI_HitTestResizeBorders` and added Per-Monitor v2 DPI awareness.
   - **`find-my-mouse.c`**: Integrated `TinyGUI_GetVirtualScreenBounds` and `TinyDPI_EnablePerMonitorAwareness`.
   - **`color-picker.c`**: Replaced custom DPI loader with `TinyDPI_EnablePerMonitorAwareness` and unified `TinyGUI_GetVirtualScreenBounds`.

### Verification
- Built all 4 targets simultaneously: `pwsh -File .\build.ps1 capture pixel-view find-my-mouse color-picker` (all exit code 0).
- Release binary sizes verified:
  - `find-my-mouse.exe`: 20.0 KB (20,480 bytes)
  - `color-picker.exe`: 25.5 KB (26,112 bytes)
  - `pixel-view.exe`: 29.0 KB (29,696 bytes)
  - `capture.exe`: 40.5 KB (41,472 bytes)
- Updated `AGENTS.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-08-24] IP Messenger Multi-File Explorer Context Menu Fix (`ip-send.c`)

### Context & Problem
When multiple files were selected in Windows Explorer and dispatched via the context menu (`Send via IP-send`), Windows Explorer spawned separate concurrent processes for each selected file (e.g. `ip-send.exe "file1"`, `ip-send.exe "file2"`, etc.). The second process encountered `ERROR_ALREADY_EXISTS` on the mutex, interpreted it as a toggle-off command, and signaled `g_hEvent`, which immediately killed the primary window, causing `ip-send` to auto-exit before opening.

### Solution & Refinements
1. **Payload-Aware Multi-Instance Handling (`HasPayloadArguments`)**:
   - When a second instance detects that the mutex already exists, it inspects its command line via `HasPayloadArguments()`.
   - If payload/file arguments are present (from Explorer or CLI), it retries `FindWindowW` to locate the primary `TinyIPComposerWindowClass` window.
   - It forwards its arguments via `WM_COPYDATA` to the running instance and exits cleanly (`0`) without signaling `g_hEvent`.
   - If no arguments are present (user triggered a hotkey / second launch to toggle off), it signals `g_hEvent` to toggle off the active instance as before.
2. **`WM_COPYDATA` Handler & Visual Feedback**:
   - `WndProc` receives `WM_COPYDATA`, parses the incoming arguments, and appends the file paths to `g_state.payloads`.
   - When more than 4 files are attached, the UI shows the first 4 files plus `+ N more files...`.
   - Calls `SetForegroundWindow(hWnd)` and `InvalidateRect(hWnd, NULL, FALSE)` to refresh the display.

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 ip-send` (`dist/release/ip-send.exe`).
- Updated `docs/CHECKLIST.md` (Section 17) and `docs/JOURNEY.md`.

---

## [2026-08-24] IP Messenger Recipient Selection Safeguard & Disabled Button Styling (`ip-send.c`)

### Context & Goal
Previously, if no recipient was explicitly selected when pressing `Enter` or clicking `[ SEND ]`, `ExecuteSend` automatically selected the top/highlighted recipient and dispatched the payload. The user requested disabling automatic auto-selection and providing a visual warning/disabled state instead.

### Implementation Details
1. **Disabled Send Appearance**:
   - In `RenderTUIWindow`, if `selCount == 0`, `To: none` is drawn in `textDim`, and `[ SEND ]` is rendered in dim gray (`textDim`) instead of active accent blue.
2. **Explicit Selection Safeguard & Warning Status**:
   - Removed the auto-selection fallback from `ExecuteSend`.
   - If `selectedCount == 0`, `ExecuteSend` halts dispatch and sets `g_state.statusText` to `"Warning: No recipients selected. Press Space to select."`, highlighted in warning coral text.

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 ip-send` (`dist/release/ip-send.exe`).
- Updated `docs/ip-send.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-08-25] OCR Interactive Native File Picker & Clipboard Output (`ocr.c`)

### Context & Goal
Clarified OCR tool requirements:
1. When an image path is passed via CLI (`ocr.exe <image>` or `ocr.exe --image <image>`), OCR extracts text from that image directly, copies it to the clipboard, and prints it to `stdout`.
2. When no image argument is passed, `ocr.exe` automatically opens a native Windows File Explorer dialog (`GetOpenFileNameW` via `comdlg32`) filtered to PNG, JPG, JPEG, and BMP images.
3. Upon selecting an image, OCR runs and copies the recognized text to the Windows Clipboard (`CF_UNICODETEXT`), prints to `stdout`, and displays a native alert / notification (`MessageBoxW`) showing the recognized text.

### Implementation Details
1. **Interactive File Picker**: Added `PickImageFileDialog` using Win32 `GetOpenFileNameW` with clean filter strings (`Image Files (*.png;*.jpg;*.jpeg;*.bmp)`). Exits cleanly (`0`) if user cancels.
2. **Subsystem & Entry Point Optimization**:
   - Converted subsystem from `console` to `windows` in `build.ps1` (`-mwindows` / `/SUBSYSTEM:WINDOWS`).
   - Implemented `wWinMain` / `WinMain` entry points and dynamic console attachment via `AttachConsole(ATTACH_PARENT_PROCESS)` when CLI arguments are present.
   - Eliminates the black Command Prompt / terminal window popup when `ocr.exe` is double-clicked or opened from File Explorer.
3. **Clipboard & Alert Integration**:
   - `CopyToClipboard` sets `CF_UNICODETEXT` on the global clipboard.
   - `OutputText` prints UTF-8 to `stdout` (attaches to parent console if present).
   - Interactive mode triggers `MessageBoxW` with recognized text preview and copy confirmation.
4. **Build & Linking**: Added `comdlg32` to the `ocr` target in `build.ps1` and verified compilation with both MSVC `clang-cl` and MinGW `clang`.

### Verification
- Built target with `pwsh -File .\build.ps1 ocr` -> `dist/release/ocr.exe`.
- Verified GUI launch: no black console window opens; File Explorer picker and native alert dialog appear directly.
- Tested `.\dist\release\ocr.exe .\dist\release\sample.png` -> recognized `Hello 123` on `stdout`.
- Verified `Get-Clipboard` contained `Hello 123`.
- Tested `ocr.exe --help` usage output.

---

## [2026-08-25] Capture Interactive Mode Selector & PNG Save Dialog Flow (`capture.c`)

### Context & Goal
When running `capture.exe` with no CLI arguments, provide an intuitive interactive flow:
1. Display a floating top-level overlay with 3 mode choices: **Snip**, **Window**, and **Full screen**.
2. Upon user selection (e.g. Snip), trigger the corresponding capture mode immediately.
3. Once captured, transition directly into the doodle annotation mode with the floating toolbar visible (`--draw --draw-toolbar`).
4. When the user confirms / saves (`Enter` / `Done` / `Save` / `S`), display the native Windows Save File Dialog (`GetSaveFileNameW`) to choose a PNG destination file, while also placing the bitmap on the Windows Clipboard.

### Implementation Details
1. **Zero-Flicker Unified Overlay Engine (`UnifiedCaptureWndProc`, `RunUnifiedCapturePipeline`)**:
   - Single top-level overlay window (`TinyUnifiedCaptureClass`) created once and kept alive through the entire capture workflow across 4 stages: `STAGE_SELECTOR`, `STAGE_SNIP`, `STAGE_WINDOW_PICK`, `STAGE_ANNOTATE`.
   - Completely eliminates the desktop flash/flicker that previously occurred during inter-window destructions and creations.
2. **Centered Group Overlay Selection (ip-send Terminal Style, No Window)**:
   - Positioned cleanly centered as a group both vertically and horizontally on the screen over the translucent dimmed backdrop, without any surrounding window card or container box.
   - Text size enlarged to prominent ~22px semi-bold Segoe UI (`hMenuFont`), and hint text enlarged to clear ~14px Segoe UI (`hFont`).
   - Text items are left-aligned in fixed sub-rectangles (`labelR.left = rowR.left + pointerW`) to eliminate any horizontal text jitter/shifting when hovering, while the `>` pointer prefix is rendered independently in `accentBlue`:
     - `> 1. Snip (Region)` (highlighted active choice in bright `accentBlue` `#60A5FA`)
     - `  2. Window` (inactive choice in `textDim` `#9196A5`)
     - `  3. Full Screen` (inactive choice in `textDim` `#9196A5`)
   - Centered helper prompt directly underneath: `[ 1 / 2 / 3 ] Select    [ Enter ] Confirm    [ Esc ] Cancel`.
   - Full keyboard navigation (`Up`/`Down`/`Left`/`Right`/`Tab`, `1`/`S`, `2`/`W`, `3`/`F`, `Enter`/`Space`, `Esc`) and mouse hover/click hit-testing.
3. **In-Place Seamless State Transitions**:
   - Choosing **Snip** immediately transforms the window into region selection with crosshair cursor and live dimension badge.
   - Choosing **Window** immediately activates interactive window frame picking.
   - Choosing **Full Screen** (or completing a snip/window capture) immediately presents the doodle annotation canvas with floating toolbar visible.
4. **Native PNG Save Dialog (`PromptSavePngDialog`)**:
   - Added `PromptSavePngDialog` utilizing Win32 `GetSaveFileNameW` via `comdlg32`.
   - Generates formatted default filenames (`snip_YYYYMMDD_HHMMSS.png`, `window_...png`, `capture_...png`).
   - In draw mode, whenever the user presses `Enter` ("Done"), clicks `Save`, or presses `S`, it checks whether an explicit `--save <path>` CLI option was passed: if no `--save` was passed, it prompts the user with the native Windows Save File Dialog (`GetSaveFileNameW`) to choose a PNG destination file; if `--save <path>` was passed, it saves directly to that path without prompting.
   - Pressing `C` or clicking "Copy & Exit" copies to clipboard and exits immediately.
   - Saves PNG via GDI+ (`GdipSaveImageToFile`), while `CopyBitmapToClipboard` ensures the image is simultaneously placed on the clipboard.
5. **Build System & Dependencies**:
   - Added `comdlg32` to the `capture` target configuration in `build.ps1`.

### Verification
- Built release binary: `pwsh -File .\build.ps1 capture` -> `dist/release/capture.exe` (clean compilation, zero warnings).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug capture` -> `dist/debug/capture.exe` (clean compilation).
- Verified double-launch toggle cancellation and single overlay lifecycle.
- Updated `docs/capture.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

## [2026-08-25] Centralized Shared Font and Clipboard Single-Header Modules (`font.h`, `clipboard.h`)

### Problem & Motivation
1. Various utilities duplicated clipboard interaction code (`OpenClipboard`, `EmptyClipboard`, `GlobalAlloc`, `GlobalLock`, `SetClipboardData`, `CloseClipboard`, retry logic) and font selection code (`EnumFontFamiliesExW` callbacks, fallback probing chains, DPI font creation).
2. Monospace font selection was inconsistent across tools: `ip-send.c` searched for JetBrains Mono -> Consolas, while `color-picker.c` and `capture.c` hardcoded Segoe UI or manually called `CreateFontW`.
3. Standard UI metrics (font point sizes, weights, and dark mode palette colors) were duplicated across different utility files.
4. Separate `.c` files in `src/common/` introduced translation unit overhead; the codebase architecture standard (`tiny_cli.h`, `tiny_gui.h`, `tiny_dpi.h`, `tiny_ipc.h`) requires header-only modules with `static inline` functions.

### Solution & Design
1. **Single-Header `font.h` (`src/common/font.h`)**:
   - Consolidated implementation entirely inside `font.h` as `static inline` functions; deleted `font.c`.
   - Centralized font constants (`TINY_FONT_SIZE_*`, `TINY_FONT_WEIGHT_*`, `TINY_COLOR_*`).
   - Defined primary monospace font face `JetBrains Mono` with fallback chain (`Consolas`, `Fira Code`, `Cascadia Code`, `Cascadia Mono`, `Courier New`).
   - Defined primary UI font face `Segoe UI` with fallback chain (`Tahoma`, `Arial`).
   - Added thread-safe font availability checking (`TinyFont_IsAvailable`) via `EnumFontFamiliesExW` with `LPARAM` context.
   - Added DPI-aware GDI font constructors (`TinyFont_Create`, `TinyFont_CreateMonospace`, `TinyFont_CreateUI`, `TinyFont_ScaleSize`) and extent measurement helpers (`TinyFont_MeasureText`, `TinyFont_GetFontHeight`).
2. **Single-Header `clipboard.h` (`src/common/clipboard.h`)**:
   - Consolidated implementation entirely inside `clipboard.h` as `static inline` functions; deleted `clipboard.c`.
   - Added retry-protected Unicode clipboard functions (`TinyClipboard_SetText`, `TinyClipboard_GetText`, `TinyClipboard_GetTextAlloc`, `TinyClipboard_HasText`).
   - Added bitmap clipboard function (`TinyClipboard_SetBitmap`) and file drop query (`TinyClipboard_HasFiles`).
3. **Refactored Utility Targets**:
   - Updated `color-picker.c`, `ocr.c`, `ip-send.c`, `capture.c`, and `tiny_gui.h` to include `common/font.h` and `common/clipboard.h`.
   - Updated `color-picker.c` tooltip rendering to use JetBrains Mono / Consolas monospace for crisp alignment of HEX, RGB, HSL, and CMYK color values.

### Verification
- Removed `src/common/font.c` and `src/common/clipboard.c`.
- Built all release targets cleanly via `pwsh -File .\build.ps1 all`.
- Verified clean compilation of all 9 utilities (`pin-to-top`, `find-my-mouse`, `color-picker`, `capture`, `pixel-view`, `ip-send`, `context-menu`, `global-cmd`, `ocr`).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

## [2026-08-25] Font Scaling Standardization, Capture Toolbar UX Upgrades & Common Modules Documentation

### Problem & Motivation
1. **Font Sizing Inconsistency**: `TinyFont_Create` used 72 DPI denominator (`-MulDiv(size, dpi, 72)`) for point scaling, while utility UIs and CLI options passed standard 96 DPI pixel sizes (`12`, `13`, `16`, `22`), causing rendered fonts to appear 33% larger than intended on standard displays.
2. **Capture Draw Toolbar UX**: The doodle annotation toolbar lacked interactive mouse hover feedback, hand cursors, distinct section dividers, and active tool glows.
3. **Common Modules Documentation**: While individual utilities had design docs in `docs/`, the 7 shared common headers (`tiny_cli.h`, `tiny_ipc.h`, `tiny_dpi.h`, `tiny_gui.h`, `font.h`, `clipboard.h`, `color-thief-algorithm.h`) lacked dedicated API reference docs.

### Solution & Design
1. **Fixed Visual Font Footprint, OS Default Size & Runtime Zoom Controls (`src/common/font.h`)**:
   - Replaced DPI font height multiplication with a fixed visual font footprint (`TinyFont_ScaleSize` returns `fontSize`).
   - Added `TinyFont_GetSystemDefaultSize()` querying `SPI_GETNONCLIENTMETRICS` (`ncm.lfMessageFont.lfHeight`) to match Windows native default font size by default across utilities.
   - Added `TinyFont_HandleZoomKey()` enabling `Ctrl +` (zoom in), `Ctrl -` (zoom out), and `Ctrl 0` (reset to OS default) key handlers across all interactive overlay tools, clamped to `[TINY_FONT_SIZE_MIN, TINY_FONT_SIZE_MAX]` (8..48px).
   - Ensures typography remains physically uniform in pixel size between laptop screens (150%–200% DPI) and desktop monitors (100%–125% DPI), avoiding text ballooning on high-density displays.
   - Preserves full Per-Monitor v2 DPI awareness for window geometry, hit-testing, overlay bounds, and layouts (`tiny_dpi.h` and `tiny_gui.h`).
   - Renamed parameter `pointSize` to `fontSize` across `font.h` and `tiny_gui.h` signatures to reflect logical pixel sizes rather than typographic points.
   - Refined typography constants (`TINY_FONT_SIZE_XS` = 10, `SMALL` = 11, `DEFAULT` = 12, `NORMAL` = 13, `MEDIUM` = 15, `LARGE` = 18, `XLARGE` = 22).
2. **Upgraded Draw Toolbar UX (`src/capture.c`)**:
   - Modernized toolbar into a floating segmented pill with thin vertical section dividers between Title, Colors, Pen Widths, History (Undo/Clear), and Actions (Copy/Save/Done/Cancel).
   - Added `GetToolbarHoverItem` and live hover tracking with responsive button highlight states (`RGB(52, 56, 68)`), action glows, and `IDC_HAND` cursor on interactive elements.
   - Added active color double-ring glow (`RGB(37, 99, 235)` outer ring, `RGB(255, 255, 255)` inner ring) and active width pill highlight.
   - Rendered shortcut badges (`Z`, `X`, `C`, `S`, `Enter`, `Esc`, `1`-`4`, `R`, `Y`, `B`, `G`, `K`, `W`) in crisp JetBrains Mono / Consolas monospace.
3. **Dedicated Documentation for All 7 Common Headers (`docs/`)**:
   - Created `docs/tiny_cli.md`, `docs/tiny_ipc.md`, `docs/tiny_dpi.md`, `docs/tiny_gui.md`, `docs/font.md`, `docs/clipboard.md`, and `docs/color-thief-algorithm.md`.

### Verification
- Built release targets cleanly via `pwsh -File .\build.ps1 all`.
- Verified compilation across all 9 utilities with 0 warnings/errors.
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

---

## 2026-08-25 — Dynamic Monitor DPI Scaling for Find My Mouse & Layout Stabilization for Capture / Color Picker

### Problem & Motivation
1. **Find My Mouse Spotlight Ballooning on External Monitors**: On laptop screens with high DPI (150%–200%), fixed pixel radius (100px) appeared compact, but when moving to an external 100% monitor (96 DPI), 100 pixels took up more than double the physical space on screen, making the spotlight balloon in size.
2. **Capture & Color Picker Layout Sensitivity**: The capture toolbar and selector option cards used an artificial `(float)fontSize / 13.0f` multiplier for coordinate scaling. Adjusting font sizes or moving across DPI displays distorted toolbar buttons, badge positions, and caused clipping or misalignment.

### Solution & Design
1. **Physical Size Consistency for Find My Mouse (`src/find-my-mouse.c`, `src/common/tiny_dpi.h`)**:
   - Added `TinyDPI_GetDpiForPoint(pt)` and `TinyDPI_GetDpiForMonitor(hMon)` to query active display DPI.
   - Scaled disc radius dynamically with monitor DPI: `g_spotRadius = MulDiv(BASE_SPOT_RADIUS, dpi, 96)` (`BASE_SPOT_RADIUS = 75`, `BASE_PULSE_RADIUS_MAX = 130`).
   - Physical diameter on screen remains constant (~1.56 inches / ~40 mm) across 100% desktop monitors and 150%/200% high-density laptop screens.
   - Dynamically re-renders the spotlight when cursor transitions across monitors of different DPIs.
2. **Physical Typography & Layout Consistency for Capture and Color Picker (`src/capture.c`, `src/color-picker.c`, `src/common/font.h`)**:
   - `TinyFont_GetSystemDefaultSize()` unscaled system font queries by `MulDiv(h, 96, sysDpi)` to establish a clean, standard 11px baseline at 96 DPI (equivalent to standard 9pt Segoe UI / Consolas) preventing double-scaling.
   - `TinyFont_ScaleSize(fontSize, dpi)` scales typography cleanly via `MulDiv(fontSize, dpi, 96)` for exact real-world physical parity across displays.
   - Refined `capture.c` (toolbar 672×40px, selector options 240×36px) and `color-picker.c` (default 10px monospace, compact padding and swatch) so typography and UI look standard, crisp, and compact.

### Verification
- Re-verified full build with `pwsh -File .\build.ps1 all` (all 9 target binaries compiled cleanly with exit code 0).

---

## 2026-08-25: IP Send Active Monitor Positioning & Theme, Pin-to-Top Window Picker, Capture Selector UX

### Problem & Motivation
1. **IP Send Opening on Wrong Monitor**: `ip-send` centered its window using `GetSystemMetrics(SM_CXSCREEN)`, forcing it to always appear on the primary display (laptop screen) rather than the active monitor where the user's mouse and focused work were located.
2. **IP Send Theme Flexibility**: Users working in bright environments requested a light theme toggle (`Ctrl + D`) with high-contrast typography.
3. **Pin to Top Blind Auto-Pinning on Double Click**: Double-clicking `pin-to-top.exe` or launching it directly immediately pinned whatever window had foreground focus (often Explorer), giving no visual choice. Users wanted an interactive window selection overlay identical to `capture --window`.
4. **Capture Mode Selector List Spacing**: The spacing between options in the mode selector was too loose, taking up excessive vertical screen space.

### Solution & Design
1. **IP Send Cursor Monitor Positioning (`src/ip-send.c`)**:
   - `wWinMain` now queries `MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST)` and centers the window within `mi.rcWork`.
2. **IP Send Light Theme (`Ctrl + D`) (`src/ip-send.c`)**:
   - Added `g_lightTheme` state toggled via `Ctrl + D` in `WM_KEYDOWN`.
   - In light mode: background `RGB(248, 249, 252)`, text `RGB(17, 20, 26)`, borders `RGB(205, 210, 222)`, accent `RGB(37, 99, 235)`.
   - Updated `/` keyboard shortcut dialog.
3. **Pin to Top Interactive Window Selection Picker (`src/pin-to-top.c`)**:
   - On double click or first launch without `--no-picker`, `pin-to-top` shows a full-desktop interactive picker overlay (`capture --window` style).
   - Real-time hover tracking outlines windows with dominant wallpaper accent borders, displaying a title badge (`[📌 Pin] App — Title` or `[📌 Pinned] App — Title`).
   - Left-click pins/unpins the window. Esc/Right-click cancels.
   - Preserves `--no-picker` / `--active` / `--foreground` for scriptable usage.
4. **Capture Mode Selector List Compacting (`src/capture.c`)**:
   - Tightened item height to 26px and gap to 3px in both `GetCenteredSelectorOptionRect` and `WM_PAINT`.

### Verification
- Built all 9 targets via `pwsh -File .\build.ps1 all` with exit code 0.

---

## 2026-09-04: IP Send Button Overlap and Dynamic Window Height Resizing Fixes (`src/ip-send.c`)

### Problem & Motivation
1. **Send Button Invisibility & Text Overlap**: When selecting multiple recipients or recipients with longer names (e.g. `To: abhijeet.j, Nikhil Kore, sachin.p...`), `TextOutW` rendered `toSummary` without width boundaries, overlapping directly with the right-aligned `[ SEND ]` button at the same `footY` position. Because background mode was transparent, the button text and recipient names collided into an unreadable mess (`[ sachin ]p,`).
2. **Window Height Resize Flinching**: When dragging window borders to adjust height, `RenderTUIWindow` executed `if (g_hWnd && height != neededH) SetWindowPos(g_hWnd, ...)` on every `WM_PAINT` cycle based on a hardcoded 4-row layout. This forced the window height to continuously snap back to ~400px while resizing.

### Solution & Design
1. **Send Button & Recipient Bounding**:
   - Anchored the Send button (`g_sendRect`) inside the bottom border frame with a dedicated styled button background and border.
   - Bounded the `To:` recipient summary using `DrawTextW` with `DT_END_ELLIPSIS | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX`, guaranteeing it cuts off cleanly before the Send button without any possibility of text collision.
2. **Dynamic Flexible List Height & Resizing**:
   - Removed `SetWindowPos` from `RenderTUIWindow`.
   - Anchored the footer section to the bottom of the client area and dynamically computed `visibleRows = availableHeight / itemHeight`.
   - Added `WM_GETMINMAXINFO` to protect against window collapse (minimum size 360×260).
   - Added `WM_MOUSEWHEEL` support for scrolling through recipient lists with the mouse wheel.
   - Added left-click item selection in `WM_LBUTTONDOWN` to easily toggle recipients.

### Verification
- Built release binary cleanly via `pwsh -File .\build.ps1 ip-send` (`dist/release/ip-send.exe`).
- Verified that resizing the window taller expands the visible recipient list smoothly without snapping back, and multiple recipients never overlap the Send button.
- Verified that offline recipients render cleanly in dim text without extra label badges, cannot be selected, and trigger a 3-second auto-dismissing toast warning (`ShowStatusToast`).

## [2026-09-04] Migration from Native C global-cmd to Dynamic cmdx (`src/cmdx.ps1`)

### Problem & Motivation
1. **Dynamic Extensibility**: In `global-cmd.c`, adding aliases or modifying paths required JSON edits and recompilation.
2. **Environment Variable Persistence**: Subprocess invocation from `.cmd` wrappers meant environment-modifying commands (like `refreshenv`) ran in child processes without affecting the active terminal session.
3. **Dedicated Directory Cleanliness**: Having 30+ generated shims in the repository root cluttered the workspace.

### Solution & Design
1. **Single-File Dynamic Registry & Dispatcher (`src/cmdx.ps1`)**:
   - Centralized declarative `$global:Commands` hashtable supporting ScriptBlocks and regex patterns with capture groups (`$1`, `$2`, ...) and `@args`.
   - Any modifications in `src/cmdx.ps1` take effect immediately on the next invocation without shim regeneration.
2. **Dedicated `cmds/` Shims Directory**:
   - `cmdx setup` generates all `.cmd` and `.ps1` shims inside `cmds/` and registers `cmds/` in User PATH (`HKCU\Environment`).
   - Clean architecture: Shims forward to `..\src\cmdx.ps1`.
3. **Dual-Shim Architecture (`.cmd` + `.ps1`)**:
   - `.cmd` shims for Command Prompt.
   - `.ps1` shims for instant in-session PowerShell execution without subprocess overhead.
   - Native batch `SET` for `refreshenv.cmd` and in-session `$env:Path` update for `refreshenv.ps1`.
4. **Environment Broadcast & Cleanup**:
   - Integrated Win32 `WM_SETTINGCHANGE` broadcast on `setup`, `clean`, and `refreshenv`.
   - `cmdx clean` purges the `cmds/` folder and cleans User PATH.

### Verification
- Ran `pwsh -File .\src\cmdx.ps1 setup` to generate 36 shims in `cmds/`.
- Verified execution of `cmdx help`, `cmdx`, `node16 -v`, `ll`, `refreshenv.cmd`, `refreshenv.ps1`.
- Cleaned up obsolete `src/global-cmd.c`, `dist/release/global-cmd.exe`, and updated `build.ps1`, `docs/cmdx.md`, `AGENTS.md`, and `CHECKLIST.md`.

---

## [2026-09-04] cmdx Path Resolution Fix for Standalone Script Directories

### Problem & Motivation
When `cmdx.ps1` was copied or executed from a standalone directory like `C:\scripts\cmdx.ps1` (outside the repo's `src/` hierarchy):
1. `Split-Path -Parent $scriptDir` incorrectly computed `$rootDir` as the drive root (`C:\`) and `$shimDir` as `C:\cmds`.
2. Running `cmdx clean` attempted to clean `C:\cmds` instead of `C:\scripts\cmds`, leaving `C:\scripts\cmds` registered in the User's PATH (`HKCU\Environment`).
3. Running `cmdx setup` generated shims pointing to relative paths `..\src\cmdx.ps1` (`C:\scripts\cmds\..\src\cmdx.ps1`), which did not exist.

### Solution & Design
1. **Zero-Hardcoding Dynamic Path Resolution (`Get-CmdxPaths`)**:
   - Computes `$scriptPath = [System.IO.Path]::GetFullPath($PSCommandPath)` based strictly on the running script's location at invocation time.
   - If the script resides inside a `src` subfolder, it locates `cmds/` in the parent repository root; otherwise, it creates `cmds/` right alongside the script in its folder.
   - Zero hardcoded path strings across setup, shim templates, or registry clean operations.
2. **Transparent Argument Passing & Execution**:
   - Replaced rigid `param()` blocks with direct `$args` slicing (`$Command = $args[0]`, `$CommandArgs = $args[1..N]`), allowing flags like `-v`, `--version`, or `--help` to pass cleanly without parameter binding collisions.
   - Used `& $scriptBlock @ArgsList` and `$global:CmdxMatches` to pass capture groups and arguments cleanly into dispatch routines.
   - Moved `Add-Type` invocation strictly inside `Broadcast-EnvironmentChange` so normal CLI executions avoid compilation delays.
3. **Dynamic PATH Cleanup**:
   - `Clean-GlobalShims` unregisters `$shimDir` and cleans User PATH (`HKCU\Environment`) purely based on the resolved paths and broadcasts `WM_SETTINGCHANGE`.

### Verification
- Verified `cmdx setup` and `cmdx clean` dynamically from both `C:\scripts\` and repository root (`.\src\cmdx.ps1`).
- Verified `node16.ps1 -v` and `node16.cmd -v` output `v16.20.2` in under 2 seconds.
- Verified User PATH is updated cleanly and dynamically with zero residue.

---

## [2026-09-06] Repository Git Initialization & Agent Hard Rule Enforcement

### Context & Goal
1. Standardize Git ignore rules for the project so build outputs (`dist/`), compiled binaries (`*.exe`, `*.pdb`), object files (`*.obj`, `*.o`), IDE files (`.vs/`, `.vscode/`), and OS temporary files are excluded.
2. Establish a non-negotiable project hard rule in `AGENTS.md` specifying that AI agents must never run `git add`, `git commit`, or `git push` unless explicitly requested by the user.

### Key Changes
1. **Created `.gitignore`**:
   - Excluded build output folder `dist/` and build artifacts (`*.exe`, `*.dll`, `*.pdb`, `*.lib`, `*.obj`, `*.o`, etc.).
   - Excluded IDE config directories (`.vs/`, `.vscode/`, `.idea/`) and OS temporary files (`Thumbs.db`, `Desktop.ini`, `*.tmp`, `*.log`).
2. **Updated `AGENTS.md` Hard Rules**:
   - Added **GIT RULE**: "Agents must NOT run `git add`, `git commit`, or `git push` unless explicitly requested by the user."

### Verification
- Ran `git status` to verify `.gitignore` excludes `dist/` and untracked binaries.
- Staged `.gitignore`, `AGENTS.md`, `docs/JOURNEY.md`, `docs/CHECKLIST.md` and created initial Git commit as requested by the user.

