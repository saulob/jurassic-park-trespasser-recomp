<#
.SYNOPSIS
  Checks a staged release folder, or the release ZIP the updater downloads, for what must ship and what must never. Local only.
.DESCRIPTION
  A release holds the program, its update helper, the Visual C++ runtime beside them and its own assets. Refused: the
  player's user data (user_data, saves, settings, the game's registry store, caches, logs), downloads, staging and mods
  folders, archives and partial downloads, the game's files, debug information, objects and sources, programs and DLLs
  that are not the listed ones, and files that mention this machine's folders or carry DEBUG_SAULO diagnostics.
  A ZIP is also checked as the asset itself: its name from config\release.json and the project VERSION, one root folder of
  that name, the .sha256 file beside it, and the version of the program inside. It is unpacked next to itself, checked
  and removed.
.EXAMPLE
  .\tools\verify-release.ps1 -Path dist\<name>
  .\tools\verify-release.ps1 -Path dist\<name>.zip
#>
param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Release = Get-Content -LiteralPath (Join-Path $Root 'config\release.json') -Raw | ConvertFrom-Json
if ((Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw) -notmatch '(?m)^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'project VERSION not found in CMakeLists.txt'
}
$Version = $Matches[1]
$AssetName = "$($Release.asset_prefix)-v$Version-$($Release.windows_suffix)"

# Folders and files of the player or of the game.
$ForbiddenNames = @('user_data', 'user_data.migrating', 'user', 'saves', 'cache', 'logs', 'downloads', 'staging', 'mods',
    'game', 'game.install-temp', 'game.install-backup', 'trespasser.ini', 'recomp-registry.txt', 'previews',
    'trespass.exe', 'build')
$ForbiddenPatterns = @('*.7z', '*.partial', 'SAVEGAME.*', '*.swp', '*.scx', '*.scn', '*.grf',
    '*.spz', '*.pid', '*.tpa', '*.smk', '*.wtd', '*.iso', '*.log', '*.dmp', '*.pdb', '*.ilk', '*.tmp', '*.bak', 'TPass*.bmp',
    '*.write_test', '.write_test', '*.obj', '*.exp', '*.lib', '*.py', '*.ps1', '*.c', '*.cpp', '*.h')

# The installed updater refuses a later package without these (required_staged_files in launcher\update_config.cpp).
$UpdaterRequiredFiles = @($Release.program, $Release.updater)
$RuntimeDlls = @('vcruntime140.dll', 'msvcp140.dll')
$RequiredFiles = @('README.txt', 'licenses\THIRD-PARTY-LICENSES.txt', 'assets\launcher\header-art.png', 'assets\launcher\sponsor-heart.png')

