# Deploys the Bug Report Worker (server\trespasser-bug-reports) with the Wrangler pinned in its package-lock.json.
#   .\tools\worker.ps1            wrangler deploy
#   .\tools\worker.ps1 -DryRun    wrangler deploy --dry-run (bundles, uploads nothing)
# Needs Node.js and the Worker's dependencies (npm ci); never a global Wrangler or npx.
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("worker: $message")
    exit 1
}

function Read-Json([string]$path) {
    try { return Get-Content -Raw -LiteralPath $path | ConvertFrom-Json } catch { Fail "cannot read $path ($($_.Exception.Message))" }
}

function Get-MinimumNode([object]$package) {
    $range = if ($package.engines) { [string]$package.engines.node } else { '' }
    if ($range -match '^\s*>=\s*v?(\d+)(?:\.(\d+))?(?:\.(\d+))?\s*$') {
        return [version]::new([int]$Matches[1], [int]('0' + $Matches[2]), [int]('0' + $Matches[3]))
    }
    return $null
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workerDir = Join-Path $repoRoot 'server\trespasser-bug-reports'

if (-not (Test-Path -LiteralPath $workerDir -PathType Container)) { Fail "missing folder $workerDir" }
if (-not (Test-Path -LiteralPath (Join-Path $workerDir 'wrangler.toml') -PathType Leaf)) {
    Fail "missing $workerDir\wrangler.toml, the Wrangler configuration"
}
$configText = Get-Content -Raw -LiteralPath (Join-Path $workerDir 'wrangler.toml')
if ($configText -match '(?m)^\s*main\s*=\s*"([^"]+)"') {
    $entry = Join-Path $workerDir $Matches[1]
    if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) { Fail "missing $entry, the entry point named by wrangler.toml" }
}
$packagePath = Join-Path $workerDir 'package.json'
$lockPath = Join-Path $workerDir 'package-lock.json'
if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf)) { Fail "missing $packagePath" }
if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) { Fail "missing $lockPath, which pins the Wrangler version" }

# The lockfile is matched as text: ConvertFrom-Json rejects its empty "" package key.
$lockText = Get-Content -Raw -LiteralPath $lockPath
if ($lockText -notmatch '"node_modules/wrangler"\s*:\s*\{\s*"version"\s*:\s*"([^"]+)"') { Fail "$lockPath does not pin wrangler" }
$lockedVersion = $Matches[1]
$installCommand = "npm ci --prefix `"$workerDir`""

$node = Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $node) { Fail 'Node.js not found; install Node.js 22.12 or newer (https://nodejs.org) and open a new terminal' }
$nodeText = (& $node.Source --version 2>$null | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $nodeText -notmatch '^v?(\d+)\.(\d+)\.(\d+)') { Fail "cannot read the Node.js version from $($node.Source)" }
$nodeVersion = [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])

$wranglerDir = Join-Path $workerDir 'node_modules\wrangler'
$wranglerPackagePath = Join-Path $wranglerDir 'package.json'
$wranglerJs = Join-Path $wranglerDir 'bin\wrangler.js'
if (-not (Test-Path -LiteralPath $wranglerPackagePath -PathType Leaf) -or -not (Test-Path -LiteralPath $wranglerJs -PathType Leaf)) {
    if (-not (Get-Command npm -CommandType Application -ErrorAction SilentlyContinue)) {
        Fail "the Worker's dependencies are not installed and npm was not found; reinstall Node.js with npm, then run: $installCommand"
    }
    Fail "the Worker's dependencies are not installed (local Wrangler $lockedVersion missing); run: $installCommand"
}
$wranglerPackage = Read-Json $wranglerPackagePath
if ([string]$wranglerPackage.version -ne $lockedVersion) {
    Fail "local Wrangler is $($wranglerPackage.version) but package-lock.json pins $lockedVersion; run: $installCommand"
}

foreach ($required in @((Get-MinimumNode (Read-Json $packagePath)), (Get-MinimumNode $wranglerPackage))) {
    if ($required -and $nodeVersion -lt $required) {
        Fail "Node.js $nodeVersion is too old for Wrangler $lockedVersion; install Node.js $required or newer"
    }
}

$arguments = @($wranglerJs, 'deploy')
if ($DryRun) { $arguments += '--dry-run' }

Push-Location -LiteralPath $workerDir
try {
    & $node.Source @arguments
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) {
    [Console]::Error.WriteLine("worker: wrangler exited with code $code")
    exit $code
}
Write-Host ($(if ($DryRun) { 'Worker bundle checked (dry run, nothing uploaded).' } else { 'Worker deployed.' }))
