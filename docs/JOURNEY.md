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

---

## [2026-09-06] Implementation of Tiny Window Switcher (`window-switcher.c`)

### Context & Goal
Added a new utility: **Tiny Window Switcher** (`window-switcher.exe`).
Requirements:
1. Native Windows utility written in C using Win32 and DWM APIs.
2. Enumerate open top-level application windows and group/sort based on CLI options (`--group=app|window|none`, default `none`; `--sort=mru|name|recent`, default `name`).
3. Compute shortest unique prefix of each app name as a hint badge (`Chrome -> C`, `Code -> CO`, `Chromium -> CH`).
4. Display minimal overlay with app hints and hardware-accelerated live DWM window previews (`DwmRegisterThumbnail`, `DwmUpdateThumbnailProperties`).
5. Support instant activation via hint key typing, cycling through multi-window app instances, and Alt+Tab muscle memory.
6. Strictly controlled via CLI args with zero GUI configuration or disk persistence.

### Architecture & Implementation Highlights
1. **Source File & Spec**:
   - Source: `src/window-switcher.c` -> `window-switcher.exe`
   - Spec: `docs/window-switcher.md`
2. **CLI Option Parsing (`tiny_cli.h`)**:
   - Enhanced `tiny_cli.h` to support `--option=value` as well as `--option value` syntax.
   - Parsed `--group` (`app`, `window`, `none`) and `--sort` (`name`, `mru`, `recent`).
3. **Window Enumeration & Filtering**:
   - Enumerate visible, non-cloaked, non-tool windows owned by applications.
   - Retained process ID and extracted base executable name (`GetProcessAppName`) for clean app naming.
4. **Shortest Unique Prefix Hint Algorithm**:
   - Implemented a greedy disambiguation prefix algorithm across unique application process names.
   - Generates shortest unique uppercase prefixes (`C`, `CO`, `CH`).
   - Appends window sub-indices (`C1`, `C2`) for multi-window applications when grouped by window/none.
5. **Live DWM Previews & GDI Double-Buffering**:
   - Created dark-mode overlay window centered on the active monitor (`MonitorFromPoint`).
   - Used `DwmRegisterThumbnail` and `DwmUpdateThumbnailProperties` for crisp hardware-accelerated window previews.
   - Rendered high-contrast hint badges and window titles on double-buffered GDI surface.
6. **Input Handling & Muscle Memory**:
   - Typing hint letters immediately matches and activates or cycles target windows.
   - Supports `Tab`, `Shift+Tab`, `Arrows`, `Enter`/`Space`, `Backspace`, `Escape`, mouse hover/click, and Alt key release.
7. **Single Instance Toggle IPC**:
   - Integrated `TinyIPC_AcquireOrToggle` with `Global\TinyWindowSwitcherMutex` and `Global\TinyWindowSwitcherEvent`.

### Refinement & Fix (Shared App Name Prefixes like `Zen` and `Zed`)
- **Problem**: When apps shared initial prefix letters (e.g. `Zen` and `Zed`), `ComputeAppHints` previously only checked collisions against previously assigned hints instead of all other application names. `Zen` claimed hint `Z` while `Zed` claimed `ZE`. When typing `Z`, an exact match for `Zen` was found immediately, causing instant auto-activation before the user could finish typing (`ZE`, `ZEN`, or `ZED`).
- **Fix**:
  1. Updated `ComputeAppHints` collision loop to verify candidate prefixes against all other open application names (`_wcsnicmp(g_appGroups[other].appName, hintCandidate, len) == 0`). `Zen` and `Zed` now correctly calculate shortest unique hints `ZEN` and `ZED`.
  2. Updated `MatchTypedBuffer` auto-activation threshold: typing `Z` or `ZE` now filters and highlights candidates without auto-activating because `totalMatches == 2`. Auto-activation triggers ONLY when `totalMatches == 1` (e.g. when typing `D` for `ZED` or `N` for `ZEN`).
  3. Added single-key repeated press cycling in `WM_CHAR`: if `Z` is pressed again while the typed buffer is `"Z"`, the selection cycles cleanly to the next matching app (`Zed`), allowing single-key cycling as well as multi-letter search.

8. **Full-Screen Monitor Background Effects System (`--background none|blur|dim|tint`)**:
   - Added CLI flags: `--background` (`blur`, `dim`, `tint`, `none`), `--blur` (`1..100`), `--dim` (`0..100`), `--tint-color` (hex `#RRGGBB`), and `--tint-opacity` (`0..100`).
   - Default: `--background blur --blur 20`.
   - `ComputeLayout`: sizes and positions `g_hwndOverlay` across the **FULL active monitor bounds** (`mi.rcMonitor`).
   - `ApplyWindowCompositionBlur`: calls `SetWindowCompositionAttribute` with `ACCENT_ENABLE_ACRYLICBLURBEHIND` for DWM Acrylic backdrop composition over the full monitor screen.
   - `PaintOverlay`: fills full-screen backdrop with translucent GDI tint/dim colors while rendering window cards directly on the backdrop seamlessly.

### Verification
- Registered target `window-switcher` in `build.ps1` with libraries `user32`, `gdi32`, `dwmapi`, `shell32`, `ole32`.
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Smoke tested executable launch and double-launch IPC toggle event handling.
- Updated `AGENTS.md`, `docs/CHECKLIST.md`, `docs/JOURNEY.md`, and `docs/window-switcher.md`.

---

## [2026-09-06] 2D Grid Arrow Key Navigation & 3-Tier Match Priority Hierarchy (`window-switcher.c`)

### Context & User Feedback
1. **Vertical Arrow Navigation**: When navigating the window overlay grid using UP and DOWN arrow keys, selection previously moved by single item offsets (`-1` / `+1`) instead of jumping vertically across grid rows (`-cols` / `+cols`).
2. **Hint vs Title Matching Collision**: When typing key `F` while both `File Pilot` (assigned app hint `F`) and `Windows Explorer` (window title `"File Explorer"`, assigned app hint `E`) were open, `Windows Explorer` matched because window title prefix matching competed directly with app hint matching.

### Solution & Engineering Refinements
1. **2D Grid Arrow Navigation**:
   - Tracked total calculated columns (`g_layoutCols`) during layout computation.
   - Refactored `VK_UP` to move index backward by `cols` (`(idx - cols + total) % total`), `VK_DOWN` to move forward by `cols` (`(idx + cols) % total`), `VK_LEFT` by `-1`, and `VK_RIGHT` by `+1`.
2. **3-Tier Match Priority Hierarchy (`MatchTypedBuffer`)**:
   - Structured search matching into explicit priority tiers:
     - **Tier 1 (Exact App Hint Match)**: Checks if typed buffer matches an app's assigned shortcut hint exactly (e.g., `F` matches `File Pilot`). If Tier 1 matches exist, all title prefix matches are ignored.
     - **Tier 2 (App Name / Hint Prefix Match)**: Checks if typed buffer is a prefix of app hint or app base name.
     - **Tier 3 (Window Title Prefix Match)**: Fallback search matching against window title prefixes.
   - Guaranteed exact app hint shortcuts take absolute priority over title prefix substrings, preventing `File Explorer` from stealing focus when `F` is typed for `File Pilot`.

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.


---

## [2026-09-06] Common Headers Enforcement Rule for AI Agents

### Context & Goal
Ensure AI agents always reuse existing shared common headers in `src/common/` (`tiny_cli.h`, `tiny_ipc.h`, `tiny_dpi.h`, `tiny_gui.h`, `font.h`, `clipboard.h`, `color-thief-algorithm.h`) rather than reinventing duplicate or custom implementations.

### Key Changes
1. **Updated `AGENTS.md`**: Added **COMMON HEADERS RULE** to the `## HARD RULES` section.
2. **Updated Header Documentation**: Documented `font.h` and `clipboard.h` in the `## Shared Common Headers` section of `AGENTS.md`.

### Verification
- Verified `AGENTS.md` formatting and references to `src/common/` headers.

---

## [2026-09-06] Full-Screen Responsive Card Layout Mode (`window-switcher.c`)

### Context & Goal
User requested an experiment: to have open window cards dynamically scale and span across 100% of the active display screen rather than being constrained inside a fixed-size card container (220×175).

### Solution & Engineering Refinements
1. **Dynamic Full-Screen Card Layout Engine**:
   - Added CLI option `--layout full|tile|center` (or `-l`), defaulting to `--layout full`.
   - In `ComputeLayout`, when `--layout full` or `--layout tile` is selected, `availW` and `availH` are calculated dynamically from active monitor bounds (`monW` × `monH`).
   - `cardW` and `cardH` scale up responsively (`availW / cols` and `availH / rows`) so that preview cards expand to occupy the entire monitor area.
   - Preserved `--layout center` option for users who prefer the compact centered card box.

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-06] Base & Extended App Name Disambiguation Hints (`Notepad` vs `Notepad+`) (`window-switcher.c`)

