<#
.SYNOPSIS
  Prepares a complete release in dist\: the Windows x86 ZIP and the Linux x86_64 AppImage, each with its .sha256 file.
  Local only: nothing is published, tagged or uploaded.
.DESCRIPTION
  Runs tools\package.ps1 for Windows, then tools\package-release-linux.sh through wsl.exe for the AppImage (the Linux
  packaging itself is tools\package-linux.sh). Both run even if the first fails, so one run shows every problem, and
  the release counts as prepared only when both artifacts exist and their .sha256 files match. The version is the
  project VERSION in CMakeLists.txt; the asset prefix and the Windows and Linux suffixes are config\release.json.
  Neither is written a second time in this script.

  The Linux side builds in the WSL2 distribution JptFocalBuild: Ubuntu 20.04, the AppImage's glibc baseline. A missing
  JptFocalBuild is created from the pinned Ubuntu base image and prepared with packaging/linux/ci-deps.sh
  (tools\release-inputs.ps1); a distribution named with -WslDistro is used as it is and never changed.

  The reviewed AppImage runtime is kept in the ignored cache\appimage-runtime\runtime-x86_64 (tools\release-inputs.ps1):
  used only with the SHA-256 pinned in patches\appimage-runtime\baseline.env and, when missing, copied from a WSL
  packaging cache that holds the same file. Any other file is deleted, so nothing unreviewed is packaged. Without a
  reviewed copy, tools/build-appimage-runtime.sh builds the runtime and refuses it unless it has the pinned SHA-256.
.PARAMETER Clean
  Clean build for both platforms.
.PARAMETER SkipBuild
  Package the existing builds of both platforms. On Linux the canonical artifact in build/linux/bin is copied into the
  WSL build folder and verified again before packaging.
.PARAMETER WslDistro
  The WSL distribution to build in.
.EXAMPLE
  .\tools\package-release.ps1
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipBuild,
    [string]$WslDistro = 'JptFocalBuild'
)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot 'release-inputs.ps1')

$root = $script:RepoRoot
$release = Get-Content -LiteralPath (Join-Path $root 'config\release.json') -Raw | ConvertFrom-Json
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$dist = Join-Path $root 'dist'
$windowsName = "$($release.asset_prefix)-v$version-$($release.windows_suffix)"
$linuxName = "$($release.asset_prefix)-v$version-$($release.linux_suffix)"
$windowsFiles = @("$windowsName.zip", "$windowsName.zip.sha256")
$linuxFiles = @("$linuxName.AppImage", "$linuxName.AppImage.sha256")

function Test-Checksum([string]$Artifact, [string]$Checksum) {
    $line = (Get-Content -LiteralPath $Checksum -TotalCount 1)
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { return "$(Split-Path $Checksum -Leaf) is not '<sha256>  <file name>'" }
    if ($Matches[2] -ne (Split-Path $Artifact -Leaf)) { return "$(Split-Path $Checksum -Leaf) names $($Matches[2])" }
    $actual = Get-Sha256 $Artifact
    if ($actual -ne $Matches[1]) { return "$(Split-Path $Checksum -Leaf) does not match the file" }
    return $null
}

function Test-Artifacts([string[]]$Files) {
    $problems = New-Object System.Collections.Generic.List[string]
    foreach ($file in $Files) {
        if (-not (Test-Path -LiteralPath (Join-Path $dist $file) -PathType Leaf)) { $problems.Add("missing: $file") }
    }
    if ($problems.Count -eq 0) {
        $problem = Test-Checksum (Join-Path $dist $Files[0]) (Join-Path $dist $Files[1])
        if ($problem) { $problems.Add($problem) }
    }
    return $problems
}

function Remove-Artifacts([string[]]$Files) {
    foreach ($file in $Files) { Remove-Item -LiteralPath (Join-Path $dist $file) -ErrorAction SilentlyContinue }
}

$results = [ordered]@{ 'Windows package' = $null; 'Linux AppImage' = $null }
$notes = New-Object System.Collections.Generic.List[string]

Write-Step "Jurassic Park: Trespasser Recompiled $version - release packages (local only)"
New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($stale in @(Get-ChildItem -LiteralPath $dist -Force | Where-Object {
        $_.Name -like "$($release.asset_prefix)*" -and $_.Name -notlike "$windowsName*" -and $_.Name -notlike "$linuxName*" })) {
    Write-Warn "Removing the stale $($stale.Name) from dist"
    Remove-Item -LiteralPath $stale.FullName -Recurse -Force
}

