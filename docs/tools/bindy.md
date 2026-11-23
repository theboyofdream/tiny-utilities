# Bindy Tool Specification (bindy.c)

## Overview

`bindy` is the Windows runtime for the [`.bindy` hotkey scripting language](../bindy-language.md).
It parses a script, runs its `on_start` block, registers the declared global hotkeys,
and stays resident on a thread message loop until a second launch toggles it off or
Ctrl+C is pressed. Remaps fire a key sequence with `SendInput` (hotkey-style — the
triggering key is not suppressed).

> [!NOTE]
> **Status: New / Experimental.** The language engine is implemented and smoke-tested;
> interactive hotkey behaviour should be verified on the target desktop.

---

## 1. CLI Arguments

```text
bindy <script.bindy> [--check | --run-once]
bindy --help | -h | /?
```

| Flag | Effect |
|---|---|
| *(no flag)* | Parse script, run `on_start`, register hotkeys, stay resident. |
| `--check` | Parse only: report counts **and list every hotkey** (marking reserved ones), exit 0. No IPC, no hotkeys. |
| `--run-once` | Run top-level assignments + `on_start` once and exit (diagnostic). No IPC, no hotkeys. |
| `--help` / `-h` / `/?` | Show declarative help (shared `tiny_cli.h`). |

**Script directive:** `max_script_chars N` (or `max_script_chars = N`) at the top level
raises the per-statement text limit from the default 1024 to N (clamped 256–8192).
`shortcuts.bindy` uses `max_script_chars 8192` for its long desktop-icon PowerShell block.

A missing/unreadable script, an unknown flag, or a parse error exits nonzero; there is
no default script path — pass one explicitly. `shortcuts.ahk` is left untouched.

---

## 2. Runtime Behaviour

1. **Load** the script (UTF-8 with optional BOM, or UTF-16LE); strip `//` and `/* */`
   comments; split into lines.
2. **Parse** into a fixed-size statement program: top-level `var`/`const`/`match`
   (init), `on_start`, functions, and hotkeys with their action statements. An
   unterminated block is a clean parse error.
3. **Execute** init then `on_start`. Within `match:`, arms are checked top-to-bottom;
   the first true arm (or `default:`) runs. Nested arm statements belong to their
   owning `match` and are not run again by the enclosing block.
4. **Install** one `WH_KEYBOARD_LL` hook. Every keydown/keyup updates a 256-entry
   state map; a hotkey matches when its main VK is pressed and its modifier set matches
   **exactly** (left/right-specific modifiers supported; extra modifiers prevent a
   match, like AHK). A matching hotkey posts `WM_APP + id` to a hidden message-only
   window and, unless marked pass-through (`~`), returns `1` to swallow both keydown
   and keyup — so `alt+tab`, `win+ctrl+c/o` and bare `F12` never reach the OS. Actions
   run on the message loop, never inside the hook callback (SendInput is dropped there
   and a blocking cmd would freeze all input). This replaces the earlier `RegisterHotKey`
   engine (Windows cannot grab those combos, and never posts `WM_HOTKEY` for bare keys).
5. **Actions:** before running a hotkey's statements, held modifiers are briefly
   released with synthetic key-ups and restored afterwards (AHK-like), so an injected
   `home`/`end`/`alt+f4` is not interpreted as `alt+home` / `ctrl+f4`. Non-capturing
   `cmd`/`ps1` actions are launched fire-and-forget; only assignments (`var x = cmd ...`)
   wait for and capture stdout.
6. **Tap semantics:** `key (Nms)` = short press (fires on key-up when held ≤ Nms);
   `key+key (Nms)` with identical keys = double-tap on the second key-up. Both default
   to pass-through so normal modifier use is unaffected.
7. **Wait** with `MsgWaitForMultipleObjects` over `{toggle event, wake event}` plus
   thread messages; hook posts and LL callbacks are dispatched on that same loop.
8. **Exit**: second launch signals `Global\TinyBindyEvent` via
   `TinyIPC_AcquireOrToggle`; Ctrl+C/Close sets `g_running = false` and a wake event.
   The hook is unhooked, events closed, exit code 0.

---

## 3. Execution Details

* **Key send (`ST_KEYS`)**: tokens are mapped to VKs, then a `SendInput` DOWN sequence
  followed by reverse-order UP sequence. Extended keys (arrows, Win, right ctrl/alt,
  etc.) get `KEYEVENTF_EXTENDEDKEY`; scan codes come from `MapVirtualKeyW`.
* **Commands (`ST_CMD` / `ST_PS1`)**: text is `$var`/`%ENV%`-expanded, then run via
  `CreateProcessW`. `cmd` uses `cmd.exe /c <text>`; `ps1` uses
  `powershell.exe -NoProfile -NonInteractive -EncodedCommand <base64 UTF-16LE>`, which
  preserves quotes and newlines in multi-line scripts. stdout is captured (up to 8 KiB)
  and, when assigned (`var x = cmd ...`), trimmed and stored in the variable. stderr
  is drained to a separate pipe and discarded. A 60 s timeout terminates a hung child.
* **Expansion**: `$name` resolves from the current function scope first, then globals;
  unknown `$name` is left intact (so PowerShell variables pass through). `%NAME%`
  resolves via `GetEnvironmentVariableW`. Assignment strips surrounding quotes from
  string literals.
* **Functions**: arguments bind positionally to `$param`; up to 8 nested call frames.

---

## 4. Hard Rules & Carve-out

Everything in PRD §1 is honoured **except** the following, granted by the `bindy` row
of the PRD §21 registry:

