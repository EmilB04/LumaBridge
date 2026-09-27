<#
.SYNOPSIS
    Sets up (or removes) the silent hand-back of the lights to Armoury Crate.

.DESCRIPTION
    LumaBridge drives the motherboard's Aura controller in "direct mode", which the
    controller only forgets when it restarts. Restarting just that USB device (what
    `pnputil /restart-device` does) makes it reload the effect Armoury Crate saved in it,
    with no reboot and no Armoury Crate window.

    Restarting a device needs administrator rights, so this script (run once, elevated)
    registers a scheduled task under your account with "run with highest privileges".
    LumaBridge then starts that task whenever it hands the lights back, without a UAC
    prompt. The task runs in the background (no window) and only restarts USB devices
    named "AURA LED Controller" from ASUS (vendor 0B05).

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

# What the task runs: restart the USB device that the "AURA LED Controller" interface
# belongs to (its parent, e.g. USB\VID_0B05&PID_1939\...), which is what worked by hand.
$command = @'
$controllers = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -eq 'AURA LED Controller' }
foreach ($c in $controllers) {
    $parent = (Get-PnpDeviceProperty -InstanceId $c.InstanceId -KeyName 'DEVPKEY_Device_Parent').Data
    if ($parent -match '^USB\\VID_0B05&PID_[0-9A-F]{4}\\') {
        & pnputil.exe /restart-device "$parent" | Out-Null
    }
}
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand $encoded"
# S4U: runs in the background without a window and without storing a password.
$principal = New-ScheduledTaskPrincipal -UserId $id.Name -LogonType S4U -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $action -Principal $principal `
    -Settings $settings -Force `
    -Description 'LumaBridge: restarts the ASUS AURA LED Controller so it goes back to the lighting Armoury Crate saved in it.' |
    Out-Null
Write-Host "Registered task $TaskPath$TaskName"
