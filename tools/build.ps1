<#
.SYNOPSIS
  Builds trespasser.exe incrementally (MSVC, CMake + Ninja). -Run starts the game after a successful build.
.DESCRIPTION
  Reuses what is current and regenerates only what is stale, in this order: the pcrecomp checkout, the game
  files extracted from the disc image, the analysis (function catalog, RTTI), the lifted C (src\recomp\gen) and the
  CMake configuration. Each generated stage records a digest of its inputs; a stage runs only when that digest or
  its outputs changed. The playable host is a 32-bit process on pcrecomp's native32 runtime (-Arch x86, the
  default, with the x64-hosted x86 MSVC). -Arch x64 builds the 64-bit host groundwork only (docs\X64_MIGRATION.md):
  it cannot run the game yet, so -Run is refused with it.
  Development configuration: RelWithDebInfo in build\cmake\msvc-<arch>; -Config Release uses build\cmake\msvc-<arch>-release.
  Each run writes one log, build\logs\trespasser_NNN.log (the next free number): the build and, with -Run, the
  game's own output after it and its exit code. The terminal shows progress and problems only.
.PARAMETER Run
  After a successful build, starts trespasser.exe --run and stays attached until it exits. Not started when the
  build fails or when an instance is already running. The game appends to the run's log (--log-file); the
  terminal stays concise, and an abnormal exit prints the log's path and its last lines.
.PARAMETER VerboseRuntime
  With -Run, shows the runtime output in the terminal as well (it is always in the log).
.PARAMETER Clean
  Deletes the configuration's build folder first. The game, the disc image, analysis, lifted C and logs are kept.
.PARAMETER Arch
  x86 (default): the playable 32-bit host. x64: the 64-bit host groundwork and its headless tests, never run.
.PARAMETER Config
  RelWithDebInfo (default) or Release.
.PARAMETER GenOpt
  Compiler optimisation for the lifted C, for example /Od for much faster compiles. Default: what the build folder
  already uses, else /O2.
.PARAMETER Trace
  Builds with RECOMP_TRACE (--calltrace, --firsthit, ...). Default: what the build folder already uses.
.PARAMETER BuildDir
  Another build folder, relative to the repository root.
.PARAMETER Jobs
  Parallel compile jobs (default: Ninja's choice).
.PARAMETER Iso
  English disc image to extract from, when the game files are missing (else TRESPASSER_ISO or config\local.json).
.PARAMETER Reanalyze
  Forces the analysis stage (about 15 minutes), and the lift if its output changed.
.PARAMETER Relift
  Forces the lift stage (about 1 minute).
.PARAMETER GameArgs
  Arguments for trespasser.exe (with -Run), in order, after --run and --log-file:
  -GameArgs "--renderer","d3d11". Unnamed trailing tokens are collected after them.
.EXAMPLE
  .\tools\build.ps1
  .\tools\build.ps1 -Run
  .\tools\build.ps1 -Run -VerboseRuntime
  .\tools\build.ps1 -GenOpt /Od -Run -GameArgs "--renderer","d3d11"
  .\tools\build.ps1 -GenOpt /Od -Run -GameArgs "--watchdog","120"
  .\tools\build.ps1 -Arch x86 -GenOpt /Od -Run
  .\tools\build.ps1 -Arch x64 -GenOpt /Od
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Run,
    [ValidateSet('x86', 'x64')][string]$Arch = 'x86',
    [switch]$VerboseRuntime,
    [switch]$Clean,
    [ValidateSet('RelWithDebInfo', 'Release')][string]$Config = 'RelWithDebInfo',
    [string]$GenOpt = '',
    [switch]$Trace,
    [string]$BuildDir = '',
    [int]$Jobs = 0,
    [string]$Iso = '',
    [switch]$Reanalyze,
    [switch]$Relift,
    [string[]]$GameArgs = @(),
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LoaderArgs = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')
$root = $script:RepoRoot
$target = Get-Content -LiteralPath (Join-Path $root 'config\target.json') -Raw | ConvertFrom-Json
$guestExe = Join-Path $root "game\$($target.installed_name)"
$pcrecomp = Join-Path $root 'external\pcrecomp'
$genDir = Join-Path $root 'src\recomp\gen'
$analysisDir = Join-Path $root 'analysis'
$hostExitMeaning = @{
    1 = 'setup failed before the game started'
    2 = 'reached a function that is not lifted'
    3 = 'fault in lifted or SelfMod code'
    4 = 'watchdog expired'
    5 = 'indirect jump or call to an address with no lifted entry'
    6 = 'the game reported that it did not finish starting up (WinMain returned 0)'
    7 = 'the game exited with an error code of its own'
    8 = 'the game did not leave its loops within 60 s of a close request'
}

if (-not $BuildDir) { $BuildDir = if ($Config -eq 'Release') { "build\cmake\msvc-$Arch-release" } else { "build\cmake\msvc-$Arch" } }
$buildDir = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$buildDir = [IO.Path]::GetFullPath($buildDir)
[string[]]$hostTargets = if ($Arch -eq 'x64') { 'trespasser_x64_probe', 'x64_tests' } else { 'trespasser' }
$hostExe = Join-Path $buildDir "$($hostTargets[0]).exe"

trap { Stop-OnError $_ }

function Get-RunningHost {
    @(Get-Process -Name trespasser -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -ieq $hostExe })
}