### Context & Goal
User requested an improvement for apps where one app name is a base prefix of another (e.g. `Notepad` vs `Notepad+` or `Notepad++`):
Assign the shortest prefix (e.g. `N`) to the base app (`Notepad`) and the base hint + distinguishing suffix (e.g. `N+` or `N++`) to the extended app (`Notepad+`).

### Solution & Engineering Refinements
1. **Base/Extended App Prefix Resolution in `ComputeAppHints`**:
   - When evaluating prefix collision for app `g` (`Notepad`), if `g` is a strict base prefix of `other` (`Notepad+`), collision against `other` is bypassed so `g` claims the clean minimal prefix (`N`).
   - For extended app `other` (`Notepad+`), its hint is constructed dynamically by taking the base app's hint (`N`) + the extra trailing distinguishing characters (`+`), producing `N+`.
2. **Multi-Match Delay Guard in `MatchTypedBuffer`**:
   - In Tier 1 exact hint matching, if `g_typedBuf` is `"N"`, exact hint match finds `Notepad` (`N`), but `prefixMatches` checks if any longer hint (like `"N+"`) also starts with `"N"`.
   - If `prefixMatches > 1`, typing `N` highlights `Notepad` without auto-activating, giving the user time to type `+` for `Notepad+` (or hit `Enter`/`Space`/`Tab` for `Notepad`).

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-06] Prominent Centered Shortcut Hint Badges (`window-switcher.c`)

### Context & Goal
User feedback: Small hint badges tucked in the top-left corner required scanning text to read shortcut keys. Placing large, bold hint badges directly in the **center** of each app preview card (matching Vimarchy / Hyprland overlay switchers) makes shortcut keys instantly readable at a glance.

### Solution & Engineering Refinements
1. **Centered Prominent Hint Badge Rendering**:
   - Created `g_hFontHintLarge` (24pt bold Segoe UI typography).
   - In `PaintOverlay`, rendered App Name + Window Title in clean top header bar.
   - Positioned rounded hint badge pill right in the center (`(left + right) / 2`, `(top + bottom) / 2`) of each app preview card over the live DWM thumbnail.
   - Styled badge with high-contrast accent fill (`RGB(0, 110, 230)` selected, `RGB(20, 32, 48)` default) and bright border outline (`RGB(80, 190, 255)`).

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Real UWP Process Name Resolution & Header Text Alignment Fix (`window-switcher.c`)

### Context & User Feedback
1. **UWP Store App Generic Process Name (`ApplicationFrameHost.exe`)**: UWP Windows Store apps (Calculator, Clock, Photos, Settings, etc.) run inside `ApplicationFrameHost.exe`. They all reported process name `"ApplicationFrameHost"`, causing them to group together under `A1`, `A2`, `A3` without individual application hints (`C`, `CL`, `S`).
2. **Top Header Text Alignment**: The top status header text (`Window Switcher | Layout: Full ...`) had an offset margin (`g_containerRect.left + 20`), causing text alignment to be slightly off relative to the left margin of the grid container.

### Solution & Engineering Refinements
1. **Real UWP App Resolution (`GetUWPRealProcessId`)**:
   - Added `GetUWPRealProcessId(hwnd)` helper querying child `Windows.UI.Core.CoreWindow` PID on `ApplicationFrameHost.exe` host windows.
   - Replaced generic `ApplicationFrameHost` process names with real UWP app names (`CalculatorApp.exe` $\rightarrow$ `Calculator`, `TimeApp.exe` $\rightarrow$ `Clock`, etc.) or parsed clean app titles.
   - `Calculator` now receives hint **`C`** (or `CA`), `Clock` receives hint **`CL`**, and `Settings` receives hint **`S`**.
2. **Precision Header Text Alignment**:
   - Aligned `headerRect.left` precisely to `g_containerRect.left`, matching the exact left margin baseline of the preview card grid.

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

---

## [2026-09-07] Card Layout Preview Thumbnail Bottom Padding Fix (`window-switcher.c`)

### Context & User Feedback
User noticed that preview cards had an extra empty footer space at the bottom.

### Cause & Solution
- **Cause**: Previously, `padBottom = 32` reserved room for window titles at the bottom of cards. When titles moved to the top header bar, `padBottom` remained 32px, creating an empty dark gap below thumbnails.
- **Fix**: Reduced `padBottom` from 32px to 8px across both `LAYOUT_FULL` and `LAYOUT_CENTER` modes. DWM live window thumbnails now expand seamlessly to fill the bottom of each card.

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

---

## [2026-09-07] Right-Side Card Header Shortcut Hint Placement & Single-Window Consolidation (`window-switcher.c`)

### Context & User Request
User requested keeping the shortcut hint badge on the **right side of the title on each card header**.

### Implementation & Architecture Benefits
1. **Right-Aligned Header Hint Badge, Normal Font Size, Borderless Styling & Common Font Header Reuse**:
   - Reused shared `font.h` module (`TinyFont_GetBestUIFace()`) for DPI-scaled system font selection across font constructors in `WM_CREATE`.
   - Updated shortcut hint text to standard/normal bold font size (`g_hFontHint` 15pt bold) while preserving pure white text color (`RGB(255, 255, 255)`).
   - Shortcut hint badge (`[ F ]`, `[ N+ ]`, `[ C1 ]`) renders cleanly on the right side of each card's top header bar (`r.right - 10 - badgeW` to `r.right - 10`).
   - Removed badge border outline (`GetStockObject(NULL_PEN)`) for a modern borderless pill badge styling.
   - Applied a distinct, vibrant accent background color (`RGB(0, 120, 240)` selected, `RGB(35, 85, 155)` hovered, `RGB(48, 64, 90)` normal) that stands out sharply against the dark card container background (`RGB(28, 30, 37)`).
   - App title (`App Name — Window Title`) renders on the left side (`r.left + 10` to `badgeRect.left - 8`) with smooth ellipsis truncation (`DT_END_ELLIPSIS`).
2. **Single-Window Architecture (`g_hwndOverlay` Only)**:
   - Because the hint badge sits in the card header bar (`r.top+5` to `r.top+29`) strictly *above* `previewRect` (`r.top+34` to `r.bottom-8`), DWM live window preview thumbnails can **never** cover or obscure the hint badge or title text.
   - Removed `g_hwndHintLayer`, eliminating all multi-window layered rendering overhead, Z-order flicker, and activation complexity.

