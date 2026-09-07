# Mouse Spotlight Design (`mouse-spotlight.c`)

Native Win32 presentation overlay tool that dims the screen except for a customizable spotlight around the mouse cursor to direct audience attention during presentations, tutorials, and live demos.

- **CLI Configuration**:
  - `--size N` / `-s N`: Sets initial spotlight base radius in pixels at 96 DPI (range `20..1000`, default `150`).
  - `--dim N` / `-d N`: Sets background dimming percentage (`0..100%`) or raw alpha translucency (`0..255`, default `65` / `165` alpha).
- **Interactive Hotkey Controls**:
  - `Ctrl +` / `Ctrl =` / `Numpad +`: Smoothly resizes the spotlight larger (+3px base radius per frame, max `1000`).
  - `Ctrl -` / `Numpad -`: Smoothly resizes the spotlight smaller (-3px base radius per frame, min `20`).
  - `Ctrl 0` / `Numpad 0`: Resets spotlight size back to the initial startup size (`g_initialBaseSize`).
  - `Esc`, any key press, or mouse click: Exits the utility cleanly (following `find-my-mouse` UX conventions, with a 250ms activation grace period).
- **Single-Instance IPC Toggle**:
  - **First launch**: Creates named mutex `Global\TinyMouseSpotlightMutex` and auto-reset event `Global\TinyMouseSpotlightEvent`, creates overlay window, enters 60 FPS loop.
  - **Second launch**: Detects existing mutex, signals event, and exits `0`. Running instance receives toggle signal and exits cleanly.
- **DPI-Aware Multi-Monitor Scaling**:
  - Uses `TinyDPI_EnablePerMonitorAwareness()` on startup for 1:1 physical pixel accuracy.
  - Dynamically calculates monitor DPI using `TinyDPI_GetDpiForPoint(cur)`.
  - Scales radius dynamically as `MulDiv(g_baseSize, dpi, 96)` so physical spotlight size on screen remains identical across displays of different DPI scaling (100%, 150%, 200%).
- **Silky Smooth Zero-Lag Per-Pixel Alpha DIB Renderer**:
  - Replaced heavy Win32 `SetWindowRgn` calls (which force Windows User32 to invalidate and recalculate non-client regions 60 times/sec) with `UpdateLayeredWindow` on a 32-bpp top-down DIB section.
  - On cursor move, clears only the bounding box of the previous circle position and renders the new spotlight circle with anti-aliased soft edge feathering (`FEATHER_PX = 6`).
  - Zero DWM window region recalculation overhead, providing silky smooth 60+ FPS mouse tracking with zero micro-stutter.
  - Mouse clicks pass straight through (`WS_EX_TRANSPARENT`).
- **Build & Dependencies**:
  - Pure Win32 C99, compiled with Clang (`clang` MinGW or `clang-cl` MSVC).
  - Uses shared common headers: `tiny_cli.h`, `tiny_ipc.h`, `tiny_dpi.h`, `tiny_gui.h`.
  - Links standard Win32 libraries: `user32`, `gdi32`.
