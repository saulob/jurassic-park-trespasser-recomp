# Shared helpers, dot-sourced by the other tools. Works in Windows PowerShell 5.1 and PowerShell 7.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:StampDir = Join-Path $script:RepoRoot 'analysis\stamps'
$script:RunLogPath = $null
$script:RunLogWriter = $null

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan; Add-RunLog "==> $Text" }
function Write-Ok([string]$Text)   { Write-Host "  [ok]   $Text" -ForegroundColor Green; Add-RunLog "  [ok]   $Text" }
function Write-Warn([string]$Text) { Write-Host "  [warn] $Text" -ForegroundColor Yellow; Add-RunLog "  [warn] $Text" }

function Stop-Tool([string]$Text, [int]$ExitCode = 1) {
    Write-Host "error: $Text" -ForegroundColor Red
    Add-RunLog "error: $Text"
    if ($script:RunLogPath) { Write-Host "Log: $($script:RunLogPath)" }
    Close-RunLog
    exit $ExitCode
}

# A PowerShell error in a tool script: the full record goes to the log, the terminal gets the essentials.
function Stop-OnError($Record) {
    $info = $Record.InvocationInfo
    $file = if ($info -and $info.ScriptName) { $info.ScriptName } else { 'unknown file' }
    $line = if ($info) { $info.ScriptLineNumber } else { 0 }
    $expression = if ($info) { "$($info.Line)".Trim() } else { '' }
    Add-RunLog 'PowerShell error'
    Add-RunLog "message    : $($Record.Exception.Message)"
    Add-RunLog "exception  : $($Record.Exception.GetType().FullName)"
    Add-RunLog "file       : $file"
    Add-RunLog "line       : $line"
    Add-RunLog "expression : $expression"
    if ($info) { Add-RunLog $info.PositionMessage }
    Add-RunLog 'stack trace:'
    Add-RunLog "$($Record.ScriptStackTrace)"
    Write-Host "error: $($Record.Exception.Message)" -ForegroundColor Red
    Write-Host "  at ${file}:${line}: $expression"
    Stop-Tool 'The script stopped on a PowerShell error.'
}

# One log per run of a tool: build\logs\trespasser_NNN.log, one past the highest number there (never
# reused). trespasser.exe numbers its own logs the same way when it is started without --log-file.
function Open-RunLog {
    $dir = Join-Path $script:RepoRoot 'build\logs'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $last = 0
    foreach ($file in @(Get-ChildItem -LiteralPath $dir -Filter 'trespasser_*.log')) {
        if ($file.Name -match '^trespasser_(\d+)\.log$') { $last = [Math]::Max($last, [int]$Matches[1]) }
    }
    for ($number = $last + 1; $number -lt $last + 1000; $number++) {
        $path = Join-Path $dir ('trespasser_{0:D3}.log' -f $number)
        try {
            $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        }
        catch [IO.IOException] { continue }
        $script:RunLogPath = $path
        $script:RunLogWriter = New-Object IO.StreamWriter($stream)
        $script:RunLogWriter.AutoFlush = $true
        Add-RunLog "started $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))"
        return
    }
    Write-Warn 'No free log number; this run is not logged.'
}

function Add-RunLog([string]$Line) {
    if ($script:RunLogWriter) { $script:RunLogWriter.WriteLine($Line) }
}

# The game appends to the same file while it runs, so the script closes its handle first and
# reopens the file for appending only after the game has exited: one writer at a time.
function Suspend-RunLog {
    if (-not $script:RunLogWriter) { return }
    $script:RunLogWriter.Dispose()
    $script:RunLogWriter = $null
}

function Resume-RunLog {
    if ($script:RunLogWriter -or -not $script:RunLogPath) { return }
    $stream = [IO.File]::Open($script:RunLogPath, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $script:RunLogWriter = New-Object IO.StreamWriter($stream)
    $script:RunLogWriter.AutoFlush = $true
}

function Close-RunLog {
    Resume-RunLog
    if (-not $script:RunLogWriter) { return }
    Add-RunLog "finished $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))"
    $script:RunLogWriter.Dispose()
    $script:RunLogWriter = $null
}

function Show-LogTail([int]$Lines = 40) {
    if (-not $script:RunLogPath) { return }
    Write-Host "--- last $Lines lines of $($script:RunLogPath) ---" -ForegroundColor DarkGray
    Get-Content -LiteralPath $script:RunLogPath -Tail $Lines | ForEach-Object { Write-Host $_ }
}

# Runs a native tool: every output line goes to the log, and only the lines $Show accepts reach the terminal.
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [scriptblock]$Show = { $true }) {
    Add-RunLog "> $Exe $($Arguments -join ' ')"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            $line = "$_"
            Add-RunLog $line
            if (& $Show $line) { Write-Host $line }
        }
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previous }
    Add-RunLog "exit code $code"
    return $code
}