### Verification
- Built release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug window-switcher` -> `dist/debug/window-switcher.exe` (0 errors).
- Smoke tested single-instance IPC toggle pattern (`Global\TinyWindowSwitcherMutex` / `Global\TinyWindowSwitcherEvent`).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

---

## [2026-09-07] Shortest Unique Prefix/Subsequence Candidate Hint Algorithm (`window-switcher.c`)

### Context & User Feedback
User reported that the application hint computation algorithm was not generating shortest unique prefix/subsequence hints as intended (e.g. `Chrome -> C`, `Code -> CO`, `Chromium -> CH`, `Zen -> ZN`, `Zed -> ZD`).

### Root Cause & Algorithm Redesign
- **Root Cause**: The previous prefix loop generated contiguous prefixes (`C`, `CH`, `CHR`, `CHRO`, `CHROM`, `CHROME`) and checked if any prefix was a prefix of another open app. When multiple apps shared a prefix (e.g. `Chrome`, `Code`, `Chromium` all starting with `C`, or `Zen` and `Zed` both starting with `ZE`), `len=1` (`C`) collided with all of them, forcing `Chrome` to require `CHROME` (6 chars) and `Zen` to require `ZEN` (3 chars).
- **New Candidate Prioritization Algorithm**:
  1. Built a candidate generator `GenerateCandidateHints(appIdx, candidates, &outCount)` that generates ordered candidate hints:
     - **Base + Extension**: For extended app names (e.g. `Notepad++` vs `Notepad`), generates base hint + suffix (`N+`).
     - **Word Initials / Acronyms**: For multi-word apps (e.g. `File Pilot` -> `FP`, `Visual Studio Code` -> `VSC`).
     - **Shared 2-Letter Prefix Distinguishing Characters**: When apps share the first 2 letters (e.g. `Zen` vs `Zed` sharing `ZE`), finds the first character index $k \ge 2$ where the names differ, producing 2-letter distinguishing hints (`ZN` for Zen, `ZD` for Zed).
     - **Single-Letter Hint**: First letter $N[0]$ (`C` for Chrome).
     - **Contiguous 2-Letter Prefix**: First 2 letters $N[0]N[1]$ (`CO` for Code, `CH` for Chromium).
     - **Contiguous 3+ Letter Prefixes & Fallback Numbers**: (`CHR`, `C1`).
  2. Multi-pass assignment selects the first available non-colliding candidate for each application group.

### Output Verification & Results
- `Chrome` -> `C`
- `Code` -> `CO`
- `Chromium` -> `CH`
- `Zen` -> `ZN`
- `Zed` -> `ZD`
- `Notepad` -> `N`, `Notepad++` -> `N+`
- `File Pilot` -> `FP`

### Build & Verification
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Windows Standard Alt-Tab Tile Grid Layout & App Grouping Verification (`window-switcher.c`)

### Context & User Feedback
1. User expected `--layout tile` to behave like the standard Windows Alt-Tab UI (a centered floating panel with neat 3:2 preview tile cards), but noticed it previously stretched edge-to-edge full screen.
2. User requested checking whether grouping by application name (`--group app` / `--sort name`) was implemented and active.

### Engineering Solution & Refinements
1. **Windows Standard Alt-Tab Centered Tile Grid Layout (`LAYOUT_TILE`)**:
   - Separated `LAYOUT_TILE` from `LAYOUT_FULL` in `ComputeLayout()`.
   - `LAYOUT_TILE` calculates a centered floating container panel (`g_containerRect`) on the active monitor with rounded corners (`16px`), background fill (`RGB(24, 26, 33)`), and subtle outline border (`RGB(55, 60, 75)`).
   - Arranges cards into a centered grid of up to 5 columns per row with fixed 240x160px tile dimensions (3:2 aspect ratio preview tiles) and 16px gap spacing, matching standard Windows Alt-Tab UI.
   - Set `LAYOUT_TILE` as default layout mode in `g_cfg.layout`.
2. **Application Grouping & Alt-Tab State Tracking**:
   - Updated `wWinMain` to check `if (g_cfg.sort == SORT_NAME || g_cfg.group == GROUP_APP)` so that passing `--group app` explicitly sorts all open window cards by `appName` (and secondarily `title`), placing windows of the same application adjacent to each other.
   - Added startup Alt-key state query (`g_altTabMode = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;`) so launching via Alt keybindings defaults selection to the 2nd item (MRU previous active window) and automatically switches upon releasing Alt key.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Layout Simplification & Container Header Text Alignment Fix (`window-switcher.c`)

### Context & User Feedback
User requested removing `tile` layout mode (consolidating layout modes into `center` default and `full`) and fixing header text alignment on `center` so that `Window Switcher | Layout...` aligns precisely with the left edge of the cards inside the centered container panel instead of sitting against the container border.

### Solution & Engineering Changes
1. **Layout Consolidation**:
   - Consolidated `LAYOUT_TILE` into `LAYOUT_CENTER` (default mode). `--layout tile` maps cleanly to `LAYOUT_CENTER` for CLI compatibility.
   - `LAYOUT_CENTER` is the default layout mode, rendering a sleek centered container panel with 240x160px cards (up to 5 columns per row).
2. **Text Alignment Fix**:
   - Updated `headerRect` calculation in `PaintOverlay()`:
     `int headerLeft = (g_cfg.layout == LAYOUT_CENTER) ? (g_containerRect.left + 20) : g_containerRect.left;`
     `int headerTop  = (g_cfg.layout == LAYOUT_CENTER) ? (g_containerRect.top + 16) : (g_containerRect.top + 4);`
     `RECT headerRect = { headerLeft, headerTop, g_containerRect.right - 20, headerTop + 24 };`
   - `headerLeft` matches card column 0 left offset (`containerX + padding`), ensuring header status text aligns perfectly with the left edge of the cards.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] UI Layout Metrics Unification & Top Header Gap Elimination (`window-switcher.c`)

### Context & User Feedback
User inquired why `center` and `full` layouts had a large vertical gap between the top header status text (`Window Switcher | Layout...`) and the card grid, and asked if layout metrics were hardcoded inline instead of centralized.

### Engineering Changes & Solution
1. **Centralized UI Layout Metrics Constants**:
   - Defined top-level layout metric constants at the header of `src/window-switcher.c`:
     ```c
     #define UI_CONTAINER_PADDING  18
     #define UI_HEADER_HEIGHT       34
     #define UI_CARD_GAP            16
     #define UI_HEADER_TEXT_HEIGHT  22
     ```
2. **Vertical Gap Elimination**:
   - Replaced scattered inline offset calculations in `ComputeLayout()` and `PaintOverlay()` with centralized constants.
   - Fixed `headerH = UI_HEADER_HEIGHT` (`34px`) and `headerTop` positioning so the vertical gap between the status header text bottom and the top row of cards is reduced from 24px down to a clean, mathematically tight **12px** across both `LAYOUT_CENTER` and `LAYOUT_FULL` modes.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Hex Color Alpha Parsing & `--tint-opacity` Removal (`window-switcher.c`)

### Context & User Directives
User requested removing `--tint-opacity` CLI flag and consolidating tint color and alpha into `--tint-color` using 8-character hex strings with alpha (e.g. `#00000005` or `#RRGGBBAA`).

### Engineering Solution
1. **Hex Color Alpha Parser (`ParseHexColorWithAlpha`)**:
   - Parses 8-digit hex strings (`#RRGGBBAA` or `0xRRGGBBAA`): extracts `R`, `G`, `B` values and 8-bit `Alpha` opacity (e.g. `05` hex $\rightarrow$ `5` out of `255`).
   - Parses 6-digit hex strings (`#RRGGBB`): extracts `RGB` and defaults `Alpha` to `255` (100% opacity).
2. **CLI & Config Cleanup**:
   - Removed `--tint-opacity` flag from `options` array in `ParseCLI()` and updated `Config` struct (`COLORREF tintColor`, `BYTE tintAlpha`).
   - Updated `ShowHelp()` documentation and `PaintOverlay()` tint background blending.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] True DWM Translucent Tint Backdrop & Sort Mode Simplification (`window-switcher.c`)

### Context & User Directives
1. **Sort Mode Consolidation**: User requested removing `mru` and keeping `recent` (and `name`). Default sort order updated to `recent`. `--sort mru` maps to `SORT_RECENT` for CLI compatibility.
2. **True DWM Translucent Backdrop Tint Fix**: User reported that passing `--tint-color #FFFFFF50` rendered a solid dimmed white box instead of a translucent 31% white tint layer over the desktop.

### Root Cause & Engineering Fix
- **Root Cause**: Previously, `PaintOverlay()` painted a 100% opaque solid RGB brush `bgFillColor` over the full screen DC `memDC`. GDI solid brush fills overwrote DWM's composition backdrop layer, turning translucent tint colors into solid opaque gray/dimmed-white pixels.
- **Fix**:
  1. Updated `ApplyBackgroundComposition(hwnd)` to apply `SetWindowCompositionAttribute` with `ACCENT_ENABLE_TRANSPARENTGRADIENT` and ABGR `gradientColor = (alpha << 24) | (b << 16) | (g << 8) | r`.
  2. For `BG_BLUR`, `BG_DIM`, and `BG_TINT`, `PaintOverlay()` fills `memDC` with `RGB(0, 0, 0)`, allowing DWM's translucent composition layer to shine through cleanly over the desktop background behind the switcher.
  3. Passing `--tint-color #FFFFFF50` now renders a true, translucent 31% white tint layer over desktop windows.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Dual-Tier Composition Policy & Container Translucency for `--blur` (`window-switcher.c`)

### Context & User Directives
User reported that there was little to no noticeable visual difference between `--blur 1` and `--blur 100`.

### Root Cause Analysis
1. **Fixed DWM Kernel Blur Radius**: Windows 10/11 DWM (`SetWindowCompositionAttribute`) uses a fixed Gaussian blur kernel radius (~30px) for `ACCENT_ENABLE_ACRYLICBLURBEHIND`. Windows OS does not expose a variable pixel blur radius parameter in its composition policy.
2. **Compressed Alpha Curve**: Previously, `blurAmount` (1..100) mapped linearly to acrylic tint alpha `(blurAmount * 220)/100 + 20` (alpha 22 to 240) using only Acrylic composition (`ACCENT_ENABLE_ACRYLICBLURBEHIND`), which only slightly adjusted the darkness of the frosted noise tint without altering the composition style.
3. **Solid Container Panel Blocking DWM Composition**: In `PaintOverlay()`, `LAYOUT_CENTER` painted an opaque solid GDI brush (`RGB(24, 26, 33)`) over `g_containerRect`. This blocked DWM's blurred backdrop from showing through the container where all the cards sit.

### Engineering Solution
1. **Dual-Tier DWM Accent Policy Mapping**:
   - **Low Blur (`1..30`)**: Mapped to Windows Aero Glass Blur (`ACCENT_ENABLE_BLURBEHIND`, policy state 3) with light gradient alpha (`5..80`). `--blur 1` produces an ultra-clear, translucent glass window where desktop windows behind the switcher are sharply visible through soft glass.
   - **High Blur (`31..100`)**: Mapped to Windows Acrylic Frosted Blur (`ACCENT_ENABLE_ACRYLICBLURBEHIND`, policy state 4) with heavy gradient alpha (`85..255`). `--blur 100` produces a deep, dark, heavy acrylic backdrop.
