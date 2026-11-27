# Processes and scheduling for tools\test.ps1: the job policy (one test process and one compiler unless asked otherwise), every
# process start behind the memory guard (tools\_memory_guard.ps1), the queue of background suites and the cleanup of the gate's
# own child processes. Dot-sourced after tools\_common.ps1 and tools\_memory_guard.ps1. Works in Windows PowerShell 5.1 and
# PowerShell 7.

$script:GateProcesses = New-Object System.Collections.Generic.List[object]

# ---- policy

function ConvertTo-JobCount($Value) {
    $number = 0
    if ($null -eq $Value -or -not [int]::TryParse("$Value".Trim(), [ref]$number) -or $number -lt 1 -or $number -gt 64) { return $null }
    return $number
}

# One test process and one compiler by default on every machine. Precedence: -Sanitize and -Sequential, then -TestJobs and
# -BuildJobs, then JPT_TEST_JOBS and JPT_BUILD_JOBS. No job count lifts a memory limit.
function Get-ResourcePolicy {
    param([int]$TestJobs = 0, [int]$BuildJobs = 0, [string]$EnvTestJobs = '', [string]$EnvBuildJobs = '', [bool]$Sequential = $false, [bool]$Sanitize = $false)
    $test = 1; $testFrom = 'default'
    $build = 1; $buildFrom = 'default'
    $fromEnv = ConvertTo-JobCount $EnvTestJobs
    if ($fromEnv) { $test = $fromEnv; $testFrom = 'JPT_TEST_JOBS' }
    $fromEnv = ConvertTo-JobCount $EnvBuildJobs
    if ($fromEnv) { $build = $fromEnv; $buildFrom = 'JPT_BUILD_JOBS' }
    if ($TestJobs -gt 0) { $test = $TestJobs; $testFrom = '-TestJobs' }
    if ($BuildJobs -gt 0) { $build = $BuildJobs; $buildFrom = '-BuildJobs' }
    if ($Sanitize) { $test = 1; $testFrom = '-Sanitize'; $build = 1; $buildFrom = '-Sanitize' }
    if ($Sequential) { $test = 1; $testFrom = '-Sequential' }
    [pscustomobject]@{
        TestJobs = $test; TestJobsFrom = $testFrom; BuildJobs = $build; BuildJobsFrom = $buildFrom
        HeavyExclusive = $true
        Background = ($test -gt 1)
        LinuxTestJobs = $(if ($Sequential -or $Sanitize) { 0 } else { $test - 1 })
        Sequential = $Sequential
    }
}

function Format-ResourcePolicy($Policy) {
    $background = if ($Policy.Background) { "background suites queued beside the current one" } else { 'every suite alone' }
    return 'test jobs {0} ({1}), build jobs {2} ({3}), Linux suite workers {4}, heavy suites alone, {5}' -f $Policy.TestJobs, $Policy.TestJobsFrom,
        $Policy.BuildJobs, $Policy.BuildJobsFrom, $Policy.LinuxTestJobs, $background
}

# ---- processes

function ConvertTo-CmdArg([string]$Text) {
    if ($Text -eq '' -or $Text -match '[\s"&|<>^()]') { return '"' + ($Text -replace '"', '""') + '"' }
    return $Text
}

