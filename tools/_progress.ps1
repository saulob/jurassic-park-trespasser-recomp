# Progress of tools\test.ps1: groups done of total, the running group, its time, the total time, the ETA from the history, the
# finish time, the checks counted and the RAM. Callers pass the time in seconds, so the same code runs against a stopwatch or
# synthetic time. Works in Windows PowerShell 5.1 and PowerShell 7.

$script:ProgressMaxSeconds = 99 * 3600
$script:ProgressOverrun = 0.15      # a group past its estimate is expected to need this share of its time again
$script:ProgressConfidence = 0.75   # share of the remaining time that must come from history for a plain ETA
$script:ProgressUnknown = 5.0       # seconds for a group nothing is known about
$script:RegressionFactor = 1.5
$script:RegressionSeconds = 10

function Format-Count([double]$Value) { ([long][Math]::Max(0, [Math]::Floor($Value))).ToString('N0', [cultureinfo]::InvariantCulture) }

function ConvertTo-SafeSeconds($Seconds) {
    if ($null -eq $Seconds) { return 0.0 }
    $value = [double]$Seconds
    if ([double]::IsNaN($value) -or $value -lt 0) { return 0.0 }
    return [Math]::Min($value, [double]$script:ProgressMaxSeconds)
}

function Format-Elapsed($Seconds) {
    $whole = [long][Math]::Floor((ConvertTo-SafeSeconds $Seconds))
    $hours = [Math]::Floor($whole / 3600)
    $minutes = [Math]::Floor(($whole % 3600) / 60)
    $secs = $whole % 60
    if ($hours -gt 0) { return '{0}h {1:00}m {2:00}s' -f $hours, $minutes, $secs }
    if ($minutes -gt 0) { return '{0}m {1:00}s' -f $minutes, $secs }
    return '{0}s' -f $secs
}

# Rounded so the estimate does not flicker: 2 s steps under a minute, 30 s under ten minutes, whole minutes above.
function Format-Rounded($Seconds) {
    $value = ConvertTo-SafeSeconds $Seconds
    if ($value -lt 60) { return '{0}s' -f [Math]::Max(2, 2 * [Math]::Round($value / 2, [MidpointRounding]::AwayFromZero)) }
    $step = if ($value -lt 600) { 30 } else { 60 }
    $rounded = $step * [Math]::Round($value / $step, [MidpointRounding]::AwayFromZero)
    $hours = [Math]::Floor($rounded / 3600)
    $minutes = [Math]::Floor(($rounded % 3600) / 60)
    $secs = $rounded % 60
    if ($hours -gt 0) { return '{0}h {1:00}m' -f $hours, $minutes }
    if ($secs -gt 0) { return '{0}m {1:00}s' -f $minutes, $secs }
    return '{0}m' -f $minutes
}

function Format-Eta($Eta) {
    if ($null -eq $Eta -or $null -eq $Eta.Seconds) { return 'calculating' }
    if ($Eta.Seconds -le 0) { return '0s' }
    if ($Eta.Confident) { return Format-Rounded $Eta.Seconds }
    return '~{0} (low confidence)' -f (Format-Rounded $Eta.Seconds)
}

function Format-Finish([datetime]$Now, $Eta) {
    if ($null -eq $Eta -or $null -eq $Eta.Seconds) { return '' }
    $end = $Now.AddSeconds((ConvertTo-SafeSeconds $Eta.Seconds))
    $days = ($end.Date - $Now.Date).Days
    $text = $end.ToString('HH:mm', [cultureinfo]::InvariantCulture)
    if ($days -gt 0) { $text += ' (+{0}d)' -f $days }
    if (-not $Eta.Confident) { $text = '~' + $text }
    return $text
}

function Get-ProgressPercent([int]$Done, [int]$Total) {
    if ($Total -le 0) { return 0 }
    return [int][Math]::Floor([Math]::Max(0, [Math]::Min($Done, $Total)) * 100.0 / $Total)
}