2. **Translucent Container Panel & Card Fills**:
   - Updated `g_containerRect` background fill to `RGB(10, 12, 16)` and idle card backgrounds to `RGB(18, 20, 26)` when blur mode is active, allowing DWM backdrop composition to shine through the entire container panel.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Verified dual-tier blur policy rendering and updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Variable Tint Transparency & Auto `BG_TINT` Activation (`window-switcher.c`)

### Context & User Directives
User reported that transparency in tint felt broken and that passing `--tint-color #FFFFFF50` vs `--tint-color #FFFFFF90` resulted in the exact same background view.

### Root Cause Analysis
1. **Unset `bgMode` Default**: `g_cfg.bgMode` defaulted to `BG_BLUR`. When `--tint-color` was passed without `--background tint`, `bgMode` remained `BG_BLUR`, so `BG_TINT` logic was completely bypassed and the blur backdrop was applied both times.
2. **Missing `WS_EX_LAYERED` Style**: Windows DWM (`ACCENT_ENABLE_TRANSPARENTGRADIENT`) requires `WS_EX_LAYERED` on the window along with `SetLayeredWindowAttributes` to enable variable composition alpha across the desktop backdrop. Without `WS_EX_LAYERED`, DWM renders a fixed solid/dim composition opacity regardless of alpha.

### Engineering Solution
1. **Automatic Tint Mode Selection**: Updated `ParseCLI()` to automatically set `g_cfg.bgMode = BG_TINT` whenever `--tint-color` is supplied on the CLI and `--background` was not explicitly passed.
2. **Layered Window Attributes**: Added `WS_EX_LAYERED` to `CreateWindowExW()` and invoked `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` in `ApplyBackgroundComposition()`. Passing `#FFFFFF50` now produces a light 31% white translucent wash, while `#FFFFFF90` produces a distinct 56% white translucent wash.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Removal of Dim Background Mode & Default Sort to Name (`window-switcher.c`)

### Context & User Directives
User requested removing `dim` background mode and changing the default sort order to `name`.

### Engineering Solution
1. **Removed `dim` Background Mode**:
   - Removed `BG_DIM` from `BgMode` enum, `dimAmount` from `Config` struct, and `--dim` CLI option from `ShowHelp()` / `ParseCLI()`.
   - Backdrop modes are streamlined to `blur`, `tint`, and `none`.
2. **Default Sort Order `name`**:
   - Updated `g_cfg.sort` default to `SORT_NAME` (alphabetical by app name). `--sort recent` remains available for MRU Z-order sorting.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Container Panel Box Removal in Center Layout (`window-switcher.c`)

### Context & User Directives
User requested removing the background box and border around the centered layout container panel to eliminate the "box inside box" visual clutter.

### Engineering Solution
- Removed the container panel background fill (`panelBg`) and outer border (`RoundRect`) in `PaintOverlay()` for `LAYOUT_CENTER`.
- Cards now float cleanly directly on the full-screen desktop backdrop (blur, tint, or none) while maintaining their centered 2D grid alignment and header status text.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Status Header Text Alignment Fix in `LAYOUT_FULL` Mode (`window-switcher.c`)

### Context & User Directives
User reported that text alignment in `--layout full` (`-l full`) mode was off.

### Root Cause & Engineering Fix
- **Root Cause**: In `PaintOverlay()`, `headerLeft` for `LAYOUT_FULL` was calculated as `g_containerRect.left + 20` (which resulted in `20 + 20 = 40px`), while column 0 cards started at `marginX = 20px`. This caused the status header text to be indented 20px to the right of column 0 cards.
- **Fix**: Updated `headerLeft` in `PaintOverlay()`:
  `int headerLeft = (g_cfg.layout == LAYOUT_CENTER) ? (g_containerRect.left + UI_CONTAINER_PADDING) : g_containerRect.left;`
  `headerLeft` in `LAYOUT_FULL` mode now evaluates directly to `g_containerRect.left` (`20px`), matching the left boundary of column 0 cards with 100% pixel precision.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Contiguous App Grouping Fix for `--group app` & `--group window` (`window-switcher.c`)

### Context & User Directives
User reported that grouping appeared to always remain `none` regardless of the `--group` mode passed on the CLI.

### Root Cause & Engineering Fix
- **Root Cause**: `wWinMain()` previously only checked `if (g_cfg.sort == SORT_NAME || g_cfg.group == GROUP_APP)`. It did NOT check `g_cfg.group == GROUP_WINDOW`. Thus, passing `--group window` fell through to `qsort(..., CompareMRU)` or `CompareName` without sorting windows of the same app into contiguous adjacent card grid slots.
- **Fix**:
  1. Created `CompareAppGroup` comparator: sorts window items by `appName` first (so all windows belonging to the same app sit adjacently in consecutive card grid cells), and sub-sorts by `mruOrder` or `title`.
  2. Updated `wWinMain()`: whenever `g_cfg.group != GROUP_NONE`, applies `qsort(..., CompareAppGroup)`.
  3. Updated `ComputeAppHints()`:
     - `--group app`: All windows of the app share the main app hint (e.g. `C`), allowing single-key shortcut typing to cycle focus through windows of the app.
     - `--group window`: Windows of the app sit adjacently in the grid and receive distinct window sub-hints (`C1`, `C2`).
     - `--group none`: Windows are sorted by the active sort mode without forcing contiguous app grouping.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Search Filter Dynamic Grid Relayout & Group Option Removal (`window-switcher.c`)

### Context & User Directives
User requested:
1. Remove `--group` CLI flag and grouping logic entirely.
2. Implement dynamic search filter grid relayout: when characters are typed into the search filter (`g_typedBuf`), hide non-matching cards completely and dynamically recalculate grid column/row metrics (`ComputeLayout`) to display **only** matching windows on screen.

### Architectural & Technical Implementation
1. **Removal of `--group` Flag**:
   - Removed `GroupMode` enum (`GROUP_NONE`, `GROUP_APP`, `GROUP_WINDOW`), `g_cfg.group`, `CompareAppGroup` comparator, and `-g, --group` CLI arguments.
   - Simplified `wWinMain()` to sort by `name` (`CompareName`) or `recent` (`CompareMRU`).
2. **Dynamic Filter & Live Grid Recalculation**:
   - Implemented `IsItemMatchingFilter()` (case-insensitive search matching hint, app name, and title) and `GetFilteredIndices()`.
   - Updated `ComputeLayout()`: computes grid dimensions based on `visibleCount = GetFilteredIndices(visibleIndices)`. If `visibleCount` is 0, layout reserves space for 1 placeholder message card ("No matching windows").
   - Updated `PaintOverlay()`: iterates strictly over `visibleIndices` to render visible matching cards. Unmatched DWM live window thumbnails are explicitly hidden via `DWM_THUMBNAIL_PROPERTIES` with `fVisible = FALSE`.
   - Navigation & Interactions: grid arrow navigation, single-key cycling, Tab/Backspace, mouse hover, and mouse click hit-testing operate exclusively over `visibleIndices`.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Smoke tested dynamic filter typing and double-launch IPC toggle.
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Background Mode Cleanup: Removed `none` (`window-switcher.c`)

### Context & User Directives
User requested: "remove none from bg"

### Changes & Implementation
1. **BgMode Enum & CLI Parsing**:
   - Removed `BG_NONE` from `enum BgMode`, leaving `BG_BLUR` and `BG_TINT`.
   - Updated `ParseCLI()` and `ShowHelp()` to restrict `-bg, --background` options strictly to `blur` or `tint` (`blur` default).
2. **Rendering & Overlay Cleanup**:
   - Removed `BG_NONE` condition from `ApplyBackdropEffect()` so `SetLayeredWindowAttributes` is always configured for translucent overlay window attributes.
   - Updated `PaintOverlay()` status header string (`BG: Blur` or `BG: Tint`) and card background color rendering (`cardBg`).

