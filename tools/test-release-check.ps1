<#
.SYNOPSIS
  Headless test of tools\verify-release.ps1 on scratch folders in build\release-check-test: a fresh release passes, and
  user data, downloaded archives, game files and wrong PE metadata (subsystem, architecture, imports, a named PDB) are
  each refused, and a release ZIP is checked as the updater asset.
#>
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Verify = Join-Path $PSScriptRoot 'verify-release.ps1'
$Scratch = Join-Path $Root 'build\release-check-test'
$failures = 0

function New-Release([string]$Name) {
    $dir = Join-Path $Scratch $Name
    New-Item -ItemType Directory -Force (Join-Path $dir 'assets\launcher') | Out-Null
    Set-Content -LiteralPath (Join-Path $dir 'trespasser_recomp.exe') -Value 'program' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'trg-updater.exe') -Value 'helper' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'README.txt') -Value 'readme' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'assets\launcher\header-art.png') -Value 'png' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'assets\launcher\sponsor-heart.png') -Value 'png' -NoNewline
    New-Item -ItemType Directory -Force (Join-Path $dir 'licenses') | Out-Null
    Set-Content -LiteralPath (Join-Path $dir 'licenses\THIRD-PARTY-LICENSES.txt') -Value 'MIT' -NoNewline
    foreach ($text in 'LICENSE.txt', 'LEGAL.txt') { Set-Content -LiteralPath (Join-Path $dir "licenses\$text") -Value 'notice' -NoNewline }
    foreach ($dll in 'vcruntime140.dll', 'msvcp140.dll') { Set-Content -LiteralPath (Join-Path $dir $dll) -Value 'runtime' -NoNewline }
    return $dir
}

$Release = Get-Content -LiteralPath (Join-Path $Root 'config\release.json') -Raw | ConvertFrom-Json
$null = (Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw) -match '(?m)^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
$AssetName = "$($Release.asset_prefix)-v$($Matches[1])-$($Release.windows_suffix)"

# A release ZIP as the updater downloads it: <asset>/<files>, with its .sha256. $Mutate changes the staged folder first.
function New-ReleaseZip([string]$Name, [scriptblock]$Mutate = {}, [string]$ZipName = "$AssetName.zip", [string]$RootName = $AssetName, [string]$Checksum = 'good') {
    $parent = Join-Path $Scratch "zip-$Name"
    New-Item -ItemType Directory -Force $parent | Out-Null
    $stage = New-Release ("zip-$Name\" + $(if ($RootName) { $RootName } else { "content" }))
    & $Mutate $stage
    $zip = Join-Path $parent $ZipName
    $source = if ($RootName) { $parent } else { $stage }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $base = (Resolve-Path -LiteralPath $source).Path
        foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
            $entry = $file.FullName.Substring($base.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry)
        }
    } finally { $archive.Dispose() }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ($Checksum -eq 'good') { [IO.File]::WriteAllText("$zip.sha256", "$hash  $ZipName`n") }
    elseif ($Checksum -eq 'wrong') { [IO.File]::WriteAllText("$zip.sha256", ('0' * 64) + "  $ZipName`n") }
    return $zip
}

function Expect-Zip([string]$Name, [bool]$Clean, [string]$Zip) {
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $Verify -Path $Zip 2>&1
    $passed = $LASTEXITCODE -eq 0 -and -not (Test-Path -LiteralPath "$Zip.verify")
    if ($passed -ne $Clean) {
        $script:failures++
        Write-Host "FAIL zip $Name (expected $(if ($Clean) { 'clean' } else { 'refused' }))" -ForegroundColor Red
    }
}

function Add-Item([string]$Dir, [string]$Relative, [string]$Text = 'x') {
    $path = Join-Path $Dir $Relative
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $Text, [Text.Encoding]::GetEncoding(28591))
}

