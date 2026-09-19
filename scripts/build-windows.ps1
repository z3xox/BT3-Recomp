# Thin wrapper: build + package the Windows release with the single games/bt3/setup.py.
#
#   scripts\build-windows.ps1 -Iso "C:\path\bt3-usa.iso" -Jobs 8
#   scripts\build-windows.ps1 -Iso ... -NoPackage      # stage tree only
#   scripts\build-windows.ps1 -SkipSetup               # reuse games/bt3/work
#
# Every other setup.py flag can be appended verbatim (e.g. --no-gate --dry-run).
[CmdletBinding()]
param(
    [string]$Iso,
    [int]$Jobs = 0,
    [string]$Output,
    [switch]$SkipSetup,
    [switch]$NoPackage,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra
)
$ErrorActionPreference = 'Stop'
$ROOT = Split-Path -Parent $PSScriptRoot

$argv = @("$ROOT\games\bt3\setup.py")
if ($Iso) { $argv += $Iso }
if ($Jobs -gt 0) { $argv += @('--jobs', "$Jobs") }
if ($Output) { $argv += @('--output', $Output) }
if ($SkipSetup) { $argv += '--skip-setup' }
if ($NoPackage) { $argv += '--no-package' }
if ($Extra) { $argv += $Extra }

Write-Host "== scripts/build-windows.ps1 -> python $($argv -join ' ')"
& python @argv
exit $LASTEXITCODE
