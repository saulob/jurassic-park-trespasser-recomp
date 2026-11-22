<#
.SYNOPSIS
  Packages the Release build as dist\<asset>-v<version>-<platform> (folder, ZIP and .zip.sha256), the asset the launcher's
  updater downloads. Local only: nothing is published, tagged or uploaded.
.DESCRIPTION
  The names come from config\release.json and the version from the project VERSION in CMakeLists.txt, the same sources the
  launcher is built from. Builds with tools\build.ps1 -Config Release unless -SkipBuild, then stages trespasser_recomp.exe,
  trg-updater.exe, the Visual C++ runtime DLLs the program imports (app-local), the assets the program reads next to itself,
  README.txt and the licence texts. Debug information, objects and every file of the player or of the game stay out.
  The staged folder and the finished ZIP are checked with tools\verify-release.ps1 before the .sha256 is written.
.PARAMETER SkipBuild
  Packages the existing build\cmake\msvc-x86-release instead of building it first.
.PARAMETER Clean
  Passes -Clean to tools\build.ps1.
.PARAMETER VerifyOnly
  Only checks an existing folder or ZIP with tools\verify-release.ps1, then exits.
.EXAMPLE
  .\tools\package.ps1
  .\tools\package.ps1 -SkipBuild
  .\tools\package.ps1 -VerifyOnly dist\<name>.zip
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Clean,
    [string]$VerifyOnly
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$verify = Join-Path $PSScriptRoot 'verify-release.ps1'

if ($VerifyOnly) {
    & $verify -Path $VerifyOnly
    exit $LASTEXITCODE
}

$release = Get-Content -LiteralPath (Join-Path $root 'config\release.json') -Raw | ConvertFrom-Json
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$name = "$($release.asset_prefix)-v$version-$($release.windows_suffix)"

$buildDir = Join-Path $root 'build\cmake\msvc-x86-release'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"
$sha = "$zip.sha256"

if (-not $SkipBuild) {
    Write-Step 'Building Release (tools\build.ps1 -Config Release)'
    $arguments = @('-Config', 'Release')
    if ($Clean) { $arguments += '-Clean' }
    & (Join-Path $PSScriptRoot 'build.ps1') @arguments
    if ($LASTEXITCODE -ne 0) { Stop-Tool "tools\build.ps1 failed with exit code $LASTEXITCODE" }
}

$program = Join-Path $buildDir $release.program
$updater = Join-Path $buildDir $release.updater
foreach ($file in $program, $updater) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Stop-Tool "missing ${file}: build Release first (tools\build.ps1 -Config Release) or drop -SkipBuild" }
}
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache) -or (Get-CmakeCacheValue $cache 'CMAKE_BUILD_TYPE') -ne 'Release') {
    Stop-Tool "$buildDir is not a Release build folder"
}
if ((Get-Item -LiteralPath $program).LastWriteTime -lt (Get-Item -LiteralPath (Join-Path $root 'CMakeLists.txt')).LastWriteTime) {
    Stop-Tool "$program is older than CMakeLists.txt: build again so the version in the program is the packaged one"
}

Write-Step "Staging $name"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
if (Test-Path -LiteralPath $sha) { Remove-Item -LiteralPath $sha -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -LiteralPath $program, $updater -Destination $stage

# The program imports the dynamic Visual C++ runtime; it ships beside it (app-local deployment).
Enter-MsvcX86 | Out-Null
$redist = Join-Path $env:VCToolsRedistDir 'x86'
$crt = Get-ChildItem -LiteralPath $redist -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { Stop-Tool "the Visual C++ runtime folder was not found in $redist" }
foreach ($dll in 'vcruntime140.dll', 'msvcp140.dll') {
    $path = Join-Path $crt.FullName $dll
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Stop-Tool "missing $path" }
    Copy-Item -LiteralPath $path -Destination $stage
}

$assets = Join-Path $buildDir 'assets'
if (-not (Test-Path -LiteralPath $assets -PathType Container)) { Stop-Tool "missing ${assets}: the build copies the assets next to the program" }
Copy-Item -LiteralPath $assets -Destination (Join-Path $stage 'assets') -Recurse

Copy-Item -LiteralPath (Join-Path $root 'packaging\README.txt') -Destination (Join-Path $stage 'README.txt')
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'docs\LEGAL.md') -Destination (Join-Path $licenses 'LEGAL.txt')

Write-Step 'Checking the staged folder'
& $verify -Path $stage
if ($LASTEXITCODE -ne 0) { Stop-Tool 'the staged folder failed tools\verify-release.ps1' }

Write-Step "Writing $name.zip"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    $base = (Resolve-Path -LiteralPath $stage).Path
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName) {
        $entry = "$name/" + $file.FullName.Substring($base.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }

$hash = Get-Sha256 $zip
[IO.File]::WriteAllText($sha, "$hash  $name.zip`n", (New-Object Text.UTF8Encoding($false)))

Write-Step 'Checking the ZIP and its checksum'
& $verify -Path $zip
if ($LASTEXITCODE -ne 0) { Stop-Tool 'the ZIP failed tools\verify-release.ps1' }

Write-Ok "$zip"
Write-Host "  SHA-256 $hash"
foreach ($file in $release.program, $release.updater) {
    Write-Host "  $file SHA-256 $(Get-Sha256 (Join-Path $stage $file))"
}
Write-Host 'Local only: nothing was published, tagged or uploaded.'
