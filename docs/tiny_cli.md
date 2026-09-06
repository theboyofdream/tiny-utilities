# TinyCLI Common Module (`src/common/tiny_cli.h`)

Declarative, zero-heap, wide-character command-line tokenizer and argument parser for Win32 utilities.

## Overview
- **Zero Allocation**: Tokenizes command-line strings in-place (`wchar_t*`) into standard `argc`/`argv` arrays without allocating heap memory or calling `CommandLineToArgvW` (eliminating `shell32.dll` dependency where unneeded).
- **Declarative Schema**: Command-line arguments and flags are parsed into a static declarative option table (`CliOption`).
- **Bounds Checking**: Automatically clamps integer arguments to specified `[minVal, maxVal]` ranges.

## API Reference

### Data Types
- `CliOptType`:
  - `CLI_OPT_BOOL`: Boolean flag (e.g. `--clipboard` / `--draw`). Sets target bool to `true`.
  - `CLI_OPT_INT`: Integer option with range clamping (e.g. `--border-width 3`, `--font-size 16`).
  - `CLI_OPT_STRING`: String option pointer assignment (e.g. `--save "C:\path"`).
- `CliOption` struct:
  ```c
  typedef struct {
      const wchar_t *name;     /* e.g. L"--border-width" or L"-b" */
      CliOptType     type;     /* Option type */
      void          *outValue; /* Pointer to target variable */
      int            minVal;   /* Range min (for CLI_OPT_INT) */
      int            maxVal;   /* Range max (for CLI_OPT_INT) */
  } CliOption;
  ```

### Functions
- `int TinyCLI_Tokenize(wchar_t *buf, wchar_t **argv, int maxArgs)`
  Splits wide command-line string into tokens in-place, handling spaces, tabs, and double-quoted strings.
- `void TinyCLI_Parse(int argc, wchar_t **argv, const CliOption *opts, int optCount)`
  Iterates over `argv` and populates the targets defined in `opts`.
- `void TinyCLI_ParseCommandLine(const CliOption *opts, int optCount)`
  Fetches the process command line via `GetCommandLineW()`, tokenizes it, and parses options in one call.
