# ⚡ Tiny Windows Productivity Utilities

An ultra-lightweight, high-performance suite of native **Win32 C & GDI** utilities designed for Windows power users and keyboard enthusiasts.

> [!NOTE]
> **Zero Runtimes. Zero Background Daemons. Zero Bloat.**  
> Every tool is a pure Win32 C binary compiled with Clang. There are no background services, no Electron runtimes, and no disk/registry persistence. Each utility launches on demand, performs its task with sub-millisecond responsiveness, and exits cleanly when toggled off.

---

## 🛠️ The Utilities Suite

Every utility runs as an independent, single-instance executable with instant hotkey toggling:

| Utility | Binary | Type | Description | Docs |
| :--- | :--- | :--- | :--- | :--- |
| **Pin to Top** | `pin-to-top.exe` | GUI | Toggles active window topmost status with wallpaper accent-colored border overlay & interactive window picker. | [Details](docs/pin-to-top.md) |
| **Window Switcher** | `window-switcher.exe` | GUI | Live thumbnail Alt+Tab replacement with unique hint badges for instant 1-key direct window jumping (no visual hunting). | [Details](docs/window-switcher.md) |
| **Find My Mouse** | `find-my-mouse.exe` | GUI | High-visibility cursor locator with smooth pulse-in / pulse-out animation disc centered on mouse pointer. | [Details](docs/find-my-mouse.md) |
| **Mouse Spotlight** | `mouse-spotlight.exe` | GUI | Dims desktop for presentations, leaving an anti-aliased spotlight hole on mouse pointer with dynamic resizing. | [Details](docs/mouse-spotlight.md) |
| **Color Picker** | `color-picker.exe` | GUI | Floating color magnifier & tooltip displaying real-time `HEX`, `RGB`, `HSL`, and `CMYK` with 1-key clipboard copy. | [Details](docs/color-picker.md) |
| **Screen Capture** | `capture.exe` | GUI | Screenshot & doodle annotation suite with interactive mode picker (Snip, Window, Fullscreen) and drawing canvas. | [Details](docs/capture.md) |
| **Pixel View** | `pixel-view.exe` | GUI | Frameless image & animated GIF viewer featuring nearest-neighbor chunky pixel block rendering for retro graphics. | [Details](docs/pixel-view.md) |
| **IP Send** | `ip-send.exe` | GUI | ASCII TUI payload composer with search, message wrapping, and file drag-and-drop. *(Requires [IP Messenger](https://ipmsg.org/en/))* | [Details](docs/ip-send.md) |
| **Context Menu Manager** *(Preview)* | `context-menu.exe` | GUI / CLI | Cascading Explorer context menu registry utility with recursive submenus and XML backup/update. *(Experimental)* | [Details](docs/context-menu.md) |
| **OCR Utility** | `ocr.exe` | GUI / CLI | Portable zero-install OCR tool leveraging Tesseract engine with automatic language downloading and clipboard copy. | [Details](docs/ocr.md) |
| **Command Manager** | `cmdx.ps1` | CLI | PowerShell command dispatcher creating `.cmd`/`.ps1` dual-shims in a dedicated folder registered in User PATH. | [Details](docs/cmdx.md) |

> [!TIP]
> *`ip-send` is an independent open-source frontend and is not affiliated with or endorsed by IP Messenger (`ipmsg.org`).*

---

## 🚀 Key Advantages & Architecture

- **Zero Background Overhead**: No resident processes, background tray services, or memory bloat. Tools launch instantly via hotkeys and exit immediately upon completion.
- **Single-Instance IPC Toggle**: Interactive overlays use named Win32 Mutex & Event primitives (`Global\Tiny<Tool>Mutex` / `Event`). Re-launching a running tool instantly toggles it off and exits cleanly.
- **Per-Monitor DPI v2 Aware**: Automatically scales overlays, fonts, spotlights, and hitboxes dynamically across multi-monitor setups with mixed DPI scaling factors (100%, 150%, 200%).
- **Pure Win32 & GDI/DWM**: Hardware-accelerated Desktop Window Manager compositing without DirectX, Electron, or heavy web engines. Memory footprint is < 10 MB per tool with < 0.2% CPU.

---

## ⌨️ Ready-to-Use Hotkeys (Powered by `shortcuts.ahk`)

The suite includes a ready-to-run AutoHotkey v2 script: [`docs/shortcuts.ahk`](docs/shortcuts.ahk). Double-clicking it in the same directory as the executables instantly activates all shortcuts:

| Shortcut | Tool Triggered | What It Does |
| :--- | :--- | :--- |
| **`Alt + Tab`** | `window-switcher` | Live thumbnail Alt+Tab replacement with instant 1-letter hint jumping |
| **`Double-Tap Ctrl`** | `mouse-spotlight` | Dims screen & spotlights cursor for instant locator or meeting presentation focus |
| **`Win + Ctrl + T`** | `pin-to-top` | Pin/unpin active window topmost with accent-colored border overlay |
| **`Win + Ctrl + O`** | `capture` + `ocr` | **Snip $\rightarrow$ OCR**: Snip any region and copy the extracted text to clipboard |
| **`Win + Ctrl + C`** | `color-picker` | Floating screen color magnifier with 1-key Hex/RGB clipboard copy |
| **`PrintScreen`** | `capture` | Fullscreen screenshot with drawing canvas and doodle annotations |
| **`Ctrl + PrintScreen`** | `capture` | Interactive rectangular region snipping & doodle |
| **`Alt + I`** | `ip-send` | Quick LAN IP Messenger composer *(requires [IP Messenger](https://ipmsg.org/en/))* |

👉 **Want to see the full daily driver setup? Read [How I Use Tiny Utilities (`docs/HOW_I_USE_IT.md`)](docs/HOW_I_USE_IT.md)**

---

## 🏗️ Building from Source & Contributing

Want to compile the utilities yourself, customize flags, or add new tools?  
👉 **Check out the full [Developer Guide (`docs/DEVELOPMENT.md`)](docs/DEVELOPMENT.md)** for:
- 1-minute quickstart & Clang installation commands.
- `build.ps1` reference and release/debug modes.
- Shared micro-headers architecture in [`src/common/`](src/common/).
- IPC contract specifications and testing workflows.

---

## 📄 License

Distributed under the [MIT License](LICENSE).
