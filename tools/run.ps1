<#
.SYNOPSIS
  Starts the game from the current build: the launcher first, then the game. Builds nothing.
.DESCRIPTION
  Runs build\cmake\msvc-x86\trespasser_recomp.exe (RelWithDebInfo, the development configuration of tools\build.ps1) from the
  repository root. It does not lift, configure, compile or clean, and it never starts a build: when the executable is
  missing it says so. The executable numbers its own log, build\logs\trespasser_recomp_NNN.log. Saves, settings, downloads
  and mods are kept in build\user_data, as with tools\build.ps1 -Run.
  Refuses to run an executable older than the sources (a failed build leaves the previous one in place); -AllowStale overrides.
  Arguments are passed to trespasser_recomp.exe as they are.
.EXAMPLE
  .\tools\run.ps1
  .\tools\run.ps1 --launcher
  .\tools\run.ps1 --renderer software --vsync off
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$AllowStale,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs = @()
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\cmake\msvc-x86\trespasser_recomp.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host 'error: trespasser_recomp.exe has not been built yet.' -ForegroundColor Red
    Write-Host "Build it first with:  .\tools\build.ps1" -ForegroundColor Yellow
    Write-Host "(the first build takes a while; add -GenOpt /Od for much faster compiles while developing)"
    exit 1
}

if (-not $AllowStale) {
    $built = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
    $newer = @(foreach ($dir in 'src', 'launcher', 'cmake') {
            $path = Join-Path $root $dir
            if (Test-Path -LiteralPath $path) {
                Get-ChildItem -LiteralPath $path -Recurse -File -Include '*.c', '*.cpp', '*.h', '*.hpp', '*.inc', '*.vert', '*.frag', '*.cmake' |
                    Where-Object { $_.LastWriteTimeUtc -gt $built }
            }
        }) + @(Get-Item -LiteralPath (Join-Path $root 'CMakeLists.txt') | Where-Object { $_.LastWriteTimeUtc -gt $built })
    if ($newer.Count -gt 0) {
        $latest = $newer | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        Write-Host "error: trespasser_recomp.exe is older than $($newer.Count) source file(s), e.g. $($latest.FullName)." -ForegroundColor Red
        Write-Host 'The last .\tools\build.ps1 probably failed: run it again and read its errors. Use -AllowStale to run this executable anyway.' -ForegroundColor Yellow
        exit 1
    }
}

$running = @(Get-Process -Name trespasser_recomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -ieq $exe })
if ($running.Count -gt 0) {
    Write-Host "error: trespasser_recomp.exe is already running (PID $($running.Id -join ', ')). Close it first." -ForegroundColor Red
    exit 1
}

if ($GameArgs -notcontains '--user_data_root' -and -not ($GameArgs | Where-Object { $_ -like '--user_data_root=*' })) {
    $GameArgs = @('--user_data_root', (Join-Path $root 'build\user_data')) + $GameArgs
}

Push-Location $root
try {
    & $exe @GameArgs
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($code -ne 0) {
    Write-Host "trespasser_recomp.exe exit code $code. The newest log is in build\logs." -ForegroundColor Yellow
}
exit $code
