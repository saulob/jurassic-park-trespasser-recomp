# tools\_link_recovery.ps1 on fake builds and a scratch build folder: LNK1207 on a target's own PDB removes that PDB and its
# ILK and builds once more; any other failure, or success with LNK4076, is left as it is. Nothing is built.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name"
    }
}

$script:log = [System.Collections.Generic.List[string]]::new()
function Write-Warn([string]$Text) { $script:log.Add("warn: $Text") }
function Add-RunLog([string]$Line) { $script:log.Add($Line) }
. (Join-Path $root 'tools\_link_recovery.ps1')

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("jpt_link_recovery_" + [guid]::NewGuid().ToString('N'))
$build = Join-Path $scratch 'build\cmake\msvc-x86'
$targets = @('trespasser_recomp')
$pdb = Join-Path $build 'trespasser_recomp.pdb'
$ilk = Join-Path $build 'trespasser_recomp.ilk'

function New-BuildFolder {
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    foreach ($file in 'trespasser_recomp.pdb', 'trespasser_recomp.ilk', 'CMakeCache.txt', 'build.ninja', 'other.pdb', 'vc140.pdb',
        'CMakeFiles\trespasser_recomp.dir\src\main.c.obj', 'CMakeFiles\trespasser_recomp.dir\vc140.pdb', 'src\recomp\lift116\part_0001.c.obj') {
        $path = Join-Path $build $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, $file)
    }
}

function Get-Snapshot { @(Get-ChildItem -LiteralPath $scratch -Recurse -Force | ForEach-Object { $_.FullName.Substring($scratch.Length) } | Sort-Object) }

# A build that answers with the given results in turn and counts its calls.
function New-FakeBuild([object[]]$Results) {
    $state = @{ Calls = 0; Results = $Results }
    $block = {
        $state.Calls++
        $state.Results[[Math]::Min($state.Calls, $state.Results.Count) - 1]
    }.GetNewClosure()
    return @{ State = $state; Block = $block }
}

$lnk1207 = "LINK : fatal error LNK1207: incompatible PDB format in '$pdb'; delete and rebuild"
$failed = @{ Code = 1; Lines = @('[1/2] Linking CXX executable trespasser_recomp.exe', 'FAILED: trespasser_recomp.exe',
        "LINK : warning LNK4076: invalid incremental status file '$ilk'; linking nonincrementally", $lnk1207, 'ninja: build stopped: subcommand failed.') }
$ok = @{ Code = 0; Lines = @('[1/1] Linking CXX executable trespasser_recomp.exe') }

# The signature, the PDB it names and the ILK beside it.
Check ((@(Get-CorruptLinkFiles $failed.Lines $build $targets) -join '|') -eq "$pdb|$ilk") 'LNK1207 incompatible PDB names the target PDB and its ILK'
Check ((@(Get-CorruptLinkFiles @('LINK : fatal error LNK1207: incompatible PDB format in build\cmake\msvc-x86\trespasser_recomp.pdb; delete and rebuild') $build $targets) -join '|') -eq "$pdb|$ilk") 'a path relative to the repository is recognised'
Check (@(Get-CorruptLinkFiles @("fatal error LNK1207: incompatible PDB format in '.\trespasser_recomp.pdb'; delete and rebuild") $build $targets).Count -eq 2) 'a path relative to the build folder is recognised'
Check (@(Get-CorruptLinkFiles @("fatal error LNK1207: incompatible PDB format in '$(Join-Path $build 'other.pdb')'; delete and rebuild") $build $targets).Count -eq 0) 'the PDB of something else is never removed'
Check (@(Get-CorruptLinkFiles @("fatal error LNK1207: incompatible PDB format in '..\trespasser_recomp.pdb'; delete and rebuild") $build $targets).Count -eq 0) 'a PDB outside the build folder is never removed'
Check (@(Get-CorruptLinkFiles @("fatal error LNK1207: incompatible PDB format in 'C:\elsewhere\trespasser_recomp.pdb'; delete and rebuild") $build $targets).Count -eq 0) 'a PDB of another build folder is never removed'
Check ((@(Get-CorruptLinkFiles @("LNK1207: incompatible PDB format in '$(Join-Path $build 'x64_tests.pdb')'; delete and rebuild") $build @('trespasser_x64_probe', 'x64_tests')) -join '|') -eq "$(Join-Path $build 'x64_tests.pdb')|$(Join-Path $build 'x64_tests.ilk')") 'every target the script builds is covered by name'
Check (@(Get-CorruptLinkFiles @('main.c(12): error C2065: undeclared identifier', 'FAILED: main.c.obj') $build $targets).Count -eq 0) 'a compile error is not a corrupted PDB'
Check (@(Get-CorruptLinkFiles @('LINK : fatal error LNK1120: 1 unresolved externals', 'error LNK2019: unresolved external symbol _f', 'LINK : fatal error LNK1104: cannot open file trespasser_recomp.exe') $build $targets).Count -eq 0) 'another link error is not a corrupted PDB'
Check (@(Get-CorruptLinkFiles @("LINK : warning LNK4076: invalid incremental status file '$ilk'; linking nonincrementally") $build $targets).Count -eq 0) 'LNK4076 alone is not a corrupted PDB'

