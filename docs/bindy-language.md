# Bindy Language Reference (`.bindy`)

> [!NOTE]
> This is the **language spec** recorded from the user's brief. The executable that
> implements it is [`bindy`](tools/bindy.md); this document owns the grammar, the tool
> doc owns the binary's CLI/verification. Each fact lives in exactly one place.

Bindy is a clean, readable keymapper and hotkey scripting language for Windows. A script
is a plain text `.bindy` file (UTF-8, with or without BOM; UTF-16LE also accepted).

---

## 1. Core Rules

* **Script length directive:** by default a single statement (e.g. a `cmd:`/`ps1:` block)
  holds up to **1024 characters**. A script can raise this with a top-level directive:
  `max_script_chars 8192` (or `max_script_chars = 8192`). The value is clamped
  to 256–8192. This is a line inside the script, not a config file.
* **Single-line rule (`=`):** variable setup, simple key remaps, and one-line actions.
* **Block rule (`:` ... `:end`):** every `:` opens a block, and every block closes with its own `:end`.
* **Comments:** `//` for single-line comments or `/* ... */` for block comments.
* **Variables:** reference Bindy vars/consts with `$name` (and env vars like `%USERPROFILE%` when expanded). Don't give a Bindy var/const the same name as a PowerShell `$var` inside the same block so the parser doesn't mix them up.

---

## 2. Variables & Constants

* `const` sets a fixed value that shouldn't change later.
* `var` sets a value that can change or get updated.

```text
const shots = %USERPROFILE%\Pictures\Screenshots
var mode = "snip"
```

Quoted string values are stored without their quotes (`"snip"` → `snip`). `%NAME%` is
resolved from the environment when the value is assigned.

---

## 3. Lifecycle & Hotkeys

### Startup Block

Runs once when the script starts.

```text
on_start:
    cmd:
        if not exist "$shots" mkdir "$shots"
    :end
:end
```

### Hotkeys & Key Remaps

Combine modifiers with `+` (`alt`, `ctrl`, `shift`, `win`, `lctrl`, `rctrl`, etc.).

```text
alt+j = home
alt+l = end
alt+q = alt+f4
alt+s = f11
rctrl+rctrl (200ms) = apps
```

### Tap Timing `(ms)`

The time window (in milliseconds) for fast repeated taps like double-tapping a key
(`300ms` = 0.3s). A combo whose two sides are the same key registers as a double-tap of
that key.

```text
lctrl+lctrl (300ms) = cmd .\mouse-spotlight.exe -s 90 -d 81
```

> By default the triggering keystroke is **suppressed** (AHK-like). Prefix a combo with
> `~` to make it pass through — e.g. `~lctrl+lctrl (300ms)`. A single-key `(Nms)` combo
> means a **short press** (fires on key-up when held ≤ Nms); two identical keys with
> `(Nms)` mean a **double-tap** on the second key-up, as in `~lctrl+lctrl (300ms)`.
> A bare modifier key can itself be a hotkey (`rctrl = apps`) — its own down-state is
> not treated as an extra modifier.
>
> **Reserved combos:** Bindy uses a low-level keyboard hook (PRD §21), so combos the OS
> owns — `alt+tab` (task switcher), `win+ctrl+c/o`, bare `F12`, etc. — work normally:
> the matching keydown is swallowed before Windows can act on it. A `RegisterHotKey`
> build would not be able to do this.

---

## 4. Running Commands (`cmd` & `ps1`)

* `cmd` runs a standard command or `.exe`.
* `ps1` runs PowerShell when you need PS features, pipelines, or cleaner scripting.

### Inline (Single-Line)

```text
win+ctrl+t = cmd .\pin-to-top.exe --border-width 2
win+ctrl+c = cmd .\color-picker.exe
alt+i = cmd .\ip-send.exe
alt+tab = cmd .\window-switcher.exe -bg blur --blur 30 -l center -s recent -a -sw 0
```

### Multi-Line (`cmd:` / `ps1:` ... `:end`)

