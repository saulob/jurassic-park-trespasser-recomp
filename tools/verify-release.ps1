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
    '*.spz', '*.pid', '*.tpa', '*.smk', '*.wtd', '*.iso', '*.log', '*.dmp', '*.pdb', '*.debug', '*.ilk', '*.tmp', '*.bak', 'TPass*.bmp',
    '*.write_test', '.write_test', '*.obj', '*.exp', '*.lib', '*.py', '*.ps1', '*.c', '*.cpp', '*.h')

# The installed updater refuses a later package without these (required_staged_files in launcher\update_config.cpp).
$UpdaterRequiredFiles = @($Release.program, $Release.updater)
$RuntimeDlls = @('vcruntime140.dll', 'msvcp140.dll')
$RequiredFiles = @('README.txt', 'licenses\LICENSE.txt', 'licenses\LEGAL.txt', 'licenses\THIRD-PARTY-LICENSES.txt',
    'assets\launcher\header-art.png', 'assets\launcher\sponsor-heart.png')

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

# trg-updater.exe runs alone from a temporary folder: it may load nothing but Windows itself.
$HelperSystemDlls = @('kernel32.dll', 'shell32.dll', 'user32.dll', 'advapi32.dll', 'ntdll.dll', 'shlwapi.dll',
    'ole32.dll', 'bcrypt.dll')

# The PE header facts a release depends on: the subsystem, the architecture of the two programs together, the DLLs the
# helper imports and a debug entry that would name a PDB. $null for a file that is not a PE image.
function Read-PeImage([byte[]]$bytes) {
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { return $null }
    $coff = $pe + 4
    $optional = $coff + 20
    $pe32plus = [BitConverter]::ToUInt16($bytes, $optional) -eq 0x20B
    $directories = $optional + $(if ($pe32plus) { 112 } else { 96 })
    $directoryCount = [BitConverter]::ToUInt32($bytes, $optional + $(if ($pe32plus) { 108 } else { 92 }))
    $table = $optional + [BitConverter]::ToUInt16($bytes, $coff + 16)
    $stringTable = [BitConverter]::ToUInt32($bytes, $coff + 8) + 18 * [BitConverter]::ToUInt32($bytes, $coff + 12)
    $sections = @(for ($i = 0; $i -lt [BitConverter]::ToUInt16($bytes, $coff + 2); $i++) {
        $at = $table + 40 * $i
        $name = [Text.Encoding]::ASCII.GetString($bytes, $at, 8).TrimEnd([char]0)
        # A name beginning with a slash is an offset into the COFF string table (e.g. /4 for .gnu_debuglink).
        if ($name -match '^/(\d+)$' -and $stringTable -gt 0) {
            $stringAt = $stringTable + [int]$Matches[1]
            $stringEnd = $stringAt
            while ($stringEnd -lt $bytes.Length -and $bytes[$stringEnd] -ne 0) { $stringEnd++ }
            if ($stringAt -ge 0 -and $stringAt -lt $bytes.Length) {
                $name = [Text.Encoding]::ASCII.GetString($bytes, $stringAt, $stringEnd - $stringAt)
            }
        }
        [pscustomobject]@{ Name = $name;
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $at + 12);
            VirtualSize = [BitConverter]::ToUInt32($bytes, $at + 8);
            RawSize = [BitConverter]::ToUInt32($bytes, $at + 16);
            RawPointer = [BitConverter]::ToUInt32($bytes, $at + 20) }
    })
    $offsetOf = {
        param([long]$Rva)
        foreach ($section in $sections) {
            $size = [Math]::Max($section.RawSize, $section.VirtualSize)
            if ($Rva -ge $section.VirtualAddress -and $Rva -lt $section.VirtualAddress + $size) {
                return $section.RawPointer + ($Rva - $section.VirtualAddress)
            }
        }
        return -1
    }
    $directory = {
        param([int]$Index)
        if ($Index -lt $directoryCount) {
            [pscustomobject]@{ Rva = [BitConverter]::ToUInt32($bytes, $directories + 8 * $Index);
                Size = [BitConverter]::ToUInt32($bytes, $directories + 8 * $Index + 4) }
        }
    }
    $imports = New-Object System.Collections.Generic.List[string]
    $import = & $directory 1
    if ($import -and $import.Rva) {
        $at = & $offsetOf ([long]$import.Rva)
        while ($at -ge 0 -and $at + 20 -le $bytes.Length) {
            $nameRva = [BitConverter]::ToUInt32($bytes, $at + 12)
            if ($nameRva -eq 0) { break }
            $nameAt = & $offsetOf ([long]$nameRva)
            if ($nameAt -lt 0) { break }
            $end = $nameAt
            while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
            $imports.Add([Text.Encoding]::ASCII.GetString($bytes, $nameAt, $end - $nameAt))
            $at += 20
        }
    }
    $pdb = $null
    $debug = & $directory 6
    if ($debug -and $debug.Rva) {
        $at = & $offsetOf ([long]$debug.Rva)
        for ($i = 0; $at -ge 0 -and $i -lt [int]($debug.Size / 28); $i++) {
            $entry = $at + 28 * $i
            if ([BitConverter]::ToUInt32($bytes, $entry + 12) -ne 2) { continue }
            $raw = [BitConverter]::ToUInt32($bytes, $entry + 24)
            $size = [BitConverter]::ToUInt32($bytes, $entry + 16)
            if ($raw -le 0 -or $size -le 24 -or $raw + $size -gt $bytes.Length) { continue }
            $end = $raw + 24
            while ($end -lt $raw + $size -and $bytes[$end] -ne 0) { $end++ }
            $pdb = [Text.Encoding]::ASCII.GetString($bytes, $raw + 24, $end - $raw - 24)
        }
    }
    if ($pdb -eq 'LLD PDB.') { $pdb = $null }
    [pscustomobject]@{ Machine = [BitConverter]::ToUInt16($bytes, $coff); Subsystem = [BitConverter]::ToUInt16($bytes, $optional + 68);
        DllCharacteristics = [BitConverter]::ToUInt16($bytes, $optional + 70);
        Sections = @($sections | ForEach-Object { $_.Name }); Imports = $imports; Pdb = $pdb }
}

