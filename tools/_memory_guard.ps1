# Memory safety of tools\test.ps1. The physical RAM of the gate's whole process tree is sampled about every 0.75 s: the running stage
# is stopped (only its own process tree) when the tree passes the hard limit; the system's free RAM only holds back the start of a
# stage and is otherwise reported. Works in Windows PowerShell 5.1 and PowerShell 7.

$script:Guard = $null

$script:MemoryProbeSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Jpt
{
    public sealed class ProcessEntry
    {
        public int Id;
        public int ParentId;
        public string Name;
        public long Created;
        public long Bytes;
    }

    public static class MemoryProbe
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct ProcessEntry32
        {
            public uint Size; public uint Usage; public uint ProcessId; public IntPtr DefaultHeapId; public uint ModuleId;
            public uint Threads; public uint ParentProcessId; public int PriorityBase; public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatusEx
        {
            public uint Length; public uint MemoryLoad; public ulong TotalPhys; public ulong AvailPhys; public ulong TotalPageFile;
            public ulong AvailPageFile; public ulong TotalVirtual; public ulong AvailVirtual; public ulong AvailExtendedVirtual;
        }

        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snapshot, ref ProcessEntry32 entry);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snapshot, ref ProcessEntry32 entry);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
        [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx status);

        public static ProcessEntry[] Processes()
        {
            var bytes = new Dictionary<int, long>();
            foreach (var process in Process.GetProcesses())
            {
                try { bytes[process.Id] = process.WorkingSet64; } catch { }
                process.Dispose();
            }
            var list = new List<ProcessEntry>();
            IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);
            if (snapshot == new IntPtr(-1)) return list.ToArray();
            try
            {
                var entry = new ProcessEntry32();
                entry.Size = (uint)Marshal.SizeOf(typeof(ProcessEntry32));
                for (bool more = Process32FirstW(snapshot, ref entry); more; more = Process32NextW(snapshot, ref entry))
                {
                    long working;
                    bytes.TryGetValue((int)entry.ProcessId, out working);
                    list.Add(new ProcessEntry { Id = (int)entry.ProcessId, ParentId = (int)entry.ParentProcessId, Name = entry.ExeFile, Created = CreationTime(entry.ProcessId), Bytes = working });
                }
            }
            finally { CloseHandle(snapshot); }
            return list.ToArray();
        }

        static long CreationTime(uint processId)
        {
            IntPtr process = OpenProcess(0x1000, false, processId);
            if (process == IntPtr.Zero) return 0;
            try
            {
                long creation, exit, kernel, user;
                return GetProcessTimes(process, out creation, out exit, out kernel, out user) ? creation : 0;
            }
            finally { CloseHandle(process); }
        }

        public static ulong[] Physical()
        {
            var status = new MemoryStatusEx();
            status.Length = (uint)Marshal.SizeOf(typeof(MemoryStatusEx));
            if (!GlobalMemoryStatusEx(ref status)) return null;
            return new[] { status.TotalPhys, status.AvailPhys, status.TotalPageFile, status.AvailPageFile };
        }
    }
}
'@

function Initialize-MemoryProbe {
    if ('Jpt.MemoryProbe' -as [type]) { return $true }
    try { Add-Type -TypeDefinition $script:MemoryProbeSource -Language CSharp -ErrorAction Stop; return $true } catch { return $false }
}

function Format-GBValue([double]$GB) { [string]::Format([cultureinfo]::InvariantCulture, '{0:0.0#} GB', $GB) }
function Format-GB($Bytes) { Format-GBValue ([double]$Bytes / 1GB) }

function Get-RoundedGB([double]$Value) { [Math]::Round($Value, 1, [MidpointRounding]::AwayFromZero) }

# What a stage of each kind is expected to add to the RAM in use (one compiler, one suite): a forecast for diagnostics and the
# history, and the bar a second stage must clear beside a running one. A serial stage never waits for it.
$script:StageEstimatesGB = [ordered]@{ light = 0.25; normal = 0.75; heavy = 1.5; build = 2.0; wsl = 2.0; sanitize = 2.5 }

# The gate's own tree is the protection: soft 25% (no second stage beside a running one) and hard 30% of the RAM (the stage is
# stopped; 20% and 25% under sanitizers). Other applications using the RAM never stop a running stage: at the start floor no new
# stage starts, and under the warning level the free RAM is only reported.
function Get-MemoryLimits([double]$TotalGB, [bool]$Sanitize = $false) {
    $tier = if ($TotalGB -gt 0) { $TotalGB } else { 16.0 }
    [pscustomobject]@{
        TotalGB = $TotalGB
        SoftGB = Get-RoundedGB ($tier * $(if ($Sanitize) { 0.20 } else { 0.25 }))
        HardGB = Get-RoundedGB ($tier * $(if ($Sanitize) { 0.25 } else { 0.30 }))
        EmergencyGB = 0.5
        WarningGB = 2.0
        Estimates = $script:StageEstimatesGB
        EmergencyWaitSeconds = 30
    }
}

function Get-StageEstimateGB($Limits, [string]$Class, [double]$EstimateGB = 0) {
    if ($EstimateGB -gt 0) { return $EstimateGB }
    if ($Limits.Estimates.Contains($Class)) { return [double]$Limits.Estimates[$Class] }
    return [double]$Limits.Estimates['normal']
}

# START or WAIT. A stage alone starts whenever the free RAM is above the start floor and the tree under its hard limit; a
# stage beside a running one also needs its estimate free above the floor and the tree under the soft limit.
function Get-MemoryAdmission($Limits, [double]$GateGB, $FreeGB, [string]$Class = 'normal', [int]$Running = 0, [double]$EstimateGB = 0) {
    $free = if ($null -eq $FreeGB) { [double]::MaxValue } else { [double]$FreeGB }
    if ($Running -gt 0) {
        if ($GateGB -lt $Limits.SoftGB -and $free -gt $Limits.EmergencyGB + (Get-StageEstimateGB $Limits $Class $EstimateGB)) { return 'START' }
        return 'WAIT'
    }
    if ($GateGB -gt $Limits.HardGB -or $free -le $Limits.EmergencyGB) { return 'WAIT' }
    return 'START'
}

# '' when a background suite may start beside the running ones, else why not.
function Get-MemoryStartBlocker($Limits, [double]$GateGB, $FreeGB, [string]$Class = 'normal', [int]$Running = 0, [double]$EstimateGB = 0) {
    if ((Get-MemoryAdmission $Limits $GateGB $FreeGB $Class $Running $EstimateGB) -eq 'START') { return '' }
    return 'memory'
}

function Get-MemoryAbortReason($Limits, [double]$GateGB) {
    if ($GateGB -gt $Limits.HardGB) { return 'MEMORY LIMIT EXCEEDED' }
    return ''
}

function Get-MemorySample {
    if ('Jpt.MemoryProbe' -as [type]) {
        $physical = [Jpt.MemoryProbe]::Physical()
        return [pscustomobject]@{ TotalGB = [double]$physical[0] / 1GB; FreeGB = [double]$physical[1] / 1GB; CommitFreeGB = [double]$physical[3] / 1GB
                                  Processes = [Jpt.MemoryProbe]::Processes() }
    }
    $os = Get-CimInstance Win32_OperatingSystem -Property TotalVisibleMemorySize, FreePhysicalMemory, FreeVirtualMemory -Verbose:$false
    $processes = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name, WorkingSetSize, CreationDate -Verbose:$false | ForEach-Object {
            [pscustomobject]@{ Id = [int]$_.ProcessId; ParentId = [int]$_.ParentProcessId; Name = [string]$_.Name; Bytes = [long]$_.WorkingSetSize
                               Created = $(if ($_.CreationDate) { $_.CreationDate.ToFileTime() } else { 0L }) } })
    return [pscustomobject]@{ TotalGB = [double]$os.TotalVisibleMemorySize / 1MB; FreeGB = [double]$os.FreePhysicalMemory / 1MB
                              CommitFreeGB = [double]$os.FreeVirtualMemory / 1MB; Processes = $processes }
}

function Get-GuardSample {
    $sample = Get-MemorySample
    if ($script:Guard -and $null -ne $script:Guard.SimulatedFreeGB) { $sample.FreeGB = $script:Guard.SimulatedFreeGB }
    return $sample
}

# Only a gate_tests fixture repository (under the temporary folder, holding .jpt-gate-fixture) may simulate the free RAM or shorten
# the wait, so its runs do not depend on the machine; the project repository never can.
function Get-FixtureNumber([string]$Root, [string]$Name) {
    $value = 0.0
    $text = [Environment]::GetEnvironmentVariable($Name)
    if (-not $text -or -not [double]::TryParse($text, [Globalization.NumberStyles]::Float, [cultureinfo]::InvariantCulture, [ref]$value)) { return $null }
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $full = [IO.Path]::GetFullPath($Root)
    if (-not $full.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath (Join-Path $full '.jpt-gate-fixture') -PathType Leaf)) { return $null }
    return $value
}

function Get-ProcessKey($Entry) { '{0}:{1}' -f $Entry.Id, $Entry.Created }

# The root and every descendant, each with its stage: the registered process (StageRoots: PID -> handle) it descends from, or 0.
# A child created before its parent is a reused PID, not a descendant. Known keeps processes whose parent already ended.
function Measure-GateTree($Processes, [int]$RootPid, [bool]$IncludeRoot, [hashtable]$StageRoots, [hashtable]$Known) {
    $byId = @{}
    $children = @{}
    $wslClients = New-Object System.Collections.Generic.List[int]
    $vmmem = $null
    foreach ($entry in $Processes) {
        $byId[$entry.Id] = $entry
        if ($entry.Name -eq 'wsl.exe') { $wslClients.Add($entry.Id) }
        elseif (($entry.Name -eq 'vmmemWSL' -or $entry.Name -eq 'vmmem') -and (-not $vmmem -or $entry.Bytes -gt $vmmem.Bytes)) { $vmmem = $entry }
        if ($entry.Id -eq $entry.ParentId) { continue }
        if (-not $children.ContainsKey($entry.ParentId)) { $children[$entry.ParentId] = New-Object System.Collections.Generic.List[object] }
        $children[$entry.ParentId].Add($entry)
    }
    $stageOf = @{}
    $queue = New-Object System.Collections.Generic.Queue[object]
    if ($byId.ContainsKey($RootPid)) { $stageOf[$RootPid] = 0; $queue.Enqueue($byId[$RootPid]) }
    for ($pass = 0; $pass -lt 2; $pass++) {
        while ($queue.Count) {
            $node = $queue.Dequeue()
            if (-not $children.ContainsKey($node.Id)) { continue }
            foreach ($child in $children[$node.Id]) {
                if ($stageOf.ContainsKey($child.Id)) { continue }
                if ($child.Created -gt 0 -and $node.Created -gt 0 -and $child.Created -lt $node.Created) { continue }
                $stageOf[$child.Id] = if ($StageRoots.ContainsKey($child.Id)) { $child.Id } else { $stageOf[$node.Id] }
                $queue.Enqueue($child)
            }
        }
        foreach ($entry in $Processes) {
            if ($stageOf.ContainsKey($entry.Id)) { continue }
            $key = Get-ProcessKey $entry
            if ($Known.ContainsKey($key)) { $stageOf[$entry.Id] = $Known[$key]; $queue.Enqueue($entry) }
        }
    }
    $Known.Clear()
    $members = New-Object System.Collections.Generic.List[object]
    $stageBytes = @{}
    $total = 0L
    foreach ($id in $stageOf.Keys) {
        $entry = $byId[$id]
        if ($id -eq $RootPid -and -not $IncludeRoot) { continue }
        if ($id -ne $RootPid) { $Known[(Get-ProcessKey $entry)] = $stageOf[$id] }
        $stage = $stageOf[$id]
        $members.Add([pscustomobject]@{ Id = $entry.Id; Name = $entry.Name; Bytes = [long]$entry.Bytes; Created = $entry.Created; Stage = $stage })
        if (-not $stageBytes.ContainsKey($stage)) { $stageBytes[$stage] = 0L }
        $stageBytes[$stage] += [long]$entry.Bytes
        $total += [long]$entry.Bytes
    }
    $outsideWsl = @($wslClients | Where-Object { -not $stageOf.ContainsKey($_) }).Count
    return [pscustomobject]@{ Bytes = $total; Members = $members; StageBytes = $stageBytes; OutsideWsl = $outsideWsl; Vmmem = $vmmem }
}

# The WSL VM is no process of the tree: its growth since a stage with wsl.exe started is counted for that stage, and only while
# no other wsl.exe runs on the machine (another WSL user would make the growth not ours).
function Add-WslGrowth($G, $Tree) {
    $wslStages = @($Tree.Members | Where-Object { $_.Name -eq 'wsl.exe' -and $_.Stage -ne 0 } | ForEach-Object { $_.Stage } | Sort-Object -Unique)
    foreach ($stage in @($G.WslBaseline.Keys)) { if ($wslStages -notcontains $stage) { $G.WslBaseline.Remove($stage) } }
    if (-not $wslStages.Count) { return }
    $now = if ($Tree.Vmmem) { [long]$Tree.Vmmem.Bytes } else { 0L }
    foreach ($stage in $wslStages) { if (-not $G.WslBaseline.ContainsKey($stage)) { $G.WslBaseline[$stage] = $now } }
    if ($Tree.OutsideWsl -gt 0) { $G.WslShared = $true; return }
    $stage = $wslStages[0]
    $growth = [Math]::Max(0L, $now - [long]$G.WslBaseline[$stage])
    if ($growth -le 0) { return }
    $Tree.Members.Add([pscustomobject]@{ Id = $(if ($Tree.Vmmem) { $Tree.Vmmem.Id } else { 0 }); Name = 'WSL VM growth'; Bytes = $growth; Created = 0; Stage = $stage; Virtual = $true })
    if (-not $Tree.StageBytes.ContainsKey($stage)) { $Tree.StageBytes[$stage] = 0L }
    $Tree.StageBytes[$stage] += $growth
    $Tree.Bytes += $growth
}

function New-MemoryGuard($Limits, [int]$RootPid = $PID, [bool]$IncludeRoot = $true) {
    $guard = [pscustomobject]@{
        Limits = $Limits; RootPid = $RootPid; IncludeRoot = $IncludeRoot; Native = (Initialize-MemoryProbe)
        Sampler = { Get-GuardSample }; Kill = { param($Guard, $Stage, $Members, $Handle) Stop-StageProcesses $Guard $Stage $Members $Handle }
        Sleep = { param($Ms) Start-Sleep -Milliseconds $Ms }; OnAbort = { param($Lines) }; OnWait = { param($Text) }; OnWarn = { param($Text) }; Tick = { }
        Interval = 0.75; LastAt = -10.0; Clock = [System.Diagnostics.Stopwatch]::StartNew(); Busy = $false
        Known = @{}; WslBaseline = @{}; WslShared = $false; Samples = 0
        GateBytes = 0L; PeakGateBytes = 0L; FreeGB = $null; LowestFreeGB = $null; CommitFreeGB = $null; TotalGB = $Limits.TotalGB
        SimulatedFreeGB = $null; Largest = $null; PeakProcess = $null; Aborts = (New-Object System.Collections.Generic.List[object]); Waits = 0; Warnings = 0; WarnLevel = 0; Stopped = ''
        Starts = 0; LowStarts = (New-Object System.Collections.Generic.List[string])
    }
    return $guard
}

function Get-LiveStages {
    $roots = @{}
    foreach ($handle in $script:GateProcesses.ToArray()) {
        try { if (-not $handle.Process.HasExited) { $roots[[int]$handle.Process.Id] = $handle } } catch { $null = $_ }
    }
    return $roots
}

function Update-MemoryStats($G, $Tree, $Sample) {
    $G.Samples++
    $G.GateBytes = $Tree.Bytes
    $G.PeakGateBytes = [Math]::Max($G.PeakGateBytes, $Tree.Bytes)
    $G.FreeGB = $Sample.FreeGB
    $G.CommitFreeGB = $Sample.CommitFreeGB
    if ($null -eq $G.LowestFreeGB -or $Sample.FreeGB -lt $G.LowestFreeGB) { $G.LowestFreeGB = $Sample.FreeGB }
    $G.Largest = $Tree.Members | Sort-Object Bytes -Descending | Select-Object -First 1
    if ($G.Largest -and (-not $G.PeakProcess -or $G.Largest.Bytes -gt $G.PeakProcess.Bytes)) { $G.PeakProcess = $G.Largest }
}

# Samples the tree (at most once per Interval unless forced), keeps each stage's peak, warns as the free RAM falls and stops the
# stage that holds the most memory when the tree passes the hard limit.
function Invoke-MemoryWatch([switch]$Force) {
    $G = $script:Guard
    if (-not $G -or $G.Busy) { return }
    $now = $G.Clock.Elapsed.TotalSeconds
    if (-not $Force -and $now - $G.LastAt -lt $G.Interval) { return }
    $G.LastAt = $now
    $G.Busy = $true
    try {
        $sample = & $G.Sampler
        if (-not $sample) { return }
        $roots = Get-LiveStages
        $tree = Measure-GateTree $sample.Processes $G.RootPid $G.IncludeRoot $roots $G.Known
        Add-WslGrowth $G $tree
        Update-MemoryStats $G $tree $sample
        foreach ($stage in @($tree.StageBytes.Keys)) {
            $handle = if ($roots.ContainsKey($stage)) { $roots[$stage] } else { $null }
            if ($handle -and $handle.PSObject.Properties['PeakBytes'] -and $tree.StageBytes[$stage] -gt $handle.PeakBytes) { $handle.PeakBytes = [long]$tree.StageBytes[$stage] }
        }
        Update-MemoryWarning $G $sample.FreeGB $roots
        $reason = Get-MemoryAbortReason $G.Limits ($tree.Bytes / 1GB)
        if (-not $reason) { return }
        $stage = @($tree.StageBytes.Keys | Where-Object { $_ -ne 0 } | Sort-Object { $tree.StageBytes[$_] } -Descending) | Select-Object -First 1
        if ($stage) { Stop-MemoryStage $G ([int]$stage) $reason $tree $sample $roots }
        else { $G.Stopped = "the gate's own process passed the hard limit ($(Format-GB $tree.Bytes) of $(Format-GBValue $G.Limits.HardGB))" }
    } finally { $G.Busy = $false }
}

function Get-FreeMemoryLevel($Limits, $FreeGB) {
    if ($null -eq $FreeGB -or $FreeGB -ge $Limits.WarningGB) { return 0 }
    if ($FreeGB -gt $Limits.EmergencyGB) { return 1 }
    return 2
}

# Once for each level the free RAM falls to while a stage runs: LOW under the warning level, CRITICAL at the start floor. The stage
# keeps running either way.
function Update-MemoryWarning($G, $FreeGB, [hashtable]$Roots) {
    $level = Get-FreeMemoryLevel $G.Limits $FreeGB
    if ($level -lt $G.WarnLevel) { $G.WarnLevel = $level }
    if ($level -le $G.WarnLevel -or -not $Roots.Count) { return }
    $G.WarnLevel = $level
    $G.Warnings++
    $labels = (@($Roots.Values) | ForEach-Object { Get-HandleLabel $_ }) -join ', '
    $head = if ($level -eq 2) { 'CRITICAL SYSTEM MEMORY: {0} free, at or under the start floor {4}' } else { 'LOW SYSTEM MEMORY: {0} free' }
    $null = & $G.OnWarn (($head + '; {1} keeps running (the gate''s processes use {2}, hard limit {3})') -f (Format-GBValue $FreeGB), $labels, (Format-GB $G.GateBytes), (Format-GBValue $G.Limits.HardGB), (Format-GBValue $G.Limits.EmergencyGB))
}

function Get-HandleLabel($Handle) {
    if (-not $Handle) { return 'processes left behind by an earlier stage' }
    if ($Handle.PSObject.Properties['Label'] -and $Handle.Label) { return $Handle.Label }
    return $Handle.Command
}

function Stop-MemoryStage($G, [int]$Stage, [string]$Reason, $Tree, $Sample, [hashtable]$Roots) {
    $members = @($Tree.Members | Where-Object { $_.Stage -eq $Stage } | Sort-Object Bytes -Descending)
    $handle = if ($Roots.ContainsKey($Stage)) { $Roots[$Stage] } else { $null }
    $label = Get-HandleLabel $handle
    $largest = $members | Select-Object -First 1
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("${Reason}: $label was stopped")
    $lines.Add(('  gate processes   {0} (soft {1}, hard {2})' -f (Format-GB $Tree.Bytes), (Format-GBValue $G.Limits.SoftGB), (Format-GBValue $G.Limits.HardGB)))
    $lines.Add(('  system free      {0} (warning below {1}, start floor {2})' -f (Format-GBValue $Sample.FreeGB), (Format-GBValue $G.Limits.WarningGB), (Format-GBValue $G.Limits.EmergencyGB)))
    $lines.Add(('  stage            {0}, PID {1}, {2} in {3} process(es)' -f $label, $Stage, (Format-GB $Tree.StageBytes[$Stage]), $members.Count))
    if ($largest) { $lines.Add(('  largest          {0}, PID {1}, {2}' -f $largest.Name, $largest.Id, (Format-GB $largest.Bytes))) }
    $lines.Add('  processes        ' + ((@($members | Select-Object -First 6) | ForEach-Object { '{0} {1} (PID {2})' -f $_.Name, (Format-GB $_.Bytes), $_.Id }) -join ', '))
    $survivors = @(& $G.Kill $G $Stage @($members | Where-Object { -not $_.PSObject.Properties['Virtual'] }) $handle)
    if ($survivors.Count) { $lines.Add('  STILL RUNNING    ' + (($survivors | ForEach-Object { '{0} (PID {1})' -f $_.Name, $_.Id }) -join ', ')) }
    else { $lines.Add('  stopped          every process of the stage ended; nothing outside the gate was touched') }
    if ($handle) { $handle | Add-Member -NotePropertyName Abort -NotePropertyValue $lines.ToArray() -Force }
    $G.Aborts.Add([pscustomobject]@{ Reason = $Reason; Label = $label; Lines = $lines.ToArray() })
    & $G.OnAbort $lines.ToArray()
}

# Kills the stage's registered tree and then each process recorded for it (same PID and creation time), and returns those
# still alive after five seconds.
function Stop-StageProcesses($G, [int]$Stage, $Members, $Handle) {
    if ($Handle) {
        try { if (-not $Handle.Process.HasExited) { & taskkill.exe /PID $Stage /T /F 2>&1 | Out-Null } } catch { $null = $_ }
        Stop-LinuxSide $Handle
    }
    foreach ($member in @($Members)) {
        try {
            $process = [System.Diagnostics.Process]::GetProcessById($member.Id)
            if ($member.Created -le 0 -or $process.StartTime.ToFileTime() -eq $member.Created) { $process.Kill() }
            $process.Dispose()
        } catch { $null = $_ }
    }
    $keys = @{}
    foreach ($member in @($Members)) { $keys[(Get-ProcessKey $member)] = $member }
    for ($i = 0; $i -lt 20; $i++) {
        $alive = @((& $G.Sampler).Processes | Where-Object { $keys.ContainsKey((Get-ProcessKey $_)) })
        if (-not $alive.Count) { return @() }
        Start-Sleep -Milliseconds 250
    }
    return $alive
}

# Holds a stage only while it cannot start at all, checking every second: the free RAM at the start floor or the tree over its
# hard limit, or (beside a running stage) no room for a second one. With nothing of the gate running that lasts at most
# EmergencyWaitSeconds before the gate stops. Under the warning level a stage still starts, with a LOW SYSTEM MEMORY line.
function Wait-MemoryRoom([string]$Label, [string]$Class = 'normal', [double]$EstimateGB = 0) {
    $G = $script:Guard
    if (-not $G) { return 'START' }
    if ($G.Stopped) { throw $G.Stopped }
    $waited = 0.0
    $told = $false
    while ($true) {
        [void](Invoke-MemoryWatch -Force)
        if ($G.Stopped) { throw $G.Stopped }
        $running = (Get-LiveStages).Count
        if ((Get-MemoryAdmission $G.Limits ($G.GateBytes / 1GB) $G.FreeGB $Class $running $EstimateGB) -eq 'START') {
            $G.Starts++
            if ($null -ne $G.FreeGB -and $G.FreeGB -lt $G.Limits.WarningGB) {
                $G.LowStarts.Add($Label)
                $G.WarnLevel = [Math]::Max($G.WarnLevel, 1)
                $G.Warnings++
                $null = & $G.OnWarn ('LOW SYSTEM MEMORY: {0} free; {1} starts anyway (the gate''s processes use {2}, hard limit {3}, start floor {4})' -f (Format-GBValue $G.FreeGB), $Label, (Format-GB $G.GateBytes), (Format-GBValue $G.Limits.HardGB), (Format-GBValue $G.Limits.EmergencyGB))
            }
            return 'START'
        }
        $why = if ($running) { 'free {0}, the gate''s processes use {1} beside a running stage (soft limit {2})' -f (Format-GBValue $G.FreeGB), (Format-GB $G.GateBytes), (Format-GBValue $G.Limits.SoftGB) }
               elseif ($G.GateBytes / 1GB -gt $G.Limits.HardGB) { 'the gate''s own processes use {0}, over the hard limit {1}' -f (Format-GB $G.GateBytes), (Format-GBValue $G.Limits.HardGB) }
               else { 'free {0}, at or under the start floor {1}' -f (Format-GBValue $G.FreeGB), (Format-GBValue $G.Limits.EmergencyGB) }
        if (-not $told) {
            $told = $true
            $G.Waits++
            $null = & $G.OnWait "waiting before ${Label}: $why; checking every second"
        }
        if ($running) { $waited = 0.0 } elseif ($waited -ge $G.Limits.EmergencyWaitSeconds) {
            $G.Stopped = "EMERGENCY SYSTEM MEMORY TOO LOW TO START NEXT STAGE: ${Label}: for $($G.Limits.EmergencyWaitSeconds) s $why"
            throw $G.Stopped
        }
        $null = & $G.Sleep 1000
        $waited += 1.0
        $null = & $G.Tick
    }
}

function Format-MemorySafety($Limits, [int]$TestJobs, [int]$BuildJobs, [bool]$Native, $FreeGB) {
    $row = { param($Name, $Value) '  {0,-19}{1}' -f $Name, $Value }
    @(
        (& $row 'Physical RAM' (Format-GBValue $Limits.TotalGB)),
        (& $row 'System free' $(if ($null -ne $FreeGB) { Format-GBValue $FreeGB } else { 'unknown' })),
        (& $row 'Warning below' "$(Format-GBValue $Limits.WarningGB)  LOW SYSTEM MEMORY is reported (CRITICAL at the start floor); a running stage goes on"),
        (& $row 'Start floor' "$(Format-GBValue $Limits.EmergencyGB)  at or under it no new stage starts"),
        (& $row 'Gate soft limit' "$(Format-GBValue $Limits.SoftGB)  no second stage beside a running one above it"),
        (& $row 'Gate hard limit' "$(Format-GBValue $Limits.HardGB)  the stage is stopped above it (MEMORY LIMIT EXCEEDED)"),
        (& $row 'Test jobs' "$TestJobs"),
        (& $row 'Build jobs' "$BuildJobs"),
        (& $row 'Suites' $(if ($TestJobs -gt 1) { "up to $TestJobs at once (background suites)" } else { 'serial' })),
        (& $row 'Linux' 'serial, after the Windows half'),
        (& $row 'Measured' "the process tree of PID $PID every 0.75 s ($(if ($Native) { 'Toolhelp snapshot' } else { 'CIM, slower' })); the WSL VM's growth while a WSL stage runs")
    )
}
