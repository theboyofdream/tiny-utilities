# Color Thief Algorithm Common Module (`src/common/color-thief-algorithm.h`)

Fast Median Cut Quantization (MMCQ) accent color extractor using pure Win32 GDI desktop sampling.

## Overview
Extracts the dominant aesthetic accent color from the user's desktop wallpaper or background using pure Win32 GDI without external graphic library dependencies (no DirectX, GDI+, or Direct2D).

1. **DWM Accent Query**: Attempts to query the active Windows DWM accent color via `DwmGetColorizationColor` (from `dwmapi.dll`).
2. **Median Cut Quantization (MMCQ)**: If DWM accent query is unavailable, samples a 32x32 pixel grid from the desktop background (`GetDesktopWindow`), computes a 3D RGB color histogram (5-bit quantization, 32,768 bins), and recursively partitions the color space to extract dominant vibrant accent tones.

## API Reference

### Functions
- `COLORREF GetWallpaperDominantColorThief(void)`
  Returns a `COLORREF` representing the primary wallpaper/desktop accent color. If sampling or quantization fails, falls back to Windows default blue (`RGB(0, 120, 215)`).
