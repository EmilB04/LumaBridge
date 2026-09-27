<#
.SYNOPSIS
    Writes (or removes) one small game config file for LumaBridge's built-in game feeds.

.DESCRIPTION
    Used by the LumaBridge app (Integrations page) when a game's folder isn't writable
    without administrator rights (e.g. under Program Files). Only these files are handled:
      <Counter-Strike 2>\game\csgo\cfg\gamestate_integration_lumabridge.cfg
      <Rocket League>\TAGame\Config\DefaultStatsAPI.ini
    An existing file is backed up once to <file>.lumabridge-backup before it is changed.

.EXAMPLE
    .\Write-GameFile.ps1 -Path '...\gamestate_integration_lumabridge.cfg' -ContentBase64 '...'
    .\Write-GameFile.ps1 -Path '...\gamestate_integration_lumabridge.cfg' -Remove
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Path,
    [string] $ContentBase64,
    [switch] $Remove
)
$ErrorActionPreference = 'Stop'

$name = Split-Path -Leaf $Path
if ($name -ne 'gamestate_integration_lumabridge.cfg' -and $name -ne 'DefaultStatsAPI.ini') {
    throw "Refusing to write $Path (not a LumaBridge game config file)"
}
if ($Remove) {
    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    Write-Host "Removed $Path"
    exit 0
}
$backup = "$Path.lumabridge-backup"
if ((Test-Path -LiteralPath $Path) -and -not (Test-Path -LiteralPath $backup)) {
    Copy-Item -LiteralPath $Path -Destination $backup
}
New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
[IO.File]::WriteAllBytes($Path, [Convert]::FromBase64String($ContentBase64))
Write-Host "Wrote $Path"
