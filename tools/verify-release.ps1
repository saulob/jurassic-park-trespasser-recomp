<#
.SYNOPSIS
  Checks a staged release folder (or a release ZIP) for files that must never ship. Local only.
.DESCRIPTION
  A release holds the program and its own assets only. Refused: the player's user data (user_data, user_data.migrating,
  saves, settings, the game's registry store, caches, logs), the earlier user folder, downloads, staging and mods
  folders, archives and partial downloads, the game's files, and files that mention this machine's folders or carry
  DEBUG_SAULO diagnostics. A ZIP is unpacked next to itself and checked, then removed.
.EXAMPLE
  .\tools\verify-release.ps1 -Path dist\JurassicParkTrespasserRecompiled
  .\tools\verify-release.ps1 -Path dist\JurassicParkTrespasserRecompiled-windows-x86.zip
#>
param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
# Folders and files of the player or of the game.
$ForbiddenNames = @('user_data', 'user_data.migrating', 'user', 'saves', 'cache', 'logs', 'downloads', 'staging', 'mods',
    'game', 'game.install-temp', 'game.install-backup', 'trespasser.ini', 'recomp-registry.txt', 'previews',
    'trespass.exe', 'build')
$ForbiddenPatterns = @('*.7z', '*.partial', 'SAVEGAME.*', '*.swp', '*.scx', '*.scn', '*.grf',
    '*.spz', '*.pid', '*.tpa', '*.smk', '*.wtd', '*.iso', '*.log', '*.dmp', '*.pdb', '*.ilk', '*.tmp', '*.bak', 'TPass*.bmp',
    '*.write_test', '.write_test')

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
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        $relative = $item.FullName.Substring($base.Length + 1)
        if ($ForbiddenNames -contains $item.Name) { $problems.Add("forbidden name: $relative") }
        foreach ($pattern in $ForbiddenPatterns) {
            if ($item.Name -like $pattern) { $problems.Add("forbidden file type: $relative"); break }
        }
        if (-not $item.PSIsContainer) {
            $text = $Latin1.GetString([IO.File]::ReadAllBytes($item.FullName))
            foreach ($local in $LocalPaths) {
                if ($text.IndexOf($local, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $problems.Add("local path in file: $relative"); break }
            }
            if ($text.IndexOf('DEBUG_SAULO', [StringComparison]::Ordinal) -ge 0) { $problems.Add("DEBUG_SAULO marker in file: $relative") }
            if ($text.StartsWith('7z' + [char]0xBC + [char]0xAF + [char]0x27 + [char]0x1C)) { $problems.Add("7z archive: $relative") }
        }
    }
    return $problems
}

$full = (Resolve-Path -LiteralPath $Path).Path
if (Test-Path -LiteralPath $full -PathType Leaf) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $folder = "$full.verify"
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    New-Item -ItemType Directory $folder | Out-Null
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead($full)
        try {
            foreach ($entry in $archive.Entries) {
                if ($entry.FullName -match '^[\\/]|\.\.|:') { throw "The ZIP holds an unsafe entry name: $($entry.FullName)" }
                $target = Join-Path $folder $entry.FullName
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                if ($entry.Name) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target) }
            }
        } finally { $archive.Dispose() }
        $problems = Test-ReleaseFolder $folder
    } finally { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
} else {
    $problems = Test-ReleaseFolder $full
}
if ($problems.Count) {
    Write-Host 'RELEASE CHECK FAILED' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  $problem" -ForegroundColor Red }
    exit 1
}
Write-Host "Release folder is clean: $Path"