$Latin1 = [Text.Encoding]::GetEncoding(28591)
$LocalPaths = foreach ($local in @($Root, $env:USERPROFILE) | Where-Object { $_ }) {
    foreach ($style in @($local, $local.Replace('\', '/'))) {
        $style
        $Latin1.GetString([Text.Encoding]::Unicode.GetBytes($style))
    }
}

# Folders the translation and audio sources were read from during development: never part of a release.
$DevelopmentSources = @(('Decomp_' + 'Downloads'), ('Trespasser_English_' + 'captions'))
$LocalPaths = @($LocalPaths) + $DevelopmentSources + @($DevelopmentSources | ForEach-Object { $Latin1.GetString([Text.Encoding]::Unicode.GetBytes($_)) })

function Test-ReleaseFolder([string]$Dir) {
    $problems = New-Object System.Collections.Generic.List[string]
    $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
    foreach ($required in @($UpdaterRequiredFiles) + @($RequiredFiles)) {
        if (-not (Test-Path -LiteralPath (Join-Path $base $required) -PathType Leaf)) { $problems.Add("missing: $required") }
    }
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        $relative = $item.FullName.Substring($base.Length + 1)
        if ($ForbiddenNames -contains $item.Name) { $problems.Add("forbidden name: $relative") }
        foreach ($pattern in $ForbiddenPatterns) {
            if ($item.Name -like $pattern) { $problems.Add("forbidden file type: $relative"); break }
        }
        if ($item.PSIsContainer) { continue }
        $atRoot = $item.DirectoryName.TrimEnd('\') -eq $base
        if ($item.Extension -eq '.exe') {
            if (-not $atRoot -or $UpdaterRequiredFiles -notcontains $item.Name) { $problems.Add("unlisted program: $relative") }
        } elseif ($item.Extension -eq '.dll') {
            if (-not $atRoot -or $RuntimeDlls -notcontains $item.Name) { $problems.Add("unlisted DLL: $relative") }
        }
        $text = $Latin1.GetString([IO.File]::ReadAllBytes($item.FullName))
        foreach ($local in $LocalPaths) {
            if ($text.IndexOf($local, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $problems.Add("local path in file: $relative"); break }
        }
        if ($text.IndexOf('DEBUG_SAULO', [StringComparison]::Ordinal) -ge 0) { $problems.Add("DEBUG_SAULO marker in file: $relative") }
        if ($text.StartsWith('7z' + [char]0xBC + [char]0xAF + [char]0x27 + [char]0x1C)) { $problems.Add("7z archive: $relative") }
    }
    $programPath = Join-Path $base $Release.program
    if (Test-Path -LiteralPath $programPath -PathType Leaf) {
        $bytes = [IO.File]::ReadAllBytes($programPath)
        if ($bytes.Length -gt 1 -and $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A) {
            $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($programPath)
            if ($info.ProductVersion -ne $Version -or $info.FileVersion -ne "$Version.0") {
                $problems.Add("$($Release.program) carries version $($info.FileVersion) / $($info.ProductVersion), the project is $Version")
            }
        }
    }
    return $problems
}

$full = (Resolve-Path -LiteralPath $Path).Path
if (Test-Path -LiteralPath $full -PathType Leaf) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $problems = New-Object System.Collections.Generic.List[string]
    $leaf = Split-Path $full -Leaf
    if ($leaf -ne "$AssetName.zip") { $problems.Add("the ZIP is named $leaf, the updater downloads $AssetName.zip") }
    $checksum = "$full.sha256"
    if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) {
        $problems.Add("missing $leaf.sha256 beside the ZIP")
    } else {
        $line = ([IO.File]::ReadAllText($checksum)).Trim()
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash.ToLowerInvariant()
        if ($line -notmatch '^([0-9a-f]{64})  (\S+)$') {
            $problems.Add("$leaf.sha256 is not '<sha256>  <file name>'")
        } else {
            if ($Matches[1] -ne $actual) { $problems.Add("$leaf.sha256 does not match the ZIP ($actual)") }
            if ($Matches[2] -ne $leaf) { $problems.Add("$leaf.sha256 names $($Matches[2])") }
        }
    }
    $folder = "$full.verify"
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    New-Item -ItemType Directory $folder | Out-Null
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead($full)
        try {
            foreach ($entry in $archive.Entries) {
                if ($entry.FullName -match '^[\\/]|\.\.|:|\\') { throw "The ZIP holds an unsafe entry name: $($entry.FullName)" }
                if (-not $entry.FullName.StartsWith("$AssetName/")) { $problems.Add("outside the root folder ${AssetName}: $($entry.FullName)"); continue }
                $target = Join-Path $folder $entry.FullName
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                if ($entry.Name) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target) }
            }
        } finally { $archive.Dispose() }
        $rootFolder = Join-Path $folder $AssetName
        if (Test-Path -LiteralPath $rootFolder -PathType Container) {
            foreach ($problem in (Test-ReleaseFolder $rootFolder)) { $problems.Add($problem) }
        } else {
            $problems.Add("the ZIP has no root folder $AssetName")
        }
    } finally { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
} else {
    $problems = Test-ReleaseFolder $full
}
if ($problems.Count) {
    Write-Host 'RELEASE CHECK FAILED' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  $problem" -ForegroundColor Red }
    exit 1
}
Write-Host "Release is clean: $Path"