# A terminal that redraws a line: output not redirected and a console window. JPT_TEST_ANIMATE=1 or 0 forces it.
function Test-InteractiveConsole {
    switch ($env:JPT_TEST_ANIMATE) { '1' { return $true } '0' { return $false } }
    if ($env:CI) { return $false }
    if ($Host.Name -in 'Windows PowerShell ISE Host', 'ServerRemoteHost') { return $false }
    try { if ([Console]::IsOutputRedirected) { return $false } } catch { return $false }
    try { return $Host.UI.RawUI.WindowSize.Width -gt 0 } catch { return $false }
}

function New-ProgressGroup([string]$Name, [string]$Platform = 'windows', $Estimate = $null, [bool]$Known = $false, $Previous = $null) {
    [pscustomobject]@{ Name = $Name; Platform = $Platform; Estimate = $Estimate; Known = $Known; Previous = $Previous
                       State = 'WAITING'; Start = 0.0; Seconds = 0.0; Checks = $null }
}

# Memory: optional scriptblock returning TotalGB and FreeGB, shown as the RAM in use.
function New-ProgressReporter($Groups, [double]$Now, [bool]$Interactive, [scriptblock]$Write, [double]$Interval = 0,
                              [datetime]$StartClock = [datetime]::Now, [scriptblock]$Memory = $null) {
    if ($Interval -le 0) { $Interval = if ($Interactive) { 0.5 } else { 30 } }
    $reporter = [pscustomobject]@{
        Groups = (New-Object System.Collections.Generic.List[object]); Index = @{}; Foreground = ''
        RunStart = $Now; StartClock = $StartClock; Interactive = $Interactive; Interval = $Interval; LastEmit = $Now
        LineDirty = $false; Tick = 0; Width = 0; Write = $Write; Memory = $Memory; Checks = 0L
    }
    foreach ($group in $Groups) { [void](Add-ProgressGroup $reporter $group) }
    return $reporter
}

function Add-ProgressGroup($R, $Group) {
    if ($Group -is [string]) { $Group = New-ProgressGroup $Group }
    if ($R.Index.ContainsKey($Group.Name)) { return $R.Index[$Group.Name] }
    $R.Groups.Add($Group)
    $R.Index[$Group.Name] = $Group
    return $Group
}

function Get-ProgressGroup($R, [string]$Name) { if ($R.Index.ContainsKey($Name)) { return $R.Index[$Name] } return $null }
function Get-RunningGroups($R) { return , @($R.Groups | Where-Object { $_.State -eq 'RUNNING' }) }
function Get-FinishedGroups($R) { return , @($R.Groups | Where-Object { $_.State -ne 'RUNNING' -and $_.State -ne 'WAITING' }) }

function Get-ShownGroup($R, $Running) {
    $shown = @($Running | Where-Object { $_.Name -eq $R.Foreground })
    if ($shown.Count) { return $shown[0] }
    return ($Running | Sort-Object -Property Start | Select-Object -First 1)
}

# A group without history of its own is taken for a slow one (75th percentile) of those that passed on its platform.
function Get-UnknownEstimate($R, [string]$Platform) {
    $alike = @($R.Groups | Where-Object { $_.Platform -eq $Platform -and $_.State -eq 'PASS' } | ForEach-Object { [double]$_.Seconds } | Sort-Object)
    if ($alike.Count -eq 0) { return [double]$script:ProgressUnknown }
    return [Math]::Max([double]$script:ProgressUnknown, [double]$alike[[int][Math]::Min($alike.Count - 1, [Math]::Floor($alike.Count * 0.75))])
}

