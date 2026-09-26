<#
.SYNOPSIS
    Installs the LumaBridge Razer Chroma emulator so Chroma-enabled games drive Aura.

.DESCRIPTION
    Chroma games LoadLibrary("RzChromaSDK64.dll") (x86 games: "RzChromaSDK.dll"). Windows
    finds it in the game folder first, then System32 / SysWOW64, which is where Razer
    Synapse installs the real one.

    Default: copies the emulator into System32 (x64) and SysWOW64 (x86) so *every* Chroma
    game sees it. Needs admin. Refuses to overwrite a real Razer DLL (i.e. if Synapse or the
    Chroma SDK Core is installed) unless -Force, which backs it up first.

    -GameDir <dir>: copy into one game's folder instead (no admin, no system-wide effect).

    Also copies LumaBridge.ini to %LOCALAPPDATA%\LumaBridge\ if none is there yet, since a
    DLL in System32 has no folder of its own to keep a config in.

.EXAMPLE
    .\Install-ChromaEmulator.ps1
    .\Install-ChromaEmulator.ps1 -GameDir "D:\Games\SomeChromaGame"
    .\Install-ChromaEmulator.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $GameDir,
    [switch] $Force,
    [switch] $Uninstall
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

function Find-Built([string] $name, [string] $arch) {
    @((Join-Path $Root $name), (Join-Path $Root "$arch\$name"), (Join-Path $Root "build\$arch\Release\$name")) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Test-RazerSigned([string] $path) {
    $sig = Get-AuthenticodeSignature $path
    return ($sig.Status -eq 'Valid' -and $sig.SignerCertificate.Subject -match 'Razer')
}

function Install-One([string] $src, [string] $dst) {
    if (-not $src) { Write-Warning "Built DLL for $dst not found - skipped."; return }
    if (Test-Path $dst) {
        if (Test-RazerSigned $dst) {
            if (-not $Force) {
                throw "$dst is Razer's own DLL (Synapse installed?). Re-run with -Force to back it up and replace it."
            }
            Copy-Item $dst "$dst.razer-backup" -Force
            Write-Host "Backed up Razer DLL to $dst.razer-backup"
        }
    }
    Copy-Item -Force $src $dst
    Write-Host "Installed $dst"
}

function Uninstall-One([string] $dst) {
    if (Test-Path "$dst.razer-backup") {
        Move-Item -Force "$dst.razer-backup" $dst
        Write-Host "Restored Razer DLL $dst"
    } elseif ((Test-Path $dst) -and -not (Test-RazerSigned $dst)) {
        Remove-Item -Force $dst
        Write-Host "Removed $dst"
    }
}

if ($GameDir) {
    $targets = @(@{ Src = (Find-Built 'RzChromaSDK64.dll' 'x64'); Dst = (Join-Path $GameDir 'RzChromaSDK64.dll') },
                 @{ Src = (Find-Built 'RzChromaSDK.dll' 'x86');   Dst = (Join-Path $GameDir 'RzChromaSDK.dll') })
} else {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'System-wide install needs an elevated PowerShell (or use -GameDir).'
    }
    # "Sysnative" isn't needed: this script must run in 64-bit PowerShell, check it.
    if (-not [Environment]::Is64BitProcess) { throw 'Run from 64-bit PowerShell.' }
    $targets = @(@{ Src = (Find-Built 'RzChromaSDK64.dll' 'x64'); Dst = "$env:WINDIR\System32\RzChromaSDK64.dll" },
                 @{ Src = (Find-Built 'RzChromaSDK.dll' 'x86');   Dst = "$env:WINDIR\SysWOW64\RzChromaSDK.dll" })
}

foreach ($t in $targets) {
    if ($Uninstall) { Uninstall-One $t.Dst } else { Install-One $t.Src $t.Dst }
}

if (-not $Uninstall) {
    $cfgDir = Join-Path $env:LOCALAPPDATA 'LumaBridge'
    $cfg = Join-Path $cfgDir 'LumaBridge.ini'
    if (-not (Test-Path $cfg)) {
        New-Item -ItemType Directory -Force $cfgDir | Out-Null
        $example = @((Join-Path $Root 'LumaBridge.ini'), (Join-Path $Root 'LumaBridge.ini.example'),
                     (Join-Path $Root 'config\LumaBridge.ini.example')) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($example) { Copy-Item $example $cfg; Write-Host "Config: $cfg" }
    }
    Write-Host 'Done. Start a Chroma game, then check %LOCALAPPDATA%\LumaBridge\lumabridge-chroma.log.'
}
