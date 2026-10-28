# TinyConfig Common Module (`src/common/tiny_config.h`)

Read-only `key = value` config file loader (plain ASCII, C99, Win32 + macOS).

## Overview
- **Read-only by contract**: the loader never writes. Callers treat the file as user-managed input only (the `mwb` / PRD §21 carve-out).
- **Format**: one `key = value` pair per line. Lines starting with `#` are comments; blank lines and lines without `=` are skipped silently.
- **Normalization**: keys and values are trimmed of surrounding blanks and lower-cased before the callback sees them.
- **Fixed line buffer**: 512 bytes per line; longer lines are truncated by `fgets` and processed as-is.

## API Reference

### Types
- `typedef void (*TinyConfigFn)(void *user, const char *key, const char *value)` — per-pair callback.

### Functions
- `int TinyConfig_Load(const char *path, TinyConfigFn fn, void *user, int *existed)`
  - Opens `path` in binary read mode and invokes `fn` for every valid pair.
  - **Returns**: number of pairs passed to the callback (`0` when the file is missing, unreadable, or has no valid pairs).
  - `*existed` (optional): set to `1` when the file was opened, `0` otherwise — lets callers distinguish "missing file" from "empty file".
  - A `NULL`/empty path or `NULL` callback returns `0` without touching the file.
- `TinyConfig_Trim(char *s)` / `TinyConfig_Lower(char *s)` — in-place helpers used by the loader; available to callers that need the same normalization.

## Usage Notes
- Parsing is deliberately minimal: no quoting, no escapes, no sections, no include support.
- Values are lower-cased, so case-sensitive data (paths, codes) cannot round-trip through the callback unchanged.