function Test-ReleaseFolder([string]$Dir) {
    $problems = New-Object System.Collections.Generic.List[string]
    $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
    foreach ($required in @($UpdaterRequiredFiles) + @($RequiredFiles)) {
        if (-not (Test-Path -LiteralPath (Join-Path $base $required) -PathType Leaf)) { $problems.Add("missing: $required") }
    }
    $facts = @{}
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
        $bytes = [IO.File]::ReadAllBytes($item.FullName)
        $text = $Latin1.GetString($bytes)
        foreach ($local in $LocalPaths) {
            if ($text.IndexOf($local, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $problems.Add("local path in file: $relative"); break }
        }
        if ($text.IndexOf('DEBUG_SAULO', [StringComparison]::Ordinal) -ge 0) { $problems.Add("DEBUG_SAULO marker in file: $relative") }
        if ($text.StartsWith('7z' + [char]0xBC + [char]0xAF + [char]0x27 + [char]0x1C)) { $problems.Add("7z archive: $relative") }
        if ($item.Name -eq $Release.program -and $bytes.Length -gt 1 -and $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A) {
            if ($text.IndexOf('<assembly', [StringComparison]::Ordinal) -lt 0) {
                $problems.Add("$($Release.program) carries no application manifest")
            } elseif ($text.IndexOf('supportedOS', [StringComparison]::Ordinal) -lt 0) {
                $problems.Add("$($Release.program) does not declare the supported Windows version")
            }
        }
        if ($UpdaterRequiredFiles -contains $item.Name) { $facts[$item.Name] = Read-PeImage $bytes }
    }
    $programFacts = $facts[$Release.program]
    $updaterFacts = $facts[$Release.updater]
    foreach ($name in $UpdaterRequiredFiles) {
        if (-not $facts[$name]) { continue }
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $base $name))
        if ($info.ProductVersion -ne $Version -or $info.FileVersion -ne "$Version.0") {
            $problems.Add("$name carries version $($info.FileVersion) / $($info.ProductVersion), the project is $Version")
        }
        if ($info.OriginalFilename -ne $name) { $problems.Add("$name names itself $($info.OriginalFilename)") }
        if ($info.CompanyName -ne 'saulob') { $problems.Add("$name carries company '$($info.CompanyName)'") }
    }
    if ($programFacts) {
        if ($programFacts.Subsystem -ne 2) { $problems.Add("$($Release.program) is not a Windows-subsystem program (subsystem $($programFacts.Subsystem))") }
        # The guest needs the host below its 2 GB window, so this program stays 32-bit and without ASLR by design.
        if ($programFacts.Machine -ne 0x14C) { $problems.Add("$($Release.program) is not the 32-bit recomp host (machine 0x$($programFacts.Machine.ToString('X4')))") }
        if ($programFacts.Pdb) { $problems.Add("$($Release.program) names a PDB: $($programFacts.Pdb)") }
        foreach ($section in $programFacts.Sections) {
            if ($section -match '^\.z?debug_' -or $section -eq '.gnu_debugdata') { $problems.Add("$($Release.program) holds debug sections: $section") }
        }
    }
    if ($updaterFacts) {
        if ($updaterFacts.Subsystem -ne 2) { $problems.Add("$($Release.updater) is not a Windows-subsystem program (subsystem $($updaterFacts.Subsystem))") }
        # The standalone helper is the CLANG64 x64 build; the 32-bit host starts it on Windows 10/11 x64.
        if ($updaterFacts.Machine -ne 0x8664) {
            $problems.Add("$($Release.updater) is not the x86-64 release helper (machine 0x$($updaterFacts.Machine.ToString('X4')))")
        }
        if ($updaterFacts.Sections -notcontains '.gnu_debuglink') {
            $problems.Add("$($Release.updater) is not the tools\build-updater.ps1 build (no .gnu_debuglink)")
        }
        if (($updaterFacts.DllCharacteristics -band 0x140) -ne 0x140 -or ($updaterFacts.DllCharacteristics -band 0x20) -eq 0) {
            $problems.Add("$($Release.updater) lacks the x64 PE mitigations (Dynamic base, NX, High Entropy VA)")
        }
        if (-not $updaterFacts.Imports.Count) { $problems.Add("$($Release.updater) names no imports") }
        foreach ($dll in $updaterFacts.Imports) {
            if ($HelperSystemDlls -notcontains $dll.ToLowerInvariant() -and $dll -notmatch '^api-ms-win-') {
                $problems.Add("$($Release.updater) imports $dll, which does not come with Windows")
            }
        }
        foreach ($section in $updaterFacts.Sections) {
            if ($section -match '^\.z?debug_' -or $section -eq '.gnu_debugdata') { $problems.Add("$($Release.updater) holds debug sections: $section") }
        }
        if ($updaterFacts.Pdb) { $problems.Add("$($Release.updater) names a PDB: $($updaterFacts.Pdb)") }
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
    $problems = @(Test-ReleaseFolder $full)
}
if ($problems.Count) {
    Write-Host 'RELEASE CHECK FAILED' -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  $problem" -ForegroundColor Red }
    exit 1
}
Write-Host "Release is clean: $Path"
