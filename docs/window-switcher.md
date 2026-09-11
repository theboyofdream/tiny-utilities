# Tiny Window Switcher Specification (`src/window-switcher.c` -> `window-switcher.exe`)

High-performance, minimal, keyboard-driven native Win32/DWM window switcher overlay utility.

## 1. Overview & Core Contract

- **Source File**: [`src/window-switcher.c`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/window-switcher.c)
- **Binary Target**: `dist/release/window-switcher.exe` / `dist/debug/window-switcher.exe`
- **Subsystem**: `windows` (`wWinMain` entry point).
- **Hard Rules**: Zero background services, zero registry keys, zero disk persistence, zero config files, zero global hooks, pure Win32 C & GDI only.
- **DPI Awareness**: Per-Monitor v2 aware via `TinyDPI_EnablePerMonitorAwareness()` (`common/tiny_dpi.h`).
- **Single Instance & IPC Toggle**:
  - Mutex: `Global\TinyWindowSwitcherMutex`
  - Event: `Global\TinyWindowSwitcherEvent`
  - Uses `TinyIPC_AcquireOrToggle()` (`common/tiny_ipc.h`). Re-invoking `window-switcher.exe` signals the event and toggles off the existing switcher overlay cleanly.

---

## 2. CLI Syntax & Options

Declarative CLI argument parsing powered by `common/tiny_cli.h`.

```cmd
window-switcher.exe [options]
```

| Option | Alias | Type / Range | Default | Description |
| :--- | :--- | :--- | :--- | :--- |
| `--layout <mode>` | `-l` | `center`, `full` | `center` | Card layout mode: `center` (centered grid container panel aligned with card columns) or `full` (card grid expands 100% to fill display screen). |
| `--sort <mode>` | `-s` | `recent`, `name` | `name` | Sorting order: `name` (alphabetical by app name, default) or `recent` (Most Recently Used Z-order). |
| `--background <mode>` | `-bg` | `blur`, `tint` | `blur` | Full-screen backdrop effect (`blur` or `tint`). |
| `--blur <amount>` | — | `1..100` | `20` | Backdrop blur intensity (1..30 maps to Aero light glass blur; 31..100 maps to heavy Acrylic frosted blur). |
| `--tint-color <hex>` | — | `#RRGGBBAA` / `#RRGGBB` | `#000000` | Backdrop tint hex color with optional alpha opacity (e.g. `#00000005`). |
| `--delay <ms>` | `-d` | `0..5000` | `300` | Auto-activation delay in milliseconds before switching to window (e.g. default `300` ms allowing fast multi-character hint typing like `C` $\rightarrow$ `CH`). |
| `--help` | `-h` | Flag | `false` | Display command-line usage and help. |
| `--version` | `-v` | Flag | `false` | Display version information. |

---

## 3. UWP Store App Resolution & Hint Generation

### UWP Host Process Resolution (`ApplicationFrameHost.exe`)
- UWP / Windows Store apps (such as `Calculator`, `Clock`, `Settings`, `Photos`) run under the host process `ApplicationFrameHost.exe`.
- Implements `GetUWPRealProcessId()` via child `Windows.UI.Core.CoreWindow` window enumeration and window title inspection.
- Resolves true UWP application titles and process IDs, assigning unique shortcut hints (e.g. `Calculator` $\rightarrow$ `C`, `Clock` $\rightarrow$ `CL`, `Settings` $\rightarrow$ `S`) instead of generic host hints (`A1`, `A2`).

### Shortest Unique Prefix Hint Assignment
- Enumerates visible top-level application windows (excluding cloaked DWM windows, tool windows, hidden windows, and the switcher overlay itself).
- Computes the shortest unique prefix hint per application across all open applications (e.g. `Chrome` $\rightarrow$ `C`, `Code` $\rightarrow$ `CO`, `Chromium` $\rightarrow$ `CH`, `Zen` $\rightarrow$ `ZN`, `Zed` $\rightarrow$ `ZD`).
- **Base / Extended App Pair Handling**: When a shorter app name is a prefix of a longer app name (e.g. `Notepad` vs `Notepad+`), `Notepad` receives `N` while `Notepad+` receives `N+`.
- Multi-window applications receive distinct window sub-indices (e.g. `C1`, `C2`).

### Typed Buffer Matching Hierarchy (`MatchTypedBuffer`)
1. **Tier 1 (Exact Hint Match)**: Matches typed buffer against exact app hint strings (e.g. `F` for `File Pilot`). Auto-activates only when `totalMatches == 1` and no shared prefix ambiguity exists.
2. **Tier 2 (App Name / Hint Prefix Match)**: Matches typed buffer as a prefix of app hints or app names.
3. **Tier 3 (Window Title Substring Prefix Match)**: Matches typed buffer as a prefix of window title strings.

---

## 4. Visual Layout & Card Architecture

### Card Container Layout
- **Full Screen Coverage**: Full-screen backdrop fill (Blur / Tint) covers the entire active display monitor (`MonitorFromPoint` / `GetMonitorInfoW`). Cards float directly on the backdrop without nested outer container box borders.
- **Container Header Bar**: Aligned precisely to card grid margins (`g_containerRect.left`). Displays search filter status or active switcher config parameters.
- **Card Header Bar** (`r.top + 2` to `r.top + 20`):
  - **Left Side**: `App Name — Window Title` rendered with `common/font.h` UI font (`TinyFont_GetBestUIFace()`) and smooth ellipsis truncation (`DT_END_ELLIPSIS`).
  - **Right Side**: Borderless shortcut hint badge (`[ F ]`, `[ N+ ]`, `[ C1 ]`) and hover close cross (`✕`) with crisp white text (`RGB(255, 255, 255)`):
    - **Selected**: `RGB(0, 120, 240)` (vibrant cyan-blue fill).
    - **Hovered**: `RGB(35, 85, 155)` (distinct indigo fill).
    - **Normal**: `RGB(44, 58, 80)` (distinct rich slate blue fill).
