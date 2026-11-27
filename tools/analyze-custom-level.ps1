<#
.SYNOPSIS
  Prints what the Custom Levels import makes of a package (ZIP, 7z or folder), or of every package below a folder. Nothing is
  installed and no window opens: it runs trespasser_recomp.exe, the same code the launcher imports with.

.PARAMETER Path
  The archive or folder.

.PARAMETER All
  Path is a folder of packages: every one below it is analysed, and the totals and the most frequent problems are printed.

.PARAMETER Report
  With -All, the folder the report is written to as well, as text and JSON (default: build\logs).

.PARAMETER Json
  The manifest the import would write, or the structured report, instead of the text.

.PARAMETER Exe
  Another trespasser_recomp.exe (default: the one built by tools\build.ps1).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string]$Path,
    [switch]$All,
    [string]$Report = '',
    [switch]$Json,
    [string]$Exe = ''
)

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\cmake\msvc-x86\trespasser_recomp.exe' }
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { Write-Host "FAILED: $Exe does not exist; build it with tools\build.ps1." -ForegroundColor Red; exit 1 }
$source = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
$arguments = if ($All) { @('--analyze-custom-levels', $source, '--report', $(if ($Report) { $Report } else { Join-Path $root 'build\logs' })) } else { @('--analyze-custom-level', $source) }
if ($Json) { $arguments += '--json' }
& $Exe @arguments
exit $LASTEXITCODE
