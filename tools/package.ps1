<#
.SYNOPSIS
  Packages the Release build as dist\<asset>-v<version>-<platform> (folder, ZIP and .zip.sha256), the asset the launcher's
  updater downloads. Local only: nothing is published, tagged or uploaded.
.DESCRIPTION
  The names come from config\release.json and the version from the project VERSION in CMakeLists.txt, the same sources the
  launcher is built from. Builds the host with tools\build.ps1 -Config Release (clang-cl x86 + lld-link) and the helper with
  tools\build-updater.ps1 (MSYS2 CLANG64 x64, static) unless -SkipBuild, then stages trespasser_recomp.exe, trg-updater.exe,
  the Visual C++ runtime DLLs the program imports (app-local), the assets the program reads next to itself, README.txt and
  the licence texts.
  Debug information, objects and every file of the player or of the game stay out: the program's PDB and the helper's
  .debug go to dist\<name>-symbols\, which is not packaged.
  The staged folder and the finished ZIP are checked with tools\verify-release.ps1 before the .sha256 is written.
.PARAMETER SkipBuild
  Packages the existing build\cmake\clangcl-x86-release and the existing CLANG64 helper instead of building them first;
  the build folder's cache must say clang-cl + lld-link, i686 Release.
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

$buildDir = Join-Path $root 'build\cmake\clangcl-x86-release'
$updaterBuild = Join-Path $root 'build\cmake\clang64-release\trg-updater-clang64'
$updaterDebug = Join-Path $updaterBuild 'trg-updater.debug'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$symbols = Join-Path $dist "$name-symbols"
$zip = Join-Path $dist "$name.zip"
$sha = "$zip.sha256"

if (-not $SkipBuild) {
    Write-Step 'Building the Release host (tools\build.ps1 -Config Release, clang-cl + lld-link)'
    $arguments = @{ Config = 'Release'; Toolchain = 'clang-cl' }
    if ($Clean) { $arguments.Clean = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @arguments
    if ($LASTEXITCODE -ne 0) { Stop-Tool "tools\build.ps1 failed with exit code $LASTEXITCODE" }
    Write-Step 'Building the release update helper (tools\build-updater.ps1)'
    & (Join-Path $PSScriptRoot 'build-updater.ps1') -OutDir $updaterBuild
    if ($LASTEXITCODE -ne 0) { Stop-Tool "tools\build-updater.ps1 failed with exit code $LASTEXITCODE" }
}

$program = Join-Path $buildDir $release.program
$updater = Join-Path $updaterBuild $release.updater
foreach ($file in $program, $updater) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Stop-Tool "missing ${file}: build Release first (tools\build.ps1 -Config Release and tools\build-updater.ps1) or drop -SkipBuild"
    }
}
if (-not (Test-Path -LiteralPath $updaterDebug -PathType Leaf)) {
    Stop-Tool "missing ${updaterDebug}: run tools\build-updater.ps1 so the helper's symbols ship outside the ZIP"
}
foreach ($source in @('helper_main.cpp', 'install_transaction.cpp', 'sha256.cpp', 'update_job.cpp', 'update_paths.cpp',
        'update_platform.cpp', 'trg-updater.rc', 'trg-updater.manifest')) {
    $path = Join-Path $root "launcher\updater\$source"
    if ((Get-Item -LiteralPath $updater).LastWriteTime -lt (Get-Item -LiteralPath $path).LastWriteTime) {
        Stop-Tool "$updater is older than launcher\updater\${source}: run tools\build-updater.ps1"
    }
}
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache) -or (Get-CmakeCacheValue $cache 'CMAKE_BUILD_TYPE') -ne 'Release') {
    Stop-Tool "$buildDir is not a Release build folder"
}
foreach ($tool in @(@('CMAKE_C_COMPILER', '^clang-cl(\.exe)?$'), @('CMAKE_CXX_COMPILER', '^clang-cl(\.exe)?$'), @('CMAKE_LINKER', '^lld-link(\.exe)?$'))) {
    $toolName = [IO.Path]::GetFileName("$(Get-CmakeCacheValue $cache $tool[0])")
    if ($toolName -notmatch $tool[1]) {
        Stop-Tool "$buildDir was not configured with clang-cl + lld-link ($($tool[0]) is '$toolName'): build it with tools\build.ps1 -Config Release"
    }
}
if ((Get-CmakeCacheValue $cache 'CMAKE_C_COMPILER_TARGET') -ne 'i686-pc-windows-msvc') {
    Stop-Tool "$buildDir does not target the MSVC i686 ABI: build it with tools\build.ps1 -Config Release"
}
if ((Get-Item -LiteralPath $program).LastWriteTime -lt (Get-Item -LiteralPath (Join-Path $root 'CMakeLists.txt')).LastWriteTime) {
    Stop-Tool "$program is older than CMakeLists.txt: build again so the version in the program is the packaged one"
}

Write-Step "Staging $name"
foreach ($old in $stage, $symbols) { if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force } }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
if (Test-Path -LiteralPath $sha) { Remove-Item -LiteralPath $sha -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -LiteralPath $program, $updater -Destination $stage
$pdbs = @(Get-ChildItem -LiteralPath $buildDir -Filter '*.pdb' -File | Where-Object { $_.BaseName -eq 'trespasser_recomp' })
if ($pdbs.Count -or (Test-Path -LiteralPath $updaterDebug -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path $symbols | Out-Null
    if ($pdbs.Count) { Copy-Item -LiteralPath $pdbs.FullName -Destination $symbols }
    if (Test-Path -LiteralPath $updaterDebug -PathType Leaf) { Copy-Item -LiteralPath $updaterDebug -Destination $symbols }
}

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
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\LICENSE.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'docs\LEGAL.md') -Destination (Join-Path $licenses 'LEGAL.txt')
# The licence texts of the code compiled into the program (the same set as the AppImage: tools/package-linux.sh).
$builtInLicenses = [ordered]@{
    'SDL'           = 'external\SDL\LICENSE.txt'
    'Dear-ImGui'    = 'external\imgui\LICENSE.txt'
    'pcrecomp'      = 'external\pcrecomp\LICENSE'
    'miniz'         = 'launcher\updater\third_party\miniz\LICENSE'
    'nanosvg'       = 'launcher\third_party\nanosvg\LICENSE.txt'
    'LZMA-SDK'      = 'launcher\third_party\lzma\LICENSE.txt'
    'ASSAO'         = 'third_party\assao\LICENSE'
    'Vulkan-Headers' = 'third_party\vulkan\LICENSE.txt'
}
foreach ($component in $builtInLicenses.Keys) {
    $source = Join-Path $root $builtInLicenses[$component]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Stop-Tool "Missing licence file: $source" }
    New-Item -ItemType Directory -Force -Path (Join-Path $licenses $component) | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $licenses $component)
}

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
if (Test-Path -LiteralPath $symbols) { Write-Host "  Symbols: $symbols (not in the package)" }
Write-Host 'Local only: nothing was published, tagged or uploaded.'