# Waits for the memory guard, then starts the program with its output going to a file, never to memory. Class and EstimateGB
# say what the stage is expected to need; NoWait: only for the helpers that stop other processes.
function Start-Proc([string]$File, [string[]]$Arguments = @(), [string]$WorkDir = $root, [int]$TimeoutSec = 900, [string]$Label = '',
                    [string]$Class = 'normal', [switch]$NoWait, [double]$EstimateGB = 0) {
    if (-not $Label) { $Label = [IO.Path]::GetFileName($File) }
    if (-not $NoWait) { [void](Wait-MemoryRoom $Label $Class $EstimateGB) }
    New-Item -ItemType Directory -Force -Path $script:GateTempDir | Out-Null
    $out = Join-Path $script:GateTempDir ('out-{0}.txt' -f [guid]::NewGuid().ToString('N'))
    $command = (@($File) + $Arguments | ForEach-Object { ConvertTo-CmdArg $_ }) -join ' '
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = '/d /s /c "{0} < nul > "{1}" 2>&1"' -f $command, $out
    $info.WorkingDirectory = $WorkDir
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    # cmd.exe truncates a PATH over 8191 characters and then cannot find wsl.exe: child processes get it de-duplicated.
    $info.EnvironmentVariables['PATH'] = Get-CleanPathValue $env:PATH
    $process = [System.Diagnostics.Process]::Start($info)
    $handle = [pscustomobject]@{ Process = $process; Out = $out; Command = $command; WorkDir = $WorkDir; Timeout = $TimeoutSec
                                 Clock = [System.Diagnostics.Stopwatch]::StartNew(); Label = $Label; Abort = $null; PeakBytes = 0L }
    $script:GateProcesses.Add($handle)
    return $handle
}

$script:OutputTailBytes = 4MB
$script:PassLinesKept = 60
$script:FailLinesKept = 500

# The whole output, unless it is larger than OutputTailBytes: then its end, which holds the counts and the failures.
function Read-ProcOutput([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $skipped = [Math]::Max(0L, $stream.Length - $script:OutputTailBytes)
        [void]$stream.Seek($skipped, 'Begin')
        $text = (New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)).ReadToEnd()
    } finally { $stream.Dispose() }
    $lines = @()
    if ($text.Length -gt 0) { $lines = @($text -split "\r?\n") }
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = @($lines[0..($lines.Count - 2)]) }
    if ($skipped -gt 0) { $lines = @("(output truncated: the first $skipped bytes are not shown)") + @($lines | Select-Object -Skip 1) }
    return $lines
}

function Test-HandleLive($Handle) {
    return ($null -ne $Handle -and $Handle -isnot [string] -and $Handle.PSObject.Properties['Process'] -and $null -ne $Handle.Process)
}

function Test-HandleDone($Handle) {
    if ($Handle.Process.HasExited) { return $true }
    $clock = $Handle.PSObject.Properties['Clock']
    return ($clock -and $Handle.Clock.Elapsed.TotalSeconds -gt $Handle.Timeout)
}

function Get-HandleAbort($Handle) {
    if ($Handle.PSObject.Properties['Abort'] -and $Handle.Abort) { return @($Handle.Abort) }
    return @()
}

$script:WaitTick = $null

# The run time of a finished process: its own start and exit times, unless the system reports no exit time (the FILETIME epoch,
# 1601) or one that does not fit the handle's monotonic clock; then that clock.
function Get-ProcSeconds($Handle) {
    $measured = [Math]::Max(0.0, $Handle.Clock.Elapsed.TotalSeconds)
    try {
        $span = ($Handle.Process.ExitTime - $Handle.Process.StartTime).TotalSeconds
        if ($span -ge 0 -and $span -le $measured + 1) { return $span }
    } catch { $null = $_ }
    return $measured
}

function Wait-Proc($Handle) {
    while (-not (Test-HandleDone $Handle)) {
        [void]$Handle.Process.WaitForExit(250)
        Invoke-MemoryWatch
        if ($script:WaitTick) { & $script:WaitTick }
    }
    $timedOut = -not $Handle.Process.HasExited
    if ($timedOut) {
        & taskkill.exe /PID $Handle.Process.Id /T /F 2>&1 | Out-Null
        $Handle.Process.WaitForExit()
    }
    $code = $Handle.Process.ExitCode
    $seconds = if ($timedOut) { $Handle.Clock.Elapsed.TotalSeconds } else { Get-ProcSeconds $Handle }
    [void]$script:GateProcesses.Remove($Handle)
    $Handle.Process.Dispose()
    $lines = @(Read-ProcOutput $Handle.Out)
    Remove-Item -LiteralPath $Handle.Out -Force -ErrorAction SilentlyContinue
    if ($timedOut) { $lines += "TIMEOUT: stopped after $($Handle.Timeout) s"; $code = 124 }
    $abort = @(Get-HandleAbort $Handle)
    if ($abort.Count) { $lines += $abort; $code = 137 }
    $peak = if ($Handle.PSObject.Properties['PeakBytes']) { [long]$Handle.PeakBytes } else { 0L }
    return [pscustomobject]@{ Code = $code; Lines = $lines; Command = $Handle.Command; WorkDir = $Handle.WorkDir; Seconds = $seconds; PeakBytes = $peak }
}

