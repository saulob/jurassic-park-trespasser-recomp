# The Linux half of tools\test.ps1: WSL discovery, the Linux gate run through wslpath, progress with an ETA, and the Windows/Linux
# feature parity computed from the per-platform results. Dot-sourced by tools\test.ps1 after its own helpers (Invoke-Process,
# Start-Proc, Wait-Proc, $root). Nothing here starts the game, the launcher, a window or a sound.

$script:ProgressPlan = New-Object System.Collections.Generic.List[string]
$script:ProgressPlanDone = 0
$script:History = @{}
$script:HistoryPath = Join-Path $root 'build\logs\test-timings.json'
$script:RunClock = [System.Diagnostics.Stopwatch]::StartNew()

function Get-LinuxSuiteNames {
    $path = Join-Path $root 'tests\linux_suites.json'
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    $config = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    return @(@($config.portable) + @($config.linux_specific) + @($config.python | ForEach-Object { "tests\$_" }) + @($config.tools | ForEach-Object { $_ -replace '/', '\' }) + @($config.static))
}

function Import-TestHistory {
    if (-not (Test-Path -LiteralPath $script:HistoryPath)) { return }
    try {
        $loaded = [IO.File]::ReadAllText($script:HistoryPath) | ConvertFrom-Json
        foreach ($property in $loaded.PSObject.Properties) { $script:History[$property.Name] = [double]$property.Value }
    } catch { $script:History = @{} }
}

function Export-TestHistory([object[]]$Rows) {
    foreach ($row in $Rows) { if ($row.Seconds -gt 0 -and $row.Status -in 'PASS', 'FAIL') { $script:History[$row.Name] = [Math]::Round([double]$row.Seconds, 1) } }
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $script:HistoryPath) | Out-Null
        [IO.File]::WriteAllText($script:HistoryPath, ($script:History | ConvertTo-Json))
    } catch { }
}

function Format-Clock([double]$Seconds) {
    $total = [int][Math]::Max(0, [Math]::Round($Seconds))
    return '{0}:{1:00}' -f [int]($total / 60), ($total % 60)
}

# "[ 37/181  20%  4:12 elapsed, ETA 11:05]": the ETA is what the rows still to come took last time (4 s for an unknown one).
function Get-ProgressPrefix {
    $total = [Math]::Max(1, $script:ProgressPlan.Count)
    $done = [Math]::Min($script:ProgressPlanDone, $total)
    $remaining = 0.0
    for ($i = $done; $i -lt $script:ProgressPlan.Count; $i++) {
        $name = $script:ProgressPlan[$i]
        $remaining += $(if ($script:History.ContainsKey($name)) { $script:History[$name] } else { 4.0 })
    }
    return '[{0,3}/{1,-3} {2,3}%  {3} elapsed, ETA {4}]' -f $done, $total, [int](100 * $done / $total), (Format-Clock $script:RunClock.Elapsed.TotalSeconds), (Format-Clock $remaining)
}

function Step-Progress { $script:ProgressPlanDone++ }

# Strict mode makes a missing property an error; the contract leaves out what a feature does not need.
function Get-Prop($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}

function Measure-Sum($Rows, [string]$Property) {
    $sum = 0.0
    foreach ($row in @($Rows)) { $sum += [double](Get-Prop $row $Property 0) }
    return $sum
}

function ConvertFrom-WslText([string[]]$Lines) {
    return @($Lines | ForEach-Object { ($_ -replace "`0", '').TrimEnd() } | Where-Object { $_ -ne '' })
}

function Get-WslDistros {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return @() }
    $run = Invoke-Process 'wsl.exe' @('-l', '-q') $root 60
    return @(ConvertFrom-WslText $run.Lines | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '\s' -and $_ -notmatch '^docker-desktop' })
}

