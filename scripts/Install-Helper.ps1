<#
.SYNOPSIS
    Sets up (or removes) LumaBridge's hardware access: RAM lighting and the built-in sensors.

.DESCRIPTION
    RAM lighting (HyperX / Kingston FURY RGB DDR4), fan speeds and CPU / board temperatures
    sit behind hardware only administrators can reach. LumaBridge reaches them through
    PawnIO, a signed driver made for this, bundled with LumaBridge (pawnio\, unmodified).

    This script (run once, elevated - the app runs it from Devices > Hardware access > Set up):
      1. installs the PawnIO driver silently from pawnio\PawnIO_setup.exe, unless PawnIO 2
         or later is already installed;
      2. copies LumaBridge-Helper.exe and the PawnIO modules it uses into
         %ProgramFiles%\LumaBridge, a folder only administrators can change, so nobody can
         swap the file the task runs;
      3. registers a scheduled task that runs the helper as SYSTEM, and gives your account the
         right to *start* it (read + execute on the task);
      4. removes the older RAM-only helper (LumaBridge 0.5.x), if it's there.
    LumaBridge then starts the helper whenever it runs, without a UAC prompt. The helper
    reads sensors, only writes the RAM's lighting registers, and exits when LumaBridge exits.
    It logs to %ProgramData%\LumaBridge\helper.log.

    -Uninstall removes the helper and its task. PawnIO stays installed (other tools such as
    LibreHardwareMonitor or OpenRGB may use it); remove it under Settings > Apps if you like.

.EXAMPLE
    .\Install-Helper.ps1
    .\Install-Helper.ps1 -Uninstall
#>
[CmdletBinding()]
param([switch] $Uninstall)

$ErrorActionPreference = 'Stop'
$TaskPath = '\LumaBridge\'
$TaskName = 'Hardware helper'
# Bumped whenever the task or the helper changes; LumaBridge asks to set it up again when
# the recorded version is older (kHelperTaskVersion in src/app/integrations.h).
$TaskVersion = 2
$VersionKey = 'HKLM:\SOFTWARE\LumaBridge'
$InstallDir = Join-Path $env:ProgramFiles 'LumaBridge'
$Modules = 'SmbusPIIX4.bin', 'LpcIO.bin', 'AMDFamily17.bin', 'IntelMSR.bin'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell (Run as administrator).'
}

# The RAM-only helper of LumaBridge 0.5.x.
function Remove-OldRamHelper {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName 'RAM lighting' -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process -Name 'LumaBridge-RAM' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $InstallDir 'LumaBridge-RAM.exe') -Force -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $VersionKey -Name RamTaskVersion -ErrorAction SilentlyContinue
}

if ($Uninstall) {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process -Name 'LumaBridge-Helper' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-OldRamHelper
    Remove-Item -Path (Join-Path $InstallDir 'LumaBridge-Helper.exe') -Force -ErrorAction SilentlyContinue
    foreach ($m in $Modules + 'SmbusPIIX4.bin') { Remove-Item -Path (Join-Path $InstallDir $m) -Force -ErrorAction SilentlyContinue }
    Remove-ItemProperty -Path $VersionKey -Name HelperTaskVersion -ErrorAction SilentlyContinue
    Write-Host 'Removed the hardware helper (PawnIO stays installed).'
    exit 0
}

$appDir = Split-Path $PSScriptRoot
$pawnDir = Join-Path $appDir 'pawnio'
$helper = Join-Path $appDir 'LumaBridge-Helper.exe'
if (-not (Test-Path $helper)) { throw "LumaBridge-Helper.exe not found next to LumaBridge.exe ($appDir)." }
foreach ($m in $Modules) {
    if (-not (Test-Path (Join-Path $pawnDir "modules\$m"))) { throw "pawnio\modules\$m is missing from LumaBridge's folder - download LumaBridge again." }
}

# 1. PawnIO: installed (version 2 or later) or install it.
function Get-PawnIOVersion {
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO') {
        $p = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($p -and $p.DisplayVersion) {
            try { return [version] $p.DisplayVersion } catch { return [version] '0.0' }
        }
    }
    return $null
}
$installed = Get-PawnIOVersion
if ($installed -and $installed -ge [version] '2.0') {
    Write-Host "PawnIO $installed is installed."
} else {
    $setup = Join-Path $pawnDir 'PawnIO_setup.exe'
    if (-not (Test-Path $setup)) { throw 'pawnio\PawnIO_setup.exe is missing from LumaBridge''s folder - download LumaBridge again.' }
    if ($installed) {
        # The installer refuses to install over an older version: remove it first.
        Write-Host "Updating PawnIO $installed..."
        $p = Start-Process -FilePath $setup -ArgumentList '-uninstall', '-silent' -Wait -PassThru
        Write-Host "PawnIO uninstall: exit $($p.ExitCode)"
    }
    Write-Host 'Installing PawnIO (bundled, signed driver)...'
    $p = Start-Process -FilePath $setup -ArgumentList '-install', '-silent' -Wait -PassThru
    $after = Get-PawnIOVersion
    if (-not $after) { throw "Installing PawnIO failed (exit code $($p.ExitCode))." }
    Write-Host "PawnIO $after installed."
}

# 2. The helper and its modules, where only administrators can change them.
Get-Process -Name 'LumaBridge-Helper' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-OldRamHelper
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item -Path $helper -Destination (Join-Path $InstallDir 'LumaBridge-Helper.exe') -Force
foreach ($m in $Modules) { Copy-Item -Path (Join-Path $pawnDir "modules\$m") -Destination (Join-Path $InstallDir $m) -Force }
Copy-Item -Path (Join-Path $pawnDir 'modules\COPYING') -Destination (Join-Path $InstallDir 'PawnIO-modules-COPYING.txt') -Force

# 3. The task. SYSTEM: can open the PawnIO driver; runs in the background, no window. No time
# limit: the helper lives as long as LumaBridge and exits by itself.
$action = New-ScheduledTaskAction -Execute (Join-Path $InstallDir 'LumaBridge-Helper.exe') -WorkingDirectory $InstallDir
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $action -Principal $principal `
    -Settings $settings -Force `
    -Description 'LumaBridge: RAM lighting and fan / temperature sensors while LumaBridge runs.' |
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
Set-ItemProperty -Path $VersionKey -Name HelperTaskVersion -Value $TaskVersion -Type DWord
Write-Host "Registered task $TaskPath$TaskName (runs $InstallDir\LumaBridge-Helper.exe as SYSTEM; $($id.Name) may start it)"
