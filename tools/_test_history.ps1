# Local suite durations for tools\test.ps1 in build\test-history\timings.json (ignored by git, disposable). Only the ETA and the
# timing report read them: every function here degrades to "no history" instead of failing. Works in Windows PowerShell 5.1
# and PowerShell 7.

$script:HistorySchema = 1
$script:HistorySampleLimit = 8

function Get-TimingKey([string]$Platform, [string]$Mode, [string]$Group) { '{0}|{1}|{2}' -f $Platform, $Mode, $Group }

function Get-Field($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { return , $property.Value }
    return $null
}

function ConvertTo-Seconds($Value) {
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    $seconds = 0.0
    $style = [Globalization.NumberStyles]::Float
    if (-not [double]::TryParse([string]::Format([cultureinfo]::InvariantCulture, '{0}', $Value), $style, [cultureinfo]::InvariantCulture, [ref]$seconds)) { return $null }
    if ([double]::IsNaN($seconds) -or [double]::IsInfinity($seconds) -or $seconds -le 0) { return $null }
    return $seconds
}

function ConvertTo-HistorySamples($Values) {
    $samples = New-Object System.Collections.Generic.List[object]
    foreach ($value in @($Values)) {
        $seconds = ConvertTo-Seconds (Get-Field $value 'seconds')
        if ($null -eq $seconds) { continue }
        $at = Get-Field $value 'at'
        if ($at -is [datetime]) { $at = $at.ToString('s', [cultureinfo]::InvariantCulture) }
        $sample = [ordered]@{ seconds = [Math]::Round($seconds, 2); at = "$at" }
        foreach ($name in 'checks', 'groups') {
            $number = ConvertTo-Seconds (Get-Field $value $name)
            if ($null -ne $number) { $sample[$name] = [long]$number }
        }
        $memory = ConvertTo-Seconds (Get-Field $value 'memory')
        if ($null -ne $memory) { $sample['memory'] = [Math]::Round($memory, 3) }
        $samples.Add([pscustomobject]$sample)
    }
    while ($samples.Count -gt $script:HistorySampleLimit) { $samples.RemoveAt(0) }
    return , $samples
}

function ConvertTo-HistoryTable($Object) {
    $table = @{}
    if ($Object -is [System.Management.Automation.PSCustomObject]) {
        foreach ($property in $Object.PSObject.Properties) {
            $samples = ConvertTo-HistorySamples $property.Value
            if ($samples.Count) { $table[$property.Name] = $samples }
        }
    }
    return $table
}

function Read-HistoryText([string]$Text) {
    $result = [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $true; Problem = '' }
    if ([string]::IsNullOrWhiteSpace($Text)) { $result.Problem = 'empty'; return $result }
    try { $json = $Text | ConvertFrom-Json } catch { $result.Problem = 'not readable as JSON'; return $result }
    if ($json -isnot [System.Management.Automation.PSCustomObject]) { $result.Problem = 'not a JSON object'; return $result }
    $schema = ConvertTo-Seconds (Get-Field $json 'schema')
    if ($null -eq $schema -or $schema -lt $script:HistorySchema) { $result.Problem = 'an older schema'; return $result }
    if ($schema -gt $script:HistorySchema) { $result.Problem = 'a newer schema'; $result.Writable = $false; return $result }
    $result.Groups = ConvertTo-HistoryTable (Get-Field $json 'groups')
    $result.Runs = ConvertTo-HistoryTable (Get-Field $json 'runs')
    return $result
}

function Read-HistoryFile([string]$Path) {
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $true; Problem = '' } }
        return Read-HistoryText ([IO.File]::ReadAllText($Path))
    } catch {
        return [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $false; Problem = "not readable: $($_.Exception.Message)" }
    }
}

function New-TestHistory([string]$Path) {
    $file = Read-HistoryFile $Path
    [pscustomobject]@{ Path = $Path; Groups = $file.Groups; Runs = $file.Runs; Writable = $file.Writable; Problem = $file.Problem; SaveProblem = ''
                       NewGroups = (New-Object System.Collections.Generic.List[object]); NewRuns = (New-Object System.Collections.Generic.List[object]) }
}

# Only a suite that ran to its end and passed is a duration worth remembering.
function Add-TimingSample($History, [string]$Key, [string]$State, $Seconds, $Checks = $null, [datetime]$At = [datetime]::Now, [double]$MemoryGB = 0) {
    $value = ConvertTo-Seconds $Seconds
    if ($State -ne 'PASS' -or $null -eq $value) { return $false }
    $sample = [ordered]@{ seconds = [Math]::Round($value, 2); at = $At.ToString('s', [cultureinfo]::InvariantCulture) }
    if ($null -ne $Checks -and $Checks -gt 0) { $sample['checks'] = [long]$Checks }
    if ($MemoryGB -gt 0) { $sample['memory'] = [Math]::Round($MemoryGB, 3) }
    $History.NewGroups.Add([pscustomobject]@{ Key = $Key; Sample = [pscustomobject]$sample })
    return $true
}