# A failed link that the retry repairs: only the PDB and the ILK go, once.
New-BuildFolder
$before = Get-Snapshot
$fake = New-FakeBuild @($failed, $ok)
$script:log.Clear()
$code = Invoke-BuildWithLinkRecovery $fake.Block $build $targets
$after = Get-Snapshot
Check ($code -eq 0 -and $fake.State.Calls -eq 2) 'LNK1207 removes the stale files and builds once more'
Check (-not (Test-Path -LiteralPath $pdb) -and -not (Test-Path -LiteralPath $ilk)) 'the target PDB and ILK are removed'
Check ((Compare-Object $before $after | Where-Object { $_.SideIndicator -eq '<=' } | ForEach-Object { $_.InputObject }) -join '|' -eq '\build\cmake\msvc-x86\trespasser_recomp.ilk|\build\cmake\msvc-x86\trespasser_recomp.pdb') 'nothing else is removed: objects, CMakeCache, build.ninja and other PDBs stay'
Check (@($after | Where-Object { $_ -like '*.obj' }).Count -eq 2 -and (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) 'the compiled objects and the CMake cache are kept'
Check ($script:log.Contains('warn: MSVC linker state is corrupted, removing stale PDB/ILK and retrying once')) 'the log says what is happening'
Check (@($script:log | Where-Object { $_ -like 'original error:*LNK1207*' }).Count -eq 1 -and $script:log.Contains("removed $pdb") -and $script:log.Contains("removed $ilk")) 'the log has the original error and each removed file'
Check ($script:log.Contains('retrying the same build once') -and $script:log.Contains('warn: The retry after removing the stale PDB/ILK succeeded')) 'the log has the retry and its result'

# Corrupted again: one retry only, its real failure is returned.
New-BuildFolder
$fake = New-FakeBuild @($failed, @{ Code = 7; Lines = $failed.Lines })
$script:log.Clear()
$code = Invoke-BuildWithLinkRecovery $fake.Block $build $targets
Check ($code -eq 7 -and $fake.State.Calls -eq 2) 'a second failure is final: one retry, its exit code returned'
Check ($script:log.Contains('warn: The retry after removing the stale PDB/ILK failed with exit code 7')) 'the failed retry is logged'

# Without an ILK the PDB alone goes.
New-BuildFolder
Remove-Item -LiteralPath $ilk
$fake = New-FakeBuild @($failed, $ok)
$script:log.Clear()
Check ((Invoke-BuildWithLinkRecovery $fake.Block $build $targets) -eq 0 -and -not (Test-Path -LiteralPath $pdb) -and $script:log.Contains("not present: $ilk")) 'a missing ILK is noted, not an error'

# Other outcomes: no cleanup, no retry.
foreach ($case in @(
        @{ Name = 'a compile error'; Result = @{ Code = 1; Lines = @('main.c(12): error C2065: x', 'FAILED: main.c.obj') }; Code = 1 },
        @{ Name = 'another link error'; Result = @{ Code = 1; Lines = @('LINK : fatal error LNK1120: 1 unresolved externals') }; Code = 1 },
        @{ Name = 'a success with LNK4076'; Result = @{ Code = 0; Lines = @("LINK : warning LNK4076: invalid incremental status file '$ilk'; linking nonincrementally") }; Code = 0 },
        @{ Name = 'LNK1207 on a build that still succeeded'; Result = @{ Code = 0; Lines = @($lnk1207) }; Code = 0 })) {
    New-BuildFolder
    $before = Get-Snapshot
    $fake = New-FakeBuild @($case.Result)
    $script:log.Clear()
    $code = Invoke-BuildWithLinkRecovery $fake.Block $build $targets
    Check ($code -eq $case.Code -and $fake.State.Calls -eq 1 -and ((Compare-Object $before (Get-Snapshot)) -eq $null) -and $script:log.Count -eq 0) "$($case.Name): no cleanup and no retry"
}

# build.ps1 uses it for its Ninja step and has no other way to clean.
$text = [IO.File]::ReadAllText((Join-Path $root 'tools\build.ps1'))
$helper = [IO.File]::ReadAllText((Join-Path $root 'tools\_link_recovery.ps1'))
Check ($text.Contains(". (Join-Path `$PSScriptRoot '_link_recovery.ps1')") -and $text.Contains('Invoke-BuildWithLinkRecovery $ninja $buildDir $hostTargets')) 'build.ps1 builds through the recovery with its own folder and targets'
Check (([regex]::Matches($text, "Invoke-Logged 'ninja'")).Count -eq 1) 'build.ps1 has one Ninja command, the one the retry repeats'
Check ($helper -notmatch '-Recurse' -and $helper -notmatch '\b(while|for|do)\s*\(' -and $helper -notmatch 'CMakeCache|\.obj|-Clean') 'the recovery removes single files, never loops and never cleans'
Check (([regex]::Matches($helper, '& \$Build')).Count -eq 2) 'the build runs at most twice'

if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