function Show-Progress([string]$Line) {
    if ($Line -match '^\[(\d+)/(\d+)\]') {
        return ([int]$Matches[1] % 20 -eq 0) -or ($Matches[1] -eq $Matches[2])
    }
    return $Line -match 'error|warning|FAILED|fatal|LNK\d'
}

function Show-PipelineLine([string]$Line) { $Line -notmatch '^\[\*\]\s{3}\d+/\d+' }

function Get-PcrecompProblem {
    if (-not (Test-Path -LiteralPath (Join-Path $pcrecomp 'runtime\native32\native32.c'))) { return 'not fetched' }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $head = @(& git -C $pcrecomp rev-parse HEAD 2>$null)
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0 -or "$($head[0])".Trim() -ne $target.pcrecomp.commit) { return "not at the pinned commit $($target.pcrecomp.commit.Substring(0, 7))" }
    return $null
}

function Get-GameFilesProblem {
    if (-not (Test-Path -LiteralPath $guestExe)) { return "no $($target.installed_name) yet" }
    if ((Get-Sha256 $guestExe) -ne $target.sha256) { return "$($target.installed_name) is not the expected build" }
    foreach ($file in 'MSVCIRT.DLL', 'SMACKW32.DLL', 'Processor.dll', 'data\menu\tpassintro.smk') {
        if (-not (Test-Path -LiteralPath (Join-Path $root "game\$file"))) { return "game\$file is missing" }
    }
    return $null
}

function Get-AnalysisOutputsMissing {
    foreach ($file in 'functions.json', 'rtti.json', 'rtti_seeds.json') {
        if (-not (Test-Path -LiteralPath (Join-Path $analysisDir $file))) { return "analysis\$file is missing" }
    }
    return $null
}

# An analysis made before stamps existed is adopted when its recorded fingerprint matches and nothing was cut short.
function Test-AnalysisAdoptable {
    $fingerprint = Join-Path $analysisDir 'fingerprint.json'
    if (-not (Test-Path -LiteralPath $fingerprint)) { return $false }
    $recorded = Get-Content -LiteralPath $fingerprint -Raw | ConvertFrom-Json
    $catalog = Get-Item -LiteralPath (Join-Path $analysisDir 'functions.json')
    return ($recorded.sha256 -eq $target.sha256) -and ($recorded.pcrecomp_commit -eq $target.pcrecomp.commit) -and
        ($catalog.LastWriteTimeUtc -ge (Get-Item -LiteralPath $fingerprint).LastWriteTimeUtc)
}

function Get-LiftOutputsMissing {
    foreach ($file in 'recomp_dispatch.c', 'recomp_funcs.h', 'recomp_0000.c') {
        if (-not (Test-Path -LiteralPath (Join-Path $genDir $file))) { return "src\recomp\gen\$file is missing" }
    }
    return $null
}

function Get-LiftInputs {
    @('analysis\functions.json', 'analysis\rtti_seeds.json', 'config\decoder_fixups.json', 'config\extra_entries.json',
        'tools\run_lift.py') | ForEach-Object { Join-Path $root $_ }
}