function Invoke-Process([string]$File, [string[]]$Arguments = @(), [string]$WorkDir = $root, [int]$TimeoutSec = 900, [string]$Label = '', [string]$Class = 'normal') {
    return Wait-Proc (Start-Proc $File $Arguments $WorkDir $TimeoutSec $Label $Class)
}

# Only the process trees this gate started: taskkill /T follows the parent links below each of our own PIDs. A PID is never
# reused while its handle is open, so nothing outside the gate can be hit.
function Stop-GateProcesses {
    $stopped = 0
    foreach ($handle in $script:GateProcesses.ToArray()) {
        try {
            if (-not $handle.Process.HasExited) {
                & taskkill.exe /PID $handle.Process.Id /T /F 2>&1 | Out-Null
                [void]$handle.Process.WaitForExit(10000)
                $stopped++
                Stop-LinuxSide $handle
            }
            [void]$script:GateProcesses.Remove($handle)
            $handle.Process.Dispose()
        } catch { $null = $_ }
        Remove-Item -LiteralPath $handle.Out -Force -ErrorAction SilentlyContinue
    }
    $script:GateProcesses.Clear()
    return $stopped
}

# A WSL stage also ends its own process tree inside the distribution: killing wsl.exe alone may leave it running.
function Stop-LinuxSide($Handle) {
    if (-not $Handle.PSObject.Properties['LinuxStopArguments']) { return }
    try { [void](Wait-Proc (Start-Proc 'wsl.exe' $Handle.LinuxStopArguments $root 60 'stop the Linux gate' -NoWait)) } catch { $null = $_ }
}

# Removes the run's temporary folder only when it is <temp>\jpt-test-*.
function Remove-GateTempDir([string]$Path) {
    if (-not $Path) { return $false }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    if ((Split-Path $full -Parent) -ne $temp -or -not (Split-Path $full -Leaf).StartsWith('jpt-test-')) { return $false }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue }
    return $true
}

# ---- scheduler

function Get-SuiteClass($Suite) {
    $class = $Suite.Options['Memory']
    if ($class -in 'light', 'normal', 'heavy', 'build', 'wsl', 'sanitize') { return $class }
    return 'normal'
}

# '' when a suite of this class may start beside the running ones, else why not. The memory guard decides separately.
function Get-StartBlocker($Policy, [string[]]$Running, [string]$Class) {
    $count = @($Running).Count
    if ($count -eq 0) { return '' }
    if ($count -ge $Policy.TestJobs) { return 'jobs' }
    if ($Class -ne 'light' -and $Class -ne 'normal') { return 'heavy' }
    if (@($Running | Where-Object { $_ -ne 'light' -and $_ -ne 'normal' }).Count) { return 'heavy' }
    return ''
}

# Launch: param($Suite) -> a handle with Process, or a finished result. Memory: param($Class) -> '' or why a background suite
# may not start now (it never waits). Pause: param($Ms). Notify: param($Kind, $Entry) for 'start' and 'finish' of background suites.
function New-SuiteScheduler($Policy, [scriptblock]$Launch, [scriptblock]$Memory, [scriptblock]$Pause, [scriptblock]$Notify = { }) {
    [pscustomobject]@{
        Policy = $Policy; Launch = $Launch; Memory = $Memory; Pause = $Pause; Notify = $Notify
        Queue = (New-Object System.Collections.Generic.List[object]); Running = (New-Object System.Collections.Generic.List[object])
        Finished = @{}; Started = (New-Object System.Collections.Generic.List[string])
        Peak = 0; HeavyOverlap = 0; HeavyWaits = 0
    }
}

