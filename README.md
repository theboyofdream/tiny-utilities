# ⚡ Tiny Windows Productivity Utilities

A ultra-lightweight, high-performance suite of agent-proof, native **Win32 C & GDI** utilities designed for Windows power users and keyboard enthusiasts.

> [!NOTE]
> All utilities are built strictly using **Clang** (`clang-cl` or MinGW `clang`) with zero external runtimes, zero background daemons, zero registry clutter (except optional Explorer context menus), and zero disk persistence. Every tool runs as an instant single-instance process that exits cleanly when toggled off.

---

## 🚀 Key Features & Principles

- **Zero Background Overhead**: No background services or persistent tray daemons. Tools launch on demand (e.g. via AutoHotkey) and exit immediately upon completion or toggle-off.
- **Single-Instance IPC Toggle**: Every interactive overlay tool uses named Win32 Mutex & Event primitives (`Global\Tiny<Tool>Mutex` / `Event`). Re-launching a running tool instantly toggles it off and exits cleanly.
- **Native Per-Monitor DPI Awareness**: Automatically scales overlays, fonts, spotlights, and hitboxes dynamically across multi-monitor setups with mixed DPI scaling factors (100%, 150%, 200%).
- **Pure Win32 & GDI/DWM**: Hardware-accelerated desktop window manager compositing without DirectX, Electron, or heavy web engines. Low memory footprint (< 10 MB per tool) and < 0.2% idle CPU.
- **Declarative Shared Micro-Headers**: Built on modular C headers in `src/common/` ([`tiny_cli.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/tiny_cli.h), [`tiny_ipc.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/tiny_ipc.h), [`tiny_dpi.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/tiny_dpi.h), [`tiny_gui.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/tiny_gui.h), [`font.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/font.h), [`clipboard.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/clipboard.h), [`color-thief-algorithm.h`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/src/common/color-thief-algorithm.h)).

---

## 🛠️ Included Utilities

