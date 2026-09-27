<#
.SYNOPSIS
    Sets up (or removes) RAM lighting: the LumaBridge-RAM helper and its scheduled task.

.DESCRIPTION
    RAM lighting controllers (HyperX / Kingston FURY RGB DDR4) sit on the motherboard's
    SMBus, which only administrators can reach, through the signed PawnIO driver
    (https://pawnio.eu) and its SmbusPIIX4 module (AMD chipsets).

    This script (run once, elevated):
      1. copies LumaBridge-RAM.exe and SmbusPIIX4.bin into %ProgramFiles%\LumaBridge, a
         folder only administrators can change, so nobody can swap the file the task runs;
      2. registers a scheduled task that runs it as SYSTEM, and gives your account the right
         to *start* it (read + execute on the task).
    LumaBridge then starts the helper when it lights the RAM, without a UAC prompt. The
    helper only writes the RAM's lighting registers (never the sticks' configuration chips)
    and exits when LumaBridge exits. It logs to %ProgramData%\LumaBridge\ram.log.

    SmbusPIIX4.bin comes from the PawnIO.Modules releases
    (https://github.com/namazso/PawnIO.Modules/releases): put it next to LumaBridge.exe
    before running this.

    The LumaBridge app runs this for you (Devices page → Memory → Set up).

.EXAMPLE
    .\Install-RamTask.ps1
    .\Install-RamTask.ps1 -Uninstall
#>
[CmdletBinding()]
param([switch] $Uninstall)

$ErrorActionPreference = 'Stop'
$TaskPath = '\LumaBridge\'
$TaskName = 'RAM lighting'
# Bumped whenever the task or the helper changes; LumaBridge asks to set it up again when
# the recorded version is older (kRamTaskVersion in src/app/integrations.h).
$TaskVersion = 1
$VersionKey = 'HKLM:\SOFTWARE\LumaBridge'
$InstallDir = Join-Path $env:ProgramFiles 'LumaBridge'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell (Run as administrator).'
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process -Name 'LumaBridge-RAM' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $InstallDir 'LumaBridge-RAM.exe'), (Join-Path $InstallDir 'SmbusPIIX4.bin') `
        -Force -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $VersionKey -Name RamTaskVersion -ErrorAction SilentlyContinue
    Write-Host 'Removed RAM lighting.'
    exit 0
}

$appDir = Split-Path $PSScriptRoot
$helper = Join-Path $appDir 'LumaBridge-RAM.exe'
if (-not (Test-Path $helper)) { throw "LumaBridge-RAM.exe not found next to LumaBridge.exe ($appDir)." }
$module = @((Join-Path $appDir 'SmbusPIIX4.bin'), (Join-Path $InstallDir 'SmbusPIIX4.bin')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $module) {
    throw 'SmbusPIIX4.bin not found. Download it from https://github.com/namazso/PawnIO.Modules/releases and put it next to LumaBridge.exe.'
}
if (-not (Test-Path (Join-Path $env:ProgramFiles 'PawnIO\PawnIOLib.dll'))) {
    throw 'The PawnIO driver is not installed. Install it from https://pawnio.eu first.'
}

# Stop a running helper so its file can be replaced.
Get-Process -Name 'LumaBridge-RAM' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item -Path $helper -Destination (Join-Path $InstallDir 'LumaBridge-RAM.exe') -Force
if ($module -ne (Join-Path $InstallDir 'SmbusPIIX4.bin')) {
    Copy-Item -Path $module -Destination (Join-Path $InstallDir 'SmbusPIIX4.bin') -Force
}

$action = New-ScheduledTaskAction -Execute (Join-Path $InstallDir 'LumaBridge-RAM.exe') -WorkingDirectory $InstallDir
# SYSTEM: can open the PawnIO driver; runs in the background, no window. No time limit: the
# helper lives as long as LumaBridge and exits by itself.
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $action -Principal $principal `
    -Settings $settings -Force `
    -Description 'LumaBridge: lights HyperX / Kingston FURY RGB memory while LumaBridge runs.' |
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
Set-ItemProperty -Path $VersionKey -Name RamTaskVersion -Value $TaskVersion -Type DWord
Write-Host "Registered task $TaskPath$TaskName (runs $InstallDir\LumaBridge-RAM.exe as SYSTEM; $($id.Name) may start it)"