# Windows x86 ZIP
Write-Step 'Windows x86 ZIP (tools\package.ps1)'
Remove-Artifacts $windowsFiles
$windowsArgs = @()
if ($Clean) { $windowsArgs += '-Clean' }
if ($SkipBuild) { $windowsArgs += '-SkipBuild' }
$powershell = (Get-Process -Id $PID).Path
& $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') @windowsArgs
$windowsProblems = @()
if ($LASTEXITCODE -ne 0) { $windowsProblems += "tools\package.ps1 failed (exit $LASTEXITCODE)" }
else {
    $windowsProblems = @(Test-Artifacts $windowsFiles)
    if ($windowsProblems.Count -eq 0) {
        & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify-release.ps1') `
            -Path (Join-Path $dist "$windowsName.zip")
        if ($LASTEXITCODE -ne 0) { $windowsProblems += 'the finished ZIP failed tools\verify-release.ps1' }
    }
}
$results['Windows package'] = ($windowsProblems.Count -eq 0)
foreach ($problem in $windowsProblems) { $notes.Add("Windows: $problem") }

# Linux x86_64 AppImage through WSL
Write-Step 'Linux x86_64 AppImage (tools/package-release-linux.sh in WSL)'
Remove-Artifacts $linuxFiles
$wslArgs = @('-d', $WslDistro)
$linuxProblems = @()
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    $linuxProblems += 'wsl.exe was not found. The AppImage is built in WSL2, in an Ubuntu 20.04 distribution (docs/LINUX.md).'
} else {
    $problem = Initialize-WslBuildEnvironment $root $WslDistro
    if ($problem) {
        $linuxProblems += $problem
    } else {
        $repoWsl = $script:RepoWsl
        $importRuntime = {
            param($File, $Sha256)
            $target = (& wsl.exe @wslArgs --exec /usr/bin/wslpath -a -u $File | Out-String).Trim()
            & wsl.exe @wslArgs --exec /bin/bash "$repoWsl/tools/import-appimage-runtime.sh" $Sha256 $target | Out-Host
        }
        $runtime = Resolve-ReviewedRuntime $root $importRuntime
        $runtimeWsl = ''
        if ($runtime) {
            $runtimeWsl = (& wsl.exe @wslArgs --exec /usr/bin/wslpath -a -u $runtime | Out-String).Trim()
            Write-Host "AppImage runtime: $runtime (the pinned SHA-256 $(Get-PinnedRuntimeSha256 $root))"
        } else {
            Write-Warn 'AppImage runtime: no reviewed copy found; tools/build-appimage-runtime.sh builds one and refuses it unless it has the pinned SHA-256.'
        }
        $linuxArgs = @()
        if ($Clean) { $linuxArgs += '--clean' }
        if ($SkipBuild) { $linuxArgs += '--skip-build' }
        & wsl.exe @wslArgs --exec /usr/bin/env "JPT_APPIMAGE_RUNTIME=$runtimeWsl" /bin/bash "$repoWsl/tools/package-release-linux.sh" @linuxArgs
        if ($LASTEXITCODE -ne 0) {
            $linuxProblems += "tools/package-release-linux.sh failed (exit $LASTEXITCODE)"
        } else {
            $linuxProblems = @(Test-Artifacts $linuxFiles)
        }
    }
}
$results['Linux AppImage'] = ($linuxProblems.Count -eq 0)
foreach ($problem in $linuxProblems) { $notes.Add("Linux: $problem") }

Write-Host ''
foreach ($key in $results.Keys) {
    if ($results[$key]) { Write-Host ("{0}: OK" -f $key) -ForegroundColor Green }
    else { Write-Host ("{0}: FAILED" -f $key) -ForegroundColor Red }
}
if ($results.Values -contains $false) {
    Write-Host ''
    foreach ($note in $notes) { Write-Host "  $note" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'RELEASE NOT PREPARED' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'RELEASE PREPARED (nothing was published)' -ForegroundColor Green
foreach ($file in $windowsFiles + $linuxFiles) { Write-Host "  $(Join-Path $dist $file)" }
foreach ($symbols in "$windowsName-symbols", "$linuxName-symbols") {
    if (Test-Path -LiteralPath (Join-Path $dist $symbols)) { Write-Host "  Symbols (not for release): $(Join-Path $dist $symbols)" }
}