- **Card Body / Live Preview Area** (`previewRect`: `r.left + 2`, `r.top + 25` to `r.right - 2`, `r.bottom - 2`):
  - DWM Hardware-Accelerated Live Window Preview Thumbnail (`DwmRegisterThumbnail` + `DwmUpdateThumbnailProperties`).
  - **Aspect-Ratio-Preserving Fit**: Queries real source window dimensions via `DwmQueryThumbnailSourceSize()` (with `GetWindowRect` fallback). Dynamically fits and centers the thumbnail inside `previewRect` without non-uniform stretching, preserving natural window proportions (portrait terminals, 16:9, ultrawide 21:9) with dark letterboxing.
  - Slim 2-3px interior margins and 5px vertical separation below the header bar ensure maximum thumbnail visibility without wasted space.

### Unified Proportional Physical Scaling Architecture
- **Per-Monitor Physical Sizing**: Queries display DPI via `TinyDPI_GetDpiForMonitor(hMon)` to compute the display scale factor `scale = dpi / 96.0f`.
- **Proportional Font & Card Parity**: Both typography (`UI_FONT_SIZE_HINT`, `UI_FONT_SIZE_TITLE`) and layout dimensions (`UI_CARD_BASE_WIDTH`, gaps, paddings, header heights, badges) scale synchronously with display DPI. This guarantees text and card boundaries maintain the exact same real-world physical size and visual proportions between high-DPI laptop displays and desktop monitors.
- **Top-Level Constants**: Base 96-DPI constants (`UI_CARD_BASE_WIDTH`, `UI_CARD_ASPECT_RATIO_W`/`H`, `UI_CARD_GAP`, `UI_CONTAINER_PADDING`, `UI_HEADER_HEIGHT`, `UI_FONT_SIZE_HINT`, `UI_FONT_SIZE_TITLE`) are declared directly at the top of `src/window-switcher.c` for quick tuning.
- **Dynamic Columns in Full Layout (`--layout full`)**: Column count is computed dynamically based on target physical card width (`availW / (targetCardW + gap)`), capping large desktop monitors (2560x1440, 4K) to 6–8 columns. Prevents cards from stretching into massive 600px+ pillars on large monitors while keeping cards at their exact configured physical dimensions.

### Robust Single-Window Architecture
- Single top-level topmost window (`g_hwndOverlay`) with GDI double-buffered rendering (`BitBlt`).
- Eliminates multi-window Z-order synchronization issues, layered window color-key clipping, and activation flicker.

---

## 5. Controls & Keybindings

| Key / Input | Action |
| :--- | :--- |
| `Alt+Tab` (held) | AHK launches `window-switcher.exe`. While `Alt` is held, timer is paused. `Tab` cycles forward (`Shift+Tab` cycles backward). |
| `Letter / Digit` (`A-Z`, `0-9`, `+`) | Filters open windows or cycles matching single-letter hint shortcuts; resets delay timer. |
| `Tab` / `Shift + Tab` | Cycles selection forward / backward through the window grid; resets delay timer. |
| `Up` / `Down` | Navigates vertical grid rows (`-cols` / `+cols` with multi-column wrapping); resets delay timer. |
| `Left` / `Right` | Navigates horizontal grid columns (`-1` / `+1`); resets delay timer. |
| `Alt` Release | Starts/restarts the 300 ms commit countdown timer (`-d, --delay <ms>`). |
| `300 ms` no input (after Alt release) | Activates selected window and exits. |
| `Enter` / `Space` | Activates selected window immediately and closes switcher. |
| `Backspace` | Deletes last character from search filter buffer; resets delay timer. |
| `Escape` | Cancels window switching and closes switcher immediately without activating. |
| `Mouse Hover` | Highlights card container or red cross close icon under cursor (changes to `IDC_HAND`). |
| `Left Click (Card)` | Activates target window under cursor immediately. Click outside cancels and closes switcher. |
| `Left Click (Red Cross ✕)` | Closes target window gracefully (`WM_CLOSE`), removing its card while keeping `window-switcher` open. If no windows remain, the switcher closes. |
| `Shift + Left Click (✕)` | Force terminates target application process immediately (`TerminateProcess` / `EndTask`) while keeping `window-switcher` open. |

### Activation & Deactivation Stability
- **Session-Lifetime Low-Level Keyboard Hook**: Installed on launch and unhooked on exit. Intercepts Alt, Tab, arrows, hints, and navigation keys globally during the session to prevent native Windows Alt+Tab dialog collisions.
- **IPC Toggle Alt-Held Awareness**: If AHK launches a second instance when `Tab` is pressed while holding `Alt`, the IPC toggle event advances window selection instead of closing the switcher.
- **Focus Loss Protection**: `WA_INACTIVE` is ignored while `Alt` is held down, preventing unwanted exits during system focus transitions.

---

## 6. Build & Verification

Build target corresponding to `src/window-switcher.c`:

```powershell
pwsh -File .\build.ps1 [-Mode release|debug] window-switcher
```

- **Target Aliases**: `window-switcher`, `switcher`, `win-switch`, `windowswitcher`.
- **Release Output**: `dist/release/window-switcher.exe`
- **Debug Output**: `dist/debug/window-switcher.exe`