# The Python that has the analysis modules. The py launcher first: a Python on PATH from MSYS2 usually has none.
function Get-Python {
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) { return @{ Exe = $launcher.Source; Args = @('-3') } }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) { return @{ Exe = $python.Source; Args = @() } }
    return $null
}

function Assert-PythonModules([string[]]$Modules) {
    $python = Get-Python
    if (-not $python) { Stop-Tool 'Python 3 not found. Install it from python.org (the py launcher is used).' }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $null = & $python.Exe @($python.Args) '-c' "import $($Modules -join ', ')" 2>&1
    $ok = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $previous
    if (-not $ok) { Stop-Tool "Python modules missing. Run: py -3 -m pip install $($Modules -join ' ')" }
}

function Invoke-Python([string]$Script, [string[]]$Arguments = @(), [scriptblock]$Show = { $true }) {
    $python = Get-Python
    $path = Join-Path $script:RepoRoot $Script
    return Invoke-Logged $python.Exe (@($python.Args) + @($path) + $Arguments) $Show
}

# Switches this session to the x64-hosted x86 MSVC toolchain. The 32-bit-hosted compiler runs out of memory on the lifted C.
# x86 uses the x64-hosted cross tools (amd64_x86), x64 the native ones (amd64).
function Enter-Msvc([ValidateSet('x86', 'x64')][string]$Arch = 'x86') {
    $vcvarsArch = if ($Arch -eq 'x64') { 'amd64' } else { 'amd64_x86' }
    $existing = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($existing -and $env:VSCMD_ARG_TGT_ARCH -eq $Arch) { return $existing.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { Stop-Tool 'vswhere.exe not found: install Visual Studio with "Desktop development with C++".' }
    $component = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    $vs = & $vswhere -latest -products * -requires $component -property installationPath | Select-Object -First 1
    if (-not $vs) { $vs = & $vswhere -latest -prerelease -products * -requires $component -property installationPath | Select-Object -First 1 }
    if (-not $vs) { Stop-Tool "No Visual Studio with the MSVC x86/x64 tools ($component) was found." }

    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { Stop-Tool "Missing $vcvars" }
    $environment = & cmd.exe /c "`"$vcvars`" $vcvarsArch >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { Stop-Tool "vcvarsall.bat $vcvarsArch failed: the MSVC $Arch tools or the Windows SDK are not installed." }
    foreach ($entry in $environment) {
        if ($entry -match '^([^=]+)=(.*)$') { Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2] }
    }
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl -or $env:VSCMD_ARG_TGT_ARCH -ne $Arch) { Stop-Tool "vcvarsall.bat $vcvarsArch did not provide an $Arch cl.exe." }

    # VS ships CMake and Ninja; they win over any copy from MSYS2 earlier on PATH.
    $cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"
    foreach ($tool in 'cmake.exe', 'ninja.exe') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Stop-Tool "$tool not found on PATH or in Visual Studio." }
    }
    return $cl.Source
}

function Enter-MsvcX86 { Enter-Msvc 'x86' }

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

# One digest for a stage's inputs: the content of each file plus any literal facts.
function Get-InputsDigest([string[]]$Files, [string[]]$Facts = @()) {
    $lines = @($Facts)
    foreach ($file in $Files) {
        $relative = $file.Substring($script:RepoRoot.Length).TrimStart('\')
        $lines += "$(Get-Sha256 $file) $relative"
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-StampPath([string]$Name) { Join-Path $script:StampDir "$Name.sha256" }

function Test-StampCurrent([string]$Name, [string]$Digest) {
    $path = Get-StampPath $Name
    return (Test-Path -LiteralPath $path) -and ((Get-Content -LiteralPath $path -Raw).Trim() -eq $Digest)
}

function Set-Stamp([string]$Name, [string]$Digest) {
    New-Item -ItemType Directory -Force -Path $script:StampDir | Out-Null
    [IO.File]::WriteAllText((Get-StampPath $Name), $Digest, [Text.Encoding]::ASCII)
}

function Remove-Stamp([string]$Name) {
    $path = Get-StampPath $Name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
}

function Get-CmakeCacheValue([string]$Cache, [string]$Name) {
    $match = Select-String -LiteralPath $Cache -Pattern "^$([regex]::Escape($Name))(:[A-Z]+)?=(.*)$" | Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[2].Value }
    return $null
}
