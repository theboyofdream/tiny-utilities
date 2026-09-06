# OCR Tool Design (src/ocr.c -> ocr.exe)

- **Source**: `src/ocr.c` -> `ocr.exe` (Win32 C Windows GUI application, Clang, tiny, strict Win32, `COBJMACROS` WIC, no services/registry/hooks, `wWinMain` with `WinMain` fallback).
- **CLI & Interactive Flow**:
  - `ocr.exe --image <png|jpg|jpeg|bmp> [--lang eng|auto] [--psm 6] [--oem 1]` (also positional `ocr.exe <path>`, `--image=`, `--lang=`, `-i`, `-l`).
  - **No CLI Argument / GUI Launch**: If launched with no parameters (e.g. double-clicked in File Explorer or run as `ocr.exe`), opens a native Windows File Explorer picker dialog (`GetOpenFileNameW` via `comdlg32`) filtered to `*.png; *.jpg; *.jpeg; *.bmp`. Exits cleanly (`0`) if cancelled.
  - `--lang` supports `+` multi (`eng+hin`, `eng+tam`), `auto` triggers all installed language models + `eng/hin/tam/tel` fallback.
  - `--psm` defaults to `6` (single uniform block) with range `0..13`, `--oem` default `1` with range `0..3`.
  - `--help`/`--version` to `stderr`. Validates existence, accepts `png`/`jpg`/`jpeg`/`bmp` (exit 2 for unsupported, exit 1 for missing).
- **Subsystem & Console Attachment**:
  - Compiled with `/SUBSYSTEM:WINDOWS` (and `-mwindows`) to avoid opening a blank Command Prompt / console window when double-clicked.
  - When executed from an existing terminal, calls `AttachConsole(ATTACH_PARENT_PROCESS)` to stream UTF-8 output to `stdout`/`stderr`.
- **Resources (always portable)**: beside executable:
  ```
  ocr.exe
  ocr-resources/
    tesseract/tesseract.exe (zstrathe portable 4.1.1, 4765184)
    tessdata/<lang>.traineddata (eng 4113088, hin 1122751, tam 3237963, tel 2769654, osd 10562727)
    tmp/ (ephemeral)
  ```
  No `%APPDATA%`/`%LOCALAPPDATA%`/`HKCU` dependency. All paths via `GetModuleFileNameW` -> `get_resource_dir()`, `get_tesseract_exe()`, `get_tessdata_dir()`. Second run reuses cached files, never redownloads valid ones. Delete `ocr-resources/` to uninstall.
- **Smart provisioning (Copyfish-like standalone, always portable)**:
  - `ensure_tesseract_runtime()`: validates `ocr-resources/tesseract/tesseract.exe` `>10KB`. If missing/corrupt -> creates `ocr-resources/{tesseract,tessdata,tmp}`, downloads `TESS_PORTABLE_URL` `zstrathe/tesseract_portable_windows` `main.zip` via `urlmon!URLDownloadToFileW` -> `curl -L` -> `powershell IWR` fallback, extracts via `powershell Expand-Archive`, copies `tesseract.exe` + `tessdata` via `copy_dir_recursive`. No elevation required.
  - `ensure_lang_file()` / `ensure_requested_langs()`: validates `tessdata/<lang>.traineddata` `>10KB`. Lazily downloads requested langs via `TESSDATA_BASE_URL` `tessdata_fast/raw/main/<lang>.traineddata` via same downloader, caches locally.
  - `detect_lang_auto()`: if `--lang auto` (default), scans `tessdata/*.traineddata` joined with `+` plus `eng+hin+tam+tel`.
- **OCR optimization** (`PreprocessImageWIC`, WIC `COBJMACROS`):
  - Decodes via `WIC` `CLSID_WICImagingFactory` -> `CreateDecoderFromFilename` -> `GetFrame` -> `GetSize`.
  - Upscaling: `scale 2.0` default, `2.5` if `<600`, `3.0` if `<400`, `1.5` if `>=2000&&>=1200`, capped at `3200` max width, `IWICBitmapScaler` `Fant`.
  - Grayscale conversion: `IWICFormatConverter` `GUID_WICPixelFormat8bppGray`.
  - Encodes to `%TEMP%\ocr_prep_<tick>.png` via WIC PNG encoder.
  - Layout: `run_tesseract` passes `-c preserve_interword_spaces=1 -c user_defined_dpi=300`.
- **Run & Output**:
  - `RunOCR`: invokes `"<exe>" "<img>" stdout -l <lang> --psm <psm> --oem <oem> --tessdata-dir "<tessdata>" -c ...`, `CreateProcessW(CREATE_NO_WINDOW)` with piped stdout/stderr, converts UTF-8 output to wide characters.
  - **Clipboard Copy**: Automatically copies recognized text to the Windows Clipboard (`CF_UNICODETEXT`).
  - **Stdout Output**: Writes formatted UTF-8 text to `stdout` (`WriteConsoleW` / `WriteFile`).
  - **Interactive Alert Dialog**: If launched interactively without CLI parameters, displays a native alert / notification dialog (`MessageBoxW`) showing the recognized text preview and copy confirmation.
- **Build**:
  - `pwsh -File .\build.ps1 ocr` -> `dist/release/ocr.exe` (Windows subsystem `-mwindows`).
  - Libs: `user32, shell32, ole32, urlmon, shlwapi, windowscodecs, gdi32, comdlg32`.
- **Conventions**: Plain C99, `wWinMain` / `WinMain`, `TinyCLI_Parse`, no services/registry, clean exit.
