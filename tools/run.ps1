<#
.SYNOPSIS
  Starts the game from the current build: the launcher first, then the game. Builds nothing.
.DESCRIPTION
  Runs build\cmake\msvc-x86\trespasser.exe (RelWithDebInfo, the development configuration of tools\build.ps1) from the
  repository root. It does not lift, configure, compile or clean, and it never starts a build: when the executable is
  missing it says so. The executable numbers its own log, build\logs\trespasser_NNN.log.
  Arguments are passed to trespasser.exe as they are.
.EXAMPLE
  .\tools\run.ps1
  .\tools\run.ps1 --launcher
  .\tools\run.ps1 --renderer software --vsync off
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs = @()
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\cmake\msvc-x86\trespasser.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host 'error: trespasser.exe has not been built yet.' -ForegroundColor Red
    Write-Host "Build it first with:  .\tools\build.ps1" -ForegroundColor Yellow
    Write-Host "(the first build takes a while; add -GenOpt /Od for much faster compiles while developing)"
    exit 1
}

$running = @(Get-Process -Name trespasser -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -ieq $exe })
if ($running.Count -gt 0) {
    Write-Host "error: trespasser.exe is already running (PID $($running.Id -join ', ')). Close it first." -ForegroundColor Red
    exit 1
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
    Write-Host "trespasser.exe exit code $code. The newest log is in build\logs." -ForegroundColor Yellow
}
exit $code
