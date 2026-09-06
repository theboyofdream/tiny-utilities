# Font Common Module (`src/common/font.h`)

Header-only font management, fallback chains, centralized typography metrics, dark-mode color palette, runtime Ctrl +/- zoom controls, and OS default font size detection.

## Overview
- **OS Default Font Size Query**: Queries `SPI_GETNONCLIENTMETRICS` (`ncm.lfMessageFont.lfHeight`) via `TinyFont_GetSystemDefaultSize()` to match the operating system's native message font height by default.
- **Runtime Zoom Controls (`Ctrl +` / `Ctrl -` / `Ctrl 0`)**: Standardized runtime font zoom handler (`TinyFont_HandleZoomKey`) supporting:
  - `Ctrl +` (`VK_OEM_PLUS` / `VK_ADD`): Increases font size by 1px (clamped to `TINY_FONT_SIZE_MAX` 48).
  - `Ctrl -` (`VK_OEM_MINUS` / `VK_SUBTRACT`): Decreases font size by 1px (clamped to `TINY_FONT_SIZE_MIN` 8).
  - `Ctrl 0` (`0` / `VK_NUMPAD0`): Resets font size to the system default OS font size.
- **Fixed Physical Footprint**: Maintains a consistent physical pixel footprint (`fontSize` pixels) across monitors instead of aggressive font DPI multipliers, keeping typography visually uniform between laptops and desktop displays while window/layout bounds retain Per-Monitor v2 DPI awareness.
- **Monospace Fallback Chain**: Probes system fonts in order: `JetBrains Mono` -> `Consolas` -> `Fira Code` -> `Cascadia Code` -> `Cascadia Mono` -> `Courier New` -> system monospace.
- **UI Sans-Serif Fallback Chain**: Probes `Segoe UI` -> `Tahoma` -> `Arial`.
- **Dark Theme Palette**: Centralized `COLORREF` constants for UI backgrounds, text, borders, and selection accents.

## Constants

### Font Sizes (Logical Pixels)
- `TINY_FONT_SIZE_MIN` (8)
- `TINY_FONT_SIZE_XS` (10)
- `TINY_FONT_SIZE_SMALL` (11)
- `TINY_FONT_SIZE_DEFAULT` (12)
- `TINY_FONT_SIZE_NORMAL` (13)
- `TINY_FONT_SIZE_MEDIUM` (15)
- `TINY_FONT_SIZE_LARGE` (18)
- `TINY_FONT_SIZE_XLARGE` (22)
- `TINY_FONT_SIZE_MAX` (48)

### Font Weights
- `TINY_FONT_WEIGHT_LIGHT` (`FW_LIGHT` / 300)
- `TINY_FONT_WEIGHT_NORMAL` (`FW_NORMAL` / 400)
- `TINY_FONT_WEIGHT_MEDIUM` (`FW_MEDIUM` / 500)
- `TINY_FONT_WEIGHT_SEMIBOLD` (`FW_SEMIBOLD` / 600)
- `TINY_FONT_WEIGHT_BOLD` (`FW_BOLD` / 700)

### UI Theme Colors
- `TINY_COLOR_BG_DARK`: `RGB(24, 24, 24)`
- `TINY_COLOR_TEXT_BRIGHT`: `RGB(238, 238, 238)`
- `TINY_COLOR_TEXT_MUTED`: `RGB(119, 119, 119)`
- `TINY_COLOR_ACCENT`: `RGB(0, 122, 204)`
- `TINY_COLOR_BORDER`: `RGB(50, 50, 50)`
- `TINY_COLOR_SELECTION`: `RGB(45, 90, 140)`

## API Reference

### Functions
- `int TinyFont_GetSystemDefaultSize(void)`
  Returns the system message font height in logical pixels from `SPI_GETNONCLIENTMETRICS`.
- `int TinyFont_StepSize(int currentSize, int delta)`
  Clamps font size adjustments strictly within `[TINY_FONT_SIZE_MIN, TINY_FONT_SIZE_MAX]` (8..48).
- `bool TinyFont_HandleZoomKey(WPARAM wParam, int *pFontSize)`
  Intercepts `Ctrl +`, `Ctrl -`, and `Ctrl 0` key messages to adjust or reset runtime font sizes.
- `bool TinyFont_IsAvailable(const wchar_t *family)`
  Thread-safe, re-entrant check for installed font families via `EnumFontFamiliesExW`.
- `const wchar_t *TinyFont_GetBestMonospaceFace(void)`
  Returns the best available monospace font face name (cached).
- `const wchar_t *TinyFont_GetBestUIFace(void)`
  Returns the best available UI font face name (cached).
- `int TinyFont_ScaleSize(int fontSize, UINT dpi)`
  Returns the DPI-scaled pixel height (`MulDiv(fontSize, dpi, 96)`) so that typography maintains the exact same physical size (in inches / millimeters) on screen across both laptop screens and desktop monitors.
- `HFONT TinyFont_Create(const wchar_t *faceName, int fontSize, int weight, bool italic, UINT dpi)`
  Constructs a ClearType GDI font with monitor-DPI physical scaling.
- `HFONT TinyFont_CreateMonospace(int fontSize, int weight, UINT dpi)`
  Constructs a monospace font (`JetBrains Mono` / fallback chain) with `FIXED_PITCH | FF_MODERN`.
- `HFONT TinyFont_CreateUI(int fontSize, int weight, UINT dpi)`
  Constructs a UI font (`Segoe UI` / fallback chain) with `VARIABLE_PITCH | FF_SWISS`.
