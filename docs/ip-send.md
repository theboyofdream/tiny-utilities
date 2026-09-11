# Send via IP Tool Specification (ip-send.c)

## Overview

`ip-send` is a fast, terminal/TUI-inspired payload composer and dispatcher for IP Messenger (`ipcmd.exe`). It eliminates content-selection overhead by launching directly with captured files or text, active recipient search by default, and a hidden message field that opens on demand.

---

## 1. CLI Arguments & Context Menu Registration

```text
ip-send.exe [--context-menu register|unregister|true|false]
            [--to <recipients>]
            [--message <text> | --msg <text> | -m <text>]
            [--file <path>]
            [--theme light|dark|system]
            [--send]
            [<file1> <file2> ...]
```

### Context Menu Integration
- **`--context-menu register` / `--context-menu true`** (or `--register-context-menu` / `--register`): Adds Explorer context menu items under:
  - `HKCU\Software\Classes\*\shell\Send via IP-send`
  - `HKCU\Software\Classes\Directory\shell\Send via IP-send`
  - `HKCU\Software\Classes\Directory\Background\shell\Send via IP-send`
  Purges all legacy entries (`Send via IP`, `IP Send`, `Send via IPMsg`) cleanly without administrator rights.
- **`--context-menu unregister` / `--context-menu false`** (or `--unregister-context-menu` / `--unregister`): Removes Explorer context menu registry entries.

### Content & Automation
- **`--theme light|dark|system`**: Selects theme mode. Default is `system` (follows Windows App Light/Dark mode). Can also be toggled dynamically at runtime with `Ctrl + D`.
- **`--to <recipients>`**: Pre-selects comma-separated recipient PC/user names (e.g. `--to PC-01,LAPTOP-03`).
- **`--message <text>` / `--msg <text>` / `-m <text>`**: Pre-populates the message body.
- **`--file <path>` / positional arguments**: Pre-populates selected files or text payloads.
- **`--send`**: Bypasses composer GUI and immediately dispatches via backend `ipcmd.exe send` when required parameters are provided.

---

## 2. Composer UI & TUI Feel

- **Window**: Native Win32 frameless dark TUI window with 2px emerald accent border (`#00C896`). Runs as a normal frameless window (no `WS_EX_TOPMOST`), handling `WM_NCACTIVATE` and `WM_NCPAINT` to prevent Windows from drawing a white frame on focus loss, and automatically sizes its height (`SetWindowPos`) to fit visible contents cleanly without empty space below the `[SEND]` button.
- **Layout Structure**:
  1. **Title Bar**: `Send` header rendered inside top-left frame border in dim text.
  2. **Top Container Section**: Displays attached files/snippets or message input line with a minimum container height (`44px`).
  3. **Message Input Section**: Hidden by default when empty; opens in edit mode on `Insert` key (`> Fixed this issue._`). Automatically wraps long text multiline (`DrawTextW` with `DT_WORDBREAK`) and resizes window height dynamically (`SetWindowPos`).
  4. **Recipient Search & Checklist**:
     - Search field active by default (`/ pc_`).
     - Real-time filtered recipient list formatted cleanly with cursor (`>`) and checkmark (`✓`).
  5. **Footer & Action Bar**:
     - `To: PC-01, LAPTOP-03` summary line (displays `To: none` in dim text when no recipients are selected).
     - `[SEND]` action button (dimmed when no recipients are selected; attempting to send with 0 recipients displays warning: `Warning: No recipients selected. Press Space to select.`).
  6. **Clean UI & Dynamic Hints**: Permanent legend bar removed for ultra-clean UI; pressing `/` key when not in message mode toggles a temporary yellow alert banner with all shortcut hints.

---

## 3. Input & Content Methods

- **`Ctrl + O`**: File Open dialog (`GetOpenFileNameW` with multi-select support).
- **Drag & Drop**: Native `WM_DROPFILES` integration for dropping files directly onto composer window.
- **Paste (`Ctrl + V`)**: Pastes copied files (`CF_HDROP`) or text snippets (`CF_UNICODETEXT`).

---

## 4. Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `↑` / `↓` | Navigate recipient search results |
| `Space` | Toggle `[x]` / `[ ]` selection for highlighted recipient |
| `Enter` | Trigger Send action (`ExecuteSend`) |
| `Ctrl + O` | Open File selection dialog |
| `Ctrl + V` | Paste copied files or text (or paste into edit box when editing) |
| `Insert` | Enter / Exit message edit mode (focuses normal native multiline text box) |
| `r` | Refresh online recipient list via `ipcmd.exe list /all` (preserves selected checks and cursor position) |
| `/` | Show clean shortcut hints popup dialog without icon (when not in message mode) |
| `Esc` | Exit message edit mode (or close composer window) |

---

## 5. Backend Execution, Detection & IPC

- **Installation Detection (`FindIPMsgExecutable`)**: Automatically searches for `ipcmd.exe` or `ipmsg.exe` across:
  1. System `PATH` environment variable via `SearchPathW`.
  2. Windows Registry App Paths (`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\ipcmd.exe` & `ipmsg.exe`).
  3. Standard installation directories (`%ProgramFiles%\IPMsg`, `%ProgramFiles(x86)%\IPMsg`, and `%LocalAppData%\IPMsg`).
- **Error Handling**: If IP Messenger is not detected on the system when attempting to send, a dialog and TUI status error are thrown:
  `For this utility to work, IP Messenger (ipcmd.exe / ipmsg.exe) needs to be installed on your system.`
- **Backend Dispatch**: Calls `<detected_ipcmd_path> send /to:<recipients> [/msg:"<message>"] "<file1>" "<file2>" ...`.
- **IPC Contract**: `Global\TinyIPSendMutex` and `Global\TinyIPSendEvent`. Double-launch signals running instance and exits cleanly.

---

## 6. Build Instructions

```powershell
pwsh -File .\build.ps1 ip-send
```

Output executables:
- `dist/release/ip-send.exe`
- `dist/debug/ip-send.exe`