### Verification & Build
- Compiled release target cleanly via `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Tiered Search Filter Priority & Instant Hint Auto-Activation (`window-switcher.c`)

### Context & User Directives
User reported: "check something is wrong with filter bcz when i pressed c then h then it should have made me go to that app instead why i have to press enter & why zed is showing?"

### Root Cause Analysis
1. **Unwanted Substring Matching**: `GetFilteredIndices()` previously performed a flat case-insensitive `wcsstr` substring search on `item->title`. Because `Zed`'s title contained `ch` (in a file path/name like `main.ch` / `checklist.md`), `Zed` matched `CH`, causing `visibleCount` to equal 2 (`Chrome` + `Zed`).
2. **Auto-Activation Block**: Because `visibleCount` was 2 instead of 1, `MatchTypedBuffer()` did not auto-activate Chrome when `CH` was typed, forcing the user to press `Enter`.

### Engineering Fixes
1. **Tiered Filtering Priority (`GetFilteredIndices`)**:
   - **Tier 1 & 2**: Checks App Name and Hint prefix/word matches (`IsItemAppOrHintMatch`). If any items match via hint or app name, those items are returned exclusively.
   - **Tier 3 (Fallback)**: Title substring matching (`wcsstr`) is executed ONLY if zero items match by app name or hint. This filters out unrelated windows like `Zed` when typing app shortcut hints like `CH`.
2. **Instant Hint Auto-Activation (`MatchTypedBuffer`)**:
   - Checks if `g_typedBuf` is an exact match for an assigned hint (`_wcsicmp(hint, g_typedBuf) == 0`). If an exact hint match exists (e.g. `CH` for Chrome), the window auto-activates immediately.
   - If a single matching window remains (`visibleCount == 1`) whose hint/appName prefix matches `g_typedBuf`, it auto-activates instantly without requiring `Enter`.

### Verification & Build
- Compiled release target cleanly via `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Configurable Auto-Activation Delay CLI Option (`-d, --delay <ms>`) (`window-switcher.c`)

### Context & User Directives
User requested: "add delay cli args in millisecond which will be used for delaying before goinh to window. This will help user if window-switcher has shotkey option c ch cm then user can fast press ch & go to that app instead of going c"

### Engineering & Architectural Implementation
1. **Config & CLI Option (`-d, --delay <ms>`)**:
   - Added `delayMs` (range `0..5000`, default `300` ms) to `Config g_cfg`.
   - Added CLI parsing for `-d, --delay <ms>` (e.g. `window-switcher.exe --delay 300`).
2. **Key Interception & Alt Hold Timer Pause (`WM_SYSKEYDOWN` / `WM_SYSKEYUP`)**:
   - Intercepts system keypresses (`WM_SYSKEYDOWN`, `WM_SYSKEYUP`) in `OverlayWndProc` so `Alt`, `Tab`, and shortcut keys (e.g. `Alt+C`, `Alt+H`) are captured cleanly during the session without losing focus or triggering default Windows menu sounds.
   - On launch: starts a 300 ms countdown timer for the initial candidate window. If no keys are pressed within 300 ms, automatically switches to that window.
   - While `Alt` is held (`IsAltKeyDown()`), the timer is **paused**.
   - On keypress / navigation (`Tab`, letter keys, arrows, backspace), the selection updates and `TriggerAutoActivate()` resets the 300 ms commit timer back to zero.
   - Releasing `Alt` (`WM_SYSKEYUP` / `WM_KEYUP` for `VK_MENU`) starts/restarts the 300 ms delay timer for the highlighted card.
   - `Enter` / `Space` / `Mouse Click` activate immediately without delay; `Esc` cancels the timer and exits.

### Verification & Build
- Compiled release target cleanly via `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Verified Alt-hold key interception, system key routing, 300 ms timer reset, and release-unpause behavior.
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Session Low-Level Keyboard Hook & Alt+Tab Cycling Fix (`window-switcher.c`)

### Context & Problem Diagnosis
User reported: "after the app is launched & while holding [Alt] I press tab it exits."

### Root Cause Analysis
1. **IPC Toggle Double-Launch Exit**: When AutoHotkey (AHK) binds `!Tab` to execute `window-switcher.exe`, pressing `Tab` a second time while holding `Alt` spawns a second instance of `window-switcher.exe`. The second instance calls `TinyIPC_AcquireOrToggle()`, signalling the named IPC event. The first instance woke up from `MsgWaitForMultipleObjectsEx` and unconditionally executed `break;`, closing the switcher immediately instead of advancing the selection.
2. **Focus Loss (`WA_INACTIVE`)**: Pressing `Alt+Tab` could cause Windows to send `WM_ACTIVATE` (`WA_INACTIVE`) to the overlay window, which previously exited the application if active for >350ms.
3. **Alt Key State Synchronization**: Standard window message queues without a low-level hook can miss `Alt` key releases if focus shifts briefly or system hotkeys consume keydown/keyup events.

### Engineering & Architectural Fixes
1. **Session-Lifetime Low-Level Keyboard Hook (`WH_KEYBOARD_LL`)**:
   - Installed via `SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0)` upon launch.
   - Cleanly removed via `UnhookWindowsHookEx(g_hKeyboardHook)` in `WM_DESTROY` and exit cleanup (strictly zero persistent hooks left on the system).
   - Low-level hook intercepts `Tab`, `Shift+Tab`, `Alt` release (`VK_MENU`/`VK_LMENU`/`VK_RMENU`), prefix letter hints, arrows, `Backspace`, `Enter`, `Space`, and `Escape`.
   - Dispatches clean custom messages (`WM_SWITCHER_TAB`, `WM_SWITCHER_ALT_UP`, etc.) to the main GUI thread, consuming handled keys (`return 1`) to prevent native Windows `Alt+Tab` UI conflicts.
2. **IPC Toggle Event Alt-Held Awareness**:
   - When the IPC toggle event is signalled while `IsAltKeyDown()` is true (e.g. AHK spawned a second instance on `Alt+Tab`), the running instance treats the event as a `Tab` press to advance selection forward (`HandleTab`), keeping the switcher open.
3. **Alt-Hold Timer Pause & Release Resume**:
   - Holding `Alt` keeps the auto-activate delay timer paused (`KillTimer`).
   - Every keypress resets the delay timer.
   - Releasing `Alt` starts/restarts the 300 ms countdown (`SetTimer` with `g_cfg.delayMs`).
   - After 300 ms with no input, the selected window activates and the switcher exits cleanly.
   - `Enter`, `Space`, or mouse click activate immediately. `Esc` or clicking outside cancels and exits.

### Verification & Build
- Compiled release target cleanly via `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
- Verified full key behavior specification: Alt-hold pauses timer, Tab cycles forward/backward, prefix keys filter/cycle, Alt release resumes 300 ms countdown, Enter/Space/Click activates immediately, Esc cancels, IPC toggle on Alt+Tab advances selection, and low-level hook cleanly unhooks on exit.
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Executable Version Info Metadata Resolution for Chromium/Electron Apps (`window-switcher.c`)

### Context & Problem
User noticed: "i have a browser opened name helium but on hint shows c why?"

### Root Cause Analysis
Helium browser is installed at `...\Helium\Application\chrome.exe`. Because its executable filename on disk is `chrome.exe`, the previous implementation stripped `.exe` to get `Chrome` $\rightarrow$ hint `C`.

### Solution
1. Added `GetExeMetadataName()` in `src/window-switcher.c` using Win32 `GetFileVersionInfoW` and `VerQueryValueW` (linked against `version.lib`).
2. Reads `FileDescription` and `ProductName` metadata strings across language translation tables (with fallback to default US English).
3. Correctly identifies Helium as **`Helium`** (producing hint **`H`**) while falling back cleanly to executable basename if metadata is unavailable or generic.
4. Updated `build.ps1` to link `version` library for `window-switcher`.

### Verification & Build
- Compiled release binary cleanly: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe`.
- Tested metadata extraction: `Helium` receives hint `H`.
- Updated `docs/CHECKLIST.md`, `docs/JOURNEY.md`, and `build.ps1`.

---

## [2026-09-07] Implementation of `mouse-spotlight` Presentation Utility (`mouse-spotlight.c`)

### Context & Goal
User requested `mouse-spotlight`, a presentation and demo utility that dims the desktop except for a spotlight surrounding the mouse cursor.
Requirements:
- Follows mouse in real time.
- `--size` controls spotlight size (radius in pixels).
- `--dim` controls background dimming percentage or alpha.
- `Ctrl + / -` resizes spotlight dynamically.
- `Ctrl 0` resets spotlight size to startup value.
- `Esc` exits utility cleanly.
- Instant on/off, click-through overlay (WS_EX_TRANSPARENT).
- Follows UX conventions of `find-my-mouse`.

### Architecture & Design Decisions
1. **Silky Smooth Zero-Lag Per-Pixel Alpha DIB Renderer (`UpdateLayeredWindow`)**:
   - Replaced heavy Win32 `SetWindowRgn` region modifications (which forced Windows User32 to recalculate and invalidate non-client window frame metrics 60 times/sec, causing DWM micro-stutter) with `UpdateLayeredWindow` on a top-down 32-bpp DIB section.
   - On cursor movement, `mouse-spotlight` clears only the bounding rectangle of the previous circle position and renders the new spotlight circle with anti-aliased soft edge feathering (`FEATHER_PX = 6`).
   - Zero DWM window region recalculation overhead, delivering silky smooth 60+ FPS mouse tracking with zero lag.

2. **Full Key & Click Dismissal (`find-my-mouse` UX Convention)**:
   - Added `find-my-mouse` dismissal matching: after a 250ms activation grace period, pressing `Esc`, any non-hotkey key, or clicking the mouse exits the utility cleanly.
   - Holding `Ctrl` while pressing `+`, `-`, or `0` resizes and resets the spotlight without triggering dismissal.

3. **DPI Awareness & Physical Consistency (`tiny_dpi.h`)**:
   - Enables Per-Monitor v2 DPI awareness (`TinyDPI_EnablePerMonitorAwareness`).
   - Dynamically scales spotlight radius across monitors via `MulDiv(baseSize, TinyDPI_GetDpiForPoint(cur), 96)`, preserving constant physical diameter on 100% desktop monitors as well as 150%/200% high-DPI screens.

4. **Single-Instance IPC Toggle (`tiny_ipc.h`)**:
   - First launch creates `Global\TinyMouseSpotlightMutex` and `Global\TinyMouseSpotlightEvent`.
   - Second launch signals event and exits immediately. Running instance receives signal and exits cleanly.

### Build & Verification
- Updated `build.ps1` to include `mouse-spotlight` target configuration (`Subsystem = 'windows'`, `Libs = user32, gdi32`, `ExtraMinGW = '-municode'`, `Aliases = spotlight, mousespotlight`).
- Built both release (`dist/release/mouse-spotlight.exe`) and debug (`dist/debug/mouse-spotlight.exe`) targets with Clang (exit code 0).
- Updated `docs/mouse-spotlight.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Red Cross (✕) Close Button for Window Switcher Cards (`window-switcher.c`)

