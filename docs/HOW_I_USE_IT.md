# ⚡ How I Use Tiny Utilities: Daily Driver Workflow

This document explains the real-world workflow behind **Tiny Windows Productivity Utilities**—how they are combined with AutoHotkey to create a fast, keyboard-first, zero-bloat Windows environment.

---

## 🎯 The Philosophy: Why Not PowerToys or Electron Tools?

Modern desktop utilities are often bloated:
- Background tray daemons running constantly and consuming **500 MB – 1.5 GB of RAM**.
- Heavy web runtimes (Electron / Chromium) for simple tasks like color picking or window switching.
- Sluggish startup times and telemetry hooks.

**The Tiny Utilities approach**:
- **0 MB RAM at idle**: No background daemons or resident background services.
- **Pure Win32 C & GDI/DWM**: Instant hardware-accelerated responsiveness (< 1 ms launch).
- **Single-Instance IPC Toggle**: Tools launch on hotkey, perform their job, and exit immediately.

---

## ⌨️ The Hotkey Map (Powered by `shortcuts.ahk`)

All utilities are wired together via the included [`docs/shortcuts.ahk`](shortcuts.ahk) (AutoHotkey v2) script. Here is the daily workflow:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        DAILY WORKFLOW HOTKEYS                          │
├───────────────────────┬───────────────────┬────────────────────────────┤
│ Shortcut              │ Tool Triggered    │ Workflow Action            │
├───────────────────────┼───────────────────┼────────────────────────────┤
│ Alt + Tab             │ window-switcher   │ Instant live Alt+Tab with  │
│                       │                   │ thumbnail hint jumping     │
│ Double Tap Ctrl       │ mouse-spotlight   │ Spotlight cursor & dim     │
│                       │                   │ background (find + meet)   │
│ Win + Ctrl + T        │ pin-to-top        │ Pin/unpin active window    │
│ Win + Ctrl + O        │ capture + ocr     │ Snip screen -> Auto OCR -> │
│                       │                   │ Copied to clipboard        │
│ Win + Ctrl + C        │ color-picker      │ Magnify pixel & copy color │
│ PrintScreen           │ capture           │ Fullscreen capture & draw  │
│ Alt + PrintScreen     │ capture           │ Active window snip & draw  │
│ Ctrl + PrintScreen    │ capture           │ Region snip & draw         │
│ Alt + I               │ ip-send           │ LAN messenger *(needs IPMsg)*│
│ F12                   │ (Built-in AHK)    │ Hide / Show Desktop Icons  │
│ Alt + J / Alt + L     │ (Built-in AHK)    │ Home / End navigation      │
└───────────────────────┴───────────────────┴────────────────────────────┘
```

---

## 🛠️ Deep Dive: The Workflows

### 1. Window Switching (`Alt + Tab`)
- **How I use it**: Bind `Alt + Tab` to `window-switcher.exe`.
- **Why**: The native Windows switcher works fine, but when you have 10–20 windows open, finding the one you want requires visually hunting across a crowded grid and tabbing through cards one by one. `window-switcher` solves that search friction with **live thumbnail hint badges** (e.g. `C` for Chrome, `CO` for VS Code, `F` for Figma). You don't scan or cycle—you just tap the hint key on your keyboard and jump directly to that window.

### 2. Research & Multitasking (`Win + Ctrl + T`)
- **How I use it**: Pin reference materials, terminals, calculators, or video streams while coding.
- **Why**: Automatically draws a clean accent-colored border around the pinned window. Double-tapping the hotkey launches an interactive window picker. Toggling it off removes the pin and exits cleanly.

### 3. Screen Snip $\rightarrow$ Instant OCR (`Win + Ctrl + O`)
- **The Magic Combo**: Pressing `Win + Ctrl + O` lets you drag a snip rectangle over any error message, video frame, or image on screen.
- **What happens**: The script sends the snip directly to `ocr.exe`, parses the text via Tesseract, and copies the clean text to your clipboard in under a second. No typing manual text from screenshots.

### 4. Cursor Finding & Meeting Spotlight (`Double-Tap Ctrl`)
- **How I use it**: Double-tap `Ctrl` to trigger `mouse-spotlight.exe`.
- **Why I switched from `find-my-mouse`**: I originally used `find-my-mouse`, but switched to `mouse-spotlight` because it does **double duty**:
  1. **Cursor Locator**: It instantly shows where the pointer is on large or multi-monitor 4K setups.
  2. **Presentation Focus**: During screen-shares, code reviews, and meetings, it dims the entire desktop and draws audience focus directly to what I'm explaining. Pressing `Esc` or tapping `Ctrl` dismisses it immediately.

### 5. Frontend & UI Design (`Win + Ctrl + C`)
- **How I use it**: Inspect colors on webpages, Figma designs, or desktop apps.
- **Why**: Displays a real-time pixel magnifier card with `HEX`, `RGB`, `HSL`, and `CMYK` values. Pressing `1` copies the Hex code and exits immediately.

### 6. Screenshots & Quick Annotations (`PrintScreen` / `Ctrl+PrintScreen`)
- **How I use it**: Take screenshots with instant doodle / annotation capabilities.
- **Why**: Saves directly to `Pictures/Screenshots` with an optional floating toolbar for drawing arrows, boxes, and text notes.

### 7. Instant LAN Messaging (`Alt + I`)
- **How I use it**: Send files, code snippets, or notes to colleagues across the local network without opening browser tabs or heavy chat apps.
- **Prerequisite & Disclaimer**: Requires [IP Messenger for Windows](https://ipmsg.org/en/) installed on your PC. `ip-send` acts as a fast, keyboard-driven frameless TUI composer that communicates through IP Messenger's backend daemon. *(Note: This project is an independent open-source frontend and is not affiliated with, sponsored, or endorsed by the original authors of IP Messenger).*

---

## 🚀 How to Set It Up in 30 Seconds

1. **Download the Release**: Download `tiny-utilities-x64.zip` (or `arm64`) from the [Latest Release](https://github.com/theboyofdream/tiny-utilities/releases/latest).
2. **Extract**: Unzip into any folder (e.g. `C:\Tools\tiny-utilities`).
3. **Install AutoHotkey v2**: Install via `winget install AutoHotkey.AutoHotkey` (or from [autohotkey.com](https://www.autohotkey.com/)).
4. **Run `shortcuts.ahk`**: Double-click `shortcuts.ahk` located in the same folder.
5. **(Optional) Run on Startup**: Press `Win + R`, type `shell:startup`, and place a shortcut to `shortcuts.ahk` there.
