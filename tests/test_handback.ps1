# Exercise the scheduled command with fake service, registry and PnP operations.
# No live service, process or device is changed by these tests.
$ErrorActionPreference = 'Stop'
$errors = $null
$tokens = $null
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../scripts/Install-HandbackTask.ps1') -Raw
$ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Handback installer does not parse.' }
$assignment = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$command'
}, $true)
$body = $assignment.Right.Expression.Value
if (-not $body) { throw 'Scheduled command not found.' }
$null = [System.Management.Automation.Language.Parser]::ParseInput($body, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Scheduled command does not parse.' }
# Replace only the absolute executable invocation and exit, keeping the actual task logic.
$body = $body.Replace('& "$env:SystemRoot\System32\sc.exe"', 'Invoke-FakeServiceConfig').Replace('exit 1', "throw 'Task reported failure'")

function Add-Content { param($Path, $Value) }
function New-Item { param($ItemType, $Force, $Path) }
function Test-Path { param($LiteralPath) return $true }
function Get-Process { param($Name, $ErrorAction) }
function Get-Service { param($Name, $ErrorAction) return $script:fakeService }
function Get-ItemProperty {
    param($LiteralPath, $Name, $ErrorAction)
    if ($Name -eq 'Start') { return [pscustomobject]@{ Start = 2 } }
    return [pscustomobject]@{ LightingServiceOriginalStart = $null; LightingServicePausedByLumaBridge = 1 }
}
function Set-ItemProperty {
    param($LiteralPath, $Name, $Type, $Value)
    $script:events.Add("save:$Name")
}
function Remove-ItemProperty {
    param($LiteralPath, $Name, $ErrorAction)
    $script:events.Add("clear:$Name")
}
function Invoke-FakeServiceConfig {
    $mode = $args[-1]
    $script:events.Add("config:$mode")
    $global:LASTEXITCODE = if ($script:scenario -eq 'restore fails' -and $mode -eq 'auto') { 1 } else { 0 }
}
function Stop-Service {
    param($Name, [switch]$Force, $ErrorAction)
    $script:events.Add('stop')
    if ($script:scenario -eq 'stop fails') { throw 'Service stop failed' }
    $script:fakeService.Status = 'Stopped'
}
function Start-Service {
    param($Name, $ErrorAction)
    $script:events.Add('start')
    if ($script:scenario -eq 'start fails') { throw 'Service start failed' }
}
function Get-PnpDevice {
    param([switch]$PresentOnly, $ErrorAction)
    return [pscustomobject]@{ InstanceId = 'USB\VID_0B05&PID_1939\TEST' }
}
function pnputil.exe {
    $script:events.Add('reset')
    $global:LASTEXITCODE = if ($script:scenario -eq 'reset fails') { 1 } else { 0 }
    return 'Simulated controller reset'
}
function Assert($condition, $message) { if (-not $condition) { throw $message } }

foreach ($script:scenario in 'success', 'stop fails', 'reset fails', 'restore fails', 'start fails') {
    $script:events = New-Object 'System.Collections.Generic.List[string]'
    $script:fakeService = [pscustomobject]@{ Status = 'Running' }
    $script:fakeService | Add-Member ScriptMethod WaitForStatus { param($status, $timeout) }
    $failed = $false
    try { & ([scriptblock]::Create($body)) } catch { $failed = $true }
    Assert ($failed -eq ($script:scenario -ne 'success')) "Incorrect result for $script:scenario"
    Assert ($script:events.IndexOf('save:LightingServiceOriginalStart') -lt $script:events.IndexOf('config:disabled')) 'Original setting must be saved before disabling starts.'
    Assert ($script:events.Contains('config:auto')) "Original startup must be restored after $script:scenario"
    if ($script:scenario -eq 'stop fails') {
        Assert (-not $script:events.Contains('reset')) 'A failed stop must prevent a controller reset.'
    } else {
        Assert ($script:events.IndexOf('stop') -lt $script:events.IndexOf('reset')) 'Stop ASUS lighting before resetting the USB controller.'
    }
    if ($script:scenario -eq 'restore fails') {
        Assert (-not $script:events.Contains('clear:LightingServiceOriginalStart')) 'Keep the saved setting after a failed restore.'
        Assert (-not $script:events.Contains('start')) 'Do not start a service whose configuration failed to restore.'
    } else {
        Assert ($script:events.Contains('start')) "ASUS lighting must be restarted after $script:scenario"
    }
    if ($script:scenario -eq 'start fails') {
        Assert (-not $script:events.Contains('clear:LightingServicePausedByLumaBridge')) 'Keep the ownership marker after a failed restart.'
    }
}
Write-Output 'Handback success, stop/reset failure, configuration restore failure and service restart failure checks passed.'