> A long-running console process that stays resident until IPC toggle-off / Ctrl+C;
> a `WH_KEYBOARD_LL` low-level keyboard hook (installed with `GetModuleHandle(NULL)`,
> unhooked on every exit path) that matches combos with left/right modifier precision
> and suppresses keys the OS would otherwise handle; `SendInput` keystroke injection.
> No GUI windows, no services, no registry, no disk writes, no config files, no
> env-var persistence. The script is user-supplied input passed by path, not
> configuration the tool reads or writes on its own.

The hook runs with a small fixed-state keyboard map (256 keys) on the existing message
loop; no tray window, no network. `shortkey.bindy` is an exact combo-for-combo port of
the author's `shortcuts.ahk` (21 hotkeys), including `alt+tab`, `win+ctrl+c/o`, bare
`F12`, `~` pass-through, short-press and release-based double-tap semantics.

---

## 5. The configured script

[`docs/shortcuts.bindy`](../shortcuts.bindy) is the author's configured script — a port
of `shortcuts.ahk` with the OS-owned combos remapped to free ones, `rctrl = apps`, and a
longer desktop-icon PowerShell block. It declares `max_script_chars 8192` so that block
is not truncated. A ready-to-run copy lives beside the binaries as `shortcuts.bindy`;
`shortkey.bindy` is the exact, no-combo-changes variant that keeps `alt+tab`,
`win+ctrl+c/o` and bare `F12` (it requires the low-level-hook build).

AHK modifiers map as `!`→`alt`, `^`→`ctrl`, `+`→`shift`, `#`→`win`, `~`→`~`. Actions AHK
did with `DllCall`/`WinHide` become a `ps1:` block; `Run(...)` becomes `cmd ...`;
`~LCtrl Up` double-tap becomes `~lctrl+lctrl (300ms)`; `~RControl` short press becomes
`~rctrl (200ms)`.

---

## Verified

- [x] Builds with Clang — `pwsh -File .\build.ps1 [-Mode debug|release] bindy` (MinGW-Clang path; release binary produced).
- [x] `--help` / `-h` / `/?` exit 0 with declarative help; `--check` parses and reports counts without touching IPC/hotkeys.
- [x] `--run-once` executes init + `on_start`: `cmd` output capture, `ps1` (`Write-Output`) capture, quoted literal vars, function params, `match:` arms (string + numeric `==`, `>`, inline `when ... =`), and `default:` fallback.
- [x] Comments (`//`, `/* */`) stripped; unterminated block → clean error + exit 1.
- [x] Double-launch toggle: first instance resident, second launch signals `Global\TinyBindyEvent`, first exits 0 (verified with `Start-Process`, no process left).
- [x] IPC names `Global\TinyBindyMutex` / `Global\TinyBindyEvent`; the hook is unhooked on exit.
- [x] Live `WH_KEYBOARD_LL` + `SendInput` on an interactive desktop: explicit key events for `alt+tab`, `win+ctrl+c`, `win+ctrl+o` and bare `F12` each ran their marker-writing action with empty stderr — combos that `RegisterHotKey` could never register. A chained-hotkey test proved synthesized `SendInput` combos fire other hotkeys.
- [x] `shortkey.bindy`: 21 exact AHK combos parse; `--check` marks `~pass-through`, `short-press` and `double-tap` semantics; live run installs the hook with no errors and toggles off cleanly.
- [x] Exact modifier matching verified with explicit key events: all 12 alt-based combos fired their own marker (`alt+j/l`, `alt+shift+j/l`, `ctrl+alt+j/l`, `ctrl+alt+shift+j/l`, `alt+q/s/i/printscreen`) with empty stderr — previously extra modifiers caused the less-specific `alt+j/l` to match first.
- [x] Tap semantics: `~rctrl (200ms)` fired on a short (60 ms) right-Ctrl press; `~lctrl+lctrl (300ms)` fired on two left-Ctrl presses 90 ms apart.
- [x] Injecting keys from the hook callback was dropped by Windows; actions now run via a hidden message window. Verified with a chained-hotkey test and with all 12 alt marker combos.
- [x] Extended-key flags corrected (generic `alt` and left-Ctrl are not extended); modifiers are released around injected keys so `alt+j`/`alt+l` produce real Home/End rather than `alt+home` or a stray bullet character.
- [x] Non-capturing actions are fire-and-forget, so launching a resident tool such as `pin-to-top.exe` no longer blocks the hook for 60 s and each press toggles it.
- [x] Modifier used by another hotkey is marked consumed so its key-up never counts as a tap; synthetic modifier release/restore events are ignored by the hook. Verified: repeated `win+ctrl+o` (150 ms apart) produced only OCR actions, while two real left-Ctrl taps still fired the double-tap — no more intermittent `lctrl+lctrl` during OCR.
- [x] Bare-modifier hotkeys (`rctrl = apps`): matched on the key itself; key-ups are passed through so Windows clears its modifier state, and the state is kept cleared while the hotkey is held (no more Ctrl+wheel browser zoom). Verified `GetAsyncKeyState(VK_CONTROL)` reads not-down while Right-Ctrl is held and after release, and the hotkey still fires.
- [x] Both shipped scripts bind `f12` **and** `ctrl+f12` to a shared `toggle_icons()` function (the exact PowerShell block from `shortcuts.ahk`). Verified both forms invoke the function; the extracted block runs via `-EncodedCommand` with exit 0.
- [ ] CPU idle % / memory measured (design: event-wait sleeps, fixed buffers ≈ 6 MiB worst case).

**Last verified:** 2026-11-23.
