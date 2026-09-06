# cmdx — Specification & Manual

* **Platform:** Windows
* **Type:** PowerShell Script + Fast Dual-Shim Generator (`.cmd` & `.ps1`)
* **Source:** `src/cmdx.ps1`
* **Shims Directory:** `cmds/` (Registered in User PATH)

---

## 1. Specification & Responsibilities

1. **User Environment PATH Registration:**
   - Registers the dedicated `cmds/` subfolder in the User PATH environment variable (`HKCU\Environment`, `Path`).
   - Automatically broadcasts `WM_SETTINGCHANGE` across Windows and refreshes the current session's `$env:Path`.
   - Never modifies System PATH.

2. **Regex-Keyed Command Definitions:**
   - Keys with regex pattern alternatives (e.g. `'^(node|npm|npx|corepack)(16|22|24)$'`) are dynamically expanded into all candidate alias names (`node16`, `node22`, ..., `corepack24`).
   - Capture groups are passed as local scope variables (`$1`, `$2`, ...) to the target ScriptBlock.
   - Arguments are forwarded seamlessly via `@args`.

3. **Dual-Shim Generation:**
   - Generates `<alias>.cmd` for Command Prompt and native CMD execution.
   - Generates `<alias>.ps1` for direct in-session execution in PowerShell.
   - Generates `cmdx.cmd` and `cmdx.ps1` root CLI wrappers in `cmds/`.
   - Native in-place execution for session-modifying tools like `refreshenv`.

4. **Instant Dynamic Dispatching:**
   - Editing `src/cmdx.ps1` immediately modifies command behaviors on the next execution without recompilation or shim regeneration.

---

## 2. CLI Usage & Commands

```powershell
# Display help and list all registered commands and aliases
pwsh -File .\src\cmdx.ps1
pwsh -File .\src\cmdx.ps1 help
cmdx help

# Register cmds/ in User PATH, generate/update all shims, and refresh environment
pwsh -File .\src\cmdx.ps1 setup
cmdx setup

# Clean/remove all generated shims and unregister cmds/ from User PATH
pwsh -File .\src\cmdx.ps1 clean
cmdx clean

# Run any registered alias from anywhere
node16 -v
npm22 install
ll
ls
qwen "Hello"
zw
refreshenv
```
