# Contract of tools\test.ps1: it parses, takes every path from the repository, runs from any folder, lists and filters its
# suites, fails on a failing suite, a missing dependency, a missing build, a changed working tree or a test nobody wired in.
# The gate runs in child PowerShell processes, on this repository with -List and one cheap suite, and on a throw-away
# repository in a temporary folder with stand-in commands. No game, launcher, window, audio, network or compiler is used.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
$gate = Join-Path $root 'tools\test.ps1'
$shell = (Get-Process -Id $PID).Path
$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name"
    }
}

function Invoke-Gate([string]$Script, [string[]]$Arguments, [string]$Path = $env:PATH, [string]$WorkDir = $root) {
    $savedPath = $env:PATH
    Push-Location -LiteralPath $WorkDir
    try {
        $env:PATH = $Path
        $text = & $shell -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments *>&1 | Out-String
        return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
    } finally {
        $env:PATH = $savedPath
        Pop-Location
    }
}

function New-StandIn([string]$Folder, [string]$Name, [string[]]$Lines, [int]$Code) {
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    $body = @('@echo off') + $Lines + "exit /b $Code"
    Set-Content -LiteralPath (Join-Path $Folder $Name) -Value $body -Encoding ascii
}

$work = Join-Path ([IO.Path]::GetTempPath()) "jpt-gate-tests-$PID"
try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    # A real gate run on this repository needs the free RAM its memory guard asks for; without it those checks fail at once
    # instead of waiting for the guard to give up.
    . (Join-Path $root 'tools\_memory_guard.ps1')
    [void](Initialize-MemoryProbe)
    $realMemory = Get-MemorySample
    $realLimits = Get-MemoryLimits $realMemory.TotalGB
    . (Join-Path $root 'tools\_test_history.ps1')
    $memoryReady = $realMemory.FreeGB -gt $realLimits.EmergencyGB
    $memoryNote = 'free RAM {0:N2} GB is at or under the {1:N1} GB start floor: close applications and run again' -f $realMemory.FreeGB, $realLimits.EmergencyGB
    Check $memoryReady "real gate runs on this repository: $memoryNote"

    $text = [IO.File]::ReadAllText($gate)
    $errors = $null
    $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$tokens, [ref]$errors)
    Check (@($errors).Count -eq 0) 'tools\test.ps1 parses'
    Check ($text -notmatch '(?m)^[^#\r\n]*\b[A-Za-z]:\\\\?[A-Za-z]') 'no drive-letter path in the gate'
    Check ($text -match '\$PSScriptRoot' -and $text -notmatch '(?i)\b(Set-Location|Push-Location|Pop-Location)\b|^\s*cd\s') 'paths come from the script folder; the caller''s folder is never used or changed'
    Check ($text -notmatch '(?i)Start-Process|build\.ps1\s+-Run|Start-Proc\s+[^\r\n]*(trespasser_recomp|jpt_launcher)') 'the gate starts neither the game nor the launcher'
    Check ($text -notmatch '(?i)Invoke-WebRequest|Invoke-RestMethod|Start-BitsTransfer|\bcurl\b|git\s+(fetch|clone|pull|push|commit|checkout|reset|clean|rebase|merge|tag)\b') 'the gate uses no network and changes no git state'
    Check ($text -match 'SDL_VIDEODRIVER = ''dummy''' -and $text -match 'SDL_AUDIODRIVER = ''dummy''') 'child processes get dummy SDL video and audio drivers'
    Check ($text -notmatch 'Invoke-Python|Join-Path \$root ''tools\\(build|run)\.ps1''') 'the gate does not call the build or run scripts'

    Check ($text.Contains("'-t', 'commands'") -and $text.Contains('JPT_GATE_COMMANDS')) 'the compile commands are exported from ninja for the static warning check, with or without a recompile'

    $command = '.\tools\test.ps1'
    foreach ($doc in 'README.md', 'docs\BUILD.md') {
        $docText = [IO.File]::ReadAllText((Join-Path $root $doc))
        Check ($docText.Contains($command) -and -not $docText.Contains("`t")) "$doc cites $command with literal backslashes and no TAB"
    }

    $common = [IO.File]::ReadAllText((Join-Path $root 'tools\_common.ps1'))
    $build = [IO.File]::ReadAllText((Join-Path $root 'tools\build.ps1'))
    Check ($text -match 'Enter-Msvc \$Arch' -and $build -match 'Enter-Msvc \$Arch' -and $text -notmatch '(?i)vcvarsall|vswhere\.exe\s+-' ) 'the gate and build.ps1 enter MSVC through the same Enter-Msvc helper, with no toolchain discovery of their own'
    Check ($common -match '(?s)function Enter-Msvc.*Reset-MsvcEnvironment.*vcvarsall' -and $common.Contains('amd64_x86')) 'Enter-Msvc resets the environment before vcvarsall and keeps the x64-hosted x86 cross tools'
    Check ($text -notmatch "[`t]") 'tools\test.ps1 has no TAB that a broken path escape could have left'
    $long = (1..400 | ForEach-Object { "C:\jpt\entry$($_ % 40)" }) -join ';'
    $probe = ". '$($root)\tools\_common.ps1'; `$env:PATH = '$long'; Reset-MsvcEnvironment; `$env:PATH.Split(';').Count"
    $probed = (& $shell -NoProfile -ExecutionPolicy Bypass -Command $probe *>&1 | Out-String).Trim()
    Check ($probed -eq '40') 'Reset-MsvcEnvironment removes repeated PATH entries, so vcvarsall stays under the cmd line limit'

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if ($memoryReady -and (Test-Path -LiteralPath $vswhere)) {
        $coldScript = Join-Path $work 'cold-start.ps1'
        [IO.File]::WriteAllText($coldScript, @'
param([string]$Gate, [string]$Mode)
if ($Mode -eq 'contaminated') {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -property installationPath | Select-Object -First 1
    $dump = & cmd.exe /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" amd64 >nul 2>&1 && set"
    foreach ($line in $dump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2] } }
    $env:PATH = "$env:PATH;$env:PATH"
    $env:VSCMD_RESIDUE = '1'
}
function Get-State { (([Environment]::GetEnvironmentVariables('Process').GetEnumerator() | Where-Object { "$($_.Key)" -notlike '=*' -and $_.Value } | Sort-Object Key | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join "`n") }
$before = Get-State
$output = & $Gate -Filter launcher_tests *>&1 | Out-String
$code = $LASTEXITCODE
Write-Output "RESULT code=$code same=$($before -eq (Get-State)) passed=$($output -match 'PASSED \(partial run\)') vcvars=$($output -match 'vcvarsall')"
'@)
        $hosts = @($shell)
        $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        if ((Test-Path -LiteralPath $windowsPowerShell) -and $windowsPowerShell -ne $shell) { $hosts += $windowsPowerShell }
        foreach ($hostExe in $hosts) {
            foreach ($mode in 'normal', 'contaminated') {
                $label = "first run in a fresh $(Split-Path -Leaf $hostExe) process, $mode environment"
                $lines = & $hostExe -NoProfile -ExecutionPolicy Bypass -File $coldScript -Gate $gate -Mode $mode 2>&1 | Out-String
                Check ($lines -match 'RESULT code=0 same=True passed=True vcvars=False') "${label}: passes at once and leaves the caller's environment as it was"
            }
        }
    }

    $linuxHelper = [IO.File]::ReadAllText((Join-Path $root 'tools\_gate_linux.ps1'))
    $linuxErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($linuxHelper, [ref]$tokens, [ref]$linuxErrors)
    Check (@($linuxErrors).Count -eq 0) 'tools\_gate_linux.ps1 parses'
    Check ($linuxHelper -match 'wsl\.exe' -and $linuxHelper -match "'wslpath'" -and $linuxHelper -match '--native-build' -and $linuxHelper -match 'test-linux\.sh') 'the Linux half goes PowerShell -> wsl.exe -> wslpath -> bash -> tools/test-linux.sh'
    Check ($linuxHelper -match "JptFocalBuild" -and $linuxHelper -match '2\.31') 'JptFocalBuild and the glibc 2.31 baseline are what the distribution choice prefers'
    Check ($linuxHelper -notmatch '(?i)/mnt/[a-z]/' -and $linuxHelper -notmatch '(?i)Start-Process|Invoke-WebRequest|wine|proton') 'no hard-coded /mnt path, no process shell-out and no Wine in the Linux half'
    Check ($text -notmatch '(?i)trespasser_recomp(\.exe)?\s+--run|--run') 'the gate never runs the game'
    Check ($text.Contains('[switch]$WindowsOnly') -and $text.Contains('[switch]$LinuxOnly') -and $text.Contains('[switch]$SkipLinux') -and $text.Contains('[string]$LinuxDistro') -and $text.Contains('[switch]$Sanitize')) 'the options of the two halves exist'
    Check ($text -match '(?m)^\s*\[switch\]\$Build\s*,' -and $text -match '(?m)^\s*\[string\]\$BuildDir\s*=') '-Build is an explicit switch, so it is never read as an abbreviation of -BuildDir'

    . (Join-Path $root 'tools\_gate_linux.ps1')
    $passEvidence = @([pscustomobject]@{ suite = 'a' })
    $parityContract = [pscustomobject]@{ features = @(
        [pscustomobject]@{ id = 'both'; scope = 'global'; shared = $passEvidence; windows = [pscustomobject]@{ status = 'PASS'; evidence = @() }; linux = [pscustomobject]@{ status = 'PASS'; evidence = @() } },
        [pscustomobject]@{ id = 'linux-gap'; scope = 'global'; shared = @(); windows = [pscustomobject]@{ status = 'PASS'; evidence = $passEvidence }; linux = [pscustomobject]@{ status = 'BLOCKED'; evidence = @(); reason = 'not ported' } },
        [pscustomobject]@{ id = 'wrongly-platform-only'; scope = 'global'; shared = @(); windows = [pscustomobject]@{ status = 'PASS'; evidence = $passEvidence }; linux = [pscustomobject]@{ status = 'PLATFORM_ONLY'; evidence = @(); reason = 'x' } },
        [pscustomobject]@{ id = 'd3d11'; scope = 'windows'; shared = @(); windows = [pscustomobject]@{ status = 'PASS'; evidence = $passEvidence }; linux = [pscustomobject]@{ status = 'PLATFORM_ONLY'; evidence = @(); reason = 'Windows only' } },
        [pscustomobject]@{ id = 'failing'; scope = 'global'; shared = @(); windows = [pscustomobject]@{ status = 'PASS'; evidence = @([pscustomobject]@{ suite = 'bad' }) }; linux = [pscustomobject]@{ status = 'PASS'; evidence = $passEvidence } },
        [pscustomobject]@{ id = 'no-data'; scope = 'global'; shared = @(); windows = [pscustomobject]@{ status = 'PASS'; evidence = $passEvidence }; linux = [pscustomobject]@{ status = 'PASS'; evidence = @([pscustomobject]@{ suite = 'unavailable' }) } },
        [pscustomobject]@{ id = 'future'; scope = 'planned'; shared = @(); windows = [pscustomobject]@{ status = 'BLOCKED'; evidence = @(); reason = 'later' }; linux = [pscustomobject]@{ status = 'BLOCKED'; evidence = @(); reason = 'later' } }) }
    $windowsMap = @{ a = [pscustomobject]@{ Status = 'PASS' }; bad = [pscustomobject]@{ Status = 'FAIL' } }
    $linuxMap = @{ a = [pscustomobject]@{ Status = 'PASS' }; unavailable = [pscustomobject]@{ Status = 'PASS'; Unavailable = $true } }
    $parity = @{}
    foreach ($row in @(Get-FeatureParity $parityContract $windowsMap $linuxMap $true $true '')) { $parity[$row.Id] = $row }
    Check ($parity['both'].Parity -eq 'PASS') 'parity: a shared suite that passed on both platforms is PASS'
    Check ($parity['linux-gap'].Parity -eq 'BLOCKED' -and $parity['linux-gap'].Linux -eq 'BLOCKED') 'parity: a global feature Windows proves and Linux does not is BLOCKED, never hidden'
    Check ($parity['wrongly-platform-only'].Parity -eq 'FAIL') 'parity: a global feature cannot hide behind PLATFORM_ONLY on Linux'
    Check ($parity['d3d11'].Parity -eq 'PASS' -and $parity['d3d11'].Linux -eq 'PLATFORM_ONLY') 'parity: a Windows-only feature with Linux PLATFORM_ONLY is justified'
    Check ($parity['failing'].Parity -eq 'FAIL' -and $parity['failing'].Windows -eq 'FAIL') 'parity: a failed evidence suite fails the feature'
    Check ($parity['no-data'].Parity -eq 'BLOCKED' -and $parity['no-data'].Linux -eq 'BLOCKED') 'parity: DATA UNAVAILABLE is BLOCKED, not PASS'
    Check ($parity['future'].Parity -eq 'PLANNED') 'parity: a planned feature is neither a pass nor a failure'
    $linuxDown = @{}
    foreach ($row in @(Get-FeatureParity $parityContract $windowsMap $linuxDown $true $false 'BLOCKED: WSL unavailable')) { $linuxDown[$row.Id] = $row }
    Check ($linuxDown['both'].Linux -eq 'BLOCKED' -and $linuxDown['both'].Parity -eq 'BLOCKED' -and $linuxDown['both'].Reason -match 'WSL unavailable') 'parity: without WSL no Linux row is PASS and the reason is named'

    # The job policy, the memory guard, the scheduler, the progress and the history in this process, on simulated RAM, time and
    # processes, then on a few real processes of some MB each.
    & {
        foreach ($helper in '_common.ps1', '_progress.ps1', '_test_history.ps1', '_memory_guard.ps1', '_test_runner.ps1') { . (Join-Path $root "tools\$helper") }
        $ErrorActionPreference = 'Continue'
        $script:GateTempDir = Join-Path $work 'runner'

        $policyOf = { param([hashtable]$More = @{}) Get-ResourcePolicy @More }
        $default = & $policyOf
        Check ($default.TestJobs -eq 1 -and $default.BuildJobs -eq 1 -and -not $default.Background -and $default.LinuxTestJobs -eq 0 -and $default.HeavyExclusive) 'jobs: one test process, one compiler, no background suite and serial Linux suites by default, whatever the machine'
        $fromEnv = & $policyOf @{ EnvTestJobs = '3'; EnvBuildJobs = '2' }
        Check ($fromEnv.TestJobs -eq 3 -and $fromEnv.TestJobsFrom -eq 'JPT_TEST_JOBS' -and $fromEnv.BuildJobs -eq 2 -and $fromEnv.BuildJobsFrom -eq 'JPT_BUILD_JOBS') 'jobs: JPT_TEST_JOBS and JPT_BUILD_JOBS replace the defaults'
        $badEnv = & $policyOf @{ EnvTestJobs = 'many'; EnvBuildJobs = '0' }
        Check ($badEnv.TestJobs -eq 1 -and $badEnv.BuildJobs -eq 1) 'jobs: an invalid JPT_TEST_JOBS or JPT_BUILD_JOBS is ignored'
        $explicit = & $policyOf @{ EnvTestJobs = '3'; EnvBuildJobs = '2'; TestJobs = 8; BuildJobs = 3 }
        Check ($explicit.TestJobs -eq 8 -and $explicit.BuildJobs -eq 3 -and $explicit.TestJobsFrom -eq '-TestJobs' -and $explicit.HeavyExclusive) '-TestJobs and -BuildJobs win over the variables; heavy suites still run alone'
        $sequential = & $policyOf @{ Sequential = $true; TestJobs = 4 }
        Check ($sequential.TestJobs -eq 1 -and -not $sequential.Background -and $sequential.LinuxTestJobs -eq 0) '-Sequential: one suite at a time in list order even with -TestJobs'
        $sanitize = & $policyOf @{ Sanitize = $true; TestJobs = 4; BuildJobs = 4 }
        Check ($sanitize.TestJobs -eq 1 -and $sanitize.BuildJobs -eq 1 -and $sanitize.LinuxTestJobs -eq 0) '-Sanitize: one suite and one compiler at a time, whatever is asked'

        $l32 = Get-MemoryLimits 31.8
        Check ($l32.SoftGB -eq 8.0 -and $l32.HardGB -eq 9.5 -and $l32.EmergencyGB -eq 0.5 -and $l32.WarningGB -eq 2.0 -and $l32.EmergencyWaitSeconds -eq 30) 'memory: 31.8 GB gives soft 8.0 (25%), hard 9.5 (30%), a start floor of 0.5 GB, a warning under 2.0 GB and a 30 s wait at the floor'
        Check ((@($l32.Estimates.Keys) -join ',') -eq 'light,normal,heavy,build,wsl,sanitize' -and $l32.Estimates['light'] -eq 0.25 -and $l32.Estimates['normal'] -eq 0.75 -and $l32.Estimates['heavy'] -eq 1.5 -and $l32.Estimates['build'] -eq 2.0 -and $l32.Estimates['wsl'] -eq 2.0 -and $l32.Estimates['sanitize'] -eq 2.5) 'memory: one table of stage estimates, kept for diagnostics and the history'
        $lSan = Get-MemoryLimits 31.8 $true
        Check ($lSan.SoftGB -eq 6.4 -and $lSan.HardGB -eq 8.0 -and $lSan.EmergencyGB -eq 0.5) 'memory: sanitizers lower the gate limits to 20% and 25%'
        $starts = @(foreach ($free in 5.0, 3.0, 1.96, 1.0, 0.57) { foreach ($class in 'light', 'normal', 'heavy', 'build', 'wsl', 'sanitize') { Get-MemoryAdmission $l32 0.23 $free $class 0 } })
        Check (@($starts | Where-Object { $_ -ne 'START' }).Count -eq 0 -and $starts.Count -eq 30) 'memory: with the gate tree at 0.23 GB a serial stage of any kind (heavy, build and WSL included) starts with 5, 3, 1.96, 1.0 or 0.57 GB free'
        Check ((Get-MemoryAdmission $l32 0.23 0.49 'normal' 0) -eq 'WAIT' -and (Get-MemoryAdmission $l32 0.23 0.5 'build' 0) -eq 'WAIT' -and (Get-MemoryAdmission $l32 0.23 0.8 'normal' 0) -eq 'START') 'memory: with no stage running, at or under the 0.5 GB start floor the next stage waits; at 0.8 GB it starts'
        Check ((Get-MemoryAdmission $l32 0.23 1.0 'heavy' 0 3.0) -eq 'START') 'memory: an estimate or a measured peak larger than the free RAM never blocks a serial stage'
        Check ((Get-MemoryAdmission $l32 8.5 15 'normal' 0) -eq 'START' -and (Get-MemoryAdmission $l32 8.5 15 'normal' 1) -eq 'WAIT' -and (Get-MemoryAdmission $l32 9.6 15 'normal' 0) -eq 'WAIT') 'memory: over the soft limit the only stage still starts and a second one waits; over the hard limit nothing starts'
        Check ((Get-MemoryStartBlocker $l32 0.23 5 'normal' 1) -eq '' -and (Get-MemoryStartBlocker $l32 0.23 1.0 'normal' 1) -eq 'memory') 'memory: a background stage beside a running one needs its estimate free above the floor'
        Check ((Get-MemoryAbortReason $l32 9.6) -eq 'MEMORY LIMIT EXCEEDED' -and (Get-MemoryAbortReason $l32 0.62) -eq '' -and (Get-MemoryAbortReason $l32 8.5) -eq '') 'memory: only the gate''s tree over its 9.5 GB hard limit stops a running stage; neither its soft limit nor the system''s free RAM does'
        Check ((Get-FreeMemoryLevel $l32 5.0) -eq 0 -and (Get-FreeMemoryLevel $l32 1.0) -eq 1 -and (Get-FreeMemoryLevel $l32 0.49) -eq 2 -and (Get-FreeMemoryLevel $l32 0.09) -eq 2) 'memory: free RAM under 2 GB is low, at or under 0.5 GB critical'

        $GB = [long]1GB
        $world = [pscustomobject]@{ Stage = 0; Bytes = 0L; Free = 15.0 }
        $fakeProcess = { param($Id, $Parent, $Name, $Bytes) [pscustomobject]@{ Id = $Id; ParentId = $Parent; Name = $Name; Created = 1000 + $Id; Bytes = [long]$Bytes } }
        $simulatedSample = {
            $list = @((& $fakeProcess 100 4 'pwsh.exe' (0.2 * $GB)), (& $fakeProcess 300 4 'chrome.exe' (6 * $GB)), (& $fakeProcess 301 300 'chrome.exe' (3 * $GB)))
            if ($world.Stage) { $list += @((& $fakeProcess $world.Stage 100 'cmd.exe' (4MB)), (& $fakeProcess ($world.Stage + 1) $world.Stage 'cl.exe' $world.Bytes)) }
            [pscustomobject]@{ TotalGB = 31.8; FreeGB = $world.Free; CommitFreeGB = 20.0; Processes = $list }
        }.GetNewClosure()
        $killed = New-Object System.Collections.Generic.List[object]
        $killer = { param($Guard, $Stage, $Members, $Handle) $killed.Add([pscustomobject]@{ Stage = $Stage; Ids = @($Members | ForEach-Object { $_.Id }) }); if ($Handle) { $Handle.Process.HasExited = $true }; return @() }.GetNewClosure()
        $newGuard = {
            $guard = New-MemoryGuard $l32 100 $true
            $guard.Sampler = $simulatedSample; $guard.Kill = $killer; $guard.Sleep = { param($Ms) }; $guard.Interval = 0
            $guard
        }
        $fakeStage = { param([int]$Id, [string]$Label) [pscustomobject]@{ Process = [pscustomobject]@{ Id = $Id; HasExited = $false }; Label = $Label; Abort = $null; Command = $Label; Out = '' } }

        $script:Guard = & $newGuard
        $warnings = New-Object System.Collections.Generic.List[string]
        $script:Guard.OnWarn = { param($Text) $warnings.Add($Text) }.GetNewClosure()
        $suiteA = & $fakeStage 200 'suite_a_tests'
        $script:GateProcesses.Add($suiteA)
        $world.Stage = 200; $world.Bytes = 4 * $GB
        Invoke-MemoryWatch -Force
        Check (-not $suiteA.Abort -and [Math]::Round($script:Guard.GateBytes / $GB, 1) -eq 4.2 -and $script:Guard.Largest.Name -eq 'cl.exe') 'memory: the tree sums the gate, the stage and its child (4.2 GB) and leaves chrome out'
        $world.Bytes = [long](8.5 * $GB)
        Invoke-MemoryWatch -Force
        Check (-not $suiteA.Abort -and $killed.Count -eq 0) 'memory: one stage over the soft limit and under the hard limit keeps running'
        $world.Bytes = [long](9.5 * $GB)
        Invoke-MemoryWatch -Force
        $abortText = @($suiteA.Abort) -join "`n"
        Check ($abortText.StartsWith('MEMORY LIMIT EXCEEDED: suite_a_tests was stopped') -and $abortText.Contains('cl.exe, PID 201, 9.5 GB') -and $abortText.Contains('soft 8.0 GB, hard 9.5 GB') -and $abortText.Contains('system free      15.0 GB (warning below 2.0 GB, start floor 0.5 GB)')) 'memory: gate 9.7 GB stops the stage and names the memory, the limits, the free RAM, the stage and its largest process'
        Check ($killed.Count -eq 1 -and $killed[0].Stage -eq 200 -and (@($killed[0].Ids | Sort-Object) -join ',') -eq '200,201') 'memory: only the processes of that stage are stopped, never the gate itself or another application'
        Check ([Math]::Round($script:Guard.PeakGateBytes / $GB, 1) -eq 9.7 -and $script:Guard.PeakProcess.Name -eq 'cl.exe' -and $script:Guard.PeakProcess.Id -eq 201 -and $script:Guard.Aborts.Count -eq 1) 'memory: the peak of the tree and the largest process are recorded'
        [void]$script:GateProcesses.Remove($suiteA)
        $suiteB = & $fakeStage 210 'tests\test_linux_parity.py'
        $suiteB | Add-Member -NotePropertyName PeakBytes -NotePropertyValue 0L
        $script:GateProcesses.Add($suiteB)
        $world.Stage = 210; $world.Bytes = [long](0.03 * $GB); $world.Free = 1.96
        Invoke-MemoryWatch -Force
        Check (-not $suiteB.Abort -and $killed.Count -eq 1 -and [Math]::Round($script:Guard.GateBytes / $GB, 2) -eq 0.23 -and $warnings.Count -eq 1 -and $warnings[0] -eq 'LOW SYSTEM MEMORY: 1.96 GB free; tests\test_linux_parity.py keeps running (the gate''s processes use 0.23 GB, hard limit 9.5 GB)') 'memory regression: test_linux_parity.py with the gate at 0.23 GB and 1.96 GB free keeps running, with a LOW SYSTEM MEMORY warning'
        foreach ($free in 1.0, 0.57) { $world.Free = $free; Invoke-MemoryWatch -Force }
        Check (-not $suiteB.Abort -and $killed.Count -eq 1 -and $warnings.Count -eq 1) 'memory: other applications pushing the free RAM to 1.0 and 0.57 GB never stop the suite, and the warning is not repeated'
        foreach ($free in 0.49, 0.09) { $world.Free = $free; Invoke-MemoryWatch -Force }
        Check (-not $suiteB.Abort -and $killed.Count -eq 1 -and $warnings.Count -eq 2 -and $warnings[1] -eq 'CRITICAL SYSTEM MEMORY: 0.49 GB free, at or under the start floor 0.5 GB; tests\test_linux_parity.py keeps running (the gate''s processes use 0.23 GB, hard limit 9.5 GB)') 'memory: free RAM falling to 0.49 and 0.09 GB with a suite running warns CRITICAL SYSTEM MEMORY once and lets the suite go on'
        [void]$script:GateProcesses.Remove($suiteB)
        $gateTests = & $fakeStage 220 'tests\gate_tests.ps1'
        $script:GateProcesses.Add($gateTests)
        $world.Stage = 220; $world.Bytes = [long](0.42 * $GB); $world.Free = 0.09
        Invoke-MemoryWatch -Force
        Check (-not $gateTests.Abort -and $killed.Count -eq 1 -and [Math]::Round($script:Guard.GateBytes / $GB, 2) -eq 0.62 -and $script:Guard.Limits.HardGB -eq 9.5) 'memory regression: tests\gate_tests.ps1 running with the gate at 0.62 GB of 9.5 GB and 0.09 GB free continues (no SYSTEM MEMORY SAFETY ABORT)'
        [void]$script:GateProcesses.Remove($gateTests)
        $world.Stage = 210
        Check ($suiteB.PeakBytes -ge [long](0.03 * $GB)) 'memory: each stage keeps the peak of its own tree for the history'
        [void]$script:GateProcesses.Remove($suiteB)

        $world.Stage = 0; $world.Free = 0.57
        $script:Guard = & $newGuard
        $waits = New-Object System.Collections.Generic.List[string]
        $lowStarts = New-Object System.Collections.Generic.List[string]
        $script:Guard.OnWait = { param($Text) $waits.Add($Text) }.GetNewClosure()
        $script:Guard.OnWarn = { param($Text) $lowStarts.Add($Text) }.GetNewClosure()
        $admission = Wait-MemoryRoom 'user_data_tests'
        Check ($admission -eq 'START' -and $waits.Count -eq 0 -and $script:Guard.Waits -eq 0 -and -not $script:Guard.Stopped -and $lowStarts.Count -eq 1 -and $lowStarts[0].StartsWith('LOW SYSTEM MEMORY: 0.57 GB free; user_data_tests starts anyway') -and (@($script:Guard.LowStarts) -join ',') -eq 'user_data_tests') 'memory regression: user_data_tests with 0.57 GB free starts at once with a warning: no 90 s wait, no INSUFFICIENT SAFE MEMORY'
        $world.Free = 1.0
        Check ((Wait-MemoryRoom 'ninja x86 build' 'build') -eq 'START' -and (Wait-MemoryRoom 'frontend_tests' 'heavy') -eq 'START' -and (Wait-MemoryRoom 'linux gate (WSL)' 'wsl') -eq 'START' -and $waits.Count -eq 0 -and $script:Guard.Starts -eq 4) 'memory: a build, a heavy suite and the WSL gate start with 1.0 GB free'
        $world.Free = 0.49
        $script:Guard.Tick = { if ($waits.Count -and $world.Free -lt 1) { $world.Free += 0.5 } }.GetNewClosure()
        $admission = Wait-MemoryRoom 'next_tests'
        Check ($admission -eq 'START' -and $waits.Count -eq 1 -and $waits[0] -eq 'waiting before next_tests: free 0.49 GB, at or under the start floor 0.5 GB; checking every second') 'memory: between stages at the start floor the next stage waits, and starts as soon as the free RAM is above it'
        $world.Free = 0.4
        $script:Guard = & $newGuard
        $script:Guard.Limits = Get-MemoryLimits 31.8
        $script:Guard.Limits.EmergencyWaitSeconds = 2
        $stoppedText = ''
        try { [void](Wait-MemoryRoom 'next_tests') } catch { $stoppedText = $_.Exception.Message }
        Check ($stoppedText -eq 'EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE: next_tests: for 2 s free 0.4 GB, at or under the start floor 0.5 GB' -and $(try { [void](Wait-MemoryRoom 'later_tests'); $false } catch { $true })) 'memory: only free RAM held at the start floor with nothing running stops the gate (EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE)'
        $world.Free = 15.0

        $checkpoint = New-RunCheckpoint 'fingerprint-1' 'config-1' 'suites-1'
        $added = @((Add-CheckpointResult $checkpoint 'a_tests' 'windows' 'PASS' 10), (Add-CheckpointResult $checkpoint 'l10n_tests' 'windows' 'PASS' 5 @('captions pt-BR: 9 lines')),
                   (Add-CheckpointResult $checkpoint 'memory_tests' 'windows' 'FAIL'), (Add-CheckpointResult $checkpoint 'later_tests' 'windows' 'PASS'))
        Check ((@($added) -join ',') -eq 'True,True,False,False' -and $checkpoint.passed.Count -eq 2 -and $checkpoint.broken) 'resume: the checkpoint keeps the suites that passed in order and stops at the first failure, memory stops included; nothing after it counts'
        $checkpointFile = Join-Path $work 'checkpoint\last-run.json'
        Save-RunCheckpoint $checkpoint $checkpointFile
        $names = @('a_tests', 'l10n_tests', 'memory_tests', 'later_tests')
        $verdict = Test-RunCheckpoint (Read-RunCheckpoint $checkpointFile) 'fingerprint-1' 'config-1' 'suites-1' $names
        Check ($verdict.Ok -and (@($verdict.Passed | ForEach-Object { $_.name }) -join ',') -eq 'a_tests,l10n_tests' -and (@($verdict.Passed)[1].lines -join '') -eq 'captions pt-BR: 9 lines') 'resume: a saved checkpoint is read back and resumes at the suite that failed, with the output a summary needs'
        $single = New-RunCheckpoint 'f' 'c' 's'
        [void](Add-CheckpointResult $single 'a_tests' 'windows' 'PASS' 1)
        Save-RunCheckpoint $single $checkpointFile
        Check ((Test-RunCheckpoint (Read-RunCheckpoint $checkpointFile) 'f' 'c' 's' $names).Passed.Count -eq 1) 'resume: a checkpoint of one suite reads back as a list'
        Save-RunCheckpoint $checkpoint $checkpointFile
        $refusals = @{ 'the sources changed' = @('fingerprint-2', 'config-1', 'suites-1', $names); 'the parameters changed' = @('fingerprint-1', 'config-2', 'suites-1', $names)
                       'the suite list changed' = @('fingerprint-1', 'config-1', 'suites-2', $names); 'does not follow the order' = @('fingerprint-1', 'config-1', 'suites-1', @('l10n_tests', 'a_tests')) }
        foreach ($why in $refusals.Keys) {
            $arguments = $refusals[$why]
            $verdict = Test-RunCheckpoint (Read-RunCheckpoint $checkpointFile) $arguments[0] $arguments[1] $arguments[2] $arguments[3]
            Check (-not $verdict.Ok -and ($verdict.Reasons -join ' ').Contains($why) -and $verdict.Passed.Count -eq 0) "resume: refused when $why"
        }
        [IO.File]::WriteAllText($checkpointFile, '{ broken json')
        $corrupt = Test-RunCheckpoint (Read-RunCheckpoint $checkpointFile) 'fingerprint-1' 'config-1' 'suites-1' $names
        [IO.File]::WriteAllText($checkpointFile, '{"schema": 7}')
        $foreign = Test-RunCheckpoint (Read-RunCheckpoint $checkpointFile) 'fingerprint-1' 'config-1' 'suites-1' $names
        $absent = Test-RunCheckpoint (Read-RunCheckpoint (Join-Path $work 'checkpoint\none.json')) 'fingerprint-1' 'config-1' 'suites-1' $names
        Check (-not $corrupt.Ok -and ($corrupt.Reasons -join '').Contains('not readable JSON') -and -not $foreign.Ok -and ($foreign.Reasons -join '').Contains('another schema') -and -not $absent.Ok -and ($absent.Reasons -join '').Contains('there is no checkpoint')) 'resume: a corrupted, foreign or missing checkpoint is refused safely'
        if (Get-Command git -ErrorAction SilentlyContinue) {
            & git -C $root check-ignore -q build/test-history/last-run.json
            Check ($LASTEXITCODE -eq 0) 'resume: the checkpoint lives in build\test-history, ignored by git'
        }

        $newSuite = { param([string]$Name, [string]$Class = 'normal', [int]$Ticks = 3, [bool]$Background = $false, [double]$GBUsed = 1)
            [pscustomobject]@{ Name = $Name; Options = @{ Memory = $Class; Ticks = $Ticks; Background = $Background; GB = $GBUsed } } }
        $simulate = {
            param($Policy, [object[]]$List, [scriptblock]$MemoryBlocker = { param($Class) '' }, [switch]$Watch)
            $state = [pscustomobject]@{ Live = (New-Object System.Collections.Generic.List[object]); Ticks = 0; Peak = 0; HeavyPairs = 0; Done = 0; Failed = (New-Object System.Collections.Generic.List[string]); FirstStep = 0 }
            $launch = {
                param($Suite)
                $null = Wait-MemoryRoom $Suite.Name $Suite.Options['Memory']
                $handle = [pscustomobject]@{ Process = [pscustomobject]@{ Id = 500 + $state.Live.Count * 2 + $state.Ticks * 10; HasExited = $false }; Left = [int]$Suite.Options['Ticks']; Class = $Suite.Options['Memory']; Label = $Suite.Name; Abort = $null; GB = $Suite.Options['GB']; Command = $Suite.Name; Out = '' }
                $state.Live.Add($handle)
                $script:GateProcesses.Add($handle)
                $state.Peak = [Math]::Max($state.Peak, $state.Live.Count)
                if (@($state.Live | Where-Object { $_.Class -eq 'heavy' }).Count -gt 1) { $state.HeavyPairs++ }
                return $handle
            }
            $pause = {
                param($Ms)
                $state.Ticks++
                if ($Watch) { Invoke-MemoryWatch -Force }
                foreach ($handle in $state.Live.ToArray()) {
                    if (-not $handle.Process.HasExited) { $handle.Left-- }
                    if ($handle.Left -le 0 -or $handle.Process.HasExited) { $handle.Process.HasExited = $true; [void]$state.Live.Remove($handle); [void]$script:GateProcesses.Remove($handle) }
                }
            }
            $scheduler = New-SuiteScheduler $Policy $launch $MemoryBlocker $pause
            foreach ($suite in $List) { if ($suite.Options['Background']) { Add-BackgroundSuite $scheduler $suite } }
            Step-Scheduler $scheduler
            $state.FirstStep = $scheduler.Running.Count
            foreach ($suite in $List) {
                $handle = Invoke-SchedulerSuite $scheduler $suite
                if ($handle.Abort) { $state.Failed.Add($suite.Name) } elseif ($handle.Process.HasExited) { $state.Done++ }
            }
            Wait-SchedulerIdle $scheduler
            return [pscustomobject]@{ Scheduler = $scheduler; State = $state }
        }
        $mix = @((& $newSuite 'exe0'), (& $newSuite 'gpu1' 'heavy' 4), (& $newSuite 'exe1'), (& $newSuite 'check1' 'light' 1)) + @(1..4 | ForEach-Object { & $newSuite "py$_" 'normal' 6 $true })
        $script:Guard = $null
        $run = & $simulate $default $mix
        Check ($run.State.Peak -eq 1 -and $run.State.FirstStep -eq 0 -and (@($run.Scheduler.Started) -join ',') -eq (@($mix | ForEach-Object { $_.Name }) -join ',') -and $run.State.Done -eq $mix.Count) 'scheduler: by default one suite at a time in list order, the four background Python suites included'
        $run = & $simulate (& $policyOf @{ TestJobs = 4 }) $mix
        Check ($run.State.FirstStep -eq 3 -and $run.State.Peak -le 4 -and $run.State.HeavyPairs -eq 0 -and $run.Scheduler.HeavyOverlap -eq 0 -and $run.State.Done -eq $mix.Count) 'scheduler: -TestJobs 4 queues background suites beside the current one, never beside a heavy suite'
        $run = & $simulate (& $policyOf @{ TestJobs = 4 }) $mix { param($Class) 'memory' }
        Check ($run.State.FirstStep -eq 0 -and $run.State.Peak -eq 1 -and $run.State.Done -eq $mix.Count) 'scheduler: -TestJobs never starts a background suite while the memory guard says no'

        $script:Guard = & $newGuard
        $world.Stage = 0; $world.Free = 15.0
        $script:Guard.Sampler = {
            $list = @((& $fakeProcess 100 4 'pwsh.exe' (0.2 * $GB)))
            $live = @($script:GateProcesses | Where-Object { -not $_.Process.HasExited })
            foreach ($handle in $live) { $list += & $fakeProcess $handle.Process.Id 100 "$($handle.Label).exe" ([long]($handle.GB * $GB)) }
            $free = if (@($live | Where-Object { $_.Label -eq 'pressure_tests' }).Count) { 0.4 } else { 1.2 }
            [pscustomobject]@{ TotalGB = 31.8; FreeGB = $free; CommitFreeGB = 20.0; Processes = $list }
        }
        $sized = @((& $newSuite 'small_tests' 'normal' 3 $false 2), (& $newSuite 'huge_tests' 'normal' 5 $false 9.6), (& $newSuite 'pressure_tests' 'normal' 5 $false 1),
                   (& $newSuite 'after_tests' 'normal' 3 $false 2))
        $run = & $simulate $default $sized -Watch
        Check ((@($run.State.Failed) -join ',') -eq 'huge_tests' -and $run.State.Done -eq 3 -and $script:Guard.Aborts.Count -eq 1 -and $script:Guard.Aborts[0].Reason -eq 'MEMORY LIMIT EXCEEDED') 'scheduler: a suite stopped for the gate''s tree fails and the next ones run; one running while other applications leave 0.4 GB free goes on to its end'
        $script:Guard = $null
        $script:GateProcesses.Clear()

        $hidden = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
        $outsideId = (Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = 'ping.exe -n 120 127.0.0.1'; ProcessStartupInformation = $hidden }).ProcessId
        try {
            $tree = Start-Proc 'cmd.exe' @('/d', '/c', 'ping.exe', '-n', '30', '127.0.0.1') $work 120 'tree fixture'
            $members = @()
            for ($i = 0; $i -lt 50 -and -not @($members | Where-Object { $_.Name -eq 'PING.EXE' }).Count; $i++) {
                Start-Sleep -Milliseconds 100
                $measured = Measure-GateTree (Get-MemorySample).Processes $PID $false @{ ([int]$tree.Process.Id) = $tree } @{}
                $members = @($measured.Members | Where-Object { $_.Stage -eq $tree.Process.Id })
            }
            $wrapper = @($members | Where-Object { $_.Id -eq $tree.Process.Id })
            $child = @($members | Where-Object { $_.Name -eq 'cmd.exe' -and $_.Id -ne $tree.Process.Id })
            $grandchild = @($members | Where-Object { $_.Name -eq 'PING.EXE' })
            $sum = [long]($members | Measure-Object -Property Bytes -Sum).Sum
            Check ($wrapper.Count -eq 1 -and $child.Count -eq 1 -and $grandchild.Count -eq 1 -and $measured.StageBytes[[int]$tree.Process.Id] -eq $sum -and $sum -gt 0 -and $sum -lt 100MB) 'memory: a real parent, child and grandchild of some MB each are summed into their stage'
            Check (-not @($measured.Members | Where-Object { $_.Id -eq $outsideId }).Count) 'memory: a process the gate did not start is not counted'
            [void](Stop-GateProcesses)

            $tinyLimits = [pscustomobject]@{ TotalGB = 31.8; SoftGB = 0.035; HardGB = 0.04; EmergencyGB = 0; WarningGB = 0; Estimates = [ordered]@{ normal = 0.01 }; EmergencyWaitSeconds = 30 }
            $script:Guard = New-MemoryGuard $tinyLimits $PID $false
            $script:Guard.Interval = 0.2
            $big = Invoke-Process $shell @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30') $work 120 'big fixture'
            $after = Invoke-Process 'cmd.exe' @('/d', '/c', 'exit', '0') $work 60 'small fixture'
            $outsideAlive = [bool](Get-Process -Id $outsideId -ErrorAction SilentlyContinue)
            Check ($big.Code -eq 137 -and ($big.Lines -join "`n").Contains('MEMORY LIMIT EXCEEDED: big fixture was stopped') -and ($big.Lines -join "`n").Contains('every process of the stage ended') -and $big.Seconds -lt 25) 'memory: a real fixture over a 40 MB hard limit is stopped and fails, its whole tree gone'
            Check ($after.Code -eq 0 -and $outsideAlive -and $script:GateProcesses.Count -eq 0) 'memory: the next fixture runs, and the outside process is untouched'
        } finally {
            $script:Guard = $null
            [void](Stop-GateProcesses)
            if ($outsideId) { Stop-Process -Id $outsideId -Force -ErrorAction SilentlyContinue }
        }

        $written = New-Object System.Collections.Generic.List[string]
        $writer = { param([string]$Text, [bool]$NoNewline) $written.Add($Text) }.GetNewClosure()
        $noon = [datetime]'2026-01-01T12:00:00'
        $plain = New-ProgressReporter @((New-ProgressGroup 'a'), (New-ProgressGroup 'b'), (New-ProgressGroup 'c')) 0 $false $writer 30 $noon
        Start-ProgressGroup $plain 'a' 0
        $early = Update-Progress $plain 10
        $late = Update-Progress $plain 31
        $line = $written[$written.Count - 1]
        Check (-not $early -and $late -and $written.Count -eq 1) 'progress: without a terminal a RUNNING line is written every 30 s, not on every tick'
        Check ($line -eq 'RUNNING [0/3] 0% | a | Group 31s | Total 31s | ETA ~14s (low confidence) | Finish ~12:00') 'progress: without history the ETA and the finish time are marked low confidence, with no animation characters'
        $memoryState = { [pscustomobject]@{ GateGB = 1.2; HardGB = 9.5; PeakGB = 6.2; FreeGB = 1.7; Low = $true } }
        $timed = New-ProgressReporter @((New-ProgressGroup 'a' 'windows' 60 $true 55), (New-ProgressGroup 'b' 'windows' 120 $true 120)) 0 $false $writer 30 $noon $memoryState
        Start-ProgressGroup $timed 'a' 0
        Complete-ProgressGroup $timed 'a' 'PASS' 50 1234 50
        Start-ProgressGroup $timed 'b' 50
        Check ((Format-ProgressLine $timed 80) -eq '[1/2] 50% | b | Group 30s of 2m | Total 1m 20s | ETA 1m 30s | Gate RAM 1.2/9.5 GB | Peak 6.2 GB | System free 1.7 GB | LOW MEMORY | Finish 12:02 | Checks 1,234') 'progress: the line shows done/total, suite, its time, total, ETA, gate RAM, its peak, system free RAM, a low-memory mark, finish and checks'
        Check ((Format-ProgressLine $timed 80 100) -eq '[1/2] 50% | b | Total 1m 20s | ETA 1m 30s | Gate RAM 1.2/9.5 GB | System free 1.7 GB | LOW MEMORY') 'progress: a narrow line keeps done/total, suite, total, ETA, gate RAM, system free and the low-memory mark before finish and checks'
        Check ((Format-RowPrefix $timed 80) -eq '[  1/2]  50% | 1m 20s | ETA 1m 30s') 'progress: every result row starts with done/total, percent, total time and ETA'
        $animated = New-ProgressReporter @((New-ProgressGroup 'x')) 0 $true $writer 0.5 $noon
        $animated.Width = 120
        Start-ProgressGroup $animated 'x' 0
        [void](Update-Progress $animated 1)
        Check ($written[$written.Count - 1].StartsWith("`r| [0/1] 0% | x |") -and $animated.LineDirty) 'progress: in a terminal the line is redrawn in place'
        Clear-ProgressLine $animated
        Check (-not $animated.LineDirty -and $written[$written.Count - 1].Trim() -eq '') 'progress: the line is cleared before a result row is written'
        $slow = New-ProgressReporter @((New-ProgressGroup 'slow_tests' 'windows' 10 $true 10)) 0 $false $writer 30 $noon
        Start-ProgressGroup $slow 'slow_tests' 0
        Complete-ProgressGroup $slow 'slow_tests' 'PASS' 40 5 40
        $report = (Format-TimingReport $slow 40 ([pscustomobject]@{ seconds = 30.0; at = '2026-01-01T11:00:00' })) -join "`n"
        Check ($report -match 'slow_tests\s+windows\s+40s' -and $report.Contains('WARNING: slow_tests (windows) took 40s, usually 10s') -and $report.Contains('previous comparable run: 30s')) 'timing report: the slowest suites, a suite clearly slower than its history and the previous comparable run'

        $historyDir = Join-Path $work 'history'
        New-Item -ItemType Directory -Force -Path $historyDir | Out-Null
        $absent = New-TestHistory (Join-Path $historyDir 'absent.json')
        $guess = Get-DurationEstimate $absent 'windows' 'default' 'host_tests'
        Check ($absent.Groups.Count -eq 0 -and -not $absent.Problem -and $absent.Writable -and -not $guess.Known -and $null -eq $guess.Seconds) 'history: a missing file is an empty history'
        $historyFile = Join-Path $historyDir 'timings.json'
        $history = New-TestHistory $historyFile
        $key = Get-TimingKey 'windows' 'default' 'host_tests'
        $kept = Add-TimingSample $history $key 'PASS' 12.5 400000
        $dropped = Add-TimingSample $history $key 'FAIL' 99
        $saved = Save-TestHistory $history
        $guess = Get-DurationEstimate (New-TestHistory $historyFile) 'windows' 'default' 'host_tests'
        Check ($kept -and -not $dropped -and $saved -and $guess.Known -and $guess.Seconds -eq 12.5) 'history: a passing suite''s time is saved and read back as a known estimate; a failing one is not kept'
        $otherMode = Get-DurationEstimate (New-TestHistory $historyFile) 'windows' 'full' 'host_tests'
        Check (-not $otherMode.Known -and $otherMode.Seconds -eq 12.5) 'history: the same suite in another mode gives an estimate of low confidence'
        [IO.File]::WriteAllText($historyFile, '{ this is not json')
        $broken = New-TestHistory $historyFile
        Check ($broken.Groups.Count -eq 0 -and $broken.Problem -eq 'not readable as JSON' -and $broken.Writable) 'history: a corrupted file is read as no history, without an error'
        [void](Add-TimingSample $broken $key 'PASS' 7)
        Check ((Save-TestHistory $broken) -and (Read-HistoryFile $historyFile).Groups.Count -eq 1) 'history: the next save replaces a corrupted file'
        [IO.File]::WriteAllText($historyFile, '{"schema": 99, "groups": {}}')
        $newer = New-TestHistory $historyFile
        [void](Add-TimingSample $newer $key 'PASS' 7)
        Check (-not $newer.Writable -and -not (Save-TestHistory $newer) -and [IO.File]::ReadAllText($historyFile).Contains('99')) 'history: a file of a newer schema is left alone'
        if (Get-Command git -ErrorAction SilentlyContinue) {
            & git -C $root check-ignore -q build/test-history/timings.json
            Check ($LASTEXITCODE -eq 0) 'history: build\test-history is ignored by git, so it never shows in git status'
        }

        $epoch = [datetime]::FromFileTime(0)
        $now = [datetime]::Now
        $clock3 = [pscustomobject]@{ Elapsed = [TimeSpan]::FromSeconds(3) }
        $noExit = [pscustomobject]@{ Process = [pscustomobject]@{ StartTime = $now; ExitTime = $epoch }; Clock = $clock3 }
        $exited = [pscustomobject]@{ Process = [pscustomobject]@{ StartTime = $now.AddSeconds(-2); ExitTime = $now }; Clock = $clock3 }
        $tooLong = [pscustomobject]@{ Process = [pscustomobject]@{ StartTime = $now.AddDays(-1); ExitTime = $now }; Clock = $clock3 }
        Check ((Get-ProcSeconds $noExit) -eq 3 -and [Math]::Round((Get-ProcSeconds $exited), 3) -eq 2 -and (Get-ProcSeconds $tooLong) -eq 3) 'duration regression: an exit time of 1601 (about -13.4 billion s) or one that does not fit the monotonic clock gives that clock''s seconds, never a negative duration'
        $negative = New-ProgressReporter @((New-ProgressGroup 'tests\build_script_tests.ps1')) 0 $false $writer 30 $noon
        Start-ProgressGroup $negative 'tests\build_script_tests.ps1' 0
        Complete-ProgressGroup $negative 'tests\build_script_tests.ps1' 'PASS' 1 1 (-13440215792.7)
        $negativeHistory = New-TestHistory (Join-Path $historyDir 'negative.json')
        Check ($negative.Groups[0].Seconds -eq 0 -and (Get-SlowestGroups $negative).Count -eq 0 -and -not (Add-TimingSample $negativeHistory 'windows|default|tests\build_script_tests.ps1' 'PASS' (-13440215792.7)) -and (Get-ProgressEta $negative 1).Seconds -eq 0) 'duration regression: a negative duration never reaches the progress, the slowest suites, the ETA or the history'

        $bigOutput = Join-Path $work 'big-output.txt'
        [IO.File]::WriteAllText($bigOutput, ((1..400 | ForEach-Object { "line $_" }) -join "`n") + "`n400 checks`n")
        $script:OutputTailBytes = 1KB
        $tail = @(Read-ProcOutput $bigOutput)
        $script:OutputTailBytes = 4MB
        Check ($tail[0] -match '^\(output truncated' -and $tail[$tail.Count - 1] -eq '400 checks' -and $tail.Count -lt 200 -and $script:FailLinesKept -le 500 -and $script:PassLinesKept -le 60) 'output: a huge output is read from its end, a passing suite keeps 60 lines and a failing one 500'

        $child = Start-Proc $shell @('-NoProfile', '-Command', 'Start-Sleep -Seconds 60') $work 120 'cleanup fixture'
        $outsideInfo = New-Object System.Diagnostics.ProcessStartInfo
        $outsideInfo.FileName = $shell
        $outsideInfo.Arguments = '-NoProfile -Command Start-Sleep -Seconds 60'
        $outsideInfo.UseShellExecute = $false
        $outsideInfo.CreateNoWindow = $true
        $outside = [System.Diagnostics.Process]::Start($outsideInfo)
        try {
            $wrapper = $child.Process.Id
            $inner = $null
            for ($i = 0; $i -lt 300 -and -not $inner; $i++) {
                $inner = Get-CimInstance Win32_Process -Filter "ParentProcessId = $wrapper" -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'conhost.exe' } | ForEach-Object { [int]$_.ProcessId } | Select-Object -First 1
                if (-not $inner) { Start-Sleep -Milliseconds 100 }
            }
            $stopped = Stop-GateProcesses
            $gone = $inner -and -not (Get-Process -Id $wrapper -ErrorAction SilentlyContinue) -and -not (Get-Process -Id $inner -ErrorAction SilentlyContinue)
            Check ($stopped -eq 1 -and $gone -and $script:GateProcesses.Count -eq 0) 'cleanup: an interrupted run stops the process tree it started, the cmd wrapper and the program under it'
            Check (-not $outside.HasExited) 'cleanup: a process the gate did not start keeps running'
        } finally {
            if (-not $outside.HasExited) { $outside.Kill(); [void]$outside.WaitForExit(5000) }
            $outside.Dispose()
        }
        $runFolder = Join-Path ([IO.Path]::GetTempPath()) "jpt-test-gate-check-$PID"
        New-Item -ItemType Directory -Force -Path $runFolder | Out-Null
        [IO.File]::WriteAllText((Join-Path $runFolder 'out.txt'), 'x')
        Check ((Remove-GateTempDir $runFolder) -and -not (Test-Path -LiteralPath $runFolder)) 'cleanup: the run''s own <temp>\jpt-test-* folder is removed'
        Check (-not (Remove-GateTempDir $root) -and (Test-Path -LiteralPath (Join-Path $root '.git')) -and -not (Remove-GateTempDir (Join-Path $work 'jpt-test-nested')) -and -not (Remove-GateTempDir ([IO.Path]::GetTempPath()))) 'cleanup: the repository, a nested folder and the temporary folder itself are refused'

        $env:JPT_GATE_TEST_FREE_GB = '20'
        try { Check ($null -eq (Get-FixtureNumber $root 'JPT_GATE_TEST_FREE_GB') -and $null -eq (Get-FixtureNumber $work 'JPT_GATE_TEST_FREE_GB')) 'memory: the project repository, or a folder without the fixture marker, can never simulate its free RAM' }
        finally { Remove-Item Env:\JPT_GATE_TEST_FREE_GB -ErrorAction SilentlyContinue }
    }

    $runnerText = (@('_progress.ps1', '_test_history.ps1', '_test_runner.ps1', '_memory_guard.ps1') | ForEach-Object { [IO.File]::ReadAllText((Join-Path $root "tools\$_")) }) -join "`n"
    foreach ($name in '[int]$TestJobs', '[int]$BuildJobs', '[switch]$LowMemory', '[switch]$Sequential', '.PARAMETER TestJobs', '.PARAMETER BuildJobs', '.PARAMETER LowMemory', '.PARAMETER Sequential',
                      'JPT_TEST_JOBS', 'JPT_BUILD_JOBS', 'Legacy alias, the same behavior as the default', '.\tools\test.ps1 -TestJobs 2 -BuildJobs 2', 'MEMORY LIMIT EXCEEDED', 'EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE') {
        Check ($text.Contains($name)) "the gate has and documents $name"
    }
    Check (-not $text.Contains('.\tools\test.ps1 -LowMemory') -and -not ([IO.File]::ReadAllText((Join-Path $root 'README.md'))).Contains('-LowMemory')) 'no documentation tells to run with -LowMemory: the default is the low-memory run'
    Check ($text.Contains('''-k'', ''0'', ''-j'', "$($script:Policy.BuildJobs)"')) 'ninja builds the Windows test targets with an explicit job limit (1 by default)'
    Check ($text -notmatch '\$pending' -and $text -match 'Add-BackgroundSuite \$script:Scheduler' -and $text -match 'Invoke-SchedulerSuite \$script:Scheduler') 'background suites go through the scheduler queue, never all started at once'
    Check ($text -match 'finally \{\s*if \(-not \$windowsDone\) \{ Stop-GateRun \}' -and $text -match 'finally \{\s*if \(-not \$linuxDone\) \{ Stop-GateRun \}') 'an interrupted Windows or Linux half stops the gate''s own processes and removes its run folder'
    Check ($text -match 'Wait-SchedulerIdle \$script:Scheduler' -and $text.Contains('$print $script:Policy.BuildJobs $script:Policy.LinuxTestJobs')) 'the Windows half ends before the Linux half starts, which gets the same job limits'
    Check ($text.Contains("Get-MemoryLimits `$memoryAtStart.TotalGB") -and $text.Contains('$script:Guard = New-MemoryGuard') -and $text.Contains('==> Memory safety') -and $runnerText.Contains("if (-not `$NoWait) { [void](Wait-MemoryRoom `$Label `$Class `$EstimateGB) }") -and $runnerText -match '(?s)function Wait-Proc.*Invoke-MemoryWatch') 'every run builds the memory guard, every process start waits for it and every wait watches the tree'
    Check ($text.Contains('[switch]$Resume') -and $text.Contains('.PARAMETER Resume') -and $text.Contains('$checkpointFolder = if ($nestedGate) { Join-Path $tempDir ''checkpoint'' } else { Join-Path $root ''build\test-history'' }')) 'the gate has a documented -Resume over a checkpoint in build\test-history, kept in its own temporary folder by a nested gate'
    Check ($text.Contains('JPT_GATE_OWNER_ROOT = $root }') -and $text.Contains('$nestedGate = [bool]$env:JPT_GATE_OWNER_ROOT -and (Test-SamePath $env:JPT_GATE_OWNER_ROOT $root)') -and $text.Contains('if ($nestedGate) { $script:History.Writable = $false }')) 'a gate marks what it starts with its repository; a gate of the same repository started that way writes neither the checkpoint nor the timings'
    Check (-not (($text + $runnerText) -cmatch 'INSUFFICIENT SAFE MEMORY|CONSTRAINED|Get-RequiredFreeGB|CriticalGB')) 'no free RAM reservation is left: no constrained start, no required free RAM, no critical floor above the emergency one'
    Check ($text.Contains('"ninja $Arch build" ''build''') -and $text.Contains('''linux audio probe'' ''wsl''') -and $linuxHelper.Contains("'sanitize' } else { 'wsl' }") -and $text -notmatch '(?i)system reserve|ReserveGB') 'builds start as build stages, WSL runs as wsl stages, and no fixed free RAM reserve is left'
    Check ($runnerText -notmatch '(?i)ForEach-Object -Parallel|ThreadJob|Start-Job|Install-Module|Import-Module|Start-Process|taskkill(\.exe)?\s+/IM|Stop-Process\s+-Name') 'the runner needs no module or thread job, and never stops processes by name'
    Check ($linuxHelper.Contains('''--jobs'', "$BuildJobs", ''--test-jobs'', "$TestJobs", ''--pid-file'', $pidFile') -and $linuxHelper.Contains("'--stop', `$pidFile")) 'the Linux half passes the jobs and a PID file, so a stop ends only its own tree in WSL'
    $testLinux = [IO.File]::ReadAllText((Join-Path $root 'tools\test-linux.sh'))
    $linuxGate = [IO.File]::ReadAllText((Join-Path $root 'tools\linux_gate.py'))
    $buildLinux = [IO.File]::ReadAllText((Join-Path $root 'tools\build-linux.sh'))
    $audioProbe = [IO.File]::ReadAllText((Join-Path $root 'tools\linux_audio_probe.sh'))
    Check ($testLinux.Contains('jobs=(--jobs 1)') -and $testLinux.Contains('test_jobs=(--jobs 0)') -and $testLinux.Contains('--build-dir "$build_dir" "${jobs[@]}"') -and $testLinux.Contains('ffmpeg-bootstrap.sh" --cc "$cc" "${jobs[@]}"')) 'test-linux.sh: one compiler for the Linux build and the FFmpeg bootstrap, one suite at a time, unless told otherwise'
    Check ($buildLinux.Contains('openssl-bootstrap.sh" --cc "$cc" --jobs "$jobs"') -and $buildLinux.Contains('cmake --build "$build_dir" --parallel "$jobs"')) 'build-linux.sh hands its job count to the OpenSSL bootstrap and to cmake --build --parallel'
    Check ($linuxGate.Contains('default=0') -and $linuxGate.Contains('if args.jobs > 0 else None') -and $audioProbe.Contains('ninja -j "$jobs"') -and $text.Contains('$repo[0], "$($script:Policy.BuildJobs)"')) 'linux_gate.py runs one process at a time by default and the audio probe builds with the gate''s job count'
    Check ($testLinux.Contains('grep -qa test-linux.sh "/proc/$pid/cmdline"') -and $testLinux -notmatch '(?m)^[^#]*\b(pkill|killall)\b') 'test-linux.sh --stop kills only the tree of the recorded run, never by name'

    $other = Join-Path $work 'elsewhere'
    New-Item -ItemType Directory -Force -Path $other | Out-Null
    $fromRoot = Invoke-Gate $gate @('-List')
    $fromOther = Invoke-Gate $gate @('-List') $env:PATH $other
    Check ($fromRoot.Code -eq 0 -and $fromOther.Code -eq 0) '-List works from the repository and from another folder'
    Check ($fromRoot.Text -eq $fromOther.Text) 'the suites do not depend on the caller''s folder'
    foreach ($section in 'Build and profiles', 'Platform', 'Input and controls', 'Graphics', 'Gameplay, CE and Patch 1.1', 'Cheats, menus and objectives', 'Localization and audio', 'Launcher', 'Release contract', 'Repository and static gates') {
        Check ($fromRoot.Text.Contains("==> $section")) "-List shows the section $section"
    }
    foreach ($name in 'platform_tests', 'input_tests', 'gamepad_tests', 'l10n_tests', 'launcher_tests', 'patch117_tests', 'ce_tests', 'water_tests', 'log_tests', 'tests\test_build_parity.py', 'tests\test_cheat_parity.py', 'tests\gate_tests.ps1', 'every test is wired in', 'working tree unchanged') {
        Check ($fromRoot.Text.Contains($name)) "-List names $name"
    }
    $listedSuites = @($fromRoot.Text -split "\r?\n" | Where-Object { $_ -match '^  \S' })
    Check ($listedSuites[-1] -match '^  working tree unchanged\s' -or $listedSuites[-2] -match '^  working tree unchanged\s') 'working tree unchanged is the last suite the gate runs'
    Check ($fromRoot.Text -match 'release artifacts in dist\s+NOT RUN') 'release artifacts are NOT RUN without -Release'
    Check ($fromRoot.Text -match '(?m)^    linux  linux_mouse_tests' -and $fromRoot.Text -match '(?m)^    linux  audio_pacer_tests' -and $fromRoot.Text -match '(?m)^    linux  elf glibc') '-List shows the Linux rows, indented below the Windows suites'
    $windowsList = Invoke-Gate $gate @('-List', '-WindowsOnly')
    Check ($windowsList.Code -eq 0) '-List with -WindowsOnly works without WSL'
    $exclusive = Invoke-Gate $gate @('-WindowsOnly', '-LinuxOnly', '-NoBuild')
    Check ($exclusive.Code -ne 0 -and $exclusive.Text -match 'exclude each other') '-WindowsOnly and -LinuxOnly exclude each other'
    $listed = Invoke-Gate $gate @('-List', '-Filter', 'platform_tests')
    Check ($listed.Code -eq 0 -and $listed.Text.Contains('platform_tests') -and -not $listed.Text.Contains('gamepad_tests')) '-Filter narrows -List to the matching suites'
    $none = Invoke-Gate $gate @('-Filter', 'no suite has this text 7f3a', '-NoBuild')
    Check ($none.Code -ne 0 -and $none.Text -match 'no suite matches') 'a filter that matches nothing fails instead of passing'
    $wiredReal = if ($memoryReady) { Invoke-Gate $gate @('-Filter', 'every test is wired in', '-NoBuild') } else { [pscustomobject]@{ Code = -1; Text = '' } }
    if ($memoryReady) { Check ($wiredReal.Code -eq 0 -and $wiredReal.Text -match 'PASS\s+every test is wired in' -and $wiredReal.Text -match 'every tool classified') 'the repository''s tools, including the packaging helpers, are each run or classified with a role' }
    $buildConflict = Invoke-Gate $gate @('-Build', '-NoBuild')
    Check ($buildConflict.Code -ne 0 -and $buildConflict.Text -match '-Build \(build only\) and -NoBuild') '-Build is an explicit build-only switch and excludes -NoBuild'
    $buildLinux = Invoke-Gate $gate @('-Build', '-LinuxOnly')
    Check ($buildLinux.Code -ne 0 -and $buildLinux.Text -match 'cannot be combined with -LinuxOnly') '-Build never pretends to build the Linux half'

    if ($memoryReady) {
    $cheap = Invoke-Gate $gate @('-Filter', 'tests\build_script_tests.ps1', '-NoBuild') $env:PATH $other
    Check ($cheap.Code -eq 0 -and $cheap.Text -match 'PASS\s+tests\\build_script_tests\.ps1' -and $cheap.Text -match 'PARTIAL RUN') 'a filtered run works from another folder and says it is partial'
    Check ($cheap.Text -notmatch 'ALL TESTS PASSED') 'a partial run never claims all tests passed'

    $builds = Invoke-Gate $gate @('-Filter', 'test_build_parity.py', '-WindowsOnly', '-NoBuild')
    Check ($builds.Code -eq 0 -and $builds.Text -match '==> Supported builds' -and $builds.Text -match 'en-116\s+lift116' -and $builds.Text -match 'ru-116\s+lift116' -and $builds.Text -match 'es-117\s+lift_es117' -and $builds.Text -match 'ptbr-117\s+lift_ptbr117' -and $builds.Text -notmatch 'Cannot convert value') 'the Supported builds table prints all four versions without a loop variable overwriting a switch parameter'

    $missing = Invoke-Gate $gate @('-Filter', 'platform_tests', '-NoBuild', '-BuildDir', (Join-Path $work 'no-build'))
    Check ($missing.Code -ne 0 -and $missing.Text -match 'no test program' -and $missing.Text.Contains('FAILED: platform_tests')) 'a missing test program fails its suite and says how to build it'
    Check ($cheap.Text.Contains('==> Memory safety') -and $cheap.Text -match 'Gate hard limit\s+\d+\.\d GB' -and $cheap.Text -match 'Test jobs\s+1' -and $cheap.Text -match 'Build jobs\s+1' -and $cheap.Text -match 'Suites\s+serial' -and $cheap.Text -match 'Linux\s+serial' -and $cheap.Text -match 'Memory: gate peak') 'a real run starts with its memory safety summary and ends with the gate''s peak RAM'
    }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $fixture = Join-Path $work 'fixture'
        New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'tools'), (Join-Path $fixture 'tests') | Out-Null
        $helpers = @('_common.ps1', '_gate_linux.ps1', '_progress.ps1', '_test_history.ps1', '_memory_guard.ps1', '_test_runner.ps1') | ForEach-Object { Join-Path $root "tools\$_" }
        Copy-Item -LiteralPath (@($gate) + $helpers) -Destination (Join-Path $fixture 'tools')
        [IO.File]::WriteAllText((Join-Path $fixture '.jpt-gate-fixture'), '')
        $env:JPT_GATE_TEST_FREE_GB = '5.7'
        $fixtureGate = Join-Path $fixture 'tools\test.ps1'
        & git -C $fixture init --quiet 2>&1 | Out-Null
        $cmake = New-Object System.Collections.Generic.List[string]
        foreach ($line in ($fromRoot.Text -split "\r?\n")) {
            if ($line -notmatch '^  (\S.*?)\s{2,}') { continue }
            $name = $Matches[1]
            if ($name -match '^[a-z0-9_]+_tests$') {
                Set-Content -LiteralPath (Join-Path $fixture "tests\$name.c") -Value '' -Encoding ascii
                $cmake.Add("add_executable($name EXCLUDE_FROM_ALL tests/$name.c)")
            } elseif ($name -match '^(tests|tools)\\') {
                Set-Content -LiteralPath (Join-Path $fixture (($name -split '\s+')[0])) -Value '' -Encoding ascii
            }
        }
        Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value $cmake -Encoding ascii
        Set-Content -LiteralPath (Join-Path $fixture '.gitignore') -Value 'build/' -Encoding ascii

        $wired = Invoke-Gate $fixtureGate @('-Filter', 'every test is wired in', '-NoBuild')
        Check ($wired.Code -eq 0 -and $wired.Text -match 'PASS\s+every test is wired in') 'a repository whose every test is registered passes the coverage check'

        Set-Content -LiteralPath (Join-Path $fixture 'tests\test_forgotten.py') -Value '' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $fixture 'tests\forgotten_tests.c') -Value '' -Encoding ascii
        Add-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'add_executable(forgotten_tests EXCLUDE_FROM_ALL tests/forgotten_tests.c)' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $fixture 'tools\new_checker.py') -Value '' -Encoding ascii
        $forgotten = Invoke-Gate $fixtureGate @('-Filter', 'every test is wired in', '-NoBuild')
        Check ($forgotten.Code -ne 0 -and $forgotten.Text.Contains('test target forgotten_tests is not run') -and $forgotten.Text.Contains('tests\test_forgotten.py is not run') -and $forgotten.Text.Contains('tools\new_checker.py')) 'a test target, a test script and a tool nobody wired in each fail the coverage check'
        Remove-Item -LiteralPath (Join-Path $fixture 'tests\test_forgotten.py'), (Join-Path $fixture 'tests\forgotten_tests.c'), (Join-Path $fixture 'tools\new_checker.py') -Force
        Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value $cmake -Encoding ascii

        $failing = Join-Path $work 'failing'
        New-StandIn $failing 'py.cmd' @('echo simulated failing tool') 1
        $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$failing;$env:PATH"
        Check ($run.Code -ne 0 -and $run.Text -match 'FAIL\s+tools\\check_profiles\.py' -and $run.Text.Contains('simulated failing tool') -and $run.Text.Contains('command:')) 'a failing suite fails the gate and shows its command and output'
        Check ($run.Text.Contains('FAILED: tools\check_profiles.py')) 'the failed suite is named in the last line'
        Check ($run.Text.Contains('exit code 1') -and $run.Text.Contains('working directory:') -and $run.Text.Contains('----- output of tools\check_profiles.py -----')) 'a failing suite prints its exit code, working directory and the whole output'

        $passing = Join-Path $work 'passing'
        New-StandIn $passing 'py.cmd' @('echo 0 problem(s)') 0
        $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$passing;$env:PATH"
        Check ($run.Code -eq 0 -and $run.Text -match 'PASS\s+tools\\check_profiles\.py') 'a passing suite passes the gate'
        Check ($run.Text -match 'System free\s+5\.7 GB' -and $run.Text -match 'Warning below\s+2\.0 GB' -and $run.Text -match 'Start floor\s+0\.5 GB' -and $run.Text -match 'Gate hard limit\s+\d+\.\d+ GB' -and $run.Text -match 'Suites\s+serial' -and $run.Text -match 'Linux\s+serial' -and $run.Text -notmatch '(?i)system reserve|waiting before|constrained|required free|build estimate') 'the run starts with the memory safety summary: free RAM, warning, start floor, gate limits, serial suites and Linux'
        Check ($run.Text -match '==> Jobs: test jobs 1 \(default\), build jobs 1 \(default\)' -and $run.Text -match 'the free RAM is simulated' -and $run.Text -match '\[\s*\d+/\d+\]\s+\d+% \| \d+s \| ETA ' -and $run.Text -match 'Concurrency: at most \d+ test process') 'a run names its jobs and their source, prefixes every row with its progress and reports its concurrency'
        Check ((Test-Path -LiteralPath (Join-Path $fixture 'build\test-history\timings.json')) -and -not (& git -C $fixture status --porcelain --untracked-files=all -- build)) 'a run keeps its timings in build\test-history, outside git status'
        foreach ($mode in @(@('-LowMemory', '(?s)legacy alias.*test jobs 1 \(default\), build jobs 1 \(default\)'), @('-Sequential', 'test jobs 1 \(-Sequential\).*every suite alone'), @('-TestJobs', 'test jobs 3 \(-TestJobs\)'))) {
            $arguments = @('-Filter', 'tools\check_profiles.py', '-NoBuild', $mode[0])
            if ($mode[0] -eq '-TestJobs') { $arguments += '3' }
            $run = Invoke-Gate $fixtureGate $arguments "$passing;$env:PATH"
            Check ($run.Code -eq 0 -and $run.Text -match $mode[1]) "$($mode[0]) is accepted and shown in the jobs line"
        }
        $env:JPT_TEST_JOBS = '3'; $env:JPT_BUILD_JOBS = '1'
        try { $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$passing;$env:PATH" } finally { Remove-Item Env:\JPT_TEST_JOBS, Env:\JPT_BUILD_JOBS -ErrorAction SilentlyContinue }
        Check ($run.Code -eq 0 -and $run.Text -match 'test jobs 3 \(JPT_TEST_JOBS\), build jobs 1 \(JPT_BUILD_JOBS\)') 'JPT_TEST_JOBS and JPT_BUILD_JOBS set the jobs of a run'

        $mixed = Join-Path $work 'mixed'
        New-StandIn $mixed 'py.cmd' @('echo %* | findstr /c:"gva_convert" >nul && (echo simulated gva failure& exit /b 1)', 'echo 0 problem(s)') 0
        $run = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild') "$mixed;$env:PATH"
        Check ($run.Code -eq 1 -and $run.Text -match 'FAIL\s+tools\\gva_convert\.py --check' -and $run.Text -match 'PASS\s+tools\\gen_guest_va\.py --check' -and $run.Text.Contains('FAILED: tools\gva_convert.py --check')) 'a failing suite fails the gate with exit code 1 and the suites after it still run'

        $leaking = Join-Path $work 'leaking'
        New-StandIn $leaking 'py.cmd' @('echo leaked> leaked.txt', 'echo 0 problem(s)') 0
        $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$leaking;$env:PATH"
        Check ($run.Code -ne 0 -and $run.Text -match 'FAIL\s+working tree unchanged' -and $run.Text.Contains('git\leaked.txt: appeared')) 'a suite that leaves a file behind fails the working-tree check and the report names the file and the kind of change'
        Remove-Item -LiteralPath (Join-Path $fixture 'leaked.txt') -Force -ErrorAction SilentlyContinue

        Set-Content -LiteralPath (Join-Path $fixture 'tracked.txt') -Value 'original' -Encoding ascii
        & git -C $fixture add tracked.txt 2>&1 | Out-Null
        $editing = Join-Path $work 'editing'
        New-StandIn $editing 'py.cmd' @('echo changed>> tracked.txt', 'echo 0 problem(s)') 0
        $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$editing;$env:PATH"
        Check ($run.Code -ne 0 -and $run.Text -match 'git\\tracked\.txt: changed; before: status A ; \d+ bytes, sha256 \w+; after: status AM; \d+ bytes, sha256 \w+' -and $run.Text.Contains('git diff: changed')) 'a suite that edits a staged file is reported with the file, the kind of change and the before and after state'
        Set-Content -LiteralPath (Join-Path $fixture 'tracked.txt') -Value 'original' -Encoding ascii
        $editingFail = Join-Path $work 'editing-fail'
        New-StandIn $editingFail 'py.cmd' @('echo changed>> tracked.txt', 'echo stand-in stdout', 'echo stand-in stderr 1>&2') 3
        $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$editingFail;$env:PATH"
        Check ($run.Text -match '(?s)FAIL\s+tools\\check_profiles\.py.*exit code 3.*files that differ from the snapshot.*git\\tracked\.txt: changed' -and $run.Text.Contains('stand-in stdout') -and $run.Text.Contains('stand-in stderr')) 'a failing suite prints its stdout, stderr, exit code and the files that changed since the snapshot'
        Set-Content -LiteralPath (Join-Path $fixture 'tracked.txt') -Value 'original' -Encoding ascii

        $bare = (Split-Path $git.Source) + ";$env:SystemRoot\System32"
        $python = & { $env:PATH = $bare; Get-Command py, python -ErrorAction SilentlyContinue }
        if (-not $python) {
            $run = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') $bare
            Check ($run.Code -ne 0 -and $run.Text -match 'Python 3 was not found') 'a missing Python is reported by name, never passed'
        }

        $noVs = Join-Path $work 'no-visual-studio'
        New-Item -ItemType Directory -Force -Path $noVs | Out-Null
        $savedProgramFiles = ${env:ProgramFiles(x86)}
        try {
            ${env:ProgramFiles(x86)} = $noVs
            $run = Invoke-Gate $fixtureGate @('-Filter', 'platform_tests')
        } finally { ${env:ProgramFiles(x86)} = $savedProgramFiles }
        Check ($run.Code -ne 0 -and $run.Text -match 'Visual Studio' -and $run.Text -match 'not built') 'a missing compiler is reported and fails the C suites'

        $release = Invoke-Gate $fixtureGate @('-Filter', 'dist artifacts', '-Release', '-NoBuild')
        Check ($release.Code -ne 0 -and $release.Text -match 'no release artifacts') '-Release fails when dist holds nothing and never packages'

        $failGen = Join-Path $work 'fail-gen'
        New-StandIn $failGen 'py.cmd' @('echo %* | findstr /c:"gen_guest_va" >nul && (echo simulated gen failure& exit /b 1)', 'echo 0 problem(s)') 0
        $fixtureCheckpoint = Join-Path $fixture 'build\test-history\last-run.json'
        $first = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild') "$failGen;$env:PATH"
        $saved = Read-RunCheckpoint $fixtureCheckpoint
        Check ($first.Code -eq 1 -and $first.Text -match 'FAIL\s+tools\\gen_guest_va\.py --check' -and -not $saved.Problem -and (@($saved.Checkpoint.passed | ForEach-Object { $_.name }) -join ',') -eq 'tools\gva_convert.py --check' -and $saved.Checkpoint.broken) 'resume: a run keeps a checkpoint of the suites that passed before the one that failed'
        $again = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild', '-Resume') "$passing;$env:PATH"
        Check ($again.Code -eq 0 -and $again.Text -match 'Resume: 1 suite\(s\) passed in the previous run with the same sources and parameters; the run goes on at tools\\gen_guest_va\.py --check' -and $again.Text -match 'PASS\s+tools\\gva_convert\.py --check\s+resumed' -and $again.Text -match 'PASS\s+tools\\gen_guest_va\.py --check\s+\d' -and $again.Text -match 'PASS\s+working tree unchanged\s+\d' -and $again.Text.Contains('RESUMED RUN: 1 suite(s)')) '-Resume leaves out the suite that passed, runs the failed one again and still checks the working tree'
        $normal = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild') "$passing;$env:PATH"
        Check ($normal.Code -eq 0 -and $normal.Text -notmatch 'resumed|Resume:' -and $normal.Text -match 'PASS\s+tools\\gva_convert\.py --check\s+\d') 'resume: a run without -Resume ignores the checkpoint and starts from the first suite'
        $otherParameters = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild', '-Resume', '-Full') "$passing;$env:PATH"
        Check ($otherParameters.Code -ne 0 -and $otherParameters.Text.Contains('-Resume refused') -and $otherParameters.Text.Contains('the parameters changed') -and $otherParameters.Text -notmatch 'PASS\s+tools\\gva') 'resume: other parameters refuse -Resume, and nothing runs'
        Add-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value '# changed after the checkpoint' -Encoding ascii
        $otherSources = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild', '-Resume') "$passing;$env:PATH"
        Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value $cmake -Encoding ascii
        Check ($otherSources.Code -ne 0 -and $otherSources.Text.Contains('-Resume refused') -and $otherSources.Text.Contains('the sources changed')) 'resume: a changed source refuses -Resume'

        $mainReports = Join-Path $fixture 'build\test-history\last-run-reports'
        $mainState = {
            $files = @(Get-ChildItem -LiteralPath (Join-Path $fixture 'build\test-history') -Recurse -File -ErrorAction SilentlyContinue | Sort-Object FullName)
            (@($files | ForEach-Object { $_.FullName.Substring($fixture.Length) + '=' + (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash })) -join ';'
        }
        $invokeNested = {
            param([string[]]$Arguments)
            $owner = $env:JPT_GATE_OWNER_ROOT
            $env:JPT_GATE_OWNER_ROOT = $fixture
            try { Invoke-Gate $fixtureGate $Arguments "$passing;$env:PATH" } finally { $env:JPT_GATE_OWNER_ROOT = $owner }
        }
        $main = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild') "$failGen;$env:PATH"
        New-Item -ItemType Directory -Force -Path $mainReports | Out-Null
        [IO.File]::WriteAllText((Join-Path $mainReports 'main_run_marker.json'), '{"from": "the main run"}')
        $before = & $mainState
        $nestedFail = & $invokeNested @('-Filter', 'launcher_tests', '-NoBuild')
        $afterFail = & $mainState
        $nestedPass = & $invokeNested @('-Filter', 'tools\check_profiles.py', '-NoBuild')
        $afterPass = & $mainState
        Check ($main.Code -eq 1 -and $before.Contains('last-run.json=') -and $before.Contains('main_run_marker.json=') -and $nestedFail.Code -ne 0 -and $nestedFail.Text.Contains('read only: this gate runs inside another gate of this repository') -and $afterFail -eq $before) 'nested gate: a gate of the same repository started by a gate (filter launcher_tests, failing) leaves the main checkpoint, its reports and the timings byte for byte'
        Check ($nestedPass.Code -eq 0 -and $afterPass -eq $before) 'nested gate: a passing nested gate leaves them byte for byte too'
        $resumed = Invoke-Gate $fixtureGate @('-Filter', '.py --check', '-NoBuild', '-Resume') "$passing;$env:PATH"
        $updated = Read-RunCheckpoint $fixtureCheckpoint
        Check ($resumed.Code -eq 0 -and $resumed.Text -match 'Resume: 1 suite\(s\) passed .*; the run goes on at tools\\gen_guest_va\.py --check' -and -not $updated.Problem -and @($updated.Checkpoint.passed).Count -eq 3) 'nested gate: afterwards -Resume still accepts the main checkpoint, goes on at the suite that failed and keeps updating it'
        Remove-Item -LiteralPath $fixtureCheckpoint -Force
        $nestedFresh = & $invokeNested @('-Filter', 'tools\check_profiles.py', '-NoBuild')
        Check ($nestedFresh.Code -eq 0 -and -not (Test-Path -LiteralPath $fixtureCheckpoint)) 'nested gate: without a main checkpoint a nested gate does not create one'
        $nestingSuite = Join-Path $fixture 'tests\link_recovery_tests.ps1'
        [IO.File]::WriteAllText($nestingSuite, @'
$history = Join-Path $PSScriptRoot '..\build\test-history'
function Get-State { (@(Get-ChildItem -LiteralPath $history -Recurse -File -ErrorAction SilentlyContinue | Sort-Object FullName | ForEach-Object { $_.FullName + '=' + (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash })) -join ';' }
$before = Get-State
& (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\tools\test.ps1') -Filter launcher_tests -NoBuild *> $null
if ((Get-State) -ne $before) { Write-Output 'FAIL: the nested gate changed the main checkpoint or timings'; exit 1 }
Write-Output 'the nested gate left the main checkpoint and timings alone: 1 checks'
'@)
        $outer = Invoke-Gate $fixtureGate @('-Filter', 'tests\link_recovery_tests.ps1', '-NoBuild') "$passing;$env:PATH"
        [IO.File]::WriteAllText($nestingSuite, '')
        $outerCheckpoint = Read-RunCheckpoint $fixtureCheckpoint
        Check ($outer.Code -eq 0 -and $outer.Text -match 'PASS\s+tests\\link_recovery_tests\.ps1' -and -not $outerCheckpoint.Problem -and $outerCheckpoint.Checkpoint.config.Contains('filter=tests\link_recovery_tests.ps1') -and (@($outerCheckpoint.Checkpoint.passed | ForEach-Object { $_.name }) -contains 'tests\link_recovery_tests.ps1')) 'nested gate: a cold-start run by a suite of a gate, as gate_tests does, leaves that gate''s checkpoint and timings alone'

        $env:JPT_GATE_TEST_FREE_GB = '0.57'
        $tight = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$passing;$env:PATH"
        Check ($tight.Code -eq 0 -and $tight.Text -match 'PASS\s+tools\\check_profiles\.py' -and $tight.Text.Contains('WARNING: 0.57 GB free: suites still start') -and $tight.Text.Contains('LOW SYSTEM MEMORY: 0.57 GB free; tools\check_profiles.py starts anyway') -and $tight.Text -match 'of them with LOW SYSTEM MEMORY: .*tools\\check_profiles\.py' -and $tight.Text -cnotmatch 'waiting before|INSUFFICIENT|EMERGENCY|SAFETY ABORT') 'with 0.57 GB free a suite starts at once with a warning, passes, and the summary counts it'

        $env:JPT_GATE_TEST_FREE_GB = '0.4'; $env:JPT_GATE_TEST_WAIT_SECONDS = '3'
        $starved = Invoke-Gate $fixtureGate @('-Filter', 'tools\check_profiles.py', '-NoBuild') "$passing;$env:PATH"
        Check ($starved.Code -ne 0 -and $starved.Text.Contains('waiting before tools\check_profiles.py: free 0.4 GB, at or under the start floor 0.5 GB') -and $starved.Text.Contains('EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE: tools\check_profiles.py: for 3 s free 0.4 GB') -and $starved.Text -notmatch 'INSUFFICIENT') 'only at the start floor, with nothing running, does a stage wait, and then the gate fails with EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE'
        Remove-Item Env:\JPT_GATE_TEST_FREE_GB, Env:\JPT_GATE_TEST_WAIT_SECONDS -ErrorAction SilentlyContinue
    }
} catch {
    Check $false "gate_tests stopped at line $($_.InvocationInfo.ScriptLineNumber) of $(Split-Path -Leaf $_.InvocationInfo.ScriptName): $($_.Exception.Message)"
} finally {
    Remove-Item Env:\JPT_GATE_TEST_FREE_GB, Env:\JPT_GATE_TEST_WAIT_SECONDS -ErrorAction SilentlyContinue
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($work)
    if ((Split-Path $full -Parent) -eq $temp -and (Split-Path $full -Leaf).StartsWith('jpt-gate-tests-')) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
    $runFolder = Join-Path $temp "jpt-test-gate-check-$PID"
    if (Test-Path -LiteralPath $runFolder) { Remove-Item -LiteralPath $runFolder -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
