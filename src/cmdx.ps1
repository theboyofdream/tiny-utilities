$Command = if ($args.Count -ge 1) { $args[0] } else { $null }
$CommandArgs = if ($args.Count -gt 1) { $args[1..($args.Count - 1)] } else { @() }

# Broadcast WM_SETTINGCHANGE to notify Windows of environment variable changes
function Broadcast-EnvironmentChange {
    try {
        if (-not ([System.Management.Automation.PSTypeName]'CmdxNative').Type) {
            Add-Type -TypeDefinition @"
            using System;
            using System.Runtime.InteropServices;
            public class CmdxNative {
                [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
                public static extern IntPtr SendMessageTimeout(
                    IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
                    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
            }
"@ -ErrorAction SilentlyContinue
        }
        $HWND_BROADCAST = [IntPtr]0xffff
        $WM_SETTINGCHANGE = 0x001A
        $SMTO_ABORTIFHUNG = 0x0002
        $result = [UIntPtr]::Zero
        [CmdxNative]::SendMessageTimeout($HWND_BROADCAST, $WM_SETTINGCHANGE, [UIntPtr]::Zero, "Environment", $SMTO_ABORTIFHUNG, 500, [ref]$result) | Out-Null
    } catch {}
}

function Refresh-ActiveEnvironment {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath    = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path    = "$machinePath;$userPath"
    Broadcast-EnvironmentChange
}

# ==============================================================================
# 1. COMMAND DEFINITIONS
# ==============================================================================
$global:Commands = [ordered]@{
    '^(node|npm|npx|corepack)(16|22|24)$' = {
        $tool = $global:CmdxMatches[1]
        $ver  = $global:CmdxMatches[2]
        $nvmDir = (Get-Item "$env:LOCALAPPDATA\nvm\v$ver*" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
        if ($nvmDir) {
            $bin = (Get-ChildItem -Path $nvmDir -Filter "$tool.*" | Where-Object { $_.Extension -in @('.exe', '.cmd', '.bat') } | Select-Object -First 1).FullName
            if ($bin) {
                & $bin @args
                return
            }
        }
        Write-Error "Command '$tool$ver' not found in NVM directories."
    }

    'll' = {
        Get-ChildItem | Format-Table
    }

    'ls' = {
        Get-ChildItem
    }

    'qwen' = {
        & "$env:LOCALAPPDATA\nvm\v24.13.0\qwen" @args
    }

    'zw' = {
        $p = @(
            'C:\ITAMEssential'
            'C:\xampp8\htdocs\business_eprompto'
            '%USERPROFILE%\Desktop\ITAM\windows'
            '%USERPROFILE%\Desktop\ITAM\windows 02'
            '%USERPROFILE%\Desktop\ITAM\windows-server'
            '%USERPROFILE%\Desktop\ITAM\mac'
            'D:\rhel-itam\v2'
            '%USERPROFILE%\Desktop\playground\windows-log-analyser'
            '%USERPROFILE%\Downloads\winlogana'
            '%USERPROFILE%\Desktop\playground\tiny-windows-utilites'
            '%USERPROFILE%\Desktop\playground\shdcn-registry-gallery'
            '%USERPROFILE%\Desktop\playground\01 dsa'
            '%USERPROFILE%\Desktop\playground\tui'
            'D:\tab-walker'
            '%USERPROFILE%\Desktop\playground\itam-logs'
            '%USERPROFILE%\Downloads\hdd_analyse'
        ) |  ForEach-Object {
			[Environment]::ExpandEnvironmentVariables($_)
		}

		& "$env:LOCALAPPDATA\Programs\Zed\bin\zed.exe" @paths
    }

    'refreshenv' = {
        Refresh-ActiveEnvironment
        Write-Host "[+] Environment and PATH refreshed for current session." -ForegroundColor Green
    }
}

# ==============================================================================
# 2. PATTERN EXPANSION HELPER
# ==============================================================================
function Expand-PatternToAliases {
    param([string]$Pattern)

    $clean = $Pattern -replace '^\^', '' -replace '\$$', ''

    if ($clean -notmatch '\([^)]+\)') {
        return @($clean)
    }

    $regex = '(?<literal>[^()]+)|(?:\((?<group>[^()]+)\))'
    $matches = [regex]::Matches($clean, $regex)

    $results = @("")
    foreach ($m in $matches) {
        if ($m.Groups['literal'].Success) {
            $lit = $m.Groups['literal'].Value
            $results = $results | ForEach-Object { "$_$lit" }
        } elseif ($m.Groups['group'].Success) {
            $options = $m.Groups['group'].Value -split '\|'
            $newResults = @()
            foreach ($prefix in $results) {
                foreach ($opt in $options) {
                    $newResults += "$prefix$opt"
                }
            }
            $results = $newResults
        }
    }

    return $results
}

# ==============================================================================
# 3. SELF-SYNC & SHIM REPLACEMENT (RUNS ON SETUP)
# ==============================================================================
function Get-CmdxPaths {
    $scriptPath = $PSCommandPath
    if (-not $scriptPath) {
        $scriptPath = $MyInvocation.MyCommand.Path
    }
    if (-not $scriptPath) {
        $scriptPath = $MyInvocation.MyCommand.Definition
    }
    if (-not $scriptPath -or -not (Test-Path $scriptPath)) {
        $scriptPath = (Get-Item ".\cmdx.ps1" -ErrorAction SilentlyContinue).FullName
    }
    $scriptPath = [System.IO.Path]::GetFullPath($scriptPath)
    $scriptDir  = Split-Path -Parent $scriptPath

    if ((Split-Path -Leaf $scriptDir) -eq 'src') {
        $rootDir = Split-Path -Parent $scriptDir
        $shimDir = Join-Path $rootDir "cmds"
    } else {
        $rootDir = $scriptDir
        $shimDir = Join-Path $scriptDir "cmds"
    }

    return @{
        ScriptPath = $scriptPath
        ScriptDir  = $scriptDir
        RootDir    = $rootDir
        ShimDir    = $shimDir
    }
}

# ==============================================================================
# 3. SELF-SYNC & SHIM REPLACEMENT (RUNS ON SETUP)
# ==============================================================================
function Sync-Shims {
    $pathsInfo  = Get-CmdxPaths
    $scriptPath = $pathsInfo.ScriptPath
    $scriptDir  = $pathsInfo.ScriptDir
    $rootDir    = $pathsInfo.RootDir
    $shimDir    = $pathsInfo.ShimDir

    if (-not (Test-Path $shimDir)) {
        New-Item -ItemType Directory -Path $shimDir -Force | Out-Null
    }

    Write-Host "`n=== Setting Up & Syncing cmdx Shims ===" -ForegroundColor Cyan
    Write-Host "Source Script : $scriptPath" -ForegroundColor DarkGray
    Write-Host "Shim Folder   : $shimDir" -ForegroundColor DarkGray

    # 1. Clean legacy shims from root directory and script directory
    $legacyShims = Get-ChildItem -Path $rootDir, $scriptDir -File -ErrorAction SilentlyContinue |
                   Where-Object { ($_.Extension -in @(".cmd", ".ps1")) -and ($_.Name -notin @("cmdx.ps1", "build.ps1")) }
    foreach ($leg in $legacyShims) {
        Remove-Item $leg.FullName -Force -ErrorAction SilentlyContinue
    }

    # 2. Ensure $shimDir is in User PATH (HKCU\Environment)
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $existingPaths = @()
    if ($userPath) {
        $existingPaths = ($userPath -split ';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    }

    if ($existingPaths -notcontains $shimDir) {
        $newPaths = $existingPaths + $shimDir
        [Environment]::SetEnvironmentVariable("Path", ($newPaths -join ';'), "User")
        Write-Host "[+] Registered in User PATH: $shimDir" -ForegroundColor Green
    } else {
        Write-Host "[+] Directory already registered in User PATH" -ForegroundColor Green
    }

    # Refresh current environment immediately
    Refresh-ActiveEnvironment

    # 3. Replace/Write both .cmd and .ps1 shims in $shimDir
    Write-Host "`nUpdating Shims in $($shimDir):" -ForegroundColor Cyan
    $count = 0

    foreach ($pattern in $global:Commands.Keys) {
        $aliases = Expand-PatternToAliases -Pattern $pattern
        foreach ($alias in $aliases) {
            $cmdShimPath = Join-Path $shimDir "$alias.cmd"
            $ps1ShimPath = Join-Path $shimDir "$alias.ps1"

            if ($alias -eq 'refreshenv') {
                # In CMD: Native SET commands so caller process environment updates in-place
                $cmdContent = "@echo off`r`nfor /f `"tokens=2*`" %%A in ('reg query `"HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`" /v Path 2^>nul') do set `"SYS_PATH=%%B`"`r`nfor /f `"tokens=2*`" %%A in ('reg query `"HKCU\Environment`" /v Path 2^>nul') do set `"USER_PATH=%%B`"`r`nset `"PATH=%SYS_PATH%;%USER_PATH%`"`r`necho [+] PATH refreshed for current CMD session."
                
                # In PowerShell: Update $env:Path directly in caller session
                $ps1Content = "`$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')`r`nWrite-Host `"[+] PATH refreshed for current PowerShell session.`" -ForegroundColor Green"
            } else {
                # Forwarding shim for CMD (points to source cmdx.ps1)
                $cmdContent = "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" %~n0 %*"
                
                # Forwarding shim for PowerShell (points to source cmdx.ps1)
                $ps1Content = "& `"$scriptPath`" $alias @args"
            }

            try {
                [System.IO.File]::WriteAllText($cmdShimPath, $cmdContent)
                [System.IO.File]::WriteAllText($ps1ShimPath, $ps1Content)
                Write-Host "  [+] $alias (.cmd, .ps1)" -ForegroundColor Green
                $count += 2
            } catch {
                Write-Warning "  [-] Failed to write shims for $($alias): $_"
            }
        }
    }

    # 4. Write root cmdx.cmd and cmdx.ps1 in $shimDir (forwarding to source cmdx.ps1)
    $cmdxCmdPath = Join-Path $shimDir "cmdx.cmd"
    $cmdxCmdContent = "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" %*"
    [System.IO.File]::WriteAllText($cmdxCmdPath, $cmdxCmdContent)

    $cmdxPs1Path = Join-Path $shimDir "cmdx.ps1"
    $cmdxPs1Content = "& `"$scriptPath`" @args"
    [System.IO.File]::WriteAllText($cmdxPs1Path, $cmdxPs1Content)

    Write-Host "  [+] cmdx (.cmd, .ps1)" -ForegroundColor Green
    $count += 2

    Write-Host "`nDone: $count shims synchronized in '$($shimDir)'.`n" -ForegroundColor Cyan
}

# ==============================================================================
# 4. CLEAN SHIMS & UNREGISTER PATH (RUNS ON 'clean')
# ==============================================================================
function Clean-GlobalShims {
    $pathsInfo  = Get-CmdxPaths
    $scriptPath = $pathsInfo.ScriptPath
    $scriptDir  = $pathsInfo.ScriptDir
    $rootDir    = $pathsInfo.RootDir
    $shimDir    = $pathsInfo.ShimDir

    Write-Host "`n=== Cleaning cmdx Shims & PATH ===" -ForegroundColor Yellow
    Write-Host "Main Script : $scriptPath" -ForegroundColor DarkGray
    Write-Host "Shim Folder : $shimDir" -ForegroundColor DarkGray

    # 1. Remove Directory from User PATH (HKCU\Environment)
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath) {
        $cleanedPaths = ($userPath -split ';') | Where-Object { 
            (-not [string]::IsNullOrWhiteSpace($_)) -and 
            ($_ -ne $shimDir) -and 
            ($_ -ne $scriptDir)
        }
        $newPath = $cleanedPaths -join ';'
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "[-] Removed from User PATH: $shimDir" -ForegroundColor Yellow
    }

    # Refresh current environment immediately
    Refresh-ActiveEnvironment

    # 2. Remove all shims in cmds folder
    $removed = 0
    if (Test-Path $shimDir) {
        $shims = Get-ChildItem -Path $shimDir -File -ErrorAction SilentlyContinue
        foreach ($shim in $shims) {
            try {
                Remove-Item $shim.FullName -Force
                Write-Host "  [-] Removed $($shim.Name)" -ForegroundColor Yellow
                $removed++
            } catch {
                Write-Warning "  Failed to remove $($shim.Name): $_"
            }
        }
        Remove-Item $shimDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    # Also clean legacy shims in root / scriptDir if any exist
    $legacyShims = Get-ChildItem -Path $rootDir, $scriptDir -File -ErrorAction SilentlyContinue |
                   Where-Object { ($_.Extension -in @(".cmd", ".ps1")) -and ($_.Name -notin @("cmdx.ps1", "build.ps1")) }
    foreach ($leg in $legacyShims) {
        Remove-Item $leg.FullName -Force -ErrorAction SilentlyContinue
        $removed++
    }

    Write-Host "`nDone: $removed shims removed & environment refreshed.`n" -ForegroundColor Green
}

# ==============================================================================
# 5. SHOW HELP & LIST COMMANDS
# ==============================================================================
function Show-Help {
    Write-Host "`n=== cmdx - Global Command Hub ===" -ForegroundColor Cyan
    Write-Host "Usage:"
    Write-Host "  cmdx                            Display this help manual and list all commands"
    Write-Host "  cmdx setup                      Register 'cmds' folder in User PATH, sync shims & refresh env"
    Write-Host "  cmdx clean                      Remove 'cmds' shims folder, unregister User PATH & refresh env"
    Write-Host "  cmdx help                       Display this help manual and list all commands"
    Write-Host "  <command> [args]                Execute registered command dynamically`n"

    Write-Host "Registered Commands & Aliases:" -ForegroundColor Cyan
    $totalAliases = 0

    foreach ($pattern in $global:Commands.Keys) {
        $aliases = Expand-PatternToAliases -Pattern $pattern
        $totalAliases += $aliases.Count
        Write-Host ("  Pattern : {0}" -f $pattern) -ForegroundColor Yellow
        Write-Host ("  Aliases : {0}" -f ($aliases -join ', ')) -ForegroundColor Green
        Write-Host ""
    }

    Write-Host "Total Available Global Aliases: $totalAliases`n" -ForegroundColor DarkGray
}

# ==============================================================================
# 6. DISPATCHER & EXECUTION
# ==============================================================================
function Dispatch-Command {
    param(
        [string]$CmdName,
        [string[]]$ArgsList
    )

    foreach ($pattern in $global:Commands.Keys) {
        if ($CmdName -match $pattern) {
            $global:CmdxMatches = $Matches
            $scriptBlock = $global:Commands[$pattern]

            # Run the ScriptBlock with passed @args
            if ($ArgsList) {
                & $scriptBlock @ArgsList
            } else {
                & $scriptBlock
            }
            return
        }
    }

    Write-Error "Unknown command '$CmdName'. Run 'cmdx help' for a list of available commands."
}

# ==============================================================================
# 7. ROUTING
# ==============================================================================
if (-not $Command -or $Command -in @('help', '-help', '--help', '-h', '/?', '?')) {
    # No arguments OR 'help' -> Show Help and list all commands
    Show-Help
    return
}

if ($Command -eq 'setup') {
    # cmdx setup -> Register PATH & sync shims
    Sync-Shims
    return
}

if ($Command -eq 'clean') {
    # cmdx clean -> Remove shims and unregister User PATH
    Clean-GlobalShims
    return
}

# Execution via shim or command call -> Dispatch
Dispatch-Command -CmdName $Command -ArgsList $CommandArgs