function Get-WslPath([string]$Distro, [string]$WindowsPath) {
    $run = Invoke-Process 'wsl.exe' @('-d', $Distro, '--exec', 'wslpath', '-a', '-u', ($WindowsPath -replace '\\', '/')) $root 60
    $lines = @(ConvertFrom-WslText $run.Lines)
    if ($run.Code -ne 0 -or $lines.Count -ne 1 -or -not $lines[0].StartsWith('/')) { return $null }
    return $lines[0]
}

function Test-LinuxDistro([string]$Name) {
    $repo = Get-WslPath $Name $root
    if (-not $repo) { return [pscustomobject]@{ Name = $Name; Usable = $false; Reason = 'wslpath could not convert the repository path (is the drive mounted?)' } }
    $run = Invoke-Process 'wsl.exe' @('-d', $Name, '--exec', 'bash', "$repo/tools/linux_probe.sh") $root 180
    $facts = @{}
    foreach ($line in (ConvertFrom-WslText $run.Lines)) { if ($line -match '^([a-z0-9-]+)=(.*)$') { $facts[$Matches[1]] = $Matches[2] } }
    $missing = @()
    foreach ($tool in 'cmake', 'ninja', 'python3') { if (-not $facts[$tool]) { $missing += $tool } }
    if (-not $facts['cc']) { $missing += 'a C compiler (gcc, gcc-13, gcc-12, gcc-11 or clang)' }
    $glibc = $null
    if ($facts['glibc'] -match '^(\d+\.\d+)') { $glibc = [version]$Matches[1] }
    return [pscustomobject]@{
        Name = $Name; Repo = $repo; Usable = ($missing.Count -eq 0 -and $run.Code -eq 0); Reason = $(if ($missing.Count) { "missing: $($missing -join ', ')" } else { '' })
        Os = $facts['os']; Glibc = $glibc; Cc = $facts['cc']; Ffmpeg = ($facts['ffmpeg'] -eq 'yes'); X11 = ($facts['x11'] -eq 'yes')
        Baseline = ($null -ne $glibc -and $glibc -le [version]'2.31')
    }
}

# The explicit choice, else JptFocalBuild, else any other distribution that looks like Ubuntu 20.04 (glibc 2.31), else a newer one
# (development only: the glibc baseline is then not proven).
function Find-LinuxEnvironment([string]$Requested) {
    $names = @(Get-WslDistros)
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return [pscustomobject]@{ Found = $false; Reason = 'wsl.exe is not installed' } }
    if (-not $names.Count) { return [pscustomobject]@{ Found = $false; Reason = 'wsl.exe lists no installed distribution' } }
    if ($Requested) {
        if ($names -notcontains $Requested) { return [pscustomobject]@{ Found = $false; Reason = "no WSL distribution named $Requested (installed: $($names -join ', '))" } }
        $candidate = Test-LinuxDistro $Requested
        if ($candidate.Usable) {
            $candidate = $candidate | Add-Member -NotePropertyName Found -NotePropertyValue $true -PassThru |
                Add-Member -NotePropertyName Why -NotePropertyValue 'chosen with -LinuxDistro' -PassThru
            return $candidate
        }
        return [pscustomobject]@{ Found = $false; Reason = "$Requested is not usable: $($candidate.Reason)" }
    }
    $preferred = @($names | Where-Object { $_ -eq 'JptFocalBuild' })
    $focal = @($names | Where-Object { $_ -match 'Focal' -and $_ -ne 'JptFocalBuild' } | Sort-Object)
    $others = @($names | Where-Object { $_ -notmatch 'Focal' } | Sort-Object)
    $rejected = New-Object System.Collections.Generic.List[string]
    foreach ($group in @(@($preferred + $focal), $others)) {
        $development = $null
        foreach ($name in $group) {
            $candidate = Test-LinuxDistro $name
            if (-not $candidate.Usable) { $rejected.Add("${name}: $($candidate.Reason)"); continue }
            if ($candidate.Baseline) {
                $why = if ($name -eq 'JptFocalBuild') { 'preferred baseline environment (Ubuntu 20.04, glibc 2.31)' } else { 'first usable glibc 2.31 distribution' }
                $candidate | Add-Member -NotePropertyName Found -NotePropertyValue $true; $candidate | Add-Member -NotePropertyName Why -NotePropertyValue $why
                return $candidate
            }
            if (-not $development) { $development = $candidate }
        }
        if ($development) {
            $development | Add-Member -NotePropertyName Found -NotePropertyValue $true
            $development | Add-Member -NotePropertyName Why -NotePropertyValue "no usable glibc 2.31 distribution: development environment only (glibc $($development.Glibc)); the glibc baseline is NOT proven"
            return $development
        }
    }
    return [pscustomobject]@{ Found = $false; Reason = "no distribution has cmake, ninja, python3 and a C compiler: $($rejected -join '; ')" }
}