### Context & Goal
User requested: "after hint add red cross icon no bg. so user can close window with mouse click while alt tab is open"

### Engineering Implementation
1. **Visual Positioning & Adaptive Layout**:
   - **Unfocused / Default State**: The shortcut hint badge is anchored right at the card's right edge (`r.right - 10`), leaving zero empty space or gap.
   - **Hover / Focused State**: The red cross (`✕`) icon appears on the far right (`r.right - 24` to `r.right - 6`), and the shortcut hint badge shifts slightly left (`closeRect.left - 4`) to fit smoothly beside it.
   - Rendered red cross (`✕`) in vibrant red (`RGB(225, 65, 65)` / `RGB(255, 95, 95)` on hover) without background fill (`no bg`).
2. **Interactive Hover & Hand Cursor**:
   - Added `g_hoverCloseIndex` state tracking in `WM_MOUSEMOVE`.
   - Set cursor to `IDC_HAND` in `WM_SETCURSOR` when hovering over any card's close button.
3. **Click-to-Close Window Lifecycle & Shift+Click Force Termination (`WM_LBUTTONDOWN`)**:
   - **Normal Click with Modal Prompt Protection**: Sends `WM_CLOSE` using `SendMessageTimeoutW`. If the window refuses to close or opens an unsaved save prompt (`IsWindow(targetCloseHwnd)` remains true), the switcher does not drop the card or shift the layout; instead, it immediately activates that window and dismisses the switcher so the user can interact directly with the save dialog.
   - **Shift + Click**: Force terminates the process immediately via `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess()` (with fallback to `EndTask`), allowing users to bypass modal "Save changes?" prompts or unclosable window states.
   - When a window actually closes: unregisters the DWM thumbnail, removes the item from `g_items`, and dynamically reflows remaining cards and hints without exiting the switcher.
   - If all windows are closed, exits the switcher cleanly.
   - Preserves auto-activation timer pause state while `Alt` is held.

### Verification & Build
- Compiled release binary cleanly: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors, 0 warnings).
- Verified hover, hand cursor, `WM_CLOSE` dispatch, `Shift+Click` force termination, remaining card reflow, and Alt+Tab session persistence.
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] LAYOUT_FULL Proportional 16:10 Aspect Ratio & Vertical Centering (`window-switcher.c`)

### Context & Problem
User noticed: "on -l full the card size seems to become wiered"

### Root Cause Analysis
Previously in `LAYOUT_FULL`, `cardH` was computed as `availH / rows`, and `cols` collapsed to 1..3 for small window counts. On a standard 1080p display with 1-4 windows, single rows stretched cards to 1014px tall (creating massive, vertically distorted pillars with ~1:2 aspect ratios).

### Solution
1. Configured minimum column grid count (defaults to 4 columns for 1..8 windows, 5 for 9..15, 6 for 16+).
2. Computed `cardH = (cardW * 10) / 16` preserving natural 16:10 widescreen card proportions.
3. Added vertical bounds checking: if total grid height exceeds screen height, dynamically constrains by height and maintains aspect ratio.
4. Positioned cards starting directly from the top below the header (`startY = marginY + headerH`).

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 warnings, 0 errors).
---

## [2026-09-07] Multi-Monitor DPI-Aware Physical Card Sizing Parity (`window-switcher.c`)

### Context & Problem
User reported: "size of card should be physically same no matter the size of size. currently cards feel little small in laptop & little big in monitor."

### Root Cause Analysis
1. In `LAYOUT_CENTER`, cards used hardcoded 240px x 160px raw pixels. On high-DPI laptop displays with 125%, 150%, or 200% Windows scaling (96 DPI vs 144/192 DPI), card boundaries did not scale while text/fonts scaled, causing cards to look cramped and physically tiny (~1.2–1.5 inches wide).
2. In `LAYOUT_FULL`, `cols` was fixed to 4 (or 5-6), forcing `cardW = availW / 4`. On 24" or 27" desktop monitors (1080p or 1440p at 100% scaling), 4 columns produced 450px–616px wide cards, making them physically humongous (~5–7 inches wide).

### Engineering Decisions & Implementation
1. **Per-Monitor DPI Query**: Queried monitor DPI (`dpi = TinyDPI_GetDpiForMonitor(hMon)`) and calculated display scale factor `scale = dpi / 96.0f`.
2. **Dynamic DPI Font Scaling**: Added `UpdateFontsForDpi(dpi)` to re-create GDI fonts (`TinyFont_Create`) scaled precisely for the target monitor DPI.
3. **Physical Target Sizing**: Base target card width set to 270px at 96 DPI (16:10 aspect ratio), yielding ~3.0 inches physical screen width. Target card width scales to `targetCardW = (int)(270 * scale)`.
4. **Dynamic Column Selection in Full Mode**: Calculated `cols = (availW + gap) / (targetCardW + gap)` (bounded between 3 and 8). On a 150% DPI laptop (1920x1080), this selects 4 columns (card width = 444px raw = 296 logical px). On a 100% DPI 27" monitor (2560x1440), this selects 8 columns (card width = 300px raw = 300 logical px).
5. **Element Ratio Consistency**: Scaled gaps, container padding, close button rects, hint badges, and live thumbnail preview padding proportionally by `scale`.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors, 0 warnings).
- Verified mathematical physical parity:
  - 14" Laptop 1080p at 150% scaling: 296 logical px (~3.0 inches physical width).
  - 24" Desktop 1080p at 100% scaling: 298 logical px (~3.0 inches physical width).
  - 27" Desktop 1440p at 100% scaling: 300 logical px (~3.0 inches physical width).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Aspect-Ratio-Preserving DWM Window Thumbnails (`window-switcher.c`)

### Context & Problem
User reported: "some window feels stretched"

### Root Cause Analysis
In `PaintOverlay()`, `DwmUpdateThumbnailProperties()` set `rcDestination = item->previewRect` directly. Because `previewRect` has a fixed ~16:10 aspect ratio, any window with a non-16:10 aspect ratio (such as portrait terminals, vertical editor splits, Calculator, WhatsApp, or ultrawide windows) was non-uniformly stretched/squashed to fill the rectangle.

### Solution
1. Used `DwmQueryThumbnailSourceSize(item->hThumbnail, &srcSize)` (with `GetWindowRect` fallback) to retrieve the real source window dimensions and aspect ratio.
2. Calculated fitted rectangle dimensions (`fitW`, `fitH`) preserving the source aspect ratio within `previewRect`.
3. Centered the thumbnail (`offsetX`, `offsetY`) horizontally and vertically inside `previewRect`, allowing dark card backgrounds to serve as natural letterboxing.

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors, 0 warnings).
- Verified portrait, square, 16:9, and ultrawide windows maintain 100% natural, undistorted proportions.
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Unified Proportional Physical Scaling for Cards & Typography (`window-switcher.c`)

### Context & Goal
User feedback: "same for font also. & now size has become worse" — card and typography physical sizing must match seamlessly across different display densities without awkward text-to-card disproportion.

### Solution & Engineering Implementation
1. **Synchronous Physical Scaling**: Configured both layout metrics (`UI_CARD_BASE_WIDTH`, gaps, paddings, headers) and typography (`UI_FONT_SIZE_HINT`, `UI_FONT_SIZE_TITLE`) to scale synchronously via `scale = dpi / 96.0f`.
2. **Proportional Typography**: Hint badges and window title text scale with display DPI (`TinyFont_Create(..., dpi)`), keeping text-to-card ratios identical on 100% desktop monitors and 150%/200% laptop displays.
3. **Harmonized Spacing & Aspect Ratio**: Preserved 16:10 card aspect ratio, proportional header text gaps, and centered thumbnail letterboxing.

