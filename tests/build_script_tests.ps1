# tools\build.ps1 is checked without running it: it parses, takes its paths from the repository, offers the documented
# parameters, builds the game only (the test targets stay out of the default build), keeps development runs in
# build\user_data, pins SDL and never reaches outside build\ when cleaning. Nothing is built, started or fetched.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $root 'tools\build.ps1'
$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name"
    }
}

$text = [IO.File]::ReadAllText($buildScript)
$errors = $null
$tokens = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($buildScript, [ref]$tokens, [ref]$errors)
Check (@($errors).Count -eq 0) 'tools\build.ps1 parses'

$parameters = @{}
foreach ($parameter in $ast.ParamBlock.Parameters) { $parameters[$parameter.Name.VariablePath.UserPath] = $parameter }
foreach ($name in 'Run', 'Arch', 'VerboseRuntime', 'Clean', 'Config', 'GenOpt', 'Trace', 'BuildDir', 'Jobs', 'GameArgs', 'LoaderArgs') {
    Check $parameters.ContainsKey($name) "build.ps1 has the -$name parameter"
}

function Get-ValidateSet($Parameter) {
    foreach ($attribute in $Parameter.Attributes) {
        if ($attribute.TypeName.Name -eq 'ValidateSet') { return @($attribute.PositionalArguments | ForEach-Object { $_.Value }) }
    }
    return @()
}
Check ((Get-ValidateSet $parameters['Arch']) -join ',' -eq 'x86,x64') '-Arch accepts x86 and x64'
Check ((Get-ValidateSet $parameters['Config']) -join ',' -eq 'RelWithDebInfo,Release') '-Config accepts RelWithDebInfo and Release'
Check ($parameters['Arch'].DefaultValue.Value -eq 'x86') 'the default architecture is the playable x86 host'
Check ($parameters['Config'].DefaultValue.Value -eq 'RelWithDebInfo') 'the default configuration is RelWithDebInfo'
Check ($text.Contains('build\cmake\msvc-$Arch-release') -and $text.Contains('build\cmake\msvc-$Arch"')) 'both configurations have a build folder under build\cmake'

Check ($text -match "\[string\[\]\]\`$hostTargets = if \(\`$Arch -eq 'x64'\) \{ 'trespasser_x64_probe', 'x64_tests' \} else \{ @\('trespasser_recomp'\) \}") 'the default build target is the game alone (x64: the probe and its tests)'
Check ($text -notmatch "hostTargets[^\r\n]*_tests'[^\r\n]*else[^\r\n]*_tests") 'no x86 test target is part of the default build'
Check ($text.Contains('$ninjaArgs += $hostTargets')) 'ninja builds only the host targets'

Check ($text.Contains("'--user_data_root', (Join-Path `$root 'build\user_data')")) 'development runs keep their data in build\user_data'
Check ($text.Contains("-notcontains '--user_data_root'")) 'an explicit --user_data_root is respected'
Check ($text.Contains("-notcontains '--game'") -and $text.Contains("(Join-Path `$root 'game')")) 'development runs use the repository game\ unless --game is given'
Check ($text.Contains("'--run', '--log-file'")) 'a run starts the game with --run and one log file'
Check ($text.Contains("'--log-dir', '--log-file'")) '--log-dir and --log-file are dropped from the arguments'
Check ($text -match 'if \(\$Run -and \$Arch -eq .x64.\)') '-Run is refused for x64'
Check ($text -match 'GameArgs.*LoaderArgs|LoaderArgs.*GameArgs') 'game arguments from -GameArgs and trailing tokens are combined'

Check ($text.Contains('StartsWith($buildRoot')) '-Clean only removes folders under build\'
Check ($text.Contains('Remove-Item -LiteralPath $buildDir -Recurse -Force')) '-Clean removes only the build folder'
Check ($text -notmatch 'Remove-Item[^\r\n]*(\$root\b(?!\s*\'')|game|user_data|lifts)') 'the script deletes nothing outside the build folder'

$deps = [IO.File]::ReadAllText((Join-Path $root 'config\launcher_deps.txt'))
Check ($deps -match 'SDL is release-3\.4\.20') 'the SDL pin is documented as release-3.4.20'
Check ($deps -match '(?m)^external/SDL\s+https://github\.com/libsdl-org/SDL\.git\s+[0-9a-f]{40}\s*$') 'SDL is pinned to a commit'
Check ($text.Contains("launcher_deps.txt") -and $text.Contains('git -C $dir fetch --quiet --depth 1 $url $commit')) 'dependencies are fetched at their pinned commit'
Check ($text.Contains('lifts.py') -and $text.Contains('verify')) 'the lifts are verified against lifts\manifest.json before building'
Check ($text.Contains('Enter-Msvc $Arch')) 'the MSVC toolchain of the architecture is entered'
Check ($text -notmatch '(?i)\btests?\b.*Invoke-Logged|Invoke-Logged[^\r\n]*_tests') 'build.ps1 never runs a test program'

$cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
$targets = [regex]::Matches($cmake, 'add_executable\(\s*([A-Za-z0-9_${}]+_tests)\b([^)]*)\)')
Check ($targets.Count -ge 20) 'CMakeLists.txt defines the test targets'
foreach ($target in $targets) { Check ($target.Groups[2].Value.Contains('EXCLUDE_FROM_ALL')) "$($target.Groups[1].Value) is EXCLUDE_FROM_ALL, so a default build never compiles it" }
Check ($cmake -match 'foreach\(model incremental non_incremental\)' -or $cmake.Contains('override_tests_${model}')) 'both override_tests link models exist'
Check ($cmake -match 'CMAKE_BUILD_TYPE STREQUAL "Debug"\)\s*\r?\n\s*message\(FATAL_ERROR') 'a Debug build is refused'
Check ($cmake -match 'RelWithDebInfo' -and $cmake -match 'TRESPASSER_GEN_OPT') 'the configurations and the lifted-code optimisation are defined'

Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
