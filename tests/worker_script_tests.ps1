# Static and stand-in checks of tools\worker.ps1 using a fake npx shim. Nothing reaches Cloudflare or the network.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shell = (Get-Process -Id $PID).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "jpt-worker-script-tests-$PID"
$failures = 0
$checks = 0

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function New-Tree([string]$Name) {
    $tree = Join-Path $temp $Name
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $tree 'tools')
    Copy-Item -LiteralPath (Join-Path $root 'tools\worker.ps1') -Destination (Join-Path $tree 'tools')
    $worker = Join-Path $tree 'server\trespasser-bug-reports'
    $null = New-Item -ItemType Directory -Force -Path $worker
    foreach ($name in 'src', 'package.json', 'wrangler.toml') {
        Copy-Item -Recurse -LiteralPath (Join-Path $root "server\trespasser-bug-reports\$name") -Destination $worker
    }
    return $tree
}

function Invoke-Worker([string]$Tree, [string[]]$Arguments = @(), [int]$NpxCode = 0) {
    $log = Join-Path $Tree 'npx.log'
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $env:JPT_FAKE_NPX_LOG = $log
    $env:JPT_FAKE_NPX_CODE = "$NpxCode"
    $outFile = Join-Path $Tree 'out.txt'
    $errFile = Join-Path $Tree 'err.txt'
    $process = Start-Process -FilePath $shell -ArgumentList (@('-NoProfile', '-NonInteractive', '-File', (Join-Path $Tree 'tools\worker.ps1')) + $Arguments) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    [pscustomobject]@{
        Code = $process.ExitCode
        Out = [System.IO.File]::ReadAllText($outFile)
        Err = [System.IO.File]::ReadAllText($errFile)
        Npx = if (Test-Path -LiteralPath $log) { [System.IO.File]::ReadAllText($log).Trim() } else { $null }
    }
}

try {
    $shim = Join-Path $temp 'bin'
    $null = New-Item -ItemType Directory -Force -Path $shim
    Set-Content -Encoding ASCII -LiteralPath (Join-Path $shim 'npx.cmd') -Value @('@echo off', '>>"%JPT_FAKE_NPX_LOG%" echo %*', 'exit /b %JPT_FAKE_NPX_CODE%')
    $env:PATH = "$shim;$env:PATH"

    $dry = Invoke-Worker (New-Tree 'dry') @('-DryRun')
    Check ($dry.Code -eq 0 -and $dry.Npx -eq 'wrangler deploy --dry-run') 'dry run: invokes wrangler deploy --dry-run'
    Check ($dry.Out -match 'Worker bundle checked' -and $dry.Out -notmatch 'Worker deployed') 'dry run: says bundle checked'

    $real = Invoke-Worker (New-Tree 'real')
    Check ($real.Code -eq 0 -and $real.Npx -eq 'wrangler deploy') 'deploy: invokes wrangler deploy'
    Check ($real.Out -match 'Worker deployed' -and $real.Out -notmatch 'nothing uploaded') 'deploy: says Worker deployed'

    $broken = Invoke-Worker (New-Tree 'broken') @() 3
    Check ($broken.Code -eq 3 -and $broken.Out -notmatch 'Worker deployed' -and $broken.Err -match 'exited with code 3') 'propagates Wrangler exit code'

    $noEntry = New-Tree 'no-entry'
    Remove-Item -LiteralPath (Join-Path $noEntry 'server\trespasser-bug-reports\src\worker.js')
    Check ((Invoke-Worker $noEntry).Code -eq 1) 'missing entry point fails'

    $noConfig = New-Tree 'no-config'
    Remove-Item -LiteralPath (Join-Path $noConfig 'server\trespasser-bug-reports\wrangler.toml')
    Check ((Invoke-Worker $noConfig).Code -eq 1) 'missing wrangler.toml fails'

    $noFolder = Join-Path $temp 'no-folder'
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $noFolder 'tools')
    Copy-Item -LiteralPath (Join-Path $root 'tools\worker.ps1') -Destination (Join-Path $noFolder 'tools')
    Check ((Invoke-Worker $noFolder).Code -eq 1) 'missing worker folder fails'
} finally {
    Remove-Item Env:\JPT_FAKE_NPX_LOG, Env:\JPT_FAKE_NPX_CODE -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force -LiteralPath $temp -ErrorAction SilentlyContinue
}

Write-Host "worker script: $checks checks, $failures failed"
if ($failures) { exit 1 }
exit 0
