[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Targets,

    [ValidateSet('release', 'debug')]
    [string]$Mode = 'release'
)

$ErrorActionPreference = 'Stop'

$ROOT = $PSScriptRoot
$SRC = Join-Path $ROOT 'src'
$DIST = Join-Path $ROOT 'dist'
$TARGET_DIST = Join-Path $DIST $Mode.ToLower()

function Ensure-ToolchainPath {
    if ((Get-Command clang -ErrorAction SilentlyContinue) -or (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
        return
    }

    $candidateDirs = @(
        'C:\Program Files\LLVM\bin',
        'C:\Program Files (x86)\LLVM\bin',
        (Join-Path $env:LOCALAPPDATA 'Programs\LLVM\bin'),
        'C:\LLVM\bin',
        'C:\msys64\clang64\bin',
        'C:\msys64\ucrt64\bin',
        'C:\msys64\mingw64\bin',
        'C:\ProgramData\chocolatey\lib\llvm\tools\llvm\bin',
        (Join-Path $env:USERPROFILE 'scoop\apps\llvm\current\bin'),
        (Join-Path $env:USERPROFILE 'scoop\shims')
    )

    $vsDirs = Get-ChildItem -Path 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\Llvm\x64\bin' -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
    if ($vsDirs) {
        $candidateDirs += $vsDirs
    }

    foreach ($dir in $candidateDirs) {
        if ($dir -and (Test-Path $dir)) {
            $hasClang = (Test-Path (Join-Path $dir 'clang.exe')) -or (Test-Path (Join-Path $dir 'clang-cl.exe'))
            if ($hasClang) {
                Write-Host "Discovered Clang toolchain at '$dir' (added to session PATH)." -ForegroundColor DarkGray
                $env:PATH = "$dir;$env:PATH"
                return
            }
        }
    }

    $errLines = @(
        "",
        "=======================================================================",
        " BUILD ERROR: Clang compiler toolchain not found",
        "=======================================================================",
        "Neither 'clang' nor 'clang-cl' was found in your PATH or in standard",
        "installation directories:",
        "  - C:\Program Files\LLVM\bin",
        "  - Visual Studio (VC\Tools\Llvm\x64\bin)",
        "  - MSYS2 (C:\msys64\clang64\bin, ucrt64, mingw64)",
        "  - Scoop / Chocolatey installation paths",
        "",
        "To build tiny-utilities, please install LLVM Clang:",
        "  - WinGet:      winget install LLVM.LLVM",
        "  - Chocolatey:  choco install llvm",
        "  - Official:    https://github.com/llvm/llvm-project/releases",
        "",
        "If LLVM is already installed in a custom location, add its 'bin' directory",
        "to your system or user PATH environment variable.",
        "======================================================================="
    )
    throw ($errLines -join [Environment]::NewLine)
}

Ensure-ToolchainPath

# Declarative target configuration table
$TARGET_CONFIGS = [ordered]@{
    'pin-to-top' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32')
        Aliases   = @('pin')
    }
    'find-my-mouse' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32')
        Aliases   = @('cursor', 'cursorfocus')
    }
    'color-picker' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32')
        Aliases   = @('colorpicker', 'picker')
    }
    'capture' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32', 'comdlg32')
        Aliases   = @('snip')
    }
    'pixel-view' = @{
        Subsystem  = 'windows'
        Libs       = @('user32', 'gdi32', 'ole32', 'windowscodecs', 'comdlg32', 'shell32')
        ExtraMinGW = @('-municode')
        Aliases    = @('pixelview')
    }
    'ip-send' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32', 'shell32', 'comdlg32', 'advapi32')
        Aliases   = @('ip', 'ipmsg')
    }
    'context-menu' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'shell32', 'comdlg32', 'advapi32', 'comctl32')
        Aliases   = @('contextmenu')
    }
    'ocr' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'shell32', 'ole32', 'urlmon', 'shlwapi', 'windowscodecs', 'gdi32', 'comdlg32')
        ExtraMinGW  = @('-municode')
        Aliases     = @()
    }
    'window-switcher' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'gdi32', 'dwmapi', 'shell32', 'ole32', 'version', 'advapi32')
        ExtraMinGW  = @('-municode')
        Aliases     = @('switcher', 'win-switch', 'windowswitcher')
    }
    'mouse-spotlight' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'gdi32')
        ExtraMinGW  = @('-municode')
        Aliases     = @('spotlight', 'mousespotlight')
    }
}

function Resolve-TargetName([string]$Name) {
    $normalized = $Name.Trim().ToLower()
    if ($TARGET_CONFIGS.Contains($normalized)) {
        return $normalized
    }
    foreach ($entry in $TARGET_CONFIGS.GetEnumerator()) {
        if ($entry.Value.Aliases -and $entry.Value.Aliases -contains $normalized) {
            return $entry.Key
        }
    }
    return $null
}