| Utility | Target Executable | Subsystem | Description & Key Functionality |
| :--- | :--- | :--- | :--- |
| **Pin to Top** | [`pin-to-top.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/pin-to-top.md) | GUI | Toggles active window topmost status with wallpaper accent-colored border overlay & interactive window picker. |
| **Find My Mouse** | [`find-my-mouse.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/find-my-mouse.md) | GUI | High-visibility cursor locator with smooth pulse-in / pulse-out animation disc centered on mouse pointer. |
| **Mouse Spotlight** | [`mouse-spotlight.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/mouse-spotlight.md) | GUI | Dims desktop for presentations, leaving a smooth anti-aliased spotlight hole on mouse pointer with dynamic sizing. |
| **Color Picker** | [`color-picker.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/color-picker.md) | GUI | Floating color magnifier & tooltip displaying real-time `HEX`, `RGB`, `HSL`, and `CMYK` values with instant 1-key clipboard copy. |
| **Screen Capture** | [`capture.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/capture.md) | GUI | Screenshot & doodle annotation suite with interactive mode picker (Snip, Window, Fullscreen), dynamic drawing canvas, and save dialog. |
| **Pixel View** | [`pixel-view.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/pixel-view.md) | GUI | Frameless image & animated GIF viewer featuring nearest-neighbor chunky pixel block rendering for retro graphics. |
| **IP Send** | [`ip-send.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/ip-send.md) | GUI | ASCII TUI IP Messenger (`ipmsg`) payload composer with search, message wrapping, file drag-and-drop, and Explorer context menu integration. |
| **Context Menu Manager**| [`context-menu.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/context-menu.md) | GUI / CLI | Cascading Explorer context menu registry utility with recursive submenus, XML backup/update GUI, and CLI support. |
| **OCR Utility** | [`ocr.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/ocr.md) | GUI / CLI | Portable zero-install OCR tool leveraging Tesseract engine with automatic language model downloading and clipboard/stdout output. |
| **Window Switcher** | [`window-switcher.exe`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/window-switcher.md) | GUI | Keyboard-driven DWM live thumbnail window switcher with unique prefix/subsequence hint badges, UWP app resolution, search filter, and instant close buttons. |
| **Command Manager (`cmdx`)** | [`cmdx.ps1`](file:///C:/Users/abhishek.c/Desktop/playground/tiny-windows-utilites/docs/cmdx.md) | CLI (PowerShell) | PowerShell command registry and shim generator creating dual `.cmd`/`.ps1` wrappers in dedicated `cmds/` folder registered in User PATH. |

---

## 📖 Detailed Utility Breakdown & CLI Options

### 📌 Pin to Top (`pin-to-top.exe`)
Toggles the topmost state of windows and displays a clean border overlay.
- **CLI Options**:
  ```cmd
  pin-to-top.exe [--border-width 1-20] [--max-windows 1-10] [--window-selection true|false]
  ```
- **Features**: Interactive window selection picker mode on double click, dominant wallpaper accent color detection via MMCQ quantization, dynamic window tracking on move/resize, and auto-cleanup on window destruction.

---

### 🔍 Find My Mouse (`find-my-mouse.exe`)
Quickly locate your cursor across high-resolution or multi-monitor setups.
- **CLI Options**:
  ```cmd
  find-my-mouse.exe
  ```
- **Features**: Smooth 400ms quadratic ease-in pulse animation, full-screen background dimming, zero-hook keyboard/click auto-dismissal, and DPI-aware disc scaling.

---

### 💡 Mouse Spotlight (`mouse-spotlight.exe`)
Presentation overlay tool that dims the screen except for a customizable spotlight around the mouse.
- **CLI Options**:
  ```cmd
  mouse-spotlight.exe [--size 20-1000] [--dim 0-100|0-255]
  ```
- **Controls**:
  - `Ctrl +` / `Numpad +`: Enlarge spotlight radius (+3px/frame).
  - `Ctrl -` / `Numpad -`: Shrink spotlight radius (-3px/frame).
  - `Ctrl 0` / `Numpad 0`: Reset to default startup size.
  - `Esc` or click: Dismiss overlay and exit.

---

### 🎨 Color Picker (`color-picker.exe`)
Floating screen color magnifier and multi-format tooltip card.
- **CLI Options**:
  ```cmd
  color-picker.exe
  ```
- **Controls**:
  - `1`: Copy `HEX` format (e.g. `#023B32`) to clipboard.
  - `2`: Copy `RGB` format (e.g. `rgb(2, 59, 50)`) to clipboard.
  - `3`: Copy `HSL` format (e.g. `hsl(169, 94%, 10%)`) to clipboard.
  - `4`: Copy `CMYK` format (e.g. `cmyk(96%, 0%, 4%, 80%)`) to clipboard.
  - `Left-Click`: Copy configured default format and exit.
  - `Esc` / `Right-Click`: Cancel without copying.

---

### 📸 Screen Capture (`capture.exe`)
Full-featured screen capture, region selection, and doodle annotation overlay.
- **CLI Options**:
  ```cmd
  capture.exe [--snip | --window | --fullscreen] [--draw] [--toolbar] [--save <path>] [--format png|bmp] [--clipboard]
  ```
- **Features**: Interactive mode selector menu (`1` Snip, `2` Window, `3` Full Screen), zero-flicker state morphing, floating annotation toolbar with color/brush pickers, keyboard shortcuts (`R`, `Y`, `B`, `G`, `K`, `W`, `1`-`4`, `Z` Undo, `X` Clear, `S` Save, `Space` Toggle Toolbar), and native Save File Dialog.

---

### 👾 Pixel View (`pixel-view.exe`)
Native frameless viewer for image files and animated GIFs designed for pixel art preservation.
- **CLI Options**:
  ```cmd
  pixel-view.exe --file <image_path> [--pixel-size 1-8] [--bg pixelated|linear|solid|transparent]
  ```
- **Features**: 100% nearest-neighbor point sampling (zero blurry filtering), WIC animated GIF playback engine, right-click context menu, `Ctrl + O` file picker, resizable border edge hit-testing.

---

### ✉️ Send via IP MSG (`ip-send.exe`)
Frameless dark/light ASCII TUI payload composer for IP Messenger (`ipmsg`).
- **CLI Options**:
  ```cmd
  ip-send.exe [--file <path>] [--to <recipient>] [--msg <text>] [--context-menu register|unregister]
  ```
- **Features**: Live user search (`/`), interactive keyboard shortcuts help (`/`), message edit mode (`i`), payload drag & drop (`Ctrl+O` / Paste), light/dark theme toggle (`Ctrl+D`), and Explorer context menu integration (`Send via IP-send`).

---

### 📁 Context Menu Manager (`context-menu.exe`)
Explorer context menu registry manager supporting unlimited nested submenus via XML configurations.
- **CLI Options**:
  ```cmd
  context-menu.exe [--update <config.xml>] [--backup <backup.xml>]
  ```
- **Features**: Native Windows choice dialog on double-click (Update / Backup), recursive XML schema support (`<item>` nested inside `<item>`), `%1` file / `%V` folder placeholders, and isolated per-user registry management under `HKCU\Software\Classes`.

---

### 🔤 OCR Utility (`ocr.exe`)
Portable zero-install optical character recognition utility powered by Tesseract OCR engine.
- **CLI Options**:
  ```cmd
  ocr.exe [<image_path>] [--lang eng|hin|tam|tel|auto] [--psm 6] [--oem 1]
  ```
- **Features**: Native File Explorer picker when run without arguments, automatic lazy downloading of Tesseract binary and language models to local exe directory, standard UTF-8 stdout output, auto clipboard copy (`CF_UNICODETEXT`), and WIC pre-processing image upscaling.

---

### 🗂️ Window Switcher (`window-switcher.exe`)
High-performance, minimal, keyboard-driven native Win32/DWM live thumbnail window switcher.
- **CLI Options**:
  ```cmd
  window-switcher.exe [--layout center|full] [--sort name|recent] [--background blur|tint] [--blur 1-100] [--tint-color #RRGGBBAA] [--delay <ms>]
  ```
- **Features**: Real UWP application process resolution (`ApplicationFrameHost.exe`), shortest unique prefix/subsequence hint generation (`Chrome` $\rightarrow$ `C`, `Code` $\rightarrow$ `CO`), single-letter hint cycling, Alt+Tab hook handling with configurable auto-commit timer (default 300 ms), search query filtering, and interactive card close buttons (`✕` graceful close, `Shift + ✕` force terminate).

---

### ⚙️ Command Manager (`cmdx`)
PowerShell global command dispatcher and dual-shim generator.
- **Commands**:
  ```powershell
  pwsh -File .\src\cmdx.ps1 setup    # Registers cmds/ in User PATH & generates .cmd / .ps1 shims
  pwsh -File .\src\cmdx.ps1 clean    # Removes shims & unregisters PATH
  pwsh -File .\src\cmdx.ps1 help     # Displays manual & registered alias mappings
  ```
- **Features**: Dynamic regex alias matching, automatic User PATH registration (`HKCU\Environment`), `WM_SETTINGCHANGE` broadcast, and in-session environment variable refreshing.

---

## 🏗️ Building from Source

### Prerequisites
- **Compiler**: Clang (`clang-cl` from MSVC Build Tools or LLVM / MinGW `clang`). **GCC is NOT supported.**
- **Shell**: PowerShell (`pwsh` or `PowerShell.exe`).
- **Libraries**: Native Win32 system libraries (`user32`, `gdi32`, `dwmapi`, `shell32`, `comdlg32`, `ole32`, `windowscodecs`, `urlmon`, `shlwapi`).

### Build Script Usage

Build targets output optimized binaries to `dist/release/` (or debug symbols to `dist/debug/` when specified).

```powershell
# Build all utilities (Release mode)
pwsh -File .\build.ps1 all

# Build specific target (e.g. window-switcher, pin-to-top, capture)
pwsh -File .\build.ps1 window-switcher
pwsh -File .\build.ps1 pin-to-top
pwsh -File .\build.ps1 capture

# Build in Debug mode (PDB / DWARF symbols)
pwsh -File .\build.ps1 -Mode debug all
```

#### Target Aliases
The build script supports convenient target aliases:
- `pin-to-top` $\rightarrow$ `pin`
- `find-my-mouse` $\rightarrow$ `cursor`
- `mouse-spotlight` $\rightarrow$ `spotlight`
- `color-picker` $\rightarrow$ `color`
- `capture` $\rightarrow$ `snip`
- `pixel-view` $\rightarrow$ `pixelview`
- `ip-send` $\rightarrow$ `ip`
- `context-menu` $\rightarrow$ `contextmenu`
- `window-switcher` $\rightarrow$ `switcher`

---

## ⌨️ AutoHotkey Integration Example

You can bind hotkeys in AutoHotkey (`.ahk`) to trigger these tools instantly:

```autohotkey
#Requires AutoHotkey v2.0

; Win + Ctrl + T -> Pin / Unpin Active Window
#^t::Run("dist\release\pin-to-top.exe --border-width 3")

; Win + Ctrl + C -> Find My Mouse Pointer
#^c::Run("dist\release\find-my-mouse.exe")

; Win + Ctrl + S -> Mouse Spotlight for Presentations
#^s::Run("dist\release\mouse-spotlight.exe --size 150 --dim 65")

; Win + Alt + C -> Color Picker
#!c::Run("dist\release\color-picker.exe")

; Win + Shift + S -> Screen Capture Mode Picker
#+s::Run("dist\release\capture.exe --draw --toolbar")

; Alt + Tab Replacement -> Window Switcher
!Tab::Run("dist\release\window-switcher.exe --delay 300")
```

---

## 📂 Repository Structure

```text
tiny-windows-utilites/
├── build.ps1                # Centralized Clang build pipeline & target configurations
├── AGENTS.md                # Agent guide & developer coding rules
├── README.md                # Project documentation & overview
├── docs/                    # Architectural specs, PRD, and per-tool documentation
│   ├── PRD.md               # Authoritative behavior specification
│   ├── CHECKLIST.md         # Implementation status & verification log
│   ├── JOURNEY.md           # Engineering journey, decisions & trade-offs
│   ├── capture.md           # Screen capture utility design
│   ├── color-picker.md      # Color picker design
│   ├── context-menu.md      # Context menu utility design
│   ├── find-my-mouse.md     # Find my mouse utility design
│   ├── ip-send.md           # IP Send utility design
│   ├── mouse-spotlight.md   # Mouse spotlight design
│   ├── ocr.md               # OCR utility design
│   ├── pin-to-top.md        # Pin to top utility design
│   ├── pixel-view.md        # Pixel view utility design
│   ├── window-switcher.md   # Window switcher design
│   └── cmdx.md              # CMDX command registry design
└── src/                     # Native Win32 C & PowerShell source code
    ├── capture.c
    ├── cmdx.ps1
    ├── color-picker.c
    ├── context-menu.c
    ├── find-my-mouse.c
    ├── ip-send.c
    ├── mouse-spotlight.c
    ├── ocr.c
    ├── pin-to-top.c
    ├── pixel-view.c
    ├── window-switcher.c
    └── common/              # Shared C micro-headers
        ├── clipboard.h      # Win32 Unicode & bitmap clipboard helpers
        ├── color-thief-algorithm.h # Desktop wallpaper accent color extractor
        ├── font.h           # Centralized typography & physical DPI scaling
        ├── tiny_cli.h       # Zero-heap declarative CLI argument tokenizer
        ├── tiny_dpi.h       # Per-Monitor v2 DPI awareness initializer
        ├── tiny_gui.h       # Win32 GDI overlay & geometry helpers
        └── tiny_ipc.h       # Single-instance mutex & event IPC contract
```

---

## 📄 License

Distributed under the MIT License.
