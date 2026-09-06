# Context Menu Utility Specification (context-menu.c)

## Overview

`context-menu` is a native Win32/C Windows Explorer context menu manager. It allows users to define, update, and backup per-user Explorer context menu entries using simple XML configuration files. It supports unlimited nested submenus, action commands, arguments with Windows placeholders (`%1`, `%V`), custom icons, complete pre-flight XML validation, strict isolation of managed entries, and idempotent registry synchronization.

---

## 1. CLI Arguments & Workflows

```text
context-menu.exe [--update <config.xml> | -u <config.xml>]
                 [--backup <backup.xml> | -b <backup.xml>]
```

### Modes of Operation

1. **`context-menu.exe --update <config.xml>` (or `-u <config.xml>`)**
   - Validates XML file completely before making any registry modifications.
   - Applies the context menu definitions immediately into `HKCU\Software\Classes`.
   - On error: outputs message to `stderr` and exits with exit code `1` without modifying the Registry.
   - On success: exits silently with exit code `0` (no success output or confirmation dialog).

2. **`context-menu.exe --backup <backup.xml>` (or `-b <backup.xml>`)**
   - Reads all context menu entries managed by `context-menu` from `HKCU\Software\Classes`.
   - Exports the managed entries into a clean XML document at the specified destination file.
   - On success: exits silently with exit code `0` (no success output or confirmation dialog).

3. **Double-Click (Launched without CLI arguments)**
   - Displays a native Windows choice dialog with three options:
     - **Update Context Menu**
     - **Backup Context Menu**
     - **Cancel**
   - **Update GUI Flow**: Opens native `GetOpenFileNameW` dialog → validates selected `.xml` file → applies updates to Registry → exits silently.
   - **Backup GUI Flow**: Opens native `GetSaveFileNameW` dialog → reads managed registry entries → exports backup XML file → exits silently.
   - **No Confirmation Dialogs**: Actions execute immediately once a file is selected and exit silently on success. Errors display a native `MessageBoxW` error dialog.

---

## 2. XML Configuration Format & Schema

The configuration file uses a structured XML format supporting target scopes, single action items, and unlimited nested submenus.

### XML Schema Example

```xml
<?xml version="1.0" encoding="UTF-8"?>
<context-menu>
    <!-- Target scope for all files -->
    <target name="*">
        <item title="Edit with Notepad" icon="notepad.exe">
            <command>notepad.exe</command>
            <arguments>"%1"</arguments>
        </item>
    </target>

    <!-- Target scope for directories -->
    <target name="Directory">
        <item title="Developer Tools" icon="cmd.exe">
            <item title="Open Command Prompt Here">
                <command>cmd.exe</command>
                <arguments>/k cd /d "%V"</arguments>
            </item>
            <item title="Open PowerShell Here" icon="powershell.exe">
                <command>powershell.exe</command>
                <arguments>-NoExit -Command Set-Location -LiteralPath '%V'</arguments>
            </item>
        </item>
    </target>

    <!-- Target scope for directory background -->
    <target name="Directory\Background">
        <item title="Open Terminal" icon="wt.exe">
            <command>wt.exe</command>
            <arguments>-d "%V"</arguments>
        </item>
    </target>
</context-menu>
```

### Supported Elements & Attributes

- **`<context-menu>`**: Root element.
- **`<target name="...">`**: Target file type or shell location (e.g. `*`, `Directory`, `Directory\Background`, `Drive`, `.txt`).
- **`<item>`**: Context menu entry.
  - Attributes or child tags:
    - `title` / `<title>`: Display text for the menu item.
    - `icon` / `<icon>`: Optional icon path (e.g. `C:\Tools\app.ico`, `cmd.exe`).
    - `command` / `<command>`: Executable path or command to run.
    - `arguments` / `<arguments>`: Arguments for the command. Supports Windows shell placeholders such as `%1` (selected file path) and `%V` (directory path).
  - **Nested Menus**: An `<item>` containing child `<item>` elements creates a cascading submenu (unlimited nesting supported).

---

## 3. Registry Architecture & Isolation

- **Registry Location**: Per-user registry hive `HKCU\Software\Classes`.
- **Top-Level Keys**: Created under `HKCU\Software\Classes\<target>\shell\<KeyName>`.
- **Managed Tag (`ManagedBy`)**: Every top-level entry created by this utility includes `ManagedBy = "context-menu"` (REG_SZ).
- **Isolation**: Non-managed context menu keys in `HKCU\Software\Classes` (created by Windows or other software) are ignored and left untouched.
- **Idempotency**: Running `--update` multiple times with the same XML configuration results in the exact same registry state.
- **Cascading Submenus**: Implemented via Windows native `SubCommands = ""` and nested `shell` subkeys.
- **Shell Notification**: Calls `SHChangeNotify(SHCNE_ASSOCCHANGED, ...)` after updates to refresh Explorer immediately.

---

## 4. Safety & Validation Rules

- **Pre-Flight Validation**: The XML file is completely loaded, parsed, and validated prior to performing any registry modifications.
- **Validation Checks**:
  - Valid XML syntax and tag matching.
  - Non-empty target names.
  - Non-empty titles for all items.
  - Action items (leaf items) must specify a non-empty `command`.
  - Submenu items (container items) must have valid child items.
- **Fail-Safe Exit**: If validation fails, an error message is generated, the utility exits cleanly, and zero registry modifications are made.

---

## 5. IPC Contract & Single Instance

- **Mutex**: `Global\TinyContextMenuMutex`
- **Event**: `Global\TinyContextMenuEvent`
- Second concurrent launch detects mutex, signals event, and exits immediately.

---

## 6. Build Instructions

```powershell
pwsh -File .\build.ps1 context-menu
```

Output binaries:
- `dist/release/context-menu.exe`
- `dist/debug/context-menu.exe`
