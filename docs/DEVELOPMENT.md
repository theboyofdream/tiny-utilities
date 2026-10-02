# 🛠️ Developer Guide & Build Setup

Welcome to the development guide for **Tiny Windows Productivity Utilities**! This document explains how to set up your environment, build tools from source, understand the architecture, and contribute new utilities.

---

## 🚀 1-Minute Quick Start

```powershell
# 1. Clone the repository
git clone https://github.com/theboyofdream/tiny-utilities.git
cd tiny-utilities

# 2. Build all utilities in Release mode (takes ~15 seconds)
pwsh -File .\build.ps1 all
# (Or using built-in Windows PowerShell: powershell -File .\build.ps1 all)

# 3. Test any utility immediately
.\dist\release\window-switcher.exe
```

---

## 🧰 Prerequisites & Toolchain Setup

All utilities are written in native **Win32 C (C99)** and compiled strictly using **Clang** (`clang-cl` or LLVM/MinGW `clang`).

### 1. Install LLVM Clang (Required)
Install LLVM via Windows Package Manager:
```cmd
winget install LLVM.LLVM
```
*(Or via Chocolatey: `choco install llvm`, or Scoop: `scoop install llvm`).*

> [!NOTE]
> `build.ps1` includes automatic toolchain discovery that scans standard installation directories (LLVM, Visual Studio, MSYS2, Scoop, Chocolatey). As long as Clang is installed, `build.ps1` will automatically find and use it without requiring manual PATH changes.

### 2. PowerShell
- Works out-of-the-box with built-in **Windows PowerShell 5.1** (`powershell.exe`).
- Also fully compatible with modern **PowerShell 7+** (`pwsh`).

### 3. Windows SDK / System Libraries
The project links against native Windows system libraries included with Windows:
`user32`, `gdi32`, `dwmapi`, `shell32`, `comdlg32`, `ole32`, `windowscodecs`, `urlmon`, `shlwapi`, `advapi32`, `comctl32`.

---

## 🏗️ Build System (`build.ps1`)

The central build script is [`build.ps1`](../build.ps1). It supports target selection, release/debug modes, and cross-compilation for `x64` and `arm64`.

### Syntax
```powershell
pwsh -File .\build.ps1 [-Mode release|debug] [-Arch x64|arm64] [target...]
```

### Common Build Commands

```powershell
# Build everything in release mode (default: x64)
pwsh -File .\build.ps1 all

# Build a specific utility
pwsh -File .\build.ps1 window-switcher
pwsh -File .\build.ps1 pin-to-top
pwsh -File .\build.ps1 capture

# Build with Debug symbols (generates PDB / DWARF symbols, disables optimization)
pwsh -File .\build.ps1 -Mode debug window-switcher

# Cross-compile for Windows on ARM64 (Surface / Snapdragon X Elite)
pwsh -File .\build.ps1 -Arch arm64 all

# Interactive menu (run without arguments)
pwsh -File .\build.ps1
```

### Supported Build Targets
To ensure consistency and avoid ambiguity, all targets use explicit, canonical names:
| Target Name | Output Binary | Description |
| :--- | :--- | :--- |
| `pin-to-top` | `dist/release/pin-to-top.exe` | Topmost window border overlay & picker |
| `find-my-mouse` | `dist/release/find-my-mouse.exe` | Animated cursor locator disc |
| `mouse-spotlight` | `dist/release/mouse-spotlight.exe` | Screen dimming spotlight overlay |
| `color-picker` | `dist/release/color-picker.exe` | Screen color magnifier & multi-format picker |
| `capture` | `dist/release/capture.exe` | Screenshot & doodle annotation suite |
| `pixel-view` | `dist/release/pixel-view.exe` | Frameless nearest-neighbor pixel art viewer |
| `ip-send` | `dist/release/ip-send.exe` | ASCII TUI IP Messenger payload composer |
| `context-menu` | `dist/release/context-menu.exe` | Explorer context menu XML registry manager *(Preview / Experimental)* |
| `ocr` | `dist/release/ocr.exe` | Zero-install Tesseract OCR CLI & picker |
| `window-switcher` | `dist/release/window-switcher.exe` | Live thumbnail Alt+Tab window switcher |

---

## 🏛️ Architecture & Shared Headers (`src/common/`)

To avoid reinventing boilerplate, all utilities share zero-dependency micro-headers located in [`src/common/`](../src/common/):

- [`tiny_cli.h`](../src/common/tiny_cli.h): Zero-heap declarative wide-character CLI argument parser (`TinyCLI_Parse`).
- [`tiny_ipc.h`](../src/common/tiny_ipc.h): Single-instance mutex & event toggle pattern helper (`TinyIPC_AcquireOrToggle`).
- [`tiny_dpi.h`](../src/common/tiny_dpi.h): Per-Monitor v2 DPI awareness initializer with dynamic monitor scaling fallbacks.
- [`tiny_gui.h`](../src/common/tiny_gui.h): Win32 GUI helpers (multi-monitor virtual bounds `TinyGUI_GetVirtualScreenBounds`, alpha dimming overlay `TinyGUI_ApplyDimOverlay`, border hit-testing).
- [`font.h`](../src/common/font.h): Typography constructors and DPI-scaled font creation (`TinyFont_Create`, `TinyFont_ScaleSize`).
- [`clipboard.h`](../src/common/clipboard.h): Win32 Unicode text, bitmap, and file drop clipboard operations (`TinyClipboard_SetText`).
- [`color-thief-algorithm.h`](../src/common/color-thief-algorithm.h): MMCQ color quantization for extracting dominant wallpaper accent colors.

### The Single-Instance Toggle IPC Contract
Interactive overlay tools (`pin-to-top`, `find-my-mouse`, `mouse-spotlight`, `color-picker`, `capture`, `window-switcher`) follow an instant toggle lifecycle:
1. **First Launch**: Creates a named Mutex (`Global\Tiny<Tool>Mutex`) and a named Event (`Global\Tiny<Tool>Event`).
2. **Second Launch**: Detects the existing Mutex (`ERROR_ALREADY_EXISTS`), signals the Event (`SetEvent`), and exits immediately with code `0`.
3. **First Instance**: Receives the signaled event and gracefully shuts down overlays, exiting cleanly.

---

## 📋 Coding Rules & Guidelines

- **Language**: Strict ISO C99, Win32 API (`W`-suffixed wide-character functions, e.g., `CreateWindowExW`).
- **No Background Services**: Tools must **never** run resident background services or system hooks. They launch on demand and exit immediately when toggled or dismissed.
- **Zero Disk Persistence / Registry Clutter**: No config files or roaming registry pollution (except Explorer context menus managed via `context-menu`).
- **Memory Footprint**: Designed to remain under 10 MB per tool with < 0.2% CPU usage.
- **Entry Points**: `WinMain` for GUI overlays; `wmain` / `main` for console tools.

---

## 🧪 Verification & Testing Workflow

After modifying or adding a tool:
1. **Compile the Target**:
   ```powershell
   pwsh -File .\build.ps1 <target>
   ```
2. **Smoke Test the Binary**:
   - Run `.\dist\release\<target>.exe`.
   - Verify double-launch toggle-off behavior (for overlay tools).
   - Test on multi-monitor setups or high-DPI scaling (125%, 150%, 200%).
3. **Documentation**:
   - Update [`CHECKLIST.md`](CHECKLIST.md) for behavior changes.
   - Update [`JOURNEY.md`](JOURNEY.md) for architectural decisions and trade-offs.