function Build-Target([string]$TargetName, [string]$BuildMode) {
    $canonicalName = Resolve-TargetName $TargetName
    if (-not $canonicalName) {
        throw "Unknown build target: '$TargetName'"
    }

    $config = $TARGET_CONFIGS[$canonicalName]
    $srcFile = Join-Path $SRC "$canonicalName.c"
    $outFile = Join-Path $TARGET_DIST "$canonicalName.exe"

    if (-not (Test-Path $srcFile)) {
        throw "Source file not found: $srcFile"
    }

    New-Item -ItemType Directory -Path $TARGET_DIST -Force | Out-Null
    Write-Host "==> $canonicalName ($BuildMode) -> dist/$BuildMode/$canonicalName.exe"

    # Generate library arguments
    $libsMinGW = @($config.Libs | ForEach-Object { "-l$_" })
    $libsMSVC  = @($config.Libs | ForEach-Object { "$_.lib" })

    # Extra toolchain flags
    $extraMinGW = if ($config.ExtraMinGW) { @($config.ExtraMinGW) } else { @() }
    $extraMSVC  = if ($config.ExtraMSVC)  { @($config.ExtraMSVC) }  else { @() }

    # Subsystem flags
    $subsystemMinGW = "-m$($config.Subsystem)"
    $subsystemMSVC  = "/SUBSYSTEM:$($config.Subsystem.ToUpper())"

    if ($BuildMode -eq 'release') {
        # 1. MinGW Clang with standardized size and performance optimization
        if (Get-Command clang -ErrorAction SilentlyContinue) {
            $cmdArgs = @(
                $srcFile,
                '-Os',
                '-flto',
                '-ffunction-sections',
                '-fdata-sections',
                '-Wl,--gc-sections',
                '-Wl,-s',
                '--target=x86_64-w64-windows-gnu',
                $subsystemMinGW
            )
            $cmdArgs += $libsMinGW
            if ($extraMinGW.Count -gt 0) {
                $cmdArgs += $extraMinGW
            }
            $cmdArgs += @('-o', $outFile)

            & clang @cmdArgs

            if ($LASTEXITCODE -eq 0) {
                return
            }
        }

        # 2. Fallback: MSVC Clang-cl with size optimization
        if (Get-Command clang-cl -ErrorAction SilentlyContinue) {
            $cmdArgs = @(
                $srcFile,
                '/O1',
                '/Gy',
                '/Gw',
                '/link'
            )
            $cmdArgs += $libsMSVC
            $cmdArgs += @($subsystemMSVC, '/OPT:REF', '/OPT:ICF', '/MANIFEST:NO')
            if ($extraMSVC.Count -gt 0) {
                $cmdArgs += $extraMSVC
            }
            $cmdArgs += "/OUT:$outFile"

            & clang-cl @cmdArgs

            if ($LASTEXITCODE -eq 0) {
                return
            }
        }
    }
    else {
        # Debug Mode: Include debug symbols and disable optimizations
        # 1. MSVC Clang-cl debug (generates standard PDB symbols)
        if (Get-Command clang-cl -ErrorAction SilentlyContinue) {
            $cmdArgs = @(
                $srcFile,
                '/Od',
                '/Zi',
                '/link'
            )
            $cmdArgs += $libsMSVC
            $cmdArgs += @($subsystemMSVC, '/DEBUG')
            if ($extraMSVC.Count -gt 0) {
                $cmdArgs += $extraMSVC
            }
            $cmdArgs += "/OUT:$outFile"

            & clang-cl @cmdArgs

            if ($LASTEXITCODE -eq 0) {
                return
            }
        }

        # 2. MinGW Clang debug (generates DWARF debug symbols)
        if (Get-Command clang -ErrorAction SilentlyContinue) {
            $cmdArgs = @(
                $srcFile,
                '-g',
                '-O0',
                '--target=x86_64-w64-windows-gnu',
                $subsystemMinGW
            )
            $cmdArgs += $libsMinGW
            if ($extraMinGW.Count -gt 0) {
                $cmdArgs += $extraMinGW
            }
            $cmdArgs += @('-o', $outFile)

            & clang @cmdArgs

            if ($LASTEXITCODE -eq 0) {
                return
            }
        }
    }

    throw "Build failed: $canonicalName ($BuildMode)"
}

function Build-All([string]$BuildMode) {
    foreach ($name in $TARGET_CONFIGS.Keys) {
        Build-Target $name $BuildMode
    }
}

if (-not $Targets -or $Targets.Count -eq 0) {
    Write-Host "Build Mode: $Mode"
    Write-Host "1) all"
    $index = 2
    $indexMap = @{}
    foreach ($name in $TARGET_CONFIGS.Keys) {
        Write-Host "$index) $name"
        $indexMap["$index"] = $name
        $index++
    }
    Write-Host ''

    $choice = Read-Host 'Build'

    if ($choice -eq '1' -or $choice.ToLower() -eq 'all') {
        Build-All $Mode
    }
    elseif ($indexMap.ContainsKey($choice)) {
        Build-Target $indexMap[$choice] $Mode
    }
    else {
        $resolved = Resolve-TargetName $choice
        if ($resolved) {
            Build-Target $resolved $Mode
        }
        else {
            Write-Error "Invalid selection: '$choice'"
            exit 1
        }
    }
}
else {
    foreach ($target in $Targets) {
        if ($target.ToLower() -eq 'all') {
            Build-All $Mode
        }
        else {
            Build-Target $target $Mode
        }
    }
}

Write-Host "Build complete ($Mode)."