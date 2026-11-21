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
    if (Test-Path -LiteralPath $vswhere) {
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
    Check ($linuxHelper -match "MuaFocalBuild" -and $linuxHelper -match '2\.31') 'MuaFocalBuild and the glibc 2.31 baseline are what the distribution choice prefers'
    Check ($linuxHelper -notmatch '(?i)/mnt/[a-z]/' -and $linuxHelper -notmatch '(?i)Start-Process|Invoke-WebRequest|wine|proton') 'no hard-coded /mnt path, no process shell-out and no Wine in the Linux half'
    Check ($text -notmatch '(?i)trespasser_recomp(\.exe)?\s+--run|--run') 'the gate never runs the game'
    Check ($text.Contains('[switch]$WindowsOnly') -and $text.Contains('[switch]$LinuxOnly') -and $text.Contains('[switch]$SkipLinux') -and $text.Contains('[string]$LinuxDistro') -and $text.Contains('[switch]$Sanitize')) 'the options of the two halves exist'

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

    $cheap = Invoke-Gate $gate @('-Filter', 'tests\build_script_tests.ps1', '-NoBuild') $env:PATH $other
    Check ($cheap.Code -eq 0 -and $cheap.Text -match 'PASS\s+tests\\build_script_tests\.ps1' -and $cheap.Text -match 'PARTIAL RUN') 'a filtered run works from another folder and says it is partial'
    Check ($cheap.Text -notmatch 'ALL TESTS PASSED') 'a partial run never claims all tests passed'

    $missing = Invoke-Gate $gate @('-Filter', 'platform_tests', '-NoBuild', '-BuildDir', (Join-Path $work 'no-build'))
    Check ($missing.Code -ne 0 -and $missing.Text -match 'no test program' -and $missing.Text.Contains('FAILED: platform_tests')) 'a missing test program fails its suite and says how to build it'

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $fixture = Join-Path $work 'fixture'
        New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'tools'), (Join-Path $fixture 'tests') | Out-Null
        Copy-Item -LiteralPath $gate, (Join-Path $root 'tools\_common.ps1'), (Join-Path $root 'tools\_gate_linux.ps1') -Destination (Join-Path $fixture 'tools')
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
    }
} finally {
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($work)
    if ((Split-Path $full -Parent) -eq $temp -and (Split-Path $full -Leaf).StartsWith('jpt-gate-tests-')) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