function Add-RunSample($History, [string]$Key, [bool]$Complete, $Seconds, [int]$Groups, [datetime]$At = [datetime]::Now) {
    $value = ConvertTo-Seconds $Seconds
    if (-not $Complete -or $null -eq $value) { return $false }
    $sample = [pscustomobject][ordered]@{ seconds = [Math]::Round($value, 2); at = $At.ToString('s', [cultureinfo]::InvariantCulture); groups = $Groups }
    $History.NewRuns.Add([pscustomobject]@{ Key = $Key; Sample = $sample })
    return $true
}

function Merge-HistorySamples([hashtable]$Table, $New) {
    foreach ($entry in $New) {
        $values = New-Object System.Collections.Generic.List[object]
        if ($Table.ContainsKey($entry.Key)) { foreach ($sample in $Table[$entry.Key]) { $values.Add($sample) } }
        $values.Add($entry.Sample)
        $Table[$entry.Key] = ConvertTo-HistorySamples $values.ToArray()
    }
}

function ConvertTo-HistoryJson([hashtable]$Groups, [hashtable]$Runs) {
    $body = [ordered]@{ schema = $script:HistorySchema; groups = [ordered]@{}; runs = [ordered]@{} }
    foreach ($key in @($Groups.Keys | Sort-Object)) { $body.groups[$key] = $Groups[$key].ToArray() }
    foreach ($key in @($Runs.Keys | Sort-Object)) { $body.runs[$key] = $Runs[$key].ToArray() }
    return ($body | ConvertTo-Json -Depth 6) + "`n"
}

function Write-AtomicText([string]$Path, [string]$Text) {
    $folder = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }
    $partial = "$Path.$PID.tmp"
    try {
        [IO.File]::WriteAllText($partial, $Text, (New-Object System.Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($partial, $Path, [NullString]::Value) } else { [IO.File]::Move($partial, $Path) }
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue }
    }
}

# Adds this run's samples to what the file holds now, so two runs never erase each other's. False when nothing was written.
function Save-TestHistory($History) {
    if (-not $History.Writable -or ($History.NewGroups.Count -eq 0 -and $History.NewRuns.Count -eq 0)) { return $false }
    try {
        $file = Read-HistoryFile $History.Path
        if (-not $file.Writable) { return $false }
        Merge-HistorySamples $file.Groups $History.NewGroups
        Merge-HistorySamples $file.Runs $History.NewRuns
        Write-AtomicText $History.Path (ConvertTo-HistoryJson $file.Groups $file.Runs)
        $History.NewGroups.Clear()
        $History.NewRuns.Clear()
        return $true
    } catch {
        $History.SaveProblem = $_.Exception.Message
        return $false
    }
}

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2
}

function Get-HistoryDurations($History, [string]$Key) {
    if (-not $History.Groups.ContainsKey($Key)) { return @() }
    return @($History.Groups[$Key] | ForEach-Object { [double]$_.seconds })
}

# What a suite should take: the median of its own recent passing runs in this mode, else in another mode on the same
# platform, else the first-run guess. Known is true only for the first.
function Get-DurationEstimate($History, [string]$Platform, [string]$Mode, [string]$Group, $Nominal = $null) {
    $own = @(Get-HistoryDurations $History (Get-TimingKey $Platform $Mode $Group))
    if ($own.Count) { return [pscustomobject]@{ Seconds = (Get-Median $own); Known = $true; Previous = $own[$own.Count - 1] } }
    $other = New-Object System.Collections.Generic.List[double]
    foreach ($key in $History.Groups.Keys) {
        $parts = $key.Split('|', 3)
        if ($parts.Count -eq 3 -and $parts[0] -eq $Platform -and $parts[2] -eq $Group) { foreach ($value in (Get-HistoryDurations $History $key)) { $other.Add($value) } }
    }
    if ($other.Count) { return [pscustomobject]@{ Seconds = (Get-Median $other.ToArray()); Known = $false; Previous = $null } }
    return [pscustomobject]@{ Seconds = (ConvertTo-Seconds $Nominal); Known = $false; Previous = $null }
}

# The largest peak (GB) the suite's process tree reached in its recent passing runs, a quarter more, or 0 when none was measured.
function Get-MemoryEstimate($History, [string]$Key) {
    if (-not $History.Groups.ContainsKey($Key)) { return 0.0 }
    $peaks = @($History.Groups[$Key] | Where-Object { $_.PSObject.Properties['memory'] } | ForEach-Object { [double]$_.memory })
    if (-not $peaks.Count) { return 0.0 }
    return [Math]::Max(0.1, 1.25 * ($peaks | Measure-Object -Maximum).Maximum)
}

