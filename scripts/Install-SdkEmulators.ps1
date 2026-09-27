<#
.SYNOPSIS
    Installs / removes the LumaBridge SDK emulators (Razer Chroma, Alienware LightFX,
    Corsair iCUE) and prepares the SteelSeries GameSense folder.

.DESCRIPTION
    The LumaBridge app calls this for you (Games page). You can also run it by hand.

    -Sdk Chroma    RzChromaSDK64.dll / RzChromaSDK.dll -> System32 / SysWOW64 (admin), or -GameDir
    -Sdk LightFX   LightFX.dll                         -> System32 / SysWOW64 (admin), or -GameDir
    -Sdk Corsair   replaces every CUESDK*.dll found in -GameDir (games ship their own copy)
    -Sdk GameSense creates %ProgramData%\SteelSeries\SteelSeries Engine 3 writable for users
                   (admin, once), so the app can publish its GameSense address there

    A vendor's own DLL (Authenticode-signed; LumaBridge's are not) is never overwritten
    without -Force, and is backed up first. -Uninstall restores backups.

.EXAMPLE
    .\Install-SdkEmulators.ps1 -Sdk Chroma
    .\Install-SdkEmulators.ps1 -Sdk Corsair -GameDir "D:\Games\SomeCorsairGame"
    .\Install-SdkEmulators.ps1 -Sdk LightFX -Uninstall
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Chroma', 'LightFX', 'Corsair', 'GameSense')] [string] $Sdk,
    [string] $GameDir,
    [switch] $Force,
    [switch] $Uninstall
)
$ErrorActionPreference = 'Stop'

$SearchDirs = @($PSScriptRoot, (Split-Path -Parent $PSScriptRoot)) | Select-Object -Unique

function Find-Built([string] $name) {
    foreach ($d in $SearchDirs) {
        foreach ($p in @("$d\integrations\$name", "$d\integrations\x86\$name", "$d\$name", "$d\x64\$name", "$d\x86\$name",
                         "$d\build\x64\Release\$name", "$d\build\x86\Release\$name")) {
            if (Test-Path $p) { return (Resolve-Path $p).Path }
        }
    }
    return $null
}

function Test-VendorSigned([string] $path) {
    (Get-AuthenticodeSignature $path).Status -eq 'Valid'
}

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This needs an elevated PowerShell (Run as administrator), or use -GameDir.'
    }
}

function Install-One([string] $src, [string] $dst) {
    if (-not $src) { Write-Warning "Built DLL for $dst not found - skipped."; return }
    if (Test-Path $dst) {
        if (Test-VendorSigned $dst) {
            if (-not $Force) { throw "$dst belongs to the vendor's software. Re-run with -Force to back it up and replace it." }
        }
        if (-not (Test-Path "$dst.lumabridge-backup")) {
            Copy-Item $dst "$dst.lumabridge-backup" -Force
            Write-Host "Backed up $dst"
        }
    }
    Copy-Item -Force $src $dst
    Write-Host "Installed $dst"
}

function Uninstall-One([string] $dst) {
    if (Test-Path "$dst.lumabridge-backup") {
        Move-Item -Force "$dst.lumabridge-backup" $dst
        Write-Host "Restored original $dst"
    } elseif ((Test-Path $dst) -and -not (Test-VendorSigned $dst)) {
        Remove-Item -Force $dst
        Write-Host "Removed $dst"
    }
}

switch ($Sdk) {
    'GameSense' {
        Assert-Admin
        $dir = Join-Path $env:ProgramData 'SteelSeries\SteelSeries Engine 3'
        New-Item -ItemType Directory -Force $dir | Out-Null
        # *S-1-5-32-545 = BUILTIN\Users (language independent)
        & icacls $dir /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "icacls failed ($LASTEXITCODE)" }
        Write-Host "Users can now write $dir"
    }

    'Corsair' {
        if (-not $GameDir) { throw 'Corsair needs -GameDir: games ship their own CUESDK DLL.' }
        $found = @(Get-ChildItem -Path $GameDir -Recurse -Depth 3 -File -Include 'CUESDK*.dll', 'CUESDK*.dll.lumabridge-backup' -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.FullName -replace '\.lumabridge-backup$', '' } | Select-Object -Unique)
        $v4 = @(Get-ChildItem -Path $GameDir -Recurse -Depth 3 -File -Filter 'iCUESDK*.dll' -ErrorAction SilentlyContinue)
        if ($v4.Count) { Write-Warning "This game uses iCUE SDK 4 ($($v4[0].Name)), which LumaBridge does not emulate yet." }
        if (-not $found.Count) {
            if (-not $v4.Count) { throw "No CUESDK*.dll found under $GameDir - does this game support Corsair lighting?" }
            exit 2
        }
        foreach ($f in $found) {
            if ($Uninstall) { Uninstall-One $f; continue }
            $name = if ((Split-Path -Leaf $f) -match 'x64') { 'CUESDK.x64_2019.dll' } else { 'CUESDK_2019.dll' }
            # A game's CUESDK is Corsair-signed; replacing it is the whole point here.
            $script:Force = $true
            Install-One (Find-Built $name) $f
        }
    }

    default {
        $files = if ($Sdk -eq 'Chroma') { @{ x64 = 'RzChromaSDK64.dll'; x86 = 'RzChromaSDK.dll' } }
                 else { @{ x64 = 'LightFX.dll'; x86 = 'LightFX.dll' } }
        if ($GameDir) {
            # Game folders: install the architecture that matches the game's own copy, else x64.
            $targets = @(@{ Src = (Find-Built $files.x64); Dst = (Join-Path $GameDir $files.x64) })
            if ($Sdk -eq 'Chroma') { $targets += @{ Src = $null; Dst = (Join-Path $GameDir $files.x86) } }
        } else {
            Assert-Admin
            if (-not [Environment]::Is64BitProcess) { throw 'Run from 64-bit PowerShell.' }
            $targets = @(@{ Src = (Find-Built $files.x64); Dst = "$env:WINDIR\System32\$($files.x64)" })
            $x86 = $SearchDirs | ForEach-Object { "$_\integrations\x86\$($files.x86)", "$_\x86\$($files.x86)" } |
                   Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($x86) { $targets += @{ Src = $x86; Dst = "$env:WINDIR\SysWOW64\$($files.x86)" } }
        }
        foreach ($t in $targets) {
            if ($Uninstall) { Uninstall-One $t.Dst }
            elseif ($t.Src) { Install-One $t.Src $t.Dst }
        }
        if (-not $Uninstall) {
            $cfgDir = Join-Path $env:LOCALAPPDATA 'LumaBridge'
            if (-not (Test-Path "$cfgDir\LumaBridge.ini")) {
                New-Item -ItemType Directory -Force $cfgDir | Out-Null
                $example = $SearchDirs | ForEach-Object { "$_\LumaBridge.ini.example", "$_\config\LumaBridge.ini.example" } |
                           Where-Object { Test-Path $_ } | Select-Object -First 1
                if ($example) { Copy-Item $example "$cfgDir\LumaBridge.ini" }
            }
        }
    }
}
Write-Host 'Done.'
