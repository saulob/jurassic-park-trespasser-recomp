<#
.SYNOPSIS
  Maintainers only: regenerates one lifted build from an original executable and packs it into lifts\.
.DESCRIPTION
  The original executables are development inputs. Nothing else in the port (build, launcher, runtime, release) opens
  or needs one: the lifts are the project's own artifacts, lifts\<lift>.tar.xz with lifts\manifest.json. Run this only when
  the lifter, the override or hook lists or a guest map change; then commit the changed archive and manifest.
  Steps: the executable's SHA-256 is checked against config; the analysis (function catalog, RTTI, about 15 minutes) runs
  when its outputs are missing or with -Reanalyze; tools\run_lift.py lifts it into src\recomp\<gen dir>; tools\lifts.py
  packs that folder.
.PARAMETER Profile
  en-116 (English and Russian share it), es-117 or ptbr-117.
.PARAMETER Source
  Path of the original executable of that build (for example tpassp6.exe from the disc).
.PARAMETER Reanalyze
  Forces the analysis even when its outputs exist.
.EXAMPLE
  .\tools\generate-lift.ps1 -Profile es-117 -Source D:\dev\es\tpassp6.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('en-116', 'es-117', 'ptbr-117')][string]$Profile,
    [Parameter(Mandatory = $true)][string]$Source,
    [switch]$Reanalyze
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
if (-not (Test-Path -LiteralPath $Source)) { Stop-Tool "$Source does not exist." }
$config = if ($Profile -eq 'en-116') { 'config\target.json' } else { "config\profiles\$Profile.json" }
$facts = Get-Content -LiteralPath (Join-Path $root $config) -Raw | ConvertFrom-Json
if ((Get-Sha256 $Source) -ne $facts.sha256) { Stop-Tool "$Source is not the $Profile build ($config names SHA-256 $($facts.sha256))." }

$lift = if ($Profile -eq 'en-116') { 'lift116' } else { $facts.lift }
$analysis = if ($Profile -eq 'en-116') { 'analysis' } else { "analysis\$Profile" }
$env:JPT_PROFILE_EXE = (Resolve-Path -LiteralPath $Source).Path
Assert-PythonModules @('capstone', 'pefile')

$missing = @('functions.json', 'rtti.json', 'rtti_seeds.json') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $root "$analysis\$_")) }
if ($Reanalyze -or $missing) {
    Write-Step "Analysing $Profile (about 15 minutes)"
    if ((Invoke-Python 'tools\analyze.py' @('--profile', $Profile)) -ne 0) { Stop-Tool 'Analysis failed' }
}
Write-Step "Lifting $Profile"
if ((Invoke-Python 'tools\run_lift.py' @('--profile', $Profile)) -ne 0) { Stop-Tool "Lift of $Profile failed or is incomplete" }
Write-Step "Packing $lift"
if ((Invoke-Python 'tools\lifts.py' @('pack', $lift)) -ne 0) { Stop-Tool "Packing $lift failed" }
Write-Host "Done: commit lifts\$lift.tar.xz and lifts\manifest.json."