# A minimal PE image with the header fields tools\verify-release.ps1 reads: one section holding an import table, and a
# CodeView debug entry when a PDB is named. It has no resources, so no version information.
function New-PeImage([string]$Path, [int]$Machine = 0x14C, [int]$Subsystem = 2, [string[]]$Dlls = @('KERNEL32.dll'), [string]$Pdb = '',
    [string]$SectionName = '.rdata', [int]$DllCharacteristics = 0x140) {
    $sectionRva = 0x1000
    $names = @()
    $cursor = 0
    foreach ($dll in $Dlls) { $names += $cursor; $cursor += $dll.Length + 1 }
    $namesStart = $sectionRva + 20 * ($Dlls.Count + 1)
    $debugStart = $namesStart + $cursor
    if ($debugStart % 4) { $debugStart += 4 - ($debugStart % 4) }
    $cvStart = $debugStart + 28
    $cvSize = $(if ($Pdb) { 24 + $Pdb.Length + 1 } else { 0 })
    $contentSize = [Math]::Max(0x200, $cvStart - $sectionRva + $cvSize)
    if ($contentSize % 0x200) { $contentSize += 0x200 - ($contentSize % 0x200) }
    $image = New-Object byte[] (0x400 + $contentSize)
    $put16 = { param([int]$At, [int]$Value) [Array]::Copy([BitConverter]::GetBytes([uint16]$Value), 0, $image, $At, 2) }
    $put32 = { param([int]$At, [long]$Value) [Array]::Copy([BitConverter]::GetBytes([uint32]$Value), 0, $image, $At, 4) }
    $image[0] = 0x4D; $image[1] = 0x5A
    & $put32 0x3C 0x80
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('PE'), 0, $image, 0x80, 2)
    $coff = 0x84
    $opt = 0x98
    & $put16 $coff $Machine
    & $put16 ($coff + 2) 1
    & $put16 ($coff + 16) 224
    & $put16 ($coff + 18) 0x0102
    & $put16 $opt 0x10B
    $image[$opt + 2] = 14
    & $put32 ($opt + 16) 0x1000
    & $put32 ($opt + 20) 0x1000
    & $put32 ($opt + 24) 0x1000
    & $put32 ($opt + 28) 0x400000
    & $put32 ($opt + 32) 0x1000
    & $put32 ($opt + 36) 0x200
    & $put16 ($opt + 40) 6
    & $put16 ($opt + 48) 6
    & $put32 ($opt + 56) 0x2000
    & $put32 ($opt + 60) 0x400
    & $put16 ($opt + 68) $Subsystem
    & $put16 ($opt + 70) $DllCharacteristics
    & $put32 ($opt + 72) 0x100000
    & $put32 ($opt + 76) 0x1000
    & $put32 ($opt + 80) 0x100000
    & $put32 ($opt + 84) 0x1000
    & $put32 ($opt + 92) 16
    & $put32 ($opt + 104) $sectionRva
    & $put32 ($opt + 108) (20 * ($Dlls.Count + 1))
    if ($Pdb) {
        & $put32 ($opt + 144) $debugStart
        & $put32 ($opt + 148) 28
    }
    $raw = 0x400
    for ($i = 0; $i -lt $Dlls.Count; $i++) {
        & $put32 ($raw + 20 * $i + 12) ($namesStart + $names[$i])
        [Array]::Copy([Text.Encoding]::ASCII.GetBytes($Dlls[$i]), 0, $image, $raw + ($namesStart - $sectionRva) + $names[$i], $Dlls[$i].Length)
    }
    if ($Pdb) {
        $entry = $raw + ($debugStart - $sectionRva)
        & $put32 ($entry + 12) 2
        & $put32 ($entry + 16) $cvSize
        & $put32 ($entry + 20) $cvStart
        & $put32 ($entry + 24) ($raw + ($cvStart - $sectionRva))
        $cv = $raw + ($cvStart - $sectionRva)
        [Array]::Copy([Text.Encoding]::ASCII.GetBytes('RSDS'), 0, $image, $cv, 4)
        [Array]::Copy([Text.Encoding]::ASCII.GetBytes($Pdb), 0, $image, $cv + 24, $Pdb.Length)
    }
    $section = $opt + 224
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes($SectionName.PadRight(8, [char]0)), 0, $image, $section, 8)
    & $put32 ($section + 8) ($cvStart - $sectionRva + $cvSize)
    & $put32 ($section + 12) $sectionRva
    & $put32 ($section + 16) $contentSize
    & $put32 ($section + 20) $raw
    & $put32 ($section + 36) 0x40000040
    [IO.File]::WriteAllBytes($Path, $image)
}

