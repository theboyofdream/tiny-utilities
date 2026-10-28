# Pixel View Tool Design (pixel-view.c)

- **CLI**: `pixel-view.exe --file <image_or_gif> [--pixel-size N] [--grid 320x200] [--ratio 400x400] [--bg transparent|solid|gradient|pixelated]` (also accepts positional file path).
- **Pixelated Game Detail Preservation Engine**:
  - Maintains a constant chunky pixel block scale (`pixelScale`, default 3px) relative to screen pixels.
  - When the window is resized larger, the pixel block size remains sharp and constant (`N x N`), which **reveals more image details** while keeping 100% hard, chunky retro pixel art edges (just like retro game engines running on high-resolution displays).
  - Also supports 1:1 direct point sampling (`--pixel-size 1`) for maximum detail & crisp pixel art.
- **Pixel Scale CLI / Options**:
  - `--pixel-size 1`: 1px block size (Crisp Pixel Art - Max Detail).
  - `--pixel-size 2`: 2px block size (Retro Game 2x - Default).
  - `--pixel-size 3`: 3px block size (Retro Game 3x).
  - `--pixel-size 4`: 4px block size (Chunky Pixel Art 4x).
  - `--pixel-size 8`: 8px block size (Ultra Chunky 8x).
  - `--grid 320x200`: Optional fixed retro console resolution grid.
- **Input Formats**: PNG, JPG, BMP, GIF (animated GIF frames rendered with exact frame delay and disposal method).
- **Window**: Frameless/borderless, freely resizable (`WM_NCHITTEST` edge controls), draggable via left-click anywhere (`HTCAPTION`).
- **Background Modes**:
  - `Pixelated Gradient` (Default): low-resolution color block grid.
  - `Linear Gradient`: configurable start/end gradient.
  - `Solid Color`: solid color background.
  - `Transparent`: layered window desktop transparency (`UpdateLayeredWindow` per-pixel alpha).
- **Keyboard Shortcuts**:
  - `Ctrl + O` → Open File dialog.
- **Right-Click Context Menu**:
  - Open File (`Ctrl+O`)
  - Submenu: Background Mode
  - Submenu: Pixel Scale / Detail (1px, 2px, 3px Default, 4px, 8px)
  - Close
- **IPC Contract**: `Global\TinyPixelViewMutex` and `Global\TinyPixelViewEvent` single instance double-launch toggle support.
- **Build**: `pwsh -File .\build.ps1 pixel-view` (outputs `dist/release/pixel-view.exe`).

---

## Verified

- [x] Native Win32 frameless overlay; WIC decodes PNG/JPG/BMP/GIF (disposals, offsets, delays)
- [x] Constant pixel scale (default 2 px), nearest-neighbor only; resizing reveals more detail at 1:1
- [x] Opens 400 px wide, aspect-preserved, kept across resizes
- [x] Backgrounds: pixelated gradient (default), linear, solid, transparent
- [x] Drag = `HTCAPTION`; resizable; right menu: open (`Ctrl+O`), bg, scale 1/2/3/4/8, close
- [x] CLI `--file` (positional ok, `--pixel-size`, `--ratio`, `--bg`, `--grid`); IPC toggle mutex/event
- [x] 174.5 → 144.0 KB (`wsprintfW`/`lstrlenW`/`lstrcatW`, 2026-08-21)
- [x] `--help`: 14 lines, exit 0 (2026-10-01); `.\build.ps1 pixel-view` clean