# A lift made before stamps existed is adopted when it is newer than every input and was made from this binary.
function Test-LiftAdoptable {
    $stats = Join-Path $analysisDir 'lift_stats.json'
    if (-not (Test-Path -LiteralPath $stats)) { return $false }
    if ((Get-Content -LiteralPath $stats -Raw | ConvertFrom-Json).target_sha256 -ne $target.sha256) { return $false }
    $made = (Get-Item -LiteralPath $stats).LastWriteTimeUtc
    foreach ($file in Get-LiftInputs) {
        if ((Get-Item -LiteralPath $file).LastWriteTimeUtc -gt $made) { return $false }
    }
    return $true
}

function Get-PatchFingerprint($Patches) {
    if (-not $Patches -or $Patches.Count -eq 0) { return '' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $text = New-Object System.Text.StringBuilder
        foreach ($patch in $Patches) {
            $hex = [System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($patch.FullName))).Replace('-', '')
            [void]$text.Append($patch.Name).Append(':').Append($hex).Append("`n")
        }
        return [System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text.ToString()))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

# The launcher's sources (config\launcher_deps.txt): fetched when missing, at another commit or with another patch set
# (launcher\patches\<name>), then patched. The stamp records the commit and the patch fingerprint.
function Sync-LauncherDependencies {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Stop-Tool 'git is needed to fetch the launcher sources (config\launcher_deps.txt).' }
    $fetched = 0
    foreach ($line in Get-Content -LiteralPath (Join-Path $root 'config\launcher_deps.txt')) {
        if ($line -match '^\s*(#|$)') { continue }
        $path, $url, $commit = -split $line
        $dir = Join-Path $root $path
        $stamp = Join-Path $dir '.jpt_commit'
        $patches = @(Get-ChildItem -LiteralPath (Join-Path $root ('launcher\patches\' + (Split-Path $path -Leaf))) -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
        $fingerprint = Get-PatchFingerprint $patches
        $expected = if ($fingerprint) { "$commit`n$fingerprint" } else { $commit }
        if ((Test-Path -LiteralPath $stamp) -and (Get-Content -LiteralPath $stamp -Raw).Trim() -eq $expected) { continue }
        Write-Step "Fetching $path ($($commit.Substring(0, 7)))"
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        & git -C $dir init --quiet
        & git -C $dir fetch --quiet --depth 1 $url $commit
        if ($LASTEXITCODE -ne 0) { Stop-Tool "git fetch failed for $url" }
        & git -C $dir -c advice.detachedHead=false checkout --quiet FETCH_HEAD
        foreach ($patch in $patches) {
            & git -C $dir apply $patch.FullName
            if ($LASTEXITCODE -ne 0) { Stop-Tool "patch $($patch.Name) does not apply to $path" }
        }
        [System.IO.File]::WriteAllText($stamp, $expected, [System.Text.Encoding]::ASCII)
        $fetched++
    }
    if ($fetched -eq 0) { Write-Step 'Launcher sources: current' }
}

function Get-CmakeProblem([string]$GenOptWanted, [string]$TraceWanted) {
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if (-not ((Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache))) { return 'not configured' }
    $sourceDir = Get-CmakeCacheValue $cache 'CMAKE_HOME_DIRECTORY'
    if (-not $sourceDir -or ([IO.Path]::GetFullPath($sourceDir) -ne $root)) { return 'configured for another source folder' }
    if ((Get-CmakeCacheValue $cache 'CMAKE_BUILD_TYPE') -ne $Config) { return 'another configuration' }
    if ((Get-CmakeCacheValue $cache 'TRESPASSER_GEN_OPT') -ne $GenOptWanted) { return 'another -GenOpt' }
    if ((Get-CmakeCacheValue $cache 'TRESPASSER_TRACE') -ne $TraceWanted) { return 'another -Trace' }
    return $null
}

Open-RunLog
Write-Host "Log: $($script:RunLogPath)"
Add-RunLog "command: tools\build.ps1 $(@($PSBoundParameters.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value -join ' ')" }) -join ' ')"
Add-RunLog "repository $root, build folder $buildDir, configuration $Config"

if ($Run -and $Arch -eq 'x64') {
    Write-Warn 'The x64 host cannot run the game yet (docs\X64_MIGRATION.md lists what is missing); -Run is for -Arch x86.'
    Close-RunLog
    exit 1
}

$running = @(Get-RunningHost)
if ($running.Count -gt 0) {
    Write-Warn "trespasser.exe is already running (PID $($running.Id -join ', ')); it cannot be rebuilt or started again."
    Write-Warn 'Close it and run this script again.'
    Close-RunLog
    if ($Run) { return }
    exit 1
}

