# Throwaway tray smoke test for kyper + whisper-cast.
Add-Type -Namespace TN -Name W -ErrorAction SilentlyContinue
if (-not ([System.Management.Automation.PSTypeName]'TN.W').Type) {
    Add-Type -Namespace TN -Name W -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern System.IntPtr SendMessageW(System.IntPtr h, uint msg, System.IntPtr wp, System.IntPtr lp);
'@
}

$k = Start-Process -FilePath 'D:\02_projects\tiny-utilities\dist\release\kyper.exe' `
     -ArgumentList '--user','admin','--pass','secret123' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1200
$hKy = [TN.W]::FindWindowW('TinyKyperTrayHost', $null)
"kyper tray host: 0x$('{0:X}' -f $hKy.ToInt64()) (nonzero = icon host up)"
# programmatic exit through the exact tray-menu code path (0xFA21 = WM_APP+0x7A21)
$null = [TN.W]::SendMessageW($hKy, 0xFA21, [IntPtr]::Zero, [IntPtr]::Zero)
$k.WaitForExit(4000) | Out-Null
"kyper exit code after tray message: $($k.ExitCode) (expect 0)"
$after = [TN.W]::FindWindowW('TinyKyperTrayHost', $null)
"kyper tray host after exit: 0x$('{0:X}' -f $after.ToInt64()) (zero = destroyed)"

# --tray false (space syntax changed in shared tiny_cli.h)
$n1 = Start-Process -FilePath 'D:\02_projects\tiny-utilities\dist\release\kyper.exe' `
      -ArgumentList '--user','admin','--pass','secret123','--tray','false' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1000
$n1h = [TN.W]::FindWindowW('TinyKyperTrayHost', $null)
"--tray false (space): host=0x$('{0:X}' -f $n1h.ToInt64()) (expect zero); process alive: $(-not $n1.HasExited)"
$null = [TN.W]::SendMessageW([TN.W]::FindWindowW($null, $null), 0, [IntPtr]::Zero, [IntPtr]::Zero) # noop
Stop-Process -Id $n1.Id -Force -ErrorAction SilentlyContinue

# --tray=false (equals syntax, original shared-header behavior)
$n2 = Start-Process -FilePath 'D:\02_projects\tiny-utilities\dist\release\kyper.exe' `
      -ArgumentList '--user=admin','--pass=secret123','--tray=false' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1000
$n2h = [TN.W]::FindWindowW('TinyKyperTrayHost', $null)
"--tray=false (equals): host=0x$('{0:X}' -f $n2h.ToInt64()) (expect zero); process alive: $(-not $n2.HasExited)"
# toggle-off still works with tray absent
$null = Start-Process -FilePath 'D:\02_projects\tiny-utilities\dist\release\kyper.exe' -WindowStyle Hidden
$n2.WaitForExit(4000) | Out-Null
"kyper toggle with --tray=false: exit=$($n2.ExitCode) (expect 0)"

$w = Start-Process -FilePath 'D:\02_projects\tiny-utilities\dist\release\whisper-cast.exe' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1200
$hWc = [TN.W]::FindWindowW('TinyWhisperCastTrayHost', $null)
"whisper-cast tray host: 0x$('{0:X}' -f $hWc.ToInt64()) (nonzero = icon host up)"
$null = [TN.W]::SendMessageW($hWc, 0xFA21, [IntPtr]::Zero, [IntPtr]::Zero)
$w.WaitForExit(4000) | Out-Null
"whisper-cast exit code after tray message: $($w.ExitCode) (expect 0)"
