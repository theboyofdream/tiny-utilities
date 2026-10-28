# TinyGUI Common Module (`src/common/tiny_gui.h`)

Win32 GDI GUI and overlay helper routines for multi-monitor geometry, translucent alpha dimming, border resize hit-testing, and scaled font delegation.

## Overview
- **Multi-Monitor Geometry**: Computes the exact bounding rectangle covering all active monitors via display monitor enumeration (`EnumDisplayMonitors`).
- **Translucent Backdrop Dimming**: Alpha blends a dark overlay layer over desktop background snapshots (`msimg32!AlphaBlend`).
- **Border Resize Hit-Testing**: Evaluates 8-direction border resize coordinates (`HTTOPLEFT`, `HTBOTTOMRIGHT`, `HTLEFT`, etc.) for frameless tool windows on `WM_NCHITTEST`.
- **Scaled Font Delegation**: Seamlessly bridges legacy font creation calls to `TinyFont_Create`.

## API Reference

### Functions
- `void TinyGUI_GetVirtualScreenBounds(int *outX, int *outY, int *outW, int *outH)`
  Calculates the multi-monitor bounding box. Correctly handles negative virtual screen offsets and monitor arrangements.
- `void TinyGUI_ApplyDimOverlay(HDC hdcDest, HDC hdcDesktop, int vw, int vh, BYTE alpha)`
  Blits the desktop image to `hdcDest` and applies a translucent dark overlay with alpha level `alpha` (e.g. `120` or `140`).
- `LRESULT TinyGUI_HitTestResizeBorders(HWND hwnd, LPARAM lParam, int borderThickness)`
  Performs 8-direction border hit-testing for frameless resizable windows. Returns appropriate `HT*` hit-test codes or `HTCLIENT`.
- `HFONT TinyGUI_CreateScaledFont(int fontSize, int weight, const wchar_t *faceName)`
  Delegates font creation to `TinyFont_Create(faceName, fontSize, weight, false, 0)`.