function Get-ProgressEta($R, [double]$Now) {
    $fallback = @{}
    $total = 0.0
    $known = 0.0
    foreach ($group in $R.Groups) {
        if ($group.State -ne 'RUNNING' -and $group.State -ne 'WAITING') { continue }
        $estimate = $group.Estimate
        if ($null -eq $estimate) {
            if (-not $fallback.ContainsKey($group.Platform)) { $fallback[$group.Platform] = Get-UnknownEstimate $R $group.Platform }
            $estimate = $fallback[$group.Platform]
        }
        $left = [double]$estimate
        if ($group.State -eq 'RUNNING') {
            $elapsed = ConvertTo-SafeSeconds ($Now - $group.Start)
            $left = [Math]::Max($left - $elapsed, $script:ProgressOverrun * $elapsed)
        }
        $total += $left
        if ($group.Known) { $known += $left }
    }
    return [pscustomobject]@{ Seconds = (ConvertTo-SafeSeconds $total); Confident = ($total -le 0 -or $known / $total -ge $script:ProgressConfidence) }
}

function Get-ProgressSpinnerFrame([long]$Tick) { '|/-\'[[int](((($Tick % 4) + 4) % 4))] }

function Get-LineWidth($R) {
    if ($R.Width -gt 0) { return $R.Width }
    try { $width = $Host.UI.RawUI.WindowSize.Width; if ($width -gt 20) { return $width - 1 } } catch { $null = $_ }
    return 160
}

function ConvertTo-InvariantGB([double]$Value) { [string]::Format([cultureinfo]::InvariantCulture, '{0:N1}', $Value) }

# Gate RAM, its peak and the system's free RAM, from the Memory scriptblock (GateGB, HardGB, PeakGB, FreeGB).
function Get-MemoryParts($R) {
    $parts = @{}
    if (-not $R.Memory) { return $parts }
    $state = & $R.Memory
    if (-not $state) { return $parts }
    $parts['gate'] = 'Gate RAM {0}/{1} GB' -f (ConvertTo-InvariantGB $state.GateGB), (ConvertTo-InvariantGB $state.HardGB)
    $parts['peak'] = 'Peak {0} GB' -f (ConvertTo-InvariantGB $state.PeakGB)
    if ($null -ne $state.FreeGB) { $parts['free'] = 'System free {0} GB' -f (ConvertTo-InvariantGB $state.FreeGB) }
    if ($state.PSObject.Properties['Low'] -and $state.Low) { $parts['low'] = 'LOW MEMORY' }
    return $parts
}

# [42/168] 25% | suite | Group 8s of 12s | Total 5m 31s | ETA 14m | Gate RAM 1.2/9.5 GB | Peak 6.2 GB | System free 1.7 GB | LOW MEMORY | Finish 18:42 | Checks 124
# A narrow line drops parts in this order: Group, Peak, Checks, Finish, System free, LOW MEMORY, Gate RAM, ETA, Total; the suite name is cut last.
function Format-ProgressLine($R, [double]$Now, [int]$Width = 0) {
    $running = Get-RunningGroups $R
    $done = (Get-FinishedGroups $R).Count
    $total = $R.Groups.Count
    $group = Get-ShownGroup $R $running
    $others = $running.Count - 1
    $eta = Get-ProgressEta $R $Now
    $name = if ($group) { $group.Name } elseif ($done -ge $total) { 'complete' } else { 'waiting' }
    if ($others -gt 0) { $name += ' +{0}' -f $others }
    $elapsed = if ($group) { ConvertTo-SafeSeconds ($Now - $group.Start) } else { 0.0 }
    $memory = Get-MemoryParts $R
    $parts = New-Object System.Collections.Generic.List[object]
    $add = { param([string]$Key, [string]$Text, [int]$Keep) if ($Text) { $parts.Add([pscustomobject]@{ Key = $Key; Text = $Text; Keep = $Keep }) } }
    & $add 'head' ('[{0}/{1}] {2}%' -f $done, $total, (Get-ProgressPercent $done $total)) 100
    & $add 'name' $name 90
    & $add 'group' ('Group {0}{1}' -f (Format-Elapsed $elapsed), $(if ($group -and $group.Known) { ' of {0}' -f (Format-Rounded $group.Estimate) } else { '' })) 10
    & $add 'total' ('Total {0}' -f (Format-Elapsed ($Now - $R.RunStart))) 80
    & $add 'eta' ('ETA {0}' -f (Format-Eta $eta)) 70
    if ($memory.ContainsKey('gate')) { & $add 'gate' $memory['gate'] 60 }
    if ($memory.ContainsKey('peak')) { & $add 'peak' $memory['peak'] 20 }
    if ($memory.ContainsKey('free')) { & $add 'free' $memory['free'] 50 }
    if ($memory.ContainsKey('low')) { & $add 'low' $memory['low'] 55 }
    $finish = Format-Finish $R.StartClock.AddSeconds((ConvertTo-SafeSeconds ($Now - $R.RunStart))) $eta
    if ($finish) { & $add 'finish' ('Finish {0}' -f $finish) 40 }
    if ($R.Checks -gt 0) { & $add 'checks' ('Checks {0}' -f (Format-Count $R.Checks)) 30 }

    if ($Width -le 0) { $Width = [int]::MaxValue }
    $join = { ($parts | ForEach-Object { $_.Text }) -join ' | ' }
    while ((& $join).Length -gt $Width) {
        $drop = $parts | Where-Object { $_.Keep -lt 90 } | Sort-Object Keep | Select-Object -First 1
        if (-not $drop) { break }
        [void]$parts.Remove($drop)
    }
    $line = & $join
    if ($line.Length -gt $Width) {
        $label = $parts | Where-Object { $_.Key -eq 'name' } | Select-Object -First 1
        $room = $label.Text.Length - ($line.Length - $Width)
        if ($room -ge 8) { $label.Text = $label.Text.Substring(0, $room - 3) + '...' } else { [void]$parts.Remove($label) }
        $line = & $join
    }
    return $line
}

# The short form in front of every result row: [37/181] 20% | Total 4m 12s | ETA 11m
function Format-RowPrefix($R, [double]$Now) {
    $done = (Get-FinishedGroups $R).Count
    $total = $R.Groups.Count
    return '[{0,3}/{1}] {2,3}% | {3} | ETA {4}' -f $done, $total, (Get-ProgressPercent $done $total), (Format-Elapsed ($Now - $R.RunStart)), (Format-Eta (Get-ProgressEta $R $Now))
}

function Clear-ProgressLine($R) {
    if (-not $R -or -not $R.LineDirty) { return }
    & $R.Write ("`r" + (' ' * (Get-LineWidth $R)) + "`r") $true
    $R.LineDirty = $false
}

function Write-ProgressUpdate($R, [double]$Now) {
    $R.LastEmit = $Now
    if ($R.Interactive) {
        $width = Get-LineWidth $R
        $text = '{0} {1}' -f (Get-ProgressSpinnerFrame $R.Tick), (Format-ProgressLine $R $Now ($width - 2))
        & $R.Write ("`r" + $text.PadRight($width)) $true
        $R.LineDirty = $true
        $R.Tick++
        return
    }
    & $R.Write ('RUNNING ' + (Format-ProgressLine $R $Now)) $false
}

function Update-Progress($R, [double]$Now, [switch]$Force) {
    if ((Get-RunningGroups $R).Count -eq 0) { return $false }
    if (-not $Force -and ($Now - $R.LastEmit) -lt ($R.Interval - 0.05)) { return $false }
    Write-ProgressUpdate $R $Now
    return $true
}

function Start-ProgressGroup($R, [string]$Name, [double]$Now, [switch]$Background, [string]$Platform = 'windows') {
    $group = Get-ProgressGroup $R $Name
    if (-not $group) { $group = Add-ProgressGroup $R (New-ProgressGroup $Name $Platform) }
    $group.State = 'RUNNING'
    $group.Start = $Now
    if (-not $Background) { $R.Foreground = $Name }
}

# Seconds: the measured duration when known (a background process may have ended before its row is reported).
function Complete-ProgressGroup($R, [string]$Name, [string]$State, [double]$Now, $Checks = $null, $Seconds = $null, [string]$Platform = 'windows') {
    $group = Get-ProgressGroup $R $Name
    if (-not $group) { $group = Add-ProgressGroup $R (New-ProgressGroup $Name $Platform) }
    $wasCounted = $group.State -ne 'RUNNING' -and $group.State -ne 'WAITING'
    if ($null -ne $Seconds) { $group.Seconds = ConvertTo-SafeSeconds $Seconds }
    elseif ($group.State -eq 'RUNNING') { $group.Seconds = ConvertTo-SafeSeconds ($Now - $group.Start) }
    if ($null -ne $Checks) {
        if ($wasCounted -and $null -ne $group.Checks) { $R.Checks -= [long]$group.Checks }
        $group.Checks = $Checks
        $R.Checks += [long]$Checks
    }
    $group.State = $State
    if ($R.Foreground -eq $Name) { $R.Foreground = '' }
}

# Groups planned but never started: they end as NOT RUN so the count reaches the total.
function Close-WaitingGroups($R, [string]$Platform) {
    foreach ($group in $R.Groups) { if ($group.State -eq 'WAITING' -and $group.Platform -eq $Platform) { $group.State = 'NOT RUN' } }
}

function Get-SlowestGroups($R, [int]$Count = 10) {
    $ranked = @((Get-FinishedGroups $R) | Where-Object { $_.State -in 'PASS', 'FAIL' -and $_.Seconds -gt 0 } |
        Sort-Object -Property @{ Expression = 'Seconds'; Descending = $true }, @{ Expression = 'Name'; Descending = $false })
    return , @($ranked | Select-Object -First $Count)
}

# Groups that passed clearly slower than their own history: diagnostic only.
function Get-Regressions($R) {
    return , @($R.Groups | Where-Object {
            $_.State -eq 'PASS' -and $_.Known -and $_.Seconds -gt $_.Estimate * $script:RegressionFactor -and $_.Seconds - $_.Estimate -ge $script:RegressionSeconds
        })
}

function Format-Change([double]$Seconds, [double]$Previous) {
    $delta = $Seconds - $Previous
    $sign = if ($delta -lt 0) { '-' } else { '+' }
    $percent = if ($Previous -gt 0) { (100.0 * [Math]::Abs($delta) / $Previous).ToString('0.0', [cultureinfo]::InvariantCulture) } else { '0.0' }
    return '{0}{1}, {0}{2}%' -f $sign, (Format-Elapsed ([Math]::Abs($delta))), $percent
}

# The slowest groups, the ones clearly slower than their history and the change since the last comparable run.
function Format-TimingReport($R, [double]$Now, $PreviousRun = $null) {
    $total = ConvertTo-SafeSeconds ($Now - $R.RunStart)
    $lines = New-Object System.Collections.Generic.List[string]
    $slowest = Get-SlowestGroups $R 10
    if ($slowest.Count) {
        $width = [Math]::Max(20, [int]($slowest | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum)
        $lines.Add('slowest suites (background suites overlap others, so the shares can add up to more than 100%):')
        $rank = 1
        foreach ($group in $slowest) {
            $share = if ($total -gt 0) { (100.0 * $group.Seconds / $total).ToString('0.0', [cultureinfo]::InvariantCulture) + '%' } else { '' }
            $line = ('  {0,2}. {1,-' + $width + '} {2,-8} {3,10} {4,7}') -f $rank, $group.Name, $group.Platform, (Format-Elapsed $group.Seconds), $share
            if ($null -ne $group.Previous) { $line += '   previous {0}' -f (Format-Elapsed $group.Previous) }
            $lines.Add($line)
            $rank++
        }
    }
    foreach ($group in (Get-Regressions $R)) {
        $lines.Add(('WARNING: {0} ({1}) took {2}, usually {3} ({4}); not a failure' -f $group.Name, $group.Platform, (Format-Elapsed $group.Seconds), (Format-Elapsed $group.Estimate), (Format-Change $group.Seconds $group.Estimate)))
    }
    if ($null -ne $PreviousRun) {
        $lines.Add(('previous comparable run: {0} ({1}); change {2}' -f (Format-Elapsed $PreviousRun.seconds), $PreviousRun.at, (Format-Change $total $PreviousRun.seconds)))
    }
    return , $lines.ToArray()
}