### Verification & Build
---

## [2026-09-07] Refined Card Padding & Title-to-Preview Vertical Gap (`window-switcher.c`)

### Context & Goal
User requested trimming the horizontal padding inside cards (left & right sides) and adding a clean vertical gap between the title header bar and the thumbnail preview image.

### Changes Made
1. **Trimmed Left/Right Card Padding**: Reduced card horizontal interior margins from 5px to 2px/3px (`previewRect.left = cx + 2`, `previewRect.right = cx + cardW - 2`, `previewRect.bottom = cy + cardH - 2`), maximizing preview thumbnail width inside each card.
2. **Title-to-Preview Separation Gap**: Offset `previewRect.top` to `cy + 25` below the header bar (`r.top + 2` to `r.top + 20`), providing a clear 5px distinct vertical separation between the app/window title text and the live preview thumbnail.
3. **Optimized Header Controls**: Adjusted close cross button and hint badge alignment (`r.top + 2` to `r.top + 19`, `r.right - 3`) to match trimmed side margins.

### Verification & Build
---

## [2026-09-07] Dynamic Per-Monitor DPI Font Scaling & Physical Typography Fix (`window-switcher.c`)

### Context & Problem
User reported: "text is too small for laptop". On high-DPI displays (such as 125%/150%/175% laptop screens), fonts appeared tiny and hard to read because font creation was unscaled (passing `0` / 96 DPI unscaled fallback) and static guards in `WM_CREATE` prevented re-creating fonts with actual display DPI.

### Root Cause Analysis
1. `WM_CREATE` initialized static font handles via `TinyGUI_CreateScaledFont()` with `dpi = 0`.
2. `UpdateFontsForDpi(dpi)` checked `if (g_hFontHint != NULL) return;`, immediately returning and ignoring the true monitor DPI retrieved via `TinyDPI_GetDpiForMonitor(hMon)`.
3. Consequently, on 144 DPI (150%) laptop screens, fonts rendered at unscaled 96-DPI pixel heights, making text physically miniature.

### Solution
1. **Dynamic DPI Font Re-creation**: Updated `UpdateFontsForDpi(dpi)` to check `g_currentDpi == dpi` instead of checking font nullness, allowing clean re-creation of `g_hFontHint`, `g_hFontTitle`, `g_hFontGroup` whenever DPI changes.
2. **Monitor DPI Pass-Through**: Passed real monitor `dpi` into `TinyFont_Create(..., dpi)` so that GDI fonts calculate `-MulDiv(fontSize, dpi, 96)` for exact physical millimeter parity across displays.
3. **Responsive Metrics Scaling**: Scaled all card paddings, close button rectangles, header bar heights, and hint badge widths proportionally with `scale = (float)dpi / 96.0f`, ensuring the relative font-to-card visual ratio remains consistent across 100% desktop monitors and 150% laptop screens.
4. **`WM_DPICHANGED` Handler**: Added `WM_DPICHANGED` handling in `OverlayWndProc` to smoothly recompute layout and fonts when the switcher moves between monitors of differing DPIs.

### Verification & Build
---

## [2026-09-07] Fine-Tuned Base Card Width (`UI_CARD_BASE_WIDTH = 255`)

### Context & Goal
User requested making cards slightly smaller by 5px.

### Changes Made
- Updated `UI_CARD_BASE_WIDTH` from `260` to `255` in `src/window-switcher.c`.
- Height automatically adjusted via 16:10 aspect ratio (`cardH = 255 * 10 / 16 = 159px` at 96 DPI).

### Verification & Build
- Compiled release binary: `pwsh -File .\build.ps1 window-switcher` -> `dist/release/window-switcher.exe` (0 errors, 0 warnings).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-07] Native Win32 EDIT Control & Insert-Only Focus for IP Send (`ip-send.c`)

### Context & Goal
User reported: "I want only insert to enable textbox not tab. also want textbox should behave like normal text box."
Previously, `ip-send` rendered the message area as a custom GDI string buffer without a real Win32 caret, mouse selection, or standard editing operations, and `<Tab>` was bound to toggle message mode.

### Engineering Implementation
1. **Integrated Native Win32 Multiline `EDIT` Control**:
   - Created a child `EDIT` control (`WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_LEFT | WS_TABSTOP`).
   - Handles standard Windows text operations out of the box: caret positioning, mouse clicking and dragging for text selection, <kbd>Ctrl</kbd>+<kbd>A</kbd>, <kbd>Ctrl</kbd>+<kbd>C</kbd>, <kbd>Ctrl</kbd>+<kbd>V</kbd>, <kbd>Ctrl</kbd>+<kbd>X</kbd>, <kbd>Ctrl</kbd>+<kbd>Z</kbd>, <kbd>Backspace</kbd>, <kbd>Delete</kbd>, <kbd>Home</kbd>, <kbd>End</kbd>, arrow key navigation, and right-click context menus.
2. **Subclassed Edit Window (`EditSubclassProc`)**:
   - <kbd>Insert</kbd> or <kbd>Esc</kbd>: Syncs message buffer, hides edit control, returns focus to recipient search view.
   - <kbd>Ctrl</kbd>+<kbd>Enter</kbd>: Triggers `ExecuteSend`.
   - <kbd>Ctrl</kbd>+<kbd>D</kbd>: Toggles light/dark theme.
   - <kbd>Ctrl</kbd>+<kbd>O</kbd>: Opens file attachment dialog.
3. **Dedicated `<Insert>` Key & Click-to-Focus**:
   - Removed `<Tab>` toggle into message mode so only <kbd>Insert</kbd> (or clicking directly on the message area) enters message edit mode.
4. **Seamless Theme Styling**:
   - Handled `WM_CTLCOLOREDIT` and `WM_CTLCOLORSTATIC` in parent `WndProc` to dynamically paint matching dark/light background brushes and high-contrast foreground text colors.
5. **DPI Awareness**:
   - `RecreateIPSendFonts` updates the `EDIT` control's font handle (`WM_SETFONT`) dynamically across monitor DPI changes.

### Verification & Build
- Compiled binary: `pwsh -File .\build.ps1 ip-send` -> `dist/release/ip-send.exe` (0 errors, 0 warnings).
- Verified IPC toggle and documentation updates.

---

## [2026-09-07] IP Send `Ctrl+Backspace` Word Deletion & Dynamic Height Expansion (`ip-send.c`)

### Context & User Feedback
User reported two issues:
1. `Ctrl+Backspace` produced weird control characters (`\x7f` DEL / box characters).
2. The message box did not dynamically resize as typed text grew over multiple lines.

### Solution & Engineering Details
1. **`Ctrl+Backspace` Backward Word Deletion**:
   - In `EditSubclassProc`, intercepted `WM_KEYDOWN` for `VK_BACK` when `VK_CONTROL` is held down.
   - Identified word boundaries by traversing backwards across trailing whitespace and preceding word characters.
   - Replaced the word range using `EM_SETSEL` and `EM_REPLACESEL`.
   - Intercepted and suppressed the `0x7F` character in `WM_CHAR` to prevent unprintable character glyphs.
2. **Dynamic Height Resizing (`editH`)**:
   - Calculated required multiline height in `RenderTUIWindow` via `DrawTextW` with `DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL`.
   - Dynamically resized `g_hEdit` and its visual focus border from 28px up to 160px as lines are typed or wrapped.
   - Handled `EN_CHANGE` in parent `WM_COMMAND` to trigger `InvalidateRect` immediately upon typing, smoothly updating the composer layout in real-time.

### Verification & Build
- Built release binary: `pwsh -File .\build.ps1 ip-send` -> `dist/release/ip-send.exe` (0 errors, 0 warnings).
- Updated `docs/CHECKLIST.md` and `docs/JOURNEY.md`.

---

## [2026-09-08] Window Switcher Keep-Open on Close Button Click (`window-switcher.c`)

### Context & Goal
User reported: "closing window in window-switcher also closes window-switcher , why? ... but i want window-switcher to stay open".
Previously, clicking the red cross (`✕`) on a card would immediately close the `window-switcher` overlay.

### Root Cause Analysis
1. **Premature `IsWindow()` Check**: In `WM_LBUTTONDOWN`, the close handler called `SendMessageTimeoutW(targetCloseHwnd, WM_CLOSE, ...)` and immediately tested `if (IsWindow(targetCloseHwnd))`. Modern Windows GUI apps (browsers, Electron apps, editors, Explorer) tear down asynchronously, so `IsWindow()` was almost always still true after 120 ms. The switcher assumed an unsaved modal save prompt was active, focused that window, and called `PostQuitMessage(0)`.
2. **Auto-Activation Timer on Card Removal**: When an item was removed, `TriggerAutoActivate()` was called on the newly selected card, which started the delay countdown timer (`-d` / 300 ms) and closed the switcher upon timeout.
3. **Focus Loss (`WM_ACTIVATE` / `WA_INACTIVE`)**: When a background window closed, Windows OS redistributed foreground focus, triggering `WA_INACTIVE` on the switcher overlay which caused an early exit.

