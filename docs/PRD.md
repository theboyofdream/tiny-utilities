
# Tiny Windows Productivity Utilities (Agent-Proof + Clang + CLI Args)

---

# 0. Purpose

This document defines **fully deterministic behavior** for two Windows utilities:

1. **Pin2Top** — toggle topmost state + border overlay for the active window
2. **CursorFocus** — toggle cursor highlight overlay

This version adds:

* ✅ **Command-line arguments support (Pin2Top only)**
* ✅ **Clang as the required compiler (NOT GCC)**
* ✅ Strict Win32 C implementation rules
* ✅ Fully deterministic agent instructions

---

# 1. HARD RULES (NON-NEGOTIABLE)

Agents MUST follow all rules:

```text
- No background services
- No registry usage
- No disk persistence
- No configuration files
- No global system hooks
- One process per executable invocation
- No multi-process coordination except defined IPC
- All overlays must be click-through
- No UI/GUI windows except overlays
- Must exit cleanly on toggle-off
- Must compile with Clang (MSVC-compatible or MinGW-clang)
```

---

# 2. COMPILER REQUIREMENT

## 2.1 REQUIRED TOOLCHAIN

```text
clang (LLVM)
```

### Allowed:

* clang-cl (MSVC-style)
* clang via MinGW-w64
* LLVM clang on Windows

### NOT allowed:

* gcc
* g++
* non-clang toolchains

---

## 2.2 BUILD COMMANDS (CLANG)

### Option A — clang-cl

```bash
clang-cl pin.c user32.lib gdi32.lib -O2 -o pin.exe
clang-cl cursor.c user32.lib gdi32.lib -O2 -o cursor.exe
```

### Option B — clang (MinGW style)

```bash
clang pin.c -o pin.exe -O2 -mwindows -luser32 -lgdi32
clang cursor.c -o cursor.exe -O2 -mwindows -luser32 -lgdi32
```

---

# 3. CLI ARGUMENT SYSTEM (PIN2TOP ONLY)

## 3.1 Overview

Pin2Top now supports optional arguments:

```text
pin.exe [--border-width N] [--max-windows N]
```

---

## 3.2 ARGUMENT RULES

```text
- Arguments are optional
- If missing → use defaults
- Invalid values → fallback to defaults
- No config files
- No environment variables
```

---

## 3.3 SUPPORTED FLAGS

### 1. Border Width

```text
--border-width N
```

#### Meaning:

Sets overlay border thickness in pixels.

#### Constraints:

```text
1 <= N <= 20
default = 2
```

#### Example:

```bash
pin.exe --border-width 5
```

---

### 2. Max Windows

```text
--max-windows N
```

#### Meaning:

Maximum number of simultaneously pinned windows allowed.

#### Constraints:

```text
1 <= N <= 10
default = 1
```

#### Behavior:

If limit reached:

```text
- ignore new pin request
- keep existing pinned windows unchanged
```

---

## 3.4 ARG PARSING RULES

Use deterministic parsing:

```c
for each argv:
    if "--border-width":
        parse next int
    if "--max-windows":
        parse next int
```

Invalid cases:

```text
missing value → ignore flag
non-integer → ignore flag
out of range → clamp or ignore
```

---

## 3.5 INTERNAL CONFIG STRUCT

```c
typedef struct {
    int borderWidth;
    int maxWindows;
} Config;
```

Defaults:

```c
borderWidth = 2;
maxWindows = 1;
```

---

# 4. GLOBAL DESIGN MODEL

## 4.1 Execution Model

```text
AHK hotkey → exe → toggle → exit OR stay active
```

No daemon processes.

---

## 4.2 STATE MODEL

### Pin2Top

```c
struct State {
    HWND target;
    bool isPinned;
    RECT lastRect;
    Config config;
}
```

### CursorFocus

```c
bool isActive;
```

---

# 5. IPC CONTRACT (STRICT)

## 5.1 Named Objects

### Pin2Top

```text
Mutex: Global\TinyPin2TopMutex
Event: Global\TinyPin2TopEvent
```

### CursorFocus

```text
Mutex: Global\TinyCursorFocusMutex
Event: Global\TinyCursorFocusEvent
```

---

## 5.2 IPC RULES

```text
- Mutex exists → send event → exit
- Mutex not exists → create instance
- Event = TOGGLE only
```

---

# 6. PIN2TOP — SPECIFICATION

---

## 6.1 Behavior Summary

```text
Win + Ctrl + T → toggle pin for active window
```

---

## 6.2 FIRST LAUNCH FLOW

```text
pin.exe [args]
    ↓
parse CLI args → Config
    ↓
GetForegroundWindow()
    ↓
CreateMutex (success → new instance)
    ↓
pin window
    ↓
create overlay (borderWidth from config)
    ↓
enter loop
```

