# TinyDPI Common Module (`src/common/tiny_dpi.h`)

Per-Monitor v2 DPI awareness initializer with dynamic legacy fallbacks for high-DPI Windows displays.

## Overview
Ensures crisp, unscaled, pixel-perfect rendering across monitors with different DPI scaling factors (e.g. 100%, 125%, 150%, 200%).
- Probes dynamic runtime support for `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` in `user32.dll`.
- Cleanly falls back to `SetProcessDPIAware()` on legacy Windows platforms (Windows 7 / 8 / early Windows 10) without crashing.

## API Reference

### Functions
- `void TinyDPI_EnablePerMonitorAwareness(void)`
  Initializes Per-Monitor v2 awareness for the calling process. Safe to call at the entry point of any GUI or overlay tool.