```text
win+ctrl+o:
ps1:
    $dir = "$shots"
    $before = @{}
    Get-ChildItem -Path $dir -File -ErrorAction SilentlyContinue | ForEach-Object { $before[$_.FullName] = $true }

    .\capture.exe --snip --save "$dir"

    $img = $null
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 100
        $found = Get-ChildItem -Path $dir -File -ErrorAction SilentlyContinue | Where-Object { -not $before.ContainsKey($_.FullName) } | Select-Object -First 1
        if ($found) { $img = $found.FullName; break }
    }

    if ($img) {
        $txt = (.\ocr.exe --image "$img" | Out-String).Trim()
        if ($txt) { Set-Clipboard -Value $txt }
    }
:end
:end
```

> Only **stdout** is captured for variable assignment; stderr is drained and discarded
> (Windows PowerShell 5.1 emits a CLIXML progress record on stderr on first use, which
> would otherwise pollute captured output).

---

## 5. Functions (`fn`)

Define small reusable helpers with parameters, closed with `:end`.

```text
fn snap(mode):
    cmd .\capture.exe --$mode --draw-toolbar --save "$shots"
:end

printscreen = snap(fullscreen)
alt+printscreen = snap(window)
ctrl+printscreen = snap(snip)
```

Arguments are bound positionally to parameter names and are available as `$param`
inside the function body. Globals (e.g. `$shots`) remain visible; assigning to a
parameter name updates the local binding.

---

## 6. Conditions (`match:`)

Checks conditions top-to-bottom (like `if` / `elseif` / `else`) across any variables,
with `default:` as the fallback. A `match:` at the **top level** is an init statement
and is evaluated once at startup — put side-effecting arms under a hotkey block if you
want them on a keypress.

* Each `when` arm or `default:` auto-closes when the next arm starts or when `match:` ends with `:end`.
* Use `=` on a `when` line for a quick single-line action.
* Use `:` for multi-line arms, and finish the whole block with a single `:end`.

```text
match:
    when battery <= 20 = cmd .\low-power-mode.exe

    when mode == "snip":
        cmd .\capture.exe --snip --save "$shots"
        cmd .\notify.exe "snipped"

    default:
        cmd .\notify.exe "fallback"
:end
```

**Comparison operators:** `==`, `!=`, `<`, `<=`, `>`, `>=` (and `=` as equality).
Operands may be string literals, numbers, or variables — a bare identifier is resolved
as a variable name (as in `when mode == "snip"`). If both operands are numeric, the
comparison is numeric; otherwise it is a case-insensitive string comparison. An arm
with no operator is true when its value is non-empty and not `0`/`false`/`no`/`off`.

> The `battery` name in the example above is **illustrative** (it was part of the
> original brief, not a declared variable). Bindy does not define built-in variables;
> declare it yourself or use a different condition in a real script. Undefined
> variables resolve to the empty string, so the example is harmless but would take the
> `default:` arm.

---

## 7. Getting Output Back from `cmd` & `ps1`

Assign the command to a variable so you can use what it prints right after:

```text
var out = cmd .\ocr.exe --image "shot.png"
var clean_out = ps1 .\ocr.exe --image "shot.png"

# multi-line ps1 returning a value
var active_win = ps1:
    Add-Type @"
    using System;
    using System.Runtime.InteropServices;
    public class Win {
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    }
"@
    $h = [Win]::GetForegroundWindow()
    $sb = New-Object System.Text.StringBuilder 256
    [Win]::GetWindowText($h, $sb, 256) | Out-Null
    $sb.ToString()
:end
```

A multi-line assignment uses `cmd:` / `ps1:` after the `=`, with its own `:end`, then
the enclosing block's `:end`.

---

## 8. Supported Key Names

Modifiers: `alt`, `ctrl`, `shift`, `win`, `lctrl`, `rctrl`, `lalt`, `ralt`, `lshift`,
`rshift`, `lwin`, `rwin`. Keys: `home`, `end`, `apps`, `printscreen`/`prtsc`, `tab`,
`esc`/`escape`, `enter`/`return`, `space`, `backspace`, `delete`/`del`, `insert`/`ins`,
`pgup`, `pgdn`, arrow keys, `f1`–`f24`, single letters, digits and OEM punctuation.

---

## 9. Limits

Fixed-size buffers keep the runtime small and deterministic: 256 variables, 256 hotkeys,
64 functions, 1024 statements, 1 KiB per command/value, 1 MiB script. Exceeding a limit
produces a warning and the line is skipped; parsing never crashes.