if ($Clean) {
    $buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'build')) + '\'
    if (-not $buildDir.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-Tool "-Clean only removes folders under $buildRoot; $buildDir is outside it."
    }
    if (Test-Path -LiteralPath $buildDir) {
        Write-Step "Cleaning $buildDir"
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
}

$msvc = Enter-Msvc $Arch
Write-Step "Toolchain MSVC $Arch ($msvc), $Config"

$problem = Get-PcrecompProblem
if ($problem) {
    Write-Step "Fetching pcrecomp: $problem"
    Assert-PythonModules @('json')
    if ((Invoke-Python 'tools\fetch_pcrecomp.py' @() { param($l) Show-PipelineLine $l }) -ne 0) { Stop-Tool 'pcrecomp fetch failed' }
} else {
    Write-Step 'pcrecomp: current'
}

$problem = Get-GameFilesProblem
if ($problem) {
    Write-Step "Extracting the game files from the disc image: $problem"
    Assert-PythonModules @('json')
    $extractArgs = @()
    if ($Iso) { $extractArgs += @('--iso', $Iso) }
    if ((Invoke-Python 'tools\extract_game.py' $extractArgs) -ne 0) { Stop-Tool 'Game file extraction failed' }
} else {
    Write-Step 'Game files: current'
}

$exeSha = $target.sha256
$analysisFacts = @("exe $exeSha", "pcrecomp $($target.pcrecomp.commit)")
$analysisDigest = Get-InputsDigest @() $analysisFacts
$problem = Get-AnalysisOutputsMissing
if (-not $problem -and -not $Reanalyze -and -not (Test-StampCurrent 'analyze' $analysisDigest) -and (Test-AnalysisAdoptable)) {
    Set-Stamp 'analyze' $analysisDigest
}
if ($Reanalyze) { $problem = 'requested' }
elseif (-not $problem -and -not (Test-StampCurrent 'analyze' $analysisDigest)) { $problem = 'its inputs changed' }
if ($problem) {
    Write-Step "Analysing trespass.exe (about 15 minutes): $problem"
    Assert-PythonModules @('capstone', 'pefile')
    Remove-Stamp 'analyze'
    if ((Invoke-Python 'tools\analyze.py' @() { param($l) Show-PipelineLine $l }) -ne 0) { Stop-Tool 'Analysis failed' }
    Set-Stamp 'analyze' $analysisDigest
} else {
    Write-Step 'Analysis: current'
}

$liftDigest = Get-InputsDigest (Get-LiftInputs) $analysisFacts
$problem = Get-LiftOutputsMissing
if (-not $problem -and -not $Relift -and -not (Test-StampCurrent 'lift' $liftDigest) -and (Test-LiftAdoptable)) {
    Set-Stamp 'lift' $liftDigest
}
if ($Relift) { $problem = 'requested' }
elseif (-not $problem -and -not (Test-StampCurrent 'lift' $liftDigest)) { $problem = 'its inputs changed' }
if ($problem) {
    Write-Step "Lifting .text to C: $problem"
    Assert-PythonModules @('capstone', 'pefile')
    Remove-Stamp 'lift'
    if ((Invoke-Python 'tools\run_lift.py' @() { param($l) Show-PipelineLine $l }) -ne 0) { Stop-Tool 'Lift failed or is incomplete (see analysis\lift_stats.json)' }
    Set-Stamp 'lift' $liftDigest
} else {
    Write-Step 'Lifted C: current'
}

if ($Arch -eq 'x86') { Sync-LauncherDependencies }

$cache = Join-Path $buildDir 'CMakeCache.txt'
$cached = Test-Path -LiteralPath $cache
$genOptWanted = if ($GenOpt) { $GenOpt } elseif ($cached -and (Get-CmakeCacheValue $cache 'TRESPASSER_GEN_OPT')) { Get-CmakeCacheValue $cache 'TRESPASSER_GEN_OPT' } else { '/O2' }
$traceOn = if ($PSBoundParameters.ContainsKey('Trace')) { $Trace.IsPresent } else { $cached -and ((Get-CmakeCacheValue $cache 'TRESPASSER_TRACE') -eq 'ON') }
$traceWanted = if ($traceOn) { 'ON' } else { 'OFF' }

