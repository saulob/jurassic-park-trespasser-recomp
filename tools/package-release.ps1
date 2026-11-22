<#
.SYNOPSIS
  Prepares the Windows release in dist\: the ZIP the updater downloads and its .sha256, built from Release and verified
  as the real asset. Local only: nothing is published, tagged or uploaded.
.DESCRIPTION
  1. tools\package.ps1 (Release build unless -SkipBuild, staging, ZIP, .sha256, tools\verify-release.ps1 on both).
  2. Gates on the finished files: the asset names are the ones config\release.json gives the launcher's updater, the ZIP
     checksum matches, and the version in the name is the project VERSION.
  Upload the ZIP and its .sha256 to one GitHub release tagged v<version> by hand; the launcher looks for exactly these
  two assets. The Linux AppImage is packaged by its own branch and is not part of this script.
.PARAMETER SkipBuild
  Package the existing Release build.
.PARAMETER Clean
  Clean Release build.
.EXAMPLE
  .\tools\package-release.ps1
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Clean
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$release = Get-Content -LiteralPath (Join-Path $root 'config\release.json') -Raw | ConvertFrom-Json
if ((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw) -notmatch '(?m)^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$name = "$($release.asset_prefix)-v$version-$($release.windows_suffix)"
$zip = Join-Path $root "dist\$name.zip"

$arguments = @()
if ($SkipBuild) { $arguments += '-SkipBuild' }
if ($Clean) { $arguments += '-Clean' }
& (Join-Path $PSScriptRoot 'package.ps1') @arguments
if ($LASTEXITCODE -ne 0) { Stop-Tool "tools\package.ps1 failed with exit code $LASTEXITCODE" }

Write-Step 'Final gates'
foreach ($file in $zip, "$zip.sha256") {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Stop-Tool "missing $file" }
}
$recorded = (([IO.File]::ReadAllText("$zip.sha256")).Trim() -split '\s+')[0]
if ($recorded -ne (Get-Sha256 $zip)) { Stop-Tool "$name.zip.sha256 does not match the ZIP" }
& (Join-Path $PSScriptRoot 'verify-release.ps1') -Path $zip
if ($LASTEXITCODE -ne 0) { Stop-Tool 'the ZIP failed tools\verify-release.ps1' }

Write-Ok "release v$version prepared in $(Join-Path $root 'dist')"
Write-Host "  upload to the GitHub release v${version}: $name.zip and $name.zip.sha256"
Write-Host '  scan both with VirusTotal before publishing (docs\RELEASE_SECURITY.md in the other ports); nothing was uploaded.'
