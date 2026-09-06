# ColorPicker Design (color-picker.c)

- **CLI** (`--output hex|rgb|rgba|hls|cmyk`, `--font-size N`, `--width N`): optional; default output is `hex`, default font size is 13 (range 8..48).
- **Single Tooltip Window**: Uses a lightweight floating layered card window (`g_tooltip`) positioned near cursor with `SetWindowPos` and clamped per-monitor using `MonitorFromPoint`.
- **Multi-format layout**: Displays clean color representations (`HEX`, `RGB`, `HSL`, `CMYK`) simultaneously in an aligned 3-column layout below a top horizontal color swatch bar.
- **Dynamic Auto-Sizing**: `RecalculateTooltipSize` measures exact text extents on each cursor update before `SetWindowPos`, avoiding clipping on any monitor or font size.
- **Live Sampling**: Samples pixel color at cursor position every 16ms using cached desktop HDC (`GetPixel`).
- **Shortcut Keys & Copying**:
  - Keys `1`–`4` copy formatted color strings (`HEX`, `RGB`, `HSL`, `CMYK`) to Clipboard (`CF_UNICODETEXT`) and exit immediately.
  - Left-click copies configured output format (or default `HEX`) and exits.
  - Right-click / ESC / second launch toggle cancels cleanly without copying.
- **Window Styles**: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, base `WS_POPUP`.
