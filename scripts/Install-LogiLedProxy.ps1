<#
.SYNOPSIS
    Points games at the LumaBridge LogiLed proxy.

.DESCRIPTION
    Games built on Logitech's LogitechLEDLib.lib find the LED SDK DLL through the registry
    value HKCR\CLSID\{a6519e67-7632-4375-afdf-caa889744403}\ServerBinary (normally
    C:\Program Files\LGHUB\sdk_legacy_led_x64.dll). This script redirects that value to the
    proxy; the proxy then loads the real DLL itself, so Logitech devices keep working.

    -Scope User     (default) writes an HKCU override. No admin, nothing Logitech-owned is
                    touched, and -Uninstall simply deletes it. Works if the game reads the
                    merged HKCR view (HKCU wins over HKLM) -- try this first.
    -Scope Machine  rewrites the HKLM value (admin). Use if the User scope has no effect.
                    The original path is saved to %LOCALAPPDATA%\LumaBridge\LumaBridge.ini
                    (RealDllPath) and to
                    logiled-registry-backup.txt. G HUB updates may reset it.

    -GameDir <dir>  instead copies the proxy into a game folder under the DLL name the game
                    loads (search-order hijack, for games that ship their own copy).

.EXAMPLE
    .\Install-LogiLedProxy.ps1                          # HKCU redirect, x64
    .\Install-LogiLedProxy.ps1 -Scope Machine
    .\Install-LogiLedProxy.ps1 -GameDir "D:\Games\Battlefield 1" -DllName LogitechLedEnginesWrapper.dll
    .\Install-LogiLedProxy.ps1 -Uninstall [-Scope Machine]
#>
[CmdletBinding()]
param(
    [ValidateSet('User', 'Machine')] [string] $Scope = 'User',
    [ValidateSet('x64', 'x86')] [string] $Arch = 'x64',
    [string] $ProxyPath,
    [string] $GameDir,
    [string] $DllName = 'LogitechLedEnginesWrapper.dll',
    [switch] $Uninstall
)
$ErrorActionPreference = 'Stop'

$Clsid = '{a6519e67-7632-4375-afdf-caa889744403}'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $ProxyPath) {
    $candidates = @(
        (Join-Path $Root "LumaBridge_$Arch.dll"),
        (Join-Path $PSScriptRoot "LumaBridge_$Arch.dll")
    )
    $ProxyPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Get-KeyPath([string] $scope, [string] $arch) {
    $hive = if ($scope -eq 'User') { 'HKCU:' } else { 'HKLM:' }
    # 32-bit processes read the WOW6432Node view of Classes.
    $wow = if ($arch -eq 'x86') { '\WOW6432Node' } else { '' }
    "$hive\SOFTWARE\Classes$wow\CLSID\$Clsid\ServerBinary"
}

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This mode needs an elevated PowerShell (Run as administrator).'
    }
}

function Set-IniValue([string] $iniPath, [string] $section, [string] $key, [string] $value) {
    $lines = if (Test-Path $iniPath) { [Collections.Generic.List[string]] (Get-Content $iniPath) }
             else { [Collections.Generic.List[string]]::new() }
    $inSection = $false; $sectionIdx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+)\]\s*$') {
            if ($inSection) { break }
            $inSection = ($Matches[1] -eq $section)
            if ($inSection) { $sectionIdx = $i }
        } elseif ($inSection -and $lines[$i] -match "^\s*$key\s*=") {
            $lines[$i] = "$key=$value"; Set-Content $iniPath $lines; return
        }
    }
    if ($sectionIdx -ge 0) { $lines.Insert($sectionIdx + 1, "$key=$value") }
    else { $lines.Add(''); $lines.Add("[$section]"); $lines.Add("$key=$value") }
    Set-Content $iniPath $lines
}

# ---- Game folder mode -------------------------------------------------------------------
if ($GameDir) {
    $target = Join-Path $GameDir $DllName
    if ($Uninstall) {
        if (Test-Path "$target.lumabridge-backup") {
            Move-Item -Force "$target.lumabridge-backup" $target
            Write-Host "Restored original $target"
        } elseif (Test-Path $target) {
            Remove-Item $target; Write-Host "Removed $target"
        }
        return
    }
    if (-not $ProxyPath) { throw "LumaBridge_$Arch.dll not found; pass -ProxyPath." }
    if ((Test-Path $target) -and -not (Test-Path "$target.lumabridge-backup")) {
        Copy-Item $target "$target.lumabridge-backup"
        Write-Host "Backed up the game's own $DllName; set RealDllPath to it in LumaBridge.ini if you want it used."
    }
    Copy-Item -Force $ProxyPath $target
    Write-Host "Copied proxy to $target"
    return
}

# ---- Registry mode ----------------------------------------------------------------------
$machineKey = Get-KeyPath 'Machine' $Arch
$key = Get-KeyPath $Scope $Arch
$backup = Join-Path $Root 'logiled-registry-backup.txt'

if ($Uninstall) {
    if ($Scope -eq 'User') {
        # Remove only our override; leave the CLSID key if something else lives there.
        Remove-Item -Path $key -Force -ErrorAction SilentlyContinue
        Write-Host "Removed HKCU override $key"
    } else {
        Assert-Admin
        if (-not (Test-Path $backup)) { throw "No backup found at $backup - restore ServerBinary manually (reinstalling G HUB also fixes it)." }
        $orig = (Get-Content $backup -Raw).Trim()
        Set-Item -Path $machineKey -Value $orig
        Write-Host "Restored $machineKey = $orig"
    }
    return
}

if (-not $ProxyPath) { throw "LumaBridge_$Arch.dll not found; pass -ProxyPath." }
$ProxyPath = (Resolve-Path $ProxyPath).Path

$current = $null
if (Test-Path $machineKey) { $current = (Get-Item $machineKey).GetValue('') }
if (-not $current) {
    Write-Warning "No Logitech LED SDK registration at $machineKey. Is G HUB (or LGS) installed? Continuing: the proxy will run Aura-only."
}

if ($Scope -eq 'User') {
    New-Item -Path $key -Force | Out-Null
    Set-Item -Path $key -Value $ProxyPath
    Write-Host "HKCU override: $key = $ProxyPath"
} else {
    Assert-Admin
    if ($current -and ($current -ne $ProxyPath)) {
        Set-Content $backup $current
        $userIni = Join-Path $env:LOCALAPPDATA 'LumaBridge\LumaBridge.ini'
        New-Item -ItemType Directory -Force (Split-Path $userIni) | Out-Null
        Set-IniValue $userIni 'Logitech' 'RealDllPath' $current
        Write-Host "Saved original ($current) to $backup and LumaBridge.ini"
    }
    Set-Item -Path $machineKey -Value $ProxyPath
    Write-Host "HKLM: $machineKey = $ProxyPath"
}
Write-Host 'Done. Start the game; LumaBridge shows it on the Lighting page.'