function Expect([string]$Name, [bool]$Clean, [scriptblock]$Setup) {
    $dir = New-Release $Name
    & $Setup $dir
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $Verify -Path $dir 2>&1
    $passed = $LASTEXITCODE -eq 0
    if ($passed -ne $Clean) {
        $script:failures++
        Write-Host "FAIL $Name (expected $(if ($Clean) { 'clean' } else { 'refused' }))" -ForegroundColor Red
    }
}

function Expect-Problem([string]$Name, [string]$Problem, [scriptblock]$Setup) {
    $dir = New-Release $Name
    & $Setup $dir
    $text = & powershell -NoProfile -ExecutionPolicy Bypass -File $Verify -Path $dir 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or -not $text.Contains($Problem)) {
        $script:failures++
        Write-Host "FAIL $Name (expected the problem: $Problem)" -ForegroundColor Red
    }
}

if (Test-Path -LiteralPath $Scratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force }
try {
    Expect 'fresh' $true { param($d) }
    Expect 'missing updater helper' $false { param($d) Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') }
    Expect 'user_data' $false { param($d) Add-Item $d 'user_data\launcher\trespasser.ini' }
    Expect 'user_data.migrating' $false { param($d) Add-Item $d 'user_data.migrating\saves\SAVEGAME.000' }
    Expect 'legacy user' $false { param($d) Add-Item $d 'user\recomp-registry.txt' }
    Expect 'saves' $false { param($d) Add-Item $d 'SAVEGAME.003' }
    Expect 'settings' $false { param($d) Add-Item $d 'trespasser.ini' }
    Expect 'registry store' $false { param($d) Add-Item $d 'recomp-registry.txt' }
    Expect 'cache' $false { param($d) Add-Item $d 'cache\x.bin' }
    Expect 'logs' $false { param($d) Add-Item $d 'logs\trespasser_001.log' }
    Expect 'downloads' $false { param($d) Add-Item $d 'downloads\archive.7z' }
    Expect 'partial download' $false { param($d) Add-Item $d 'archive.7z.partial' }
    Expect 'staging' $false { param($d) Add-Item $d 'staging\work\a.bin' }
    Expect 'mods' $false { param($d) Add-Item $d 'mods\one\a.bin' }
    Expect 'renamed 7z' $false { param($d) Add-Item $d 'assets\textures.dat' ("7z" + [char]0xBC + [char]0xAF + [char]0x27 + [char]0x1C) }
    Expect 'game files' $false { param($d) Add-Item $d 'game\data\be.scn' }
    Expect 'debug marker' $false { param($d) Add-Item $d 'notes.txt' 'DEBUG_SAULO' }
    Expect 'local path' $false { param($d) Add-Item $d 'notes.txt' $Root }
    Expect 'forward slash local path' $false { param($d) Add-Item $d 'notes.txt' $Root.Replace('\', '/') }
    Expect 'user profile path' $false { param($d) Add-Item $d 'notes.txt' "$env:USERPROFILE\AppData" }
    Expect 'development source folder' $false { param($d) Add-Item $d 'notes.txt' ('E:\Decomp_' + 'Downloads\translations') }
    Expect 'unexpected pdb' $false { param($d) Add-Item $d 'trespasser_recomp.pdb' }
    Expect 'crash dump' $false { param($d) Add-Item $d 'trespasser_recomp.dmp' }
    Expect 'incremental link state' $false { param($d) Add-Item $d 'trespasser_recomp.ilk' }
    Expect 'temporary file' $false { param($d) Add-Item $d 'assets\work.tmp' }
    Expect 'backup file' $false { param($d) Add-Item $d 'README.txt.bak' }
    Expect 'game audio' $false { param($d) Add-Item $d 'data\Stream.tpa' }
    Expect 'disc image' $false { param($d) Add-Item $d 'trespasser.iso' }
    Expect 'licenses are shipped' $true { param($d) Add-Item $d 'licenses\THIRD-PARTY-LICENSES.txt' 'MIT' }

    Expect 'missing README' $false { param($d) Remove-Item -LiteralPath (Join-Path $d 'README.txt') }
    Expect 'missing header art' $false { param($d) Remove-Item -LiteralPath (Join-Path $d 'assets\launcher\header-art.png') }
    Expect 'unlisted DLL' $false { param($d) Add-Item $d 'libffmpeg.dll' }
    Expect 'unlisted program' $false { param($d) Add-Item $d 'tool.exe' }
    Expect 'DLL in a subfolder' $false { param($d) Add-Item $d 'assets\x.dll' }
    Expect 'object file' $false { param($d) Add-Item $d 'trespasser_recomp.obj' }
    Expect 'source file' $false { param($d) Add-Item $d 'launcher.cpp' }
    Expect 'real program of another version' $false { param($d) Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\notepad.exe') -Destination (Join-Path $d 'trespasser_recomp.exe') -Force }

    Expect-Problem 'program without the Windows subsystem' 'not a Windows-subsystem program' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trespasser_recomp.exe') -Force
        New-PeImage -Path (Join-Path $d 'trespasser_recomp.exe') -Subsystem 3 }
    Expect-Problem 'helper without the Windows subsystem' 'not a Windows-subsystem program' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') -Subsystem 3 }
    Expect-Problem 'helper for another architecture' 'is not the x86-64 release helper' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trespasser_recomp.exe'), (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trespasser_recomp.exe')
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') }
    Expect-Problem 'helper from the CMake build' 'not the tools\build-updater.ps1 build' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') -Machine 0x8664 -DllCharacteristics 0x160 }
    Expect-Problem 'helper with debug sections' 'holds debug sections' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') -Machine 0x8664 -DllCharacteristics 0x160 -SectionName '.debug_info' }
    Expect 'helper debug file' $false { param($d) Add-Item $d 'trg-updater.debug' }
    Expect-Problem 'helper importing a DLL that is not Windows' 'does not come with Windows' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') -Dlls 'KERNEL32.dll', 'SDL3.dll' }
    Expect-Problem 'helper naming a PDB' 'names a PDB' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') -Force
        New-PeImage -Path (Join-Path $d 'trg-updater.exe') -Pdb 'C:\build\trg-updater.pdb' }

    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    Expect-Zip 'clean' $true (New-ReleaseZip 'clean')
    Expect-Zip 'wrong name' $false (New-ReleaseZip 'name' {} 'Other-v0.0.1-windows-x86.zip')
    Expect-Zip 'wrong version in the name' $false (New-ReleaseZip 'version' {} (($AssetName -replace '-v[0-9.]+-', '-v9.9.9-') + '.zip'))
    Expect-Zip 'no checksum file' $false (New-ReleaseZip 'nosha' {} "$AssetName.zip" $AssetName 'none')
    Expect-Zip 'wrong checksum' $false (New-ReleaseZip 'badsha' {} "$AssetName.zip" $AssetName 'wrong')
    Expect-Zip 'no root folder' $false (New-ReleaseZip 'flat' {} "$AssetName.zip" '')
    Expect-Zip 'another root folder' $false (New-ReleaseZip 'root' {} "$AssetName.zip" 'something')
    Expect-Zip 'user_data inside' $false (New-ReleaseZip 'userdata' { param($d) Add-Item $d 'user_data\saves\SAVEGAME.001' })
    Expect-Zip 'game folder inside' $false (New-ReleaseZip 'game' { param($d) Add-Item $d 'game\data\be.scn' })
    Expect-Zip 'missing updater helper' $false (New-ReleaseZip 'nohelper' { param($d) Remove-Item -LiteralPath (Join-Path $d 'trg-updater.exe') })
} finally {
    Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "release check tests: $failures failures" -ForegroundColor Red; exit 1 }
Write-Host 'release check tests: all passed'
