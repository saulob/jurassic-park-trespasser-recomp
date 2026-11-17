<#
.SYNOPSIS
  Headless test of tools\verify-release.ps1 on scratch folders in build\release-check-test: a fresh release passes, and
  user data, downloaded archives and game files are each refused.
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
    Set-Content -LiteralPath (Join-Path $dir 'README.txt') -Value 'readme' -NoNewline
    Set-Content -LiteralPath (Join-Path $dir 'assets\launcher\header-art.png') -Value 'png' -NoNewline
    return $dir
}

function Add-Item([string]$Dir, [string]$Relative, [string]$Text = 'x') {
    $path = Join-Path $Dir $Relative
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    Set-Content -LiteralPath $path -Value $Text -NoNewline
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

if (Test-Path -LiteralPath $Scratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force }
try {
    Expect 'fresh' $true { param($d) }
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

    # A ZIP of a fresh release passes; with user_data it does not.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $clean = New-Release 'zip-clean'
    [IO.Compression.ZipFile]::CreateFromDirectory($clean, "$clean.zip")
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $Verify -Path "$clean.zip" 2>&1
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath "$clean.zip.verify")) { $failures++; Write-Host 'FAIL clean zip' -ForegroundColor Red }
    $dirty = New-Release 'zip-dirty'
    Add-Item $dirty 'user_data\saves\SAVEGAME.001'
    [IO.Compression.ZipFile]::CreateFromDirectory($dirty, "$dirty.zip")
    $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $Verify -Path "$dirty.zip" 2>&1
    if ($LASTEXITCODE -eq 0) { $failures++; Write-Host 'FAIL zip with user_data' -ForegroundColor Red }
} finally {
    Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { Write-Host "release check tests: $failures failures" -ForegroundColor Red; exit 1 }
Write-Host 'release check tests: all passed'
