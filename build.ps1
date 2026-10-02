[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Targets,

    [ValidateSet('release', 'debug')]
    [string]$Mode = 'release',

    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    [switch]$Package
)

$ErrorActionPreference = 'Stop'

$ROOT = $PSScriptRoot
$SRC = Join-Path $ROOT 'src'
$DIST = Join-Path $ROOT 'dist'
$TARGET_DIST = if ($Arch -eq 'arm64') {
    Join-Path (Join-Path $DIST $Mode.ToLower()) 'arm64'
} else {
    Join-Path $DIST $Mode.ToLower()
}

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
    }
    'find-my-mouse' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32')
    }
    'color-picker' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32')
    }
    'capture' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32', 'comdlg32')
    }
    'pixel-view' = @{
        Subsystem  = 'windows'
        Libs       = @('user32', 'gdi32', 'ole32', 'windowscodecs', 'comdlg32', 'shell32')
        ExtraMinGW = @('-municode')
    }
    'ip-send' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'gdi32', 'shell32', 'comdlg32', 'advapi32')
    }
    'context-menu' = @{
        Subsystem = 'windows'
        Libs      = @('user32', 'shell32', 'comdlg32', 'advapi32', 'comctl32')
    }
    'ocr' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'shell32', 'ole32', 'urlmon', 'shlwapi', 'windowscodecs', 'gdi32', 'comdlg32')
        ExtraMinGW  = @('-municode')
    }
    'window-switcher' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'gdi32', 'dwmapi', 'shell32', 'ole32', 'version', 'advapi32')
        ExtraMinGW  = @('-municode')
    }
    'mouse-spotlight' = @{
        Subsystem   = 'windows'
        Libs        = @('user32', 'gdi32')
        ExtraMinGW  = @('-municode')
    }
}

function Resolve-TargetName([string]$Name) {
    $normalized = $Name.Trim().ToLower()
    if ($TARGET_CONFIGS.Contains($normalized)) {
        return $normalized
    }
    return $null
}

function Build-Target([string]$TargetName, [string]$BuildMode, [string]$BuildArch = 'x64') {
    $canonicalName = Resolve-TargetName $TargetName
    if (-not $canonicalName) {
        $validTargets = ($TARGET_CONFIGS.Keys) -join ', '
        throw "Unknown build target: '$TargetName'. Valid targets are: $validTargets"
    }

    $config = $TARGET_CONFIGS[$canonicalName]
    $srcFile = Join-Path $SRC "$canonicalName.c"
    $targetDist = if ($BuildArch -eq 'arm64') {
        Join-Path (Join-Path $DIST $BuildMode.ToLower()) 'arm64'
    } else {
        Join-Path $DIST $BuildMode.ToLower()
    }
    $outFile = Join-Path $targetDist "$canonicalName.exe"

    if (-not (Test-Path $srcFile)) {
        throw "Source file not found: $srcFile"
    }

    New-Item -ItemType Directory -Path $targetDist -Force | Out-Null
    Write-Host "==> $canonicalName ($BuildMode, $BuildArch) -> dist/$BuildMode/$canonicalName.exe"

    # Generate library arguments
    $libsMinGW = @($config.Libs | ForEach-Object { "-l$_" })
    $libsMSVC  = @($config.Libs | ForEach-Object { "$_.lib" })

    # Extra toolchain flags
    $extraMinGW = if ($config.ExtraMinGW) { @($config.ExtraMinGW) } else { @() }
    $extraMSVC  = if ($config.ExtraMSVC)  { @($config.ExtraMSVC) }  else { @() }

    # Subsystem flags
    $subsystemMinGW = "-m$($config.Subsystem)"
    $subsystemMSVC  = "/SUBSYSTEM:$($config.Subsystem.ToUpper())"

    $targetTripleMinGW = if ($BuildArch -eq 'arm64') { 'aarch64-w64-windows-gnu' } else { 'x86_64-w64-windows-gnu' }
    $targetTripleMSVC  = if ($BuildArch -eq 'arm64') { 'arm64-pc-windows-msvc' }   else { 'x86_64-pc-windows-msvc' }

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
                "--target=$targetTripleMinGW",
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
                "--target=$targetTripleMSVC",
                '/O1',
                '/MD',
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
                "--target=$targetTripleMSVC",
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
                "--target=$targetTripleMinGW",
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

    throw "Build failed: $canonicalName ($BuildMode, $BuildArch)"
}

function Build-All([string]$BuildMode, [string]$BuildArch = 'x64') {
    foreach ($name in $TARGET_CONFIGS.Keys) {
        Build-Target $name $BuildMode $BuildArch
    }
}

function Package-Artifacts([string]$BuildMode, [string]$BuildArch = 'x64') {
    $srcDir = if ($BuildArch -eq 'arm64') {
        Join-Path (Join-Path $DIST $BuildMode.ToLower()) 'arm64'
    } else {
        Join-Path $DIST $BuildMode.ToLower()
    }
    $pkgDir = Join-Path (Join-Path $DIST 'packages') $BuildArch
    New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null

    # 1. Clean bundle folder for all-in-one ZIP (un-suffixed .exe files + shortcuts.ahk)
    $bundleDir = Join-Path (Join-Path $DIST 'bundle') $BuildArch
    New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null
    Get-ChildItem -Path $srcDir -Filter "*.exe" | ForEach-Object {
        Copy-Item $_.FullName "$bundleDir\$($_.Name)" -Force
    }
    $shortcuts = Join-Path (Join-Path $ROOT 'docs') 'shortcuts.ahk'
    if (Test-Path $shortcuts) {
        Copy-Item $shortcuts "$bundleDir\shortcuts.ahk" -Force
    }
    $zipPath = Join-Path $pkgDir "tiny-utilities-$BuildArch.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$bundleDir\*" -DestinationPath $zipPath

    # 2. Individual standalone executables with arch suffix for single-file downloads
    Get-ChildItem -Path $srcDir -Filter "*.exe" | ForEach-Object {
        Copy-Item $_.FullName "$pkgDir\$($_.BaseName)-$BuildArch.exe" -Force
    }
    Write-Host "Packaged $BuildArch binaries & ZIP archive to: $pkgDir"
}

if (-not $Targets -or $Targets.Count -eq 0) {
    Write-Host "Build Mode: $Mode ($Arch)"
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
        Build-All $Mode $Arch
    }
    elseif ($indexMap.ContainsKey($choice)) {
        Build-Target $indexMap[$choice] $Mode $Arch
    }
    else {
        $resolved = Resolve-TargetName $choice
        if ($resolved) {
            Build-Target $resolved $Mode $Arch
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
            Build-All $Mode $Arch
        }
        else {
            Build-Target $target $Mode $Arch
        }
    }
}

Write-Host "Build complete ($Mode, $Arch)."

if ($Package) {
    Package-Artifacts $Mode $Arch
}