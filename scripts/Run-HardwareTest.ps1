<#
.SYNOPSIS
    Guided LumaBridge hardware test. Runs every check, asks what lit up, and produces one
    report (saved to the Desktop and copied to the clipboard) to paste back.

.DESCRIPTION
    Run it from the LumaBridge folder:
        powershell -ExecutionPolicy Bypass -File .\tools\Run-HardwareTest.ps1

    What it does:
      1. Collects system info: motherboard, Windows version, ASUS services, ASUS USB devices.
      2. Closes the Armoury Crate app so it doesn't repaint during the test. If run as
         administrator, it can also pause ASUS's lighting service (asks first).
      3. Runs aura-test.exe (Aura SDK) and aura-usb-test.exe info (direct USB).
      4. Lights each Aura USB channel red, one at a time, and asks what turned red.
      5. Restores everything and writes the report.

    Nothing is saved to the hardware. Reopening Armoury Crate or rebooting restores the
    normal lighting.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$dir = $PSScriptRoot
$report = New-Object System.Collections.Generic.List[string]

function Log([string] $text) {
    Write-Host $text
    $report.Add($text)
}

function Section([string] $title) {
    Log ''
    Log "=== $title ==="
}

# Runs one of the test tools and records its output and exit code.
function Run-Tool([string] $exe, [string[]] $toolArgs, [switch] $Quiet) {
    $path = Join-Path $dir $exe
    if (-not (Test-Path $path)) {
        Log "$exe not found in $dir"
        return
    }
    $out = & $path @toolArgs 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($Quiet) {
        if ($code -ne 0) { Log "> $exe $($toolArgs -join ' ')  (exit code $code)"; Log $out.TrimEnd() }
        return
    }
    Log "> $exe $($toolArgs -join ' ')"
    Log $out.TrimEnd()
    Log "exit code: $code"
}

function Ask([string] $question) {
    Write-Host ''
    $answer = Read-Host $question
    return $answer.Trim()
}

function Describe([string] $answer) {
    switch ($answer.ToLower()) {
        'm' { 'motherboard LEDs' }
        'f' { 'fans' }
        'b' { 'motherboard LEDs + fans' }
        'n' { 'nothing' }
        ''  { 'nothing (no answer)' }
        default { $answer }
    }
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host ''
Write-Host 'LumaBridge hardware test' -ForegroundColor Cyan
Write-Host 'Keep an eye on your PC: you will be asked what lights up.' -ForegroundColor Cyan

# ---- 1. System info ---------------------------------------------------------------------
Section 'System'
Log ("Date:        " + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$exe = @((Join-Path $dir 'LumaBridge.exe'), (Join-Path (Split-Path -Parent $dir) 'LumaBridge.exe')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if ($exe) { Log ("LumaBridge:  " + (Get-Item $exe).VersionInfo.ProductVersion) }
Log ("Windows:     " + [Environment]::OSVersion.VersionString)
$board = Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue
if ($board) { Log ("Motherboard: $($board.Manufacturer) $($board.Product)") }
Log ("Admin:       $isAdmin")

Section 'ASUS services'
Get-Service -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'Armoury|Aura|Lighting|ASUS|Asus' -or $_.DisplayName -match 'Armoury|Aura|Lighting|ASUS' } |
    ForEach-Object { Log ("{0,-40} {1,-8} {2}" -f $_.Name, $_.Status, $_.DisplayName) }

Section 'ASUS USB devices'
Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'VID_0B05' -and $_.InstanceId -match '^USB\\' } |
    ForEach-Object { Log ("{0,-35} {1}" -f $_.FriendlyName, $_.InstanceId) }

# ---- 2. Quiet Armoury Crate ---------------------------------------------------------------
Section 'Preparing'
$acNames = 'ArmouryCrate', 'ArmouryCrate.UserSessionHelper'
$acRunning = @(Get-Process -Name $acNames -ErrorAction SilentlyContinue)
if ($acRunning.Count) {
    $acRunning | Stop-Process -Force -ErrorAction SilentlyContinue
    Log "Closed the Armoury Crate app ($($acRunning.Count) process(es))."
} else {
    Log 'Armoury Crate app was not running.'
}

$paused = @()
if ($isAdmin) {
    $lighting = @(Get-Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'Lighting' -and $_.Status -eq 'Running' })
    if ($lighting.Count) {
        $a = Ask "Also pause ASUS's lighting service during the test? It is restarted at the end. [y/N]"
        if ($a -match '^[yY]') {
            foreach ($s in $lighting) {
                Stop-Service -Name $s.Name -Force -ErrorAction SilentlyContinue
                $paused += $s.Name
            }
            Log ("Paused services: " + ($paused -join ', '))
        } else {
            Log 'Lighting service left running.'
        }
    }
} else {
    Log 'Not running as administrator: ASUS lighting service left running.'
}
Start-Sleep -Seconds 2