function Add-BackgroundSuite($S, $Suite) { if ($S.Policy.Background) { $S.Queue.Add($Suite) } }

function Get-RunningClasses($S) { return , [string[]]@($S.Running | ForEach-Object { $_.Class }) }

function Start-SchedulerEntry($S, $Suite, [bool]$Foreground) {
    $class = Get-SuiteClass $Suite
    $S.Started.Add($Suite.Name)
    $entry = [pscustomobject]@{ Suite = $Suite; Class = $class; Handle = $null; Foreground = $Foreground }
    $entry.Handle = & $S.Launch $Suite
    if (Test-HandleLive $entry.Handle) {
        $S.Running.Add($entry)
        $S.Peak = [Math]::Max($S.Peak, $S.Running.Count)
        if ($S.Running.Count -gt 1 -and (Get-RunningClasses $S) -contains 'heavy') { $S.HeavyOverlap++ }
        if (-not $Foreground) { & $S.Notify 'start' $entry }
    } elseif (-not $Foreground) {
        $S.Finished[$Suite.Name] = $entry.Handle
        & $S.Notify 'finish' $entry
    }
    return $entry
}

# Collects finished background suites, then starts queued ones while the policy and the memory guard allow. One of the test
# jobs always stays for the suite in list order.
function Step-Scheduler($S, [switch]$ReapOnly) {
    foreach ($entry in $S.Running.ToArray()) {
        if ($entry.Foreground -or -not (Test-HandleDone $entry.Handle)) { continue }
        [void]$S.Running.Remove($entry)
        $S.Finished[$entry.Suite.Name] = $entry.Handle
        & $S.Notify 'finish' $entry
    }
    if ($ReapOnly) { return }
    while ($S.Queue.Count) {
        if (@($S.Running | Where-Object { -not $_.Foreground }).Count -ge $S.Policy.TestJobs - 1) { break }
        $next = $S.Queue[0]
        $class = Get-SuiteClass $next
        if ((Get-StartBlocker $S.Policy (Get-RunningClasses $S) $class) -or (& $S.Memory $class)) { break }
        $S.Queue.RemoveAt(0)
        [void](Start-SchedulerEntry $S $next $false)
    }
}

# Runs one suite of the list in order and returns its handle: the background run when it already started, else a foreground
# run once no job slot or heavy suite stands in the way (the memory guard holds the start itself).
function Invoke-SchedulerSuite($S, $Suite) {
    $name = $Suite.Name
    for ($i = 0; $i -lt $S.Queue.Count; $i++) { if ($S.Queue[$i].Name -eq $name) { $S.Queue.RemoveAt($i); break } }
    while (@($S.Running | Where-Object { $_.Suite.Name -eq $name }).Count) { & $S.Pause 200; Step-Scheduler $S }
    if ($S.Finished.ContainsKey($name)) {
        $handle = $S.Finished[$name]
        $S.Finished.Remove($name)
        return $handle
    }
    $class = Get-SuiteClass $Suite
    $counted = $false
    while (Get-StartBlocker $S.Policy (Get-RunningClasses $S) $class) {
        if (-not $counted) { $counted = $true; $S.HeavyWaits++ }
        & $S.Pause 200
        Step-Scheduler $S -ReapOnly
    }
    $entry = Start-SchedulerEntry $S $Suite $true
    if (Test-HandleLive $entry.Handle) {
        while (-not (Test-HandleDone $entry.Handle)) { & $S.Pause 200; Step-Scheduler $S }
        [void]$S.Running.Remove($entry)
    }
    return $entry.Handle
}

# Waits for every background suite; nothing of the Windows half may overlap the Linux half.
function Wait-SchedulerIdle($S) {
    $S.Queue.Clear()
    while ($S.Running.Count) { & $S.Pause 200; Step-Scheduler $S -ReapOnly }
}
