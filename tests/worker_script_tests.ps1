# tools\worker.ps1 against synthetic trees: a fake local Wrangler, stand-in node/npm, decoy global wrangler and npx.
# Nothing reaches Cloudflare, the npm registry or the real Wrangler.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shell = (Get-Process -Id $PID).Path
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temp = Join-Path $tempRoot "jpt-worker-script-tests-$PID"
$systemPath = "$env:SystemRoot\System32;$env:SystemRoot"
$failures = 0
$checks = 0

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function Remove-TestTemp {
    $full = [System.IO.Path]::GetFullPath($temp)
    if (-not $full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or $full.Length -le $tempRoot.Length -or
        $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to remove $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -Recurse -Force -LiteralPath $full }
}

function Write-Ascii([string]$Path, [string[]]$Lines) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    [System.IO.File]::WriteAllText($Path, (($Lines -join "`r`n") + "`r`n"), [System.Text.Encoding]::ASCII)
}

$lockText = [System.IO.File]::ReadAllText((Join-Path $root 'server\trespasser-bug-reports\package-lock.json'))
$null = $lockText -match '"node_modules/wrangler"\s*:\s*\{\s*"version"\s*:\s*"([^"]+)"'
$lockedVersion = $Matches[1]

function New-Tree([string]$Name, [string]$WranglerVersion = $lockedVersion, [switch]$NoWrangler) {
    $tree = Join-Path $temp $Name
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $tree 'tools')
    Copy-Item -LiteralPath (Join-Path $root 'tools\worker.ps1') -Destination (Join-Path $tree 'tools')
    $worker = Join-Path $tree 'server\trespasser-bug-reports'
    $null = New-Item -ItemType Directory -Force -Path $worker
    foreach ($name in 'src', 'package.json', 'package-lock.json', 'wrangler.toml') {
        Copy-Item -Recurse -LiteralPath (Join-Path $root "server\trespasser-bug-reports\$name") -Destination $worker
    }
    if (-not $NoWrangler) {
        $wrangler = Join-Path $worker 'node_modules\wrangler'
        Write-Ascii (Join-Path $wrangler 'package.json') @("{ `"name`": `"wrangler`", `"version`": `"$WranglerVersion`", `"engines`": { `"node`": `">=22.12.0`" } }")
        Write-Ascii (Join-Path $wrangler 'bin\wrangler.js') @(
            "const fs = require('fs');"
            "fs.writeFileSync(process.env.JPT_FAKE_WRANGLER_LOG, JSON.stringify({ argv: process.argv.slice(2), cwd: process.cwd() }), 'utf8');"
            "process.exit(Number(process.env.JPT_FAKE_WRANGLER_CODE || 0));")
    }
    return $tree
}

function Invoke-Worker([string]$Tree, [string]$PathValue, [string[]]$Arguments = @(), [int]$WranglerCode = 0, [string]$NodeVersion = 'v26.0.0') {
    $logs = Join-Path $temp 'logs'
    $null = New-Item -ItemType Directory -Force -Path $logs
    $wranglerLog = Join-Path $logs 'wrangler.json'
    $decoyLog = Join-Path $logs 'decoy.log'
    $outFile = Join-Path $logs 'out.txt'
    $errFile = Join-Path $logs 'err.txt'
    Remove-Item -LiteralPath $wranglerLog, $decoyLog -ErrorAction SilentlyContinue
    $env:JPT_FAKE_WRANGLER_LOG = $wranglerLog
    $env:JPT_FAKE_WRANGLER_CODE = "$WranglerCode"
    $env:JPT_FAKE_NODE_VERSION = $NodeVersion
    $env:JPT_DECOY_LOG = $decoyLog
    $savedPath = $env:PATH
    $env:PATH = $PathValue
    try {
        $argumentLine = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$(Join-Path $Tree 'tools\worker.ps1')`" $($Arguments -join ' ')"
        $process = Start-Process -FilePath $shell -ArgumentList $argumentLine -WorkingDirectory $temp `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    } finally {
        $env:PATH = $savedPath
    }
    [pscustomobject]@{
        Code = $process.ExitCode
        Out = [System.IO.File]::ReadAllText($outFile)
        Err = [System.IO.File]::ReadAllText($errFile)
        Wrangler = if (Test-Path -LiteralPath $wranglerLog) { [System.IO.File]::ReadAllText($wranglerLog) | ConvertFrom-Json } else { $null }
        DecoyUsed = Test-Path -LiteralPath $decoyLog
    }
}

function Test-Ran($Result, [string[]]$ExpectedArguments, [string]$Tree) {
    $Result.Wrangler -and (@($Result.Wrangler.argv) -join '|') -eq ($ExpectedArguments -join '|') -and
        $Result.Wrangler.cwd -eq (Join-Path $Tree 'server\trespasser-bug-reports')
}

try {
    Remove-TestTemp
    $decoy = Join-Path $temp 'decoy'
    foreach ($name in 'npx', 'wrangler') {
        Write-Ascii (Join-Path $decoy "$name.cmd") @('@echo off', ">>`"%JPT_DECOY_LOG%`" echo $name %*", 'exit /b 9')
    }
    $fakeNode = Join-Path $temp 'fake-node'
    Write-Ascii (Join-Path $fakeNode 'node.cmd') @('@echo off', 'if "%~1"=="--version" (echo %JPT_FAKE_NODE_VERSION%& exit /b 0)', 'exit /b %JPT_FAKE_WRANGLER_CODE%')
    $fakeNpm = Join-Path $temp 'fake-npm'
    Write-Ascii (Join-Path $fakeNpm 'npm.cmd') @('@echo off', 'exit /b 9')
    $withFakeNode = "$decoy;$fakeNode;$systemPath"

    $scriptText = [System.IO.File]::ReadAllText((Join-Path $root 'tools\worker.ps1'))
    Check ($scriptText -notmatch '(?m)^[^#]*\bnpx\b' -and $scriptText -notmatch '@latest') 'never runs npx or an unpinned Wrangler'
    $package = Get-Content -Raw -LiteralPath (Join-Path $root 'server\trespasser-bug-reports\package.json') | ConvertFrom-Json
    Check ($lockedVersion -match '^\d+\.\d+\.\d+$' -and [string]$package.devDependencies.wrangler -eq $lockedVersion) 'package.json and package-lock.json pin the same exact Wrangler'

    $realNode = Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($realNode) {
        $withRealNode = "$decoy;$(Split-Path -Parent $realNode.Source);$systemPath"

        $dryTree = New-Tree 'tree with spaces'
        $dry = Invoke-Worker $dryTree $withRealNode @('-DryRun')
        Check ($dry.Code -eq 0 -and (Test-Ran $dry @('deploy', '--dry-run') $dryTree)) 'dry run: local Wrangler gets deploy --dry-run in the Worker folder'
        Check ($dry.Out -match 'Worker bundle checked' -and $dry.Out -notmatch 'Worker deployed') 'dry run: says bundle checked'
        Check (-not $dry.DecoyUsed) 'dry run: no global wrangler, no npx'

        $realTree = New-Tree 'deploy'
        $real = Invoke-Worker $realTree $withRealNode
        Check ($real.Code -eq 0 -and (Test-Ran $real @('deploy') $realTree) -and -not $real.DecoyUsed) 'deploy: local Wrangler gets deploy'
        Check ($real.Out -match 'Worker deployed' -and $real.Out -notmatch 'nothing uploaded') 'deploy: says Worker deployed'

        $unicodeTree = New-Tree "unicod$([char]0x00E9) $([char]0x6050)$([char]0x7ADC)"
        $unicode = Invoke-Worker $unicodeTree $withRealNode @('-DryRun')
        Check ($unicode.Code -eq 0 -and (Test-Ran $unicode @('deploy', '--dry-run') $unicodeTree)) 'Unicode path: runs in the Worker folder'

        $broken = Invoke-Worker (New-Tree 'broken') $withRealNode @() 3
        Check ($broken.Code -eq 3 -and $broken.Out -notmatch 'Worker deployed' -and $broken.Err -match 'exited with code 3') 'propagates Wrangler exit code'
    } else {
        Write-Host 'worker script: real-node checks skipped - DATA UNAVAILABLE (Node.js is not installed)'
    }

    $noNode = Invoke-Worker (New-Tree 'no-node') "$decoy;$systemPath"
    Check ($noNode.Code -eq 1 -and $noNode.Err -match 'Node\.js not found' -and -not $noNode.DecoyUsed) 'missing Node.js fails without falling back to a global Wrangler'

    $oldNode = Invoke-Worker (New-Tree 'old-node') $withFakeNode @() 0 'v22.11.0'
    Check ($oldNode.Code -eq 1 -and $oldNode.Err -match 'too old') 'Node.js older than Wrangler''s engines fails'
    Check ((Invoke-Worker (New-Tree 'min-node') $withFakeNode @() 0 'v22.12.0').Code -eq 0) 'Node.js at Wrangler''s minimum runs'

    $noDepsTree = New-Tree 'no deps' -NoWrangler
    $noDeps = Invoke-Worker $noDepsTree "$decoy;$fakeNode;$fakeNpm;$systemPath"
    $expected = 'npm ci --prefix "' + (Join-Path $noDepsTree 'server\trespasser-bug-reports') + '"'
    Check ($noDeps.Code -eq 1 -and $noDeps.Err.Contains($expected) -and -not $noDeps.DecoyUsed) 'missing dependencies: prints the exact npm ci command'

    $noNpm = Invoke-Worker (New-Tree 'no-npm' -NoWrangler) $withFakeNode
    Check ($noNpm.Code -eq 1 -and $noNpm.Err -match 'npm was not found') 'missing dependencies and npm: says npm is missing'

    $mismatch = Invoke-Worker (New-Tree 'mismatch' '0.0.1') $withFakeNode
    Check ($mismatch.Code -eq 1 -and $mismatch.Err -match "0\.0\.1 but package-lock\.json pins $([regex]::Escape($lockedVersion))") 'local Wrangler different from the lockfile fails'

    $noLock = New-Tree 'no-lock'
    Remove-Item -LiteralPath (Join-Path $noLock 'server\trespasser-bug-reports\package-lock.json')
    Check ((Invoke-Worker $noLock $withFakeNode).Code -eq 1) 'missing package-lock.json fails'

    $noEntry = New-Tree 'no-entry'
    Remove-Item -LiteralPath (Join-Path $noEntry 'server\trespasser-bug-reports\src\worker.js')
    Check ((Invoke-Worker $noEntry $withFakeNode).Code -eq 1) 'missing entry point fails'

    $noConfig = New-Tree 'no-config'
    Remove-Item -LiteralPath (Join-Path $noConfig 'server\trespasser-bug-reports\wrangler.toml')
    Check ((Invoke-Worker $noConfig $withFakeNode).Code -eq 1) 'missing wrangler.toml fails'

    $noFolder = Join-Path $temp 'no-folder'
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $noFolder 'tools')
    Copy-Item -LiteralPath (Join-Path $root 'tools\worker.ps1') -Destination (Join-Path $noFolder 'tools')
    Check ((Invoke-Worker $noFolder $withFakeNode).Code -eq 1) 'missing worker folder fails'
} finally {
    Remove-Item Env:\JPT_FAKE_WRANGLER_LOG, Env:\JPT_FAKE_WRANGLER_CODE, Env:\JPT_FAKE_NODE_VERSION, Env:\JPT_DECOY_LOG -ErrorAction SilentlyContinue
    Remove-TestTemp
}

Write-Host "worker script: $checks checks, $failures failed"
if ($failures) { exit 1 }
exit 0
