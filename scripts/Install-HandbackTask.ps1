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

    The LumaBridge app runs this for you (Integrations page → Armoury Crate hand-back → Set up).

.EXAMPLE
    .\Install-HandbackTask.ps1
    .\Install-HandbackTask.ps1 -Uninstall
#>
[CmdletBinding()]
param([switch] $Uninstall)

$ErrorActionPreference = 'Stop'
$TaskPath = '\LumaBridge\'
$TaskName = 'Hand back lighting'
# Bumped whenever the task changes; LumaBridge asks to set it up again when the recorded
# version is older (kHandbackTaskVersion in src/app/integrations.h).
$TaskVersion = 6
$VersionKey = 'HKLM:\SOFTWARE\LumaBridge'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell (Run as administrator).'
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $VersionKey -Name HandbackTaskVersion -ErrorAction SilentlyContinue
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

# Is any program holding one of the controller's HID interfaces open? Checked by opening it
# exclusively (and closing it right away): a restart while it's open gets deferred to the
# next reboot, and until then Windows refuses every further restart.
# Uses System.IO.File.Open (CreateFile under the hood) instead of Add-Type/P-Invoke: Add-Type
# compiles with csc.exe on every run, which alone can cost a couple of seconds.
function Get-HeldInterfaces($usbId) {
    $vidPid = ($usbId -split '\\')[1]
    $held = @()
    foreach ($hid in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                       Where-Object { $_.InstanceId -like "HID\$vidPid*" })) {
        $path = '\\?\' + ($hid.InstanceId -replace '\\', '#') + '#{4d1e55b2-f16f-11cf-88cb-001111000030}'
        try {
            ([System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)).Close()
        } catch [System.IO.IOException] {
            if (($_.Exception.HResult -band 0xFFFF) -eq 32) { $held += $hid.InstanceId }  # ERROR_SHARING_VIOLATION
        } catch { }
    }
    return ,$held
}

# Polls Get-HeldInterfaces until it's clear or `capMs` elapses, instead of a fixed sleep -
# Stop-Process/Stop-Service usually release the handle in well under that.
function Wait-Released($usbId, $capMs) {
    $held = Get-HeldInterfaces $usbId
    $waited = 0
    while ($held.Count -and $waited -lt $capMs) {
        Start-Sleep -Milliseconds 150
        $waited += 150
        $held = Get-HeldInterfaces $usbId
    }
    return ,$held
}

$pattern = '^USB\\VID_0B05&PID_(1867|1872|18A3|18A5|1939|19AF|1AA6)\\[^\\]+$'
$controllers = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match $pattern })
if (-not $controllers.Count) {
    Write-Log 'no Aura motherboard controller (USB 0B05:1939 or similar) found'
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_0B05*' } |
        ForEach-Object { Write-Log "  ASUS USB device: $($_.InstanceId) ($($_.FriendlyName))" }
}
foreach ($c in $controllers) {
    $id = $c.InstanceId
    $held = Get-HeldInterfaces $id
    $stoppedLightingService = $false
    if ($held.Count) {
        Write-Log "controller in use by another program ($($held -join ', '))"
        # Armoury Crate's motherboard helper keeps the controller open; its service starts it
        # again by itself.
        foreach ($p in @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'Aac*MbHal*' })) {
            Write-Log "stopping $($p.ProcessName) (pid $($p.Id)), Armoury Crate's motherboard helper"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
        $held = Wait-Released $id 1500
    }
    if ($held.Count) {
        # The helper process is only a client; LightingService is what actually keeps the
        # device open on its behalf, so killing the helper alone doesn't release it.
        $svc = Get-Service -Name LightingService -ErrorAction SilentlyContinue
        if ($svc -and $svc.Status -eq 'Running') {
            Write-Log 'still in use - stopping LightingService, the ASUS service that holds the device open'
            Stop-Service -Name LightingService -Force -ErrorAction SilentlyContinue
            $stoppedLightingService = $true
            $held = Wait-Released $id 1500
        }
    }
    if ($held.Count) {
        Write-Log "still in use by another program - not restarting $id (Windows would only finish it at the next reboot)"
        Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'aac|armoury|lighting|aura|asus|rgb' } |
            ForEach-Object { Write-Log "  running: $($_.ProcessName) (pid $($_.Id))" }
    } else {
        $out = & pnputil.exe /restart-device "$id" 2>&1 | Out-String
        $code = $LASTEXITCODE
        Write-Log ("pnputil /restart-device {0} -> exit {1}: {2}" -f $id, $code, ($out.Trim() -replace '\s+', ' '))
        if ($code -eq 50 -or $code -eq 3010) {
            Write-Log 'Windows has the restart waiting for a reboot: restart the PC once, then hand-back works again'
        }
    }
    if ($stoppedLightingService) {
        Write-Log 'restarting LightingService'
        Start-Service -Name LightingService -ErrorAction SilentlyContinue
    }
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
if (-not (Test-Path $VersionKey)) { New-Item -Path $VersionKey | Out-Null }
Set-ItemProperty -Path $VersionKey -Name HandbackTaskVersion -Value $TaskVersion -Type DWord
Write-Host "Registered task $TaskPath$TaskName (runs as SYSTEM; $($id.Name) may start it)"