# Reads the new lines of a file a process is still writing, until the process ends; returns the exit code.
function Wait-ProcStreaming($Handle, [scriptblock]$OnLine) {
    $streamState = @{ Carry = ''; File = $null }
    $drain = {
        param($final)
        if ($null -eq $streamState.File -and (Test-Path -LiteralPath $Handle.Out)) { $streamState.File = [System.IO.File]::Open($Handle.Out, 'Open', 'Read', 'ReadWrite') }
        if ($null -eq $streamState.File) { return }
        $buffer = New-Object byte[] 65536
        while (($read = $streamState.File.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $streamState.Carry += [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
            while (($newline = $streamState.Carry.IndexOf("`n")) -ge 0) {
                $line = $streamState.Carry.Substring(0, $newline).TrimEnd("`r")
                $streamState.Carry = $streamState.Carry.Substring($newline + 1)
                & $OnLine $line
            }
        }
        if ($final -and $streamState.Carry.Length) { $rest = $streamState.Carry.TrimEnd("`r"); $streamState.Carry = ''; & $OnLine $rest }
    }
    try {
        while (-not $Handle.Process.HasExited) {
            & $drain $false
            if ($Handle.Clock.Elapsed.TotalSeconds -gt $Handle.Timeout) { break }
            Start-Sleep -Milliseconds 150
        }
        $timedOut = -not $Handle.Process.HasExited
        if ($timedOut) { & taskkill.exe /PID $Handle.Process.Id /T /F 2>&1 | Out-Null }
        $Handle.Process.WaitForExit()
        & $drain $true
        $code = if ($timedOut) { 124 } else { $Handle.Process.ExitCode }
    } finally {
        if ($streamState.File) { $streamState.File.Dispose() }
        $Handle.Process.Dispose()
        Remove-Item -LiteralPath $Handle.Out -Force -ErrorAction SilentlyContinue
    }
    return $code
}

# Runs tools/test-linux.sh in the chosen distribution (PowerShell -> wsl.exe -> wslpath -> bash -> the Linux gate) and returns its rows.
# A row is printed when the next one starts, so the notes and the output lines that follow it are on it.
function Invoke-LinuxGate($Environment, [string]$Filter, [bool]$NoBuild, [bool]$Sanitize, [string]$ImportDir, [scriptblock]$Print) {
    $rows = New-Object System.Collections.Generic.List[object]
    $info = @{}
    $other = New-Object System.Collections.Generic.List[string]
    $gateState = @{ Pending = $null }
    $arguments = @('-d', $Environment.Name, '--exec', 'bash', "$($Environment.Repo)/tools/test-linux.sh", '--native-build', '--machine', '--cc', $Environment.Cc)
    if ($NoBuild) { $arguments += '--no-build' }
    if ($Filter) { $arguments += @('--filter', $Filter) }
    if ($Sanitize) { $arguments += '--sanitize' }
    if ($ImportDir) {
        $wslImport = if ($ImportDir.StartsWith('/')) { $ImportDir } else { Get-WslPath $Environment.Name $ImportDir }
        if ($wslImport) { $arguments += @('--import-dir', $wslImport) }
    }
    $flush = { if ($gateState.Pending) { & $Print $gateState.Pending; $gateState.Pending = $null } }
    $handle = Start-Proc 'wsl.exe' $arguments $root 3600
    $onLine = {
        param([string]$line)
        $line = $line -replace "`0", ''
        if (-not $line.StartsWith('@@')) { if ($line.Trim()) { $other.Add($line) }; return }
        $parts = $line.Substring(2).Split('|')
        $tail = if ($parts.Count -gt 2) { $parts[2..($parts.Count - 1)] -join '|' } else { '' }
        switch ($parts[0]) {
            'INFO' { $info[$parts[1]] = $tail }
            'BUILD' {
                & $flush
                $seconds = 0.0
                [void][double]::TryParse($parts[2], [ref]$seconds)
                $row = [pscustomobject]@{ Name = 'linux build'; Status = $(if ($parts[1] -eq 'PASS') { 'PASS' } else { 'FAIL' }); Checks = 0; Seconds = $seconds; Unavailable = $false; Summary = ($parts[3..($parts.Count - 1)] -join '|'); Lines = @(); Notes = @() }
                $rows.Add($row); $gateState.Pending = $row
            }
            'IMPORT' {
                & $flush
                $seconds = 0.0
                [void][double]::TryParse($parts[2], [ref]$seconds)
                $row = [pscustomobject]@{ Name = 'game imports'; Status = $(if ($parts[1] -eq 'PASS') { 'PASS' } else { 'FAIL' }); Checks = 0; Seconds = $seconds; Unavailable = $false; Summary = ($parts[3..($parts.Count - 1)] -join '|'); Lines = @(); Notes = @() }
                $rows.Add($row); $gateState.Pending = $row
            }
            'SUITE' {
                & $flush
                $row = [pscustomobject]@{ Name = $parts[1]; Status = $parts[2]; Checks = [int]$parts[3]; Seconds = [double]$parts[4]; Unavailable = ($parts[5] -eq '1'); Summary = ($parts[6..($parts.Count - 1)] -join '|'); Lines = @(); Notes = @() }
                $rows.Add($row); $gateState.Pending = $row
            }
            'NOTE' { if ($gateState.Pending -and $gateState.Pending.Name -eq $parts[1]) { $gateState.Pending.Notes += $tail } }
            'OUT' {
                $target = $gateState.Pending
                if ($target -and ($target.Name -eq $parts[1] -or ($parts[1] -eq 'build' -and $target.Name -eq 'linux build') -or
                                  ($parts[1] -eq 'import' -and $target.Name -eq 'game imports'))) { $target.Lines += $tail }
            }
            'END' { & $flush }
        }
    }
    $code = Wait-ProcStreaming $handle $onLine
    & $flush
    if ($code -ne 0 -and @($rows | Where-Object { $_.Status -eq 'FAIL' }).Count -eq 0) {
        $row = [pscustomobject]@{ Name = 'linux gate'; Status = 'FAIL'; Checks = 0; Seconds = 0; Unavailable = $false; Summary = "the Linux gate exited with code $code"; Lines = @($other | Select-Object -Last 40); Notes = @() }
        $rows.Add($row); & $Print $row
    }
    return [pscustomobject]@{ Rows = $rows; Info = $info; Code = $code; Other = $other }
}

# ---------------------------------------------------------------------------------------------------------------------
# Feature parity
# ---------------------------------------------------------------------------------------------------------------------

function Get-EvidenceState($Evidence, $Results, [bool]$Ran, [string]$NotRunReason) {
    $failed = @(); $blocked = @()
    if (@($Evidence).Count -eq 0) { return [pscustomobject]@{ Status = 'FAIL'; Reason = 'the contract lists no evidence' } }
    foreach ($item in @($Evidence)) {
        if (-not $Ran) { $blocked += "$($item.suite): $NotRunReason"; continue }
        $result = $Results[$item.suite]
        if (-not $result) { $failed += "$($item.suite): not run"; continue }
        if ($result.Status -eq 'FAIL') { $failed += "$($item.suite): FAIL"; continue }
        if ($result.Status -ne 'PASS') { $blocked += "$($item.suite): $($result.Status)"; continue }
        if ($result.PSObject.Properties['Unavailable'] -and $result.Unavailable) { $blocked += "$($item.suite): DATA UNAVAILABLE" }
    }
    if ($failed.Count) { return [pscustomobject]@{ Status = 'FAIL'; Reason = ($failed -join '; ') } }
    if ($blocked.Count) { return [pscustomobject]@{ Status = 'BLOCKED'; Reason = ($blocked -join '; ') } }
    return [pscustomobject]@{ Status = 'PASS'; Reason = '' }
}

function Get-PlatformState($Feature, [string]$Platform, $Results, [bool]$Ran, [string]$NotRunReason) {
    $section = $Feature.$Platform
    $evidence = @(Get-Prop $Feature 'shared' @()) + @(Get-Prop $section 'evidence' @())
    $evidence = @($evidence | Where-Object { $_ })
    switch ($section.status) {
        'PLATFORM_ONLY' { return [pscustomobject]@{ Status = 'PLATFORM_ONLY'; Reason = (Get-Prop $section 'reason' '') } }
        'BLOCKED' { return [pscustomobject]@{ Status = 'BLOCKED'; Reason = (Get-Prop $section 'reason' '') } }
        default {
            # A shared suite needs both platforms: its own platform's result is read from that platform's map.
            return (Get-EvidenceState $evidence $Results $Ran $NotRunReason)
        }
    }
}

# One row per feature with the status of each platform and the parity verdict. $WindowsRan and $LinuxRan say whether that half ran.
function Get-FeatureParity($Contract, $WindowsResults, $LinuxResults, [bool]$WindowsRan, [bool]$LinuxRan, [string]$LinuxWhy) {
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($feature in @($Contract.features)) {
        $windows = Get-PlatformState $feature 'windows' $WindowsResults $WindowsRan 'the Windows half did not run'
        $linux = Get-PlatformState $feature 'linux' $LinuxResults $LinuxRan $LinuxWhy
        $parity = switch ($feature.scope) {
            'global' {
                if ($windows.Status -eq 'FAIL' -or $linux.Status -eq 'FAIL') { 'FAIL' }
                elseif ($windows.Status -eq 'PASS' -and $linux.Status -eq 'PASS') { 'PASS' }
                elseif ($windows.Status -eq 'PLATFORM_ONLY' -or $linux.Status -eq 'PLATFORM_ONLY') { 'FAIL' }
                else { 'BLOCKED' }
            }
            'windows' { if ($windows.Status -eq 'PASS' -and $linux.Status -eq 'PLATFORM_ONLY') { 'PASS' } elseif ($windows.Status -eq 'FAIL') { 'FAIL' } elseif ($windows.Status -eq 'BLOCKED') { 'BLOCKED' } else { 'FAIL' } }
            'linux' { if ($linux.Status -eq 'PASS' -and $windows.Status -eq 'PLATFORM_ONLY') { 'PASS' } elseif ($linux.Status -eq 'FAIL') { 'FAIL' } elseif ($linux.Status -eq 'BLOCKED') { 'BLOCKED' } else { 'FAIL' } }
            'planned' { 'PLANNED' }
            default { 'FAIL' }
        }
        $reason = @($windows.Reason, $linux.Reason | Where-Object { $_ }) -join ' | '
        $rows.Add([pscustomobject]@{ Id = $feature.id; Name = $feature.name; Owner = $feature.owner; Scope = $feature.scope; Windows = $windows.Status; Linux = $linux.Status; Parity = $parity; Reason = $reason })
    }
    return $rows
}