try {
    # ---- 3. Tools ---------------------------------------------------------------------------
    Section 'Aura SDK (aura-test list)'
    Run-Tool 'aura-test.exe' @('list')

    Section 'Aura USB controller (aura-usb-test info)'
    Run-Tool 'aura-usb-test.exe' @('info')

    # ---- 4. Channel scan --------------------------------------------------------------------
    Section 'Channel scan'
    Run-Tool 'aura-usb-test.exe' @('color', '000000', 'all') -Quiet
    $dark = Ask 'All motherboard and fan lights should now be OFF. Are they? [y/n, or describe]'
    Log ("All off:     " + $dark)

    for ($ch = 0; $ch -le 7; $ch++) {
        Run-Tool 'aura-usb-test.exe' @('color', 'FF0000', "$ch") -Quiet
        $what = Ask "Channel $ch is now RED. What turned red?  [m] motherboard  [f] fans  [b] both  [n] nothing  (or type)"
        $blink = ''
        if ($what -and $what -ne 'n') {
            $blink = Ask 'Is it steady, or blinking/flickering?  [s] steady  [b] blinking'
            $blink = if ($blink -match '^[bB]') { ', blinking' } else { ', steady' }
        }
        Log ("Channel ${ch}:   " + (Describe $what) + $blink)
        Run-Tool 'aura-usb-test.exe' @('color', '000000', "$ch") -Quiet
    }

    Run-Tool 'aura-usb-test.exe' @('color', '00FF00', 'all') -Quiet
    $green = Ask 'Everything should now be GREEN. What is green?  [m] motherboard  [f] fans  [b] both  [n] nothing  (or type)'
    Log ("All green:   " + (Describe $green))
    Start-Sleep -Seconds 3
    $still = Ask 'After a few seconds: is it still green?  [y/n, or describe]'
    Log ("Stays green: " + $still)
}
finally {
    # ---- 5. Restore -------------------------------------------------------------------------
    Section 'Restore'
    foreach ($name in $paused) {
        Start-Service -Name $name -ErrorAction SilentlyContinue
        Log "Restarted service $name"
    }
    Log 'Open Armoury Crate again (or reboot) to get your normal lighting back.'

    $logFile = Join-Path $env:LOCALAPPDATA 'LumaBridge\lumabridge-app.log'
    if (Test-Path $logFile) {
        Section 'lumabridge-app.log (last 30 lines)'
        Get-Content $logFile -Tail 30 | ForEach-Object { Log $_ }
    }

    $text = $report -join "`r`n"
    $desktop = [Environment]::GetFolderPath('Desktop')
    if (-not $desktop -or -not (Test-Path $desktop)) { $desktop = $dir }
    $out = Join-Path $desktop 'LumaBridge-test-report.txt'
    Set-Content -Path $out -Value $text -Encoding UTF8
    try { Set-Clipboard -Value $text; $clip = $true } catch { $clip = $false }

    Write-Host ''
    Write-Host "Report saved to $out" -ForegroundColor Green
    if ($clip) { Write-Host 'It is also on your clipboard: just paste it (Ctrl+V).' -ForegroundColor Green }
}
