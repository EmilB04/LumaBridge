<#
.SYNOPSIS
    Sets up (or removes) the silent hand-back of the lights to Armoury Crate.

.DESCRIPTION
    LumaBridge drives the motherboard's Aura controller in "direct mode", which the
    controller only forgets when it restarts. Restarting just that USB device (what
    `pnputil /restart-device` does) makes it reload the effect Armoury Crate saved in it,
    with no reboot and no Armoury Crate window.

    Restarting a device needs full administrator rights, so this script (run once,
    elevated) registers a scheduled task that runs as SYSTEM, and gives your account the
    right to *start* it (read + execute on the task). LumaBridge then starts it whenever it
    hands the lights back, without a UAC prompt. The task runs in the background (no
    window), only restarts USB devices named "AURA LED Controller" from ASUS (vendor 0B05),
    and logs every step to %ProgramData%\LumaBridge\handback.log.

    (A first version ran the task under your own account with "highest privileges". The
    background logon Windows uses for that didn't have the rights pnputil needs, so the
    task ended with result 1 and the lights stayed frozen.)

    The LumaBridge app runs this for you (Games page → Armoury Crate hand-back → Set up).

.EXAMPLE
    .\Install-HandbackTask.ps1
    .\Install-HandbackTask.ps1 -Uninstall
#>
[CmdletBinding()]
param([switch] $Uninstall)

$ErrorActionPreference = 'Stop'
$TaskPath = '\LumaBridge\'
$TaskName = 'Hand back lighting'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell (Run as administrator).'
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host 'Removed the hand-back task.'
    exit 0
}

# What the task runs: restart the Aura motherboard controller's USB device (e.g.
# USB\VID_0B05&PID_1939\9876543210), the same thing `pnputil /restart-device` did by hand.
# It is matched by its USB id, not by name: depending on the driver, "AURA LED Controller"
# is the USB device itself or its HID child. Only whole USB devices of ASUS's Aura
# motherboard controllers match (not interfaces, not other ASUS gear like keyboards).
# Everything it does is logged to %ProgramData%\LumaBridge\handback.log.
$command = @'
$log = Join-Path $env:ProgramData 'LumaBridge\handback.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
function Write-Log($text) { Add-Content -Path $log -Value ("{0} {1}" -f (Get-Date -Format s), $text) }
$me = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = ([Security.Principal.WindowsPrincipal] $me).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Log "hand-back started as $($me.Name), elevated=$admin"
$pattern = '^USB\\VID_0B05&PID_(1867|1872|18A3|18A5|1939|19AF|1AA6)\\[^\\]+$'
$controllers = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match $pattern })
if (-not $controllers.Count) {
    Write-Log 'no Aura motherboard controller (USB 0B05:1939 or similar) found'
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_0B05*' } |
        ForEach-Object { Write-Log "  ASUS USB device: $($_.InstanceId) ($($_.FriendlyName))" }
}
foreach ($c in $controllers) {
    $out = & pnputil.exe /restart-device "$($c.InstanceId)" 2>&1 | Out-String
    Write-Log ("pnputil /restart-device {0} ({1}) -> exit {2}: {3}" -f $c.InstanceId, $c.FriendlyName, $LASTEXITCODE, $out.Trim())
}
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand $encoded"
# SYSTEM: always has the rights pnputil needs; runs in the background, no window.
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $action -Principal $principal `
    -Settings $settings -Force `
    -Description 'LumaBridge: restarts the ASUS AURA LED Controller so it goes back to the lighting Armoury Crate saved in it.' |
    Out-Null

# Let the current user start (not change) the task: read + execute on the task's DACL.
$service = New-Object -ComObject Schedule.Service
$service.Connect()
$task = $service.GetFolder($TaskPath.TrimEnd('\')).GetTask($TaskName)
$sddl = $task.GetSecurityDescriptor(4)   # DACL_SECURITY_INFORMATION
$ace = "(A;;GRGX;;;$($id.User.Value))"
if (-not $sddl.Contains($ace)) {
    $dacl = $sddl.IndexOf('D:')
    $firstAce = $sddl.IndexOf('(', $dacl)
    $sddl = if ($firstAce -ge 0) { $sddl.Insert($firstAce, $ace) } else { $sddl + $ace }
    $task.SetSecurityDescriptor($sddl, 0)
}
Write-Host "Registered task $TaskPath$TaskName (runs as SYSTEM; $($id.Name) may start it)"