### Engineering Implementation
1. **Asynchronous Graceful Dispatch**: Dispatched `WM_CLOSE` via `SendMessageTimeoutW` (or `TerminateProcess` / `EndTask` on `Shift + Click`), removed the closed card item from `g_items`, cleaned up the DWM thumbnail, recomputed hints and layout, and kept the switcher open.
2. **Auto-Activation Suppression**: Cancelled pending auto-activate timers (`CancelPendingAutoActivate`) on close button clicks so closing windows acts as a pure management action without scheduling unwanted window switches.
3. **Deactivation Shielding (`g_lastCloseTime`)**: Tracked `g_lastCloseTime = GetTickCount64()` on close events and shielded `OverlayWndProc` against `WA_INACTIVE` focus transitions within a 600 ms window by reclaiming foreground focus (`SetForegroundWindow(hwnd)`).

### Verification & Build
- Built release binary via `pwsh -File .\build.ps1 -Mode release window-switcher` -> `dist/release/window-switcher.exe` (0 errors, 0 warnings).
- Updated `docs/window-switcher.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

---

## [2026-09-11] IP Send Recipient Selection & Focus Preservation Across Refreshes (`ip-send.c`)

### Context & Motivation
User reported: "if i have selected users in ip-send & refresh the list those checks gets unselected".
In `ip-send`, users can select one or multiple online contacts to compose and dispatch messages or files. However, pressing `r` to refresh the live user list via `ipcmd.exe list /all` wiped all checked selections (`selected = false`) and reset the cursor position because incoming lines were parsed directly into `g_state.recipients` starting from index 0 without retaining the previous selection state.

### Root Cause Analysis
1. **Unconditional Selection Reset**: `QueryLiveIPMsgRecipients` parsed output lines directly into `g_state.recipients[count]`, hardcoding `g_state.recipients[count].selected = false` for every line.
2. **Loss of Custom CLI Contacts**: Any selected recipients added via CLI `--to <name>` or unlisted contacts were completely overwritten if not present in the new `ipcmd` output.
3. **Cursor Jump**: After refresh, `FilterRecipients()` adjusted `highlightedFilteredIdx` solely based on raw numeric boundaries, causing the user's cursor (`>`) to jump to a different user if list positions shifted.
4. **Offline Deselection Lock**: In `WM_LBUTTONDOWN` and `VK_SPACE`, clicking or pressing Space checked `if (r->active)` before toggling selection. If an already-selected user was reported offline after a refresh, the user could not deselect them and received an offline warning toast instead.

### Engineering Implementation
1. **Recipient Equality Matching (`AreRecipientsEqual`)**:
   - Implemented multi-tiered matching comparing non-empty UIDs (unique in IPMsg protocol), display names with hostnames, IP addresses, and custom CLI tokens.
2. **Buffered Query & Selection Reconciliation**:
   - Parsed query lines into a temporary array `s_tempRecipients` before mutating application state.
   - For each newly parsed recipient, matched against `g_state.recipients` and transferred previous `selected` flags.
   - Preserved any previously selected recipients that were not returned in the new query (such as custom CLI contacts or temporarily offline users).
3. **Highlighted Cursor & Viewport Stabilization**:
   - Captured the active highlighted recipient before refresh, and restored `g_state.highlightedFilteredIdx` and `scrollOffset` to the same recipient in the refreshed list.
4. **Offline Deselection Guard**:
   - In both `WM_LBUTTONDOWN` and `VK_SPACE`, allowed immediate deselection if `r->selected` is true, while preserving the offline warning guard for new selections.
5. **IPC `--to` Support**:
   - Implemented recipient parsing for `--to` arguments delivered via `WM_COPYDATA`.

### Verification & Build
- Built release binary: `pwsh -File .\build.ps1 ip-send` -> `dist/release/ip-send.exe` (0 errors, 0 warnings).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug ip-send` -> `dist/debug/ip-send.exe` (0 errors, 0 warnings).
- Smoke tested double-launch IPC toggle contract (`Exited: True`).
- Verified documentation updates in `docs/ip-send.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.

## 2026-09-11: Pin to Top Premature Exit Bug, IPC Modernization & Window Filtering

### Problems Identified
1. **Premature Process Exit on Window Selection in Interactive Picker**:
   - In `PickerWndProc`, `case WM_DESTROY:` called `PostQuitMessage(0);`.
   - When the user clicked a window to pin it, `DestroyWindow(hwnd)` was called, synchronously triggering `WM_DESTROY`, which posted `WM_QUIT` to the thread's message queue.
   - When `ShowInteractiveWindowPicker()` finished and `WinMain` entered its message loop, `PeekMessageW` immediately retrieved `WM_QUIT`, broke the tracking loop, called `RemovePinnedAt(0, true)` (unpinning the window that was just selected!), and terminated the process in ~16 ms.
   - To the user, clicking a window closed the picker but nothing was pinned.
2. **Common Headers Rule Violations (`tiny_ipc.h`, `tiny_cli.h`)**:
   - `src/pin-to-top.c` was manually calling `CreateMutexW` without `tiny_ipc.h` and attempting to call `ReleaseMutex(mutex)` without owning the mutex (failing with `ERROR_NOT_OWNER`).
   - `BuildConfig()` used a custom manual loop rather than `TinyCLI_ParseCommandLine()`.
3. **Cloaked & Suspended Background Windows**:
   - `EnumWindowsPickerProc` did not query `DWMWA_CLOAKED` (attribute 14) or filter `WS_EX_TOOLWINDOW`, causing invisible/suspended UWP apps or windows on other virtual desktops to be hovered and selected.
4. **Window Replacement Lockout when `maxWindows` Reached**:
   - When 1 window was pinned (`maxWindows = 1`), clicking a new window in the picker was silently rejected because `g_pinnedCount < g_cfg.maxWindows` evaluated to false.
5. **Zombie Process on Window Closure**:
   - If the user closed all pinned windows, `pin-to-top` stayed resident polling at 60 FPS instead of cleanly exiting.

### Architectural Decisions & Changes
1. **Picker Overlay Destruction Decoupled from Process Exit**:
   - Removed `PostQuitMessage(0)` from `PickerWndProc`'s `WM_DESTROY`. The picker loop `while (g_pickerActive && GetMessageW(...))` terminates cleanly on `g_pickerActive = false`, leaving the process tracking loop intact.
2. **Standardized Single-Instance IPC**:
   - Integrated `TinyIPC_AcquireOrToggle(APPMUTEX_NAME, APPEVENT_NAME, &hEvent)` from `src/common/tiny_ipc.h`. Second launch signals `g_event` and exits with code 0; primary instance handles the toggle cleanly.
3. **Declarative CLI Option Parsing**:
   - Integrated `TinyCLI_ParseCommandLine` (`src/common/tiny_cli.h`) with a `CliOption` table for `--border-width`, `--max-windows`, `--window-selection`, and flags (`--no-picker`, `--active`, `--foreground`), retaining backward-compatible parsing for `--key=val` formats.
4. **Enhanced Target Window Validation & Cloaking Detection**:
   - Added `IsWindowCloaked(HWND)` using dynamic `dwmapi.dll` `DwmGetWindowAttribute` query with `DWMWA_CLOAKED`.
   - Filtered out tool windows (`WS_EX_TOOLWINDOW` unless `WS_EX_APPWINDOW`), desktop/shell windows, and windows with <= 10 px dimensions.
5. **FIFO Pin Limit Replacement**:
   - When a user pins a new window while `g_pinnedCount >= g_cfg.maxWindows`, the oldest pinned window is unpinned (`RemovePinnedAt(0, true)`), allowing seamless switching.
6. **Z-Order Preservation & Resize Repainting**:
   - Registered overlay window class with `CS_HREDRAW | CS_VREDRAW` and added explicit invalidation on dimension changes to eliminate dirty border trails.
   - When the pinned target is active foreground, the overlay border is maintained at topmost Z-order above it.
7. **Clean Auto-Exit on Window Destruction**:
   - When all pinned windows are closed by the user, `g_running` is set to `false`, exiting the process cleanly with code 0.

### Verification & Build
- Built release binary: `pwsh -File .\build.ps1 pin-to-top` -> `dist/release/pin-to-top.exe` (0 errors, 0 warnings).
- Built debug binary: `pwsh -File .\build.ps1 -Mode debug pin-to-top` -> `dist/debug/pin-to-top.exe` (0 errors, 0 warnings).
- Smoke tested single-instance double-launch toggle IPC pattern (`p1 running: True`, `p2 exited with: 0`).
- Verified documentation updates in `docs/pin-to-top.md`, `docs/CHECKLIST.md`, and `docs/JOURNEY.md`.