$problem = Get-CmakeProblem $genOptWanted $traceWanted
if ($problem) {
    Write-Step "Configuring CMake: $problem"
    $configureArgs = @('-S', $root, '-B', $buildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config", '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        "-DTRESPASSER_GEN_OPT=$genOptWanted", "-DTRESPASSER_TRACE=$traceWanted")
    if ((Invoke-Logged 'cmake' $configureArgs { param($l) $l -match 'error|warning|CMake Error' }) -ne 0) {
        Show-LogTail
        Stop-Tool 'CMake configuration failed'
    }
}

Write-Step 'Building incrementally (the first build compiles about 1.5 million lines of C and takes a while)'
$ninjaArgs = @('-C', $buildDir)
if ($Jobs -gt 0) { $ninjaArgs += @('-j', $Jobs) }
$ninjaArgs += $hostTargets
$buildCode = Invoke-Logged 'ninja' $ninjaArgs { param($l) Show-Progress $l }
if ($buildCode -ne 0) {
    Show-LogTail
    Stop-Tool 'Build failed' $buildCode
}
if (-not (Test-Path -LiteralPath $hostExe)) { Stop-Tool "Missing $hostExe" }
Write-Host ''
Write-Host "Built $hostExe" -ForegroundColor Green
Add-RunLog "built $hostExe"
if (-not $Run) {
    Close-RunLog
    Write-Host "Log: $($script:RunLogPath)"
    return
}

# One log per run: the game appends to this one, so options that would make it open another are dropped.
$forwarded = @()
$LoaderArgs = @($GameArgs) + @($LoaderArgs)
for ($i = 0; $i -lt $LoaderArgs.Count; $i++) {
    if ($LoaderArgs[$i] -eq '--') {
        $forwarded += $LoaderArgs[$i..($LoaderArgs.Count - 1)]
        break
    }
    if ($LoaderArgs[$i] -in @('--log-dir', '--log-file')) {
        Write-Warn "$($LoaderArgs[$i]) ignored: this run logs to $($script:RunLogPath)"
        $i++
        continue
    }
    $forwarded += $LoaderArgs[$i]
}
$runArguments = @('--run', '--log-file', $script:RunLogPath) + $forwarded
$quiet = -not $VerboseRuntime
$code = $null
$startError = $null

Write-Step 'Starting trespasser.exe --run'
if ($quiet) { Write-Host 'Run output goes to the log only; -VerboseRuntime shows it here.' }
$quotedArguments = @($runArguments | ForEach-Object {
    if ($_ -eq '' -or $_ -match '[\s"]') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ }
})
Add-RunLog "> $hostExe $($quotedArguments -join ' ')"
Add-RunLog '--- trespasser.exe appends to this file from here until it exits ---'
Suspend-RunLog
Push-Location $root
try {
    if ($quiet) {
        # The host echoes its log to the stderr it inherits; NUL keeps that out of the terminal.
        # Start-Process rejects the same string for both redirections, so stderr uses the device path form of NUL.
        $process = Start-Process -FilePath $hostExe -ArgumentList $quotedArguments -NoNewWindow -PassThru `
            -RedirectStandardOutput 'NUL' -RedirectStandardError '\\.\NUL'
        $null = $process.Handle # keeps the exit code readable after the process ends
        $process.WaitForExit()
        $code = $process.ExitCode
    } else {
        & $hostExe @runArguments
        $code = $LASTEXITCODE
    }
}
catch { $startError = $_ }
finally {
    Pop-Location
    Resume-RunLog
}

if ($startError) {
    Add-RunLog "trespasser.exe could not be started: $($startError.Exception.Message)"
    Close-RunLog
    Write-Host "error: trespasser.exe could not be started: $($startError.Exception.Message)" -ForegroundColor Red
    Write-Host "Log: $($script:RunLogPath)"
    exit 1
}
$meaning = if ($code -eq 0) { 'normal exit' } elseif ($hostExitMeaning.ContainsKey($code)) { $hostExitMeaning[$code] } else { 'abnormal exit' }
Add-RunLog "--- trespasser.exe exited with code $code ($meaning) ---"
Close-RunLog
Write-Host "trespasser.exe exit code $code"
Write-Host "Log: $($script:RunLogPath)"
if ($code -ne 0) {
    Write-Warn "Exit ${code}: $meaning."
    if ($quiet) { Show-LogTail 30 }
}
exit $code
