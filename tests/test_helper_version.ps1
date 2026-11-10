# Upgrade prompts must track the installed helper executable as well as its wire layout.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ipc = Get-Content (Join-Path $root 'src/app/peripherals/helper_ipc.h') -Raw
$installer = Get-Content (Join-Path $root 'scripts/Install-Helper.ps1') -Raw
$wire = [regex]::Match($ipc, 'kTaskVersion\s*=\s*(\d+)').Groups[1].Value
$task = [regex]::Match($installer, '\$TaskVersion\s*=\s*(\d+)').Groups[1].Value
if (-not $wire -or $wire -ne $task) { throw 'Helper code and installer versions disagree.' }
$app = Get-Content (Join-Path $root 'src/app/integrations.h') -Raw
$helper = Get-Content (Join-Path $root 'src/helper/main.cpp') -Raw
if ($app -notmatch 'kHelperTaskVersion\s*=\s*helper::kTaskVersion' -or
    $helper -notmatch 'taskVersion\s*=\s*helper::kTaskVersion') { throw 'Helper consumers must use the shared task version.' }
Write-Output "Helper upgrade version $task is consistent."