function Get-PreviousRun($History, [string]$Key) {
    if (-not $History.Runs.ContainsKey($Key)) { return $null }
    $samples = $History.Runs[$Key]
    return $samples[$samples.Count - 1]
}

# ---- checkpoint of the last run, read only by -Resume

$script:CheckpointSchema = 1

function New-RunCheckpoint([string]$Fingerprint, [string]$Config, [string]$Suites) {
    [pscustomobject]@{ schema = $script:CheckpointSchema; fingerprint = $Fingerprint; config = $Config; suites = $Suites
                       started = [datetime]::Now.ToString('s', [cultureinfo]::InvariantCulture); broken = $false
                       passed = (New-Object System.Collections.Generic.List[object]) }
}

function Test-CheckpointHas($Checkpoint, [string]$Name) {
    foreach ($entry in $Checkpoint.passed) { if ($entry.name -eq $Name) { return $true } }
    return $false
}

# Records the suites that passed in list order until the first one that did not: a failure, memory stops included, freezes the
# checkpoint before it, so -Resume runs it again.
function Add-CheckpointResult($Checkpoint, [string]$Name, [string]$Platform, [string]$Status, $Checks = 0, [string[]]$Lines = @()) {
    if ($Status -ne 'PASS') { $Checkpoint.broken = $true; return $false }
    if ($Checkpoint.broken -or (Test-CheckpointHas $Checkpoint $Name)) { return $false }
    $entry = [ordered]@{ name = $Name; platform = $Platform; position = $Checkpoint.passed.Count + 1; status = 'PASS'; checks = [long]$Checks }
    if ($null -ne $Lines -and $Lines.Length -gt 0) { $entry['lines'] = $Lines }
    $Checkpoint.passed.Add([pscustomobject]$entry)
    return $true
}

function Save-RunCheckpoint($Checkpoint, [string]$Path) {
    $body = [ordered]@{ schema = $Checkpoint.schema; fingerprint = $Checkpoint.fingerprint; config = $Checkpoint.config; suites = $Checkpoint.suites
                        started = $Checkpoint.started; broken = $Checkpoint.broken; passed = $Checkpoint.passed.ToArray() }
    Write-AtomicText $Path (($body | ConvertTo-Json -Depth 6) + "`n")
}

function Read-RunCheckpoint([string]$Path) {
    $result = [pscustomobject]@{ Checkpoint = $null; Problem = '' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { $result.Problem = "there is no checkpoint in $Path (run .\tools\test.ps1 first)"; return $result }
    try { $json = [IO.File]::ReadAllText($Path) | ConvertFrom-Json } catch { $result.Problem = 'the checkpoint is not readable JSON'; return $result }
    $fieldsOk = $json -is [System.Management.Automation.PSCustomObject] -and (Get-Field $json 'schema') -eq $script:CheckpointSchema
    foreach ($name in 'fingerprint', 'config', 'suites') { if (-not ((Get-Field $json $name) -is [string])) { $fieldsOk = $false } }
    if ($fieldsOk -and $json.PSObject.Properties['passed']) {
        foreach ($entry in @($json.passed)) { if (-not ((Get-Field $entry 'name') -is [string]) -or (Get-Field $entry 'status') -ne 'PASS') { $fieldsOk = $false } }
    } else { $fieldsOk = $false }
    if (-not $fieldsOk) { $result.Problem = 'the checkpoint is incomplete or of another schema'; return $result }
    $result.Checkpoint = $json
    return $result
}

# The suites a -Resume may leave out: those the checkpoint passed, only under the same sources, parameters and suite list, and in
# the same order as the suites of this run.
function Test-RunCheckpoint($Read, [string]$Fingerprint, [string]$Config, [string]$Suites, [string[]]$Names) {
    $reasons = New-Object System.Collections.Generic.List[string]
    $passed = @()
    if ($Read.Problem) { $reasons.Add($Read.Problem) }
    else {
        $checkpoint = $Read.Checkpoint
        $passed = @($checkpoint.passed)
        if ($checkpoint.fingerprint -ne $Fingerprint) { $reasons.Add('the sources changed since the checkpoint (git HEAD, tracked changes, untracked files or game data)') }
        if ($checkpoint.config -ne $Config) { $reasons.Add("the parameters changed: the checkpoint ran with [$($checkpoint.config)], this run is [$Config]") }
        if ($checkpoint.suites -ne $Suites) { $reasons.Add('the suite list changed since the checkpoint') }
        if (-not $reasons.Count) {
            for ($i = 0; $i -lt $passed.Count; $i++) {
                if ($i -ge @($Names).Count -or $Names[$i] -ne $passed[$i].name) { $reasons.Add('the checkpoint does not follow the order of the suites'); break }
            }
        }
    }
    if ($reasons.Count) { $passed = @() }
    return [pscustomobject]@{ Ok = ($reasons.Count -eq 0); Reasons = $reasons.ToArray(); Passed = $passed }
}
