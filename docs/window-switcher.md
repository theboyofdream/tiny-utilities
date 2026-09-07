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
- **Card Header Bar** (`r.top + 5` to `r.top + 29`):
  - **Left Side**: `App Name — Window Title` rendered with `common/font.h` UI font (`TinyFont_GetBestUIFace()`) and smooth ellipsis truncation (`DT_END_ELLIPSIS`).
  - **Right Side**: Borderless shortcut hint badge (`[ F ]`, `[ N+ ]`, `[ C1 ]`) in standard-size bold font (`g_hFontHint` 15pt bold) with crisp white text (`RGB(255, 255, 255)`) and a distinct accent background color:
    - **Selected**: `RGB(0, 120, 240)` (vibrant cyan-blue fill).
    - **Hovered**: `RGB(35, 85, 155)` (distinct indigo fill).
    - **Normal**: `RGB(48, 64, 90)` (distinct rich deep slate blue fill).
- **Card Body / Live Preview Area** (`previewRect`: `r.top + 34` to `r.bottom - 8`):
  - DWM Hardware-Accelerated Live Window Preview Thumbnail (`DwmRegisterThumbnail` + `DwmUpdateThumbnailProperties`).
  - Because the hint badge and title text sit strictly *above* `previewRect`, DWM live thumbnail compositing can **never** cover or obscure hint badges or window titles.

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
| `Mouse Hover` | Highlights card container under cursor. |
| `Left Click` | Activates target window under cursor immediately. Click outside cancels and closes switcher. |

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
