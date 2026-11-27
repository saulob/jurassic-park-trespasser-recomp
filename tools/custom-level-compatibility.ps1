<#
.SYNOPSIS
  The compatibility of every custom level package below a folder: a summary here and CUSTOM_LEVEL_COMPATIBILITY.md in that folder.
  Nothing is installed, no window opens and the packages are only read.

.PARAMETER Path
  The folder of packages (ZIP, 7z, RAR, folders).

.PARAMETER Exe
  custom_level_scan.exe, a target of CMakeLists.txt: ninja -C <build dir> custom_level_scan

.PARAMETER Data
  The data folder of an installed game (default: game\versions\en-116\data).

.PARAMETER Baseline
  A state file of an earlier run, to show what changed.

.PARAMETER State
  Where this run's state is written.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string]$Path,
    [Parameter(Mandatory)][string]$Exe,
    [string]$Data = '',
    [string]$Baseline = '',
    [string]$State = ''
)

$root = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
if (-not $Data) { $Data = Join-Path (Split-Path -Parent $PSScriptRoot) 'game\versions\en-116\data' }
$arguments = @($root, '--data', $Data, '--md', (Join-Path $root 'CUSTOM_LEVEL_COMPATIBILITY.md'))
if ($Baseline) { $arguments += @('--baseline', $Baseline) }
if ($State) { $arguments += @('--state', $State) }

& $Exe @arguments 2>$null
exit $LASTEXITCODE