---

## 6.3 SECOND LAUNCH FLOW

```text
pin.exe [args]
    ↓
Mutex exists
    ↓
SendEvent(TOGGLE)
    ↓
exit immediately
```

---

## 6.4 TOGGLE LOGIC (RUNNING PROCESS)

```text
on EVENT:
    if isPinned == false:
        if pinnedWindowCount < maxWindows:
            pin window
            create overlay (borderWidth)
            isPinned = true
        else:
            ignore request
    else:
        unpin window
        destroy overlay
        exit process
```

---

## 6.5 WINDOW PIN RULES

```c
SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0,
             SWP_NOMOVE | SWP_NOSIZE);
```

Unpin:

```c
SetWindowPos(hwnd, HWND_NOTOPMOST, 0,0,0,0,
             SWP_NOMOVE | SWP_NOSIZE);
```

---

## 6.6 OVERLAY RULES

Border width is dynamic:

```text
borderWidth = Config.borderWidth
```

Overlay must:

```text
- match window rect
- draw borderWidth px border
- remain click-through
```

---

## 6.7 TRACKING LOOP

```text
every 16ms:
    if !IsWindow(target):
        exit

    if IsIconic(target):
        hide overlay
        continue

    RECT r = GetWindowRect(target)

    if r changed:
        move overlay
```

---

# 7. CURSORFOCUS — SPECIFICATION

---

## 7.1 Behavior

```text
Win + Ctrl + C → toggle cursor highlight
```

---

## 7.2 FIRST LAUNCH

```text
create mutex
create overlay
loop
```

---

## 7.3 SECOND LAUNCH

```text
send event → exit
```

---

## 7.4 LOOP

```text
GetCursorPos()
move overlay center
```

---

# 8. OVERLAY SYSTEM (SHARED)

---

## 8.1 WINDOW STYLE

```c
WS_EX_LAYERED
WS_EX_TRANSPARENT
WS_EX_NOACTIVATE
WS_EX_TOOLWINDOW
```

---

## 8.2 RENDERING

GDI ONLY:

* Rectangle border (Pin2Top)
* Circle highlight (CursorFocus)

No DirectX.

---

## 8.3 UPDATE RULE

```text
max 60 FPS
only redraw on change
```

---

# 9. ERROR HANDLING

```text
- fail → exit cleanly
- no infinite loops
- no crash recovery systems
```

---

# 10. PERFORMANCE LIMITS

```text
CPU idle < 0.2%
Memory < 10MB per tool
Loop ≤ 60Hz
```

---

# 11. AUTOHOTKEY CONTRACT

```ahk
#^t::Run("pin.exe --border-width 3 --max-windows 2")
#^c::Run("cursor.exe")
```

---

# 12. FINAL STATE MACHINES

---

## 12.1 PIN2TOP FSM

```text
START
  ↓
parse args → Config
  ↓
GetForegroundWindow
  ↓
Mutex exists?
   ├── NO → CREATE + PIN + LOOP
   └── YES → SEND EVENT → EXIT
```

Inside process:

```text
EVENT:
   if pinned:
       UNPIN + EXIT
   else:
       if maxWindows not exceeded:
           PIN + SHOW OVERLAY
       else:
           IGNORE
```

---

## 12.2 CURSOR FSM

UNCHANGED

---

# 13. FINAL GUARANTEE

This version guarantees:

* deterministic CLI parsing
* clang-only build compatibility
* strict Win32-only implementation
* no undefined IPC behavior
* no hidden state
* predictable overlay behavior
* safe multi-window limit enforcement

---

# 20. GLOBAL-CMD SPECIFICATION

* **Type:** Tiny native CLI executable (`global-cmd.exe`).
* **Input:** `--dest <folder-path>` (also `--dist`, `--destination`, `-d`) and `--config <config.json>` (also `-c`).
* **Behavior:**
  - Validates and creates destination directory if missing.
  - Ensures destination directory is present in User PATH (`HKCU\Environment`, `Path`), broadcasting `WM_SETTINGCHANGE`.
  - Loads and parses JSON configuration with regex keys and command templates (`cmd` and `ps1`).
  - Expands regex capture groups `(group1|group2)` into candidate alias names (`$1`, `$2`, etc.).
  - Resolves wildcard `*` components in target paths against disk directories (e.g. `v16*` -> `v16.20.2`).
  - Probes target executable files on disk and automatically corrects extensions (e.g. `.cmd` -> `.exe` when target is a native binary).
  - Generates `<name>.cmd` and `<name>.ps1` alias wrappers directly in `--dest`.
  - Prints compact CLI output with `✓` and `x` indicators.

---

# END OF PRD