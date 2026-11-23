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
foreach ($name in 'Run', 'Arch', 'VerboseRuntime', 'Clean', 'Config', 'Toolchain', 'GenOpt', 'Trace', 'BuildDir', 'Jobs', 'GameArgs', 'LoaderArgs') {
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
Check ((Get-ValidateSet $parameters['Toolchain']) -join ',' -eq 'auto,msvc,clang-cl') '-Toolchain accepts auto, msvc and clang-cl'
Check ($parameters['Arch'].DefaultValue.Value -eq 'x86') 'the default architecture is the playable x86 host'
Check ($parameters['Config'].DefaultValue.Value -eq 'RelWithDebInfo') 'the default configuration is RelWithDebInfo'
Check ($parameters['Toolchain'].DefaultValue.Value -eq 'auto') 'the default toolchain is auto (clang-cl for Release)'
Check ($text.Contains("'build\cmake\clangcl-x86-release'") -and $text.Contains('build\cmake\msvc-$Arch-release') -and $text.Contains('build\cmake\msvc-$Arch"')) 'Release has its clang-cl folder and MSVC keeps its folders'
Check ($text.Contains("Config -eq 'Release' -and `$Arch -eq 'x86'")) 'Release defaults to clang-cl on x86'
Check ($text.Contains("Toolchain -eq 'clang-cl' -and (`$Config -ne 'Release' -or `$Arch -ne 'x86')")) 'clang-cl is refused outside the x86 Release host'
Check ($text.Contains('-DCMAKE_C_COMPILER=clang-cl') -and $text.Contains('-DCMAKE_C_COMPILER_TARGET=i686-pc-windows-msvc')) 'the clang-cl configure pins the i686 MSVC ABI'
Check ($text.Contains("'another toolchain'")) 'a build folder configured with another compiler is refused'

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

# The Windows executables: GUI subsystem, the project's own manifest and version resource, and a helper linked for the
# same architecture with the static runtime. Checked statically; the staged files are checked by tools\verify-release.ps1.
$rc = [IO.File]::ReadAllText((Join-Path $root 'src\trespasser_recomp.rc'))
Check (Test-Path -LiteralPath (Join-Path $root 'src\trespasser_recomp.manifest') -PathType Leaf) 'the program has its own application manifest'
Check (-not $rc.Contains('RT_MANIFEST')) 'the program resource carries no second manifest'
Check ($cmake.Contains('set(JPT_RESOURCE_SOURCES src/trespasser_recomp.rc src/trespasser_recomp.manifest)')) 'the program links the manifest as an input'
Check ($rc.Contains('VALUE "OriginalFilename", "trespasser_recomp.exe"')) 'the program resource names its own file'
Check ($cmake -match 'add_executable\(trespasser_recomp WIN32') 'the program is a Windows-subsystem executable'
Check ($cmake.Contains('/ENTRY:mainCRTStartup')) 'the program keeps the C runtime entry point'
Check ($cmake -notmatch '/subsystem:console') 'the program never links as a console program'
Check ($cmake -match 'add_executable\(trg-updater WIN32') 'the update helper is a Windows-subsystem executable'
Check ($cmake -match 'JPT_UPDATER_HELPER_SOURCES') 'the helper sources have one list'
Check ($cmake -match 'MSVC_RUNTIME_LIBRARY "MultiThreaded"') 'the development helper links the static runtime'
Check ($cmake -match 'CMAKE_BUILD_TYPE STREQUAL "Release"\)[\s\S]{0,200}?/d1trimfile') 'a Release trims this machine''s source path from the objects'
Check ($cmake.Contains('/clang:-fmacro-prefix-map=')) 'the clang-cl Release trims the source path with the clang flag'
Check ($cmake -match 'CMAKE_CXX_COMPILER_ID MATCHES "Clang"\)[\s\S]{0,240}-fno-strict-aliasing') 'the clang build keeps MSVC aliasing behaviour'
Check ($cmake -match 'CMAKE_CXX_COMPILER_ID MATCHES "Clang"\)[\s\S]{0,260}/FIXED /SAFESEH') 'the clang build keeps the fixed image and the SafeSEH table'

# The clang-cl x86 Release build: its own preset and script, the MSVC development pipeline untouched.
$presetFile = Join-Path $root 'CMakePresets.json'
Check (Test-Path -LiteralPath $presetFile -PathType Leaf) 'the clang-cl preset exists'
$presets = Get-Content -LiteralPath $presetFile -Raw | ConvertFrom-Json
$configure = @($presets.configurePresets | Where-Object { $_.name -eq 'clang-cl-x86-release' })
Check ($configure.Count -eq 1) 'CMakePresets.json defines clang-cl-x86-release'
Check ($configure[0].generator -eq 'Ninja' -and $configure[0].binaryDir -match 'clangcl-x86-release') 'the preset has its own Ninja build folder'
Check ($configure[0].cacheVariables.CMAKE_BUILD_TYPE -eq 'Release') 'the preset is Release'
Check ($configure[0].cacheVariables.CMAKE_C_COMPILER -eq 'clang-cl' -and $configure[0].cacheVariables.CMAKE_CXX_COMPILER -eq 'clang-cl') 'the preset uses clang-cl'
Check ($configure[0].cacheVariables.CMAKE_C_COMPILER_TARGET -eq 'i686-pc-windows-msvc' -and
    $configure[0].cacheVariables.CMAKE_CXX_COMPILER_TARGET -eq 'i686-pc-windows-msvc') 'the preset targets the 32-bit MSVC ABI'
$buildPreset = @($presets.buildPresets | Where-Object { $_.name -eq 'clang-cl-x86-release' })
Check ($buildPreset.Count -eq 1 -and ($buildPreset[0].targets -contains 'trespasser_recomp')) 'the build preset builds the host'
$clangBuild = [IO.File]::ReadAllText((Join-Path $root 'tools\build-clang-cl.ps1'))
foreach ($needle in "Config = 'Release'", "Toolchain = 'clang-cl'", 'build.ps1') {
    Check ($clangBuild.Contains($needle)) "the clang-cl shorthand uses $needle"
}
$packageText = [IO.File]::ReadAllText((Join-Path $root 'tools\package.ps1'))
Check ($packageText.Contains("build\cmake\clangcl-x86-release") -and -not $packageText.Contains("build\cmake\msvc-x86-release")) 'the package stages the clang-cl Release host'
Check ($packageText.Contains("Toolchain = 'clang-cl'")) 'the package builds with the clang-cl toolchain'
Check ($packageText.Contains("'CMAKE_CXX_COMPILER', '^clang-cl(\.exe)?$'") -and $packageText.Contains("'CMAKE_LINKER', '^lld-link(\.exe)?$'")) 'the package refuses a build folder from another compiler'
Check ($packageText.Contains("'i686-pc-windows-msvc'")) 'the package checks the i686 target'
$updaterRc = [IO.File]::ReadAllText((Join-Path $root 'launcher\updater\trg-updater.rc'))
Check ($updaterRc.Contains('VALUE "OriginalFilename", "trg-updater.exe"')) 'the update helper resource names its own file'
Check ($updaterRc.Contains('RT_MANIFEST "trg-updater.manifest"')) 'the update helper resource embeds the manifest'
Check (Test-Path -LiteralPath (Join-Path $root 'launcher\updater\trg-updater.manifest') -PathType Leaf) 'the update helper manifest exists'
$updaterTarget = [regex]::Match($cmake, '(?s)add_executable\(trg-updater[^)]*\)').Value
Check (-not $updaterTarget.Contains('trg-updater.manifest')) 'the resource compiler owns the helper manifest'
Check (-not $updaterTarget.Contains('trg-updater.rc')) 'the MSVC helper target leaves the resource script to the release build'
Check ($cmake.Contains('set(JPT_UPDATER_RELEASE_RESOURCE launcher/updater/trg-updater.rc launcher/updater/trg-updater.manifest)')) 'the release helper resource has its own list'
$updaterBuild = [IO.File]::ReadAllText((Join-Path $root 'tools\build-updater.ps1'))
foreach ($needle in 'clang++', 'llvm-objcopy', 'llvm-objdump', 'windres', '-mwindows', '-static', '-fuse-ld=lld',
    '--no-insert-timestamp', '--only-keep-debug', '--strip-debug', '--add-gnu-debuglink', '.gnu_debuglink',
    'api-ms-win-', 'JPT_UPDATER_HELPER_SOURCES', 'JPT_UPDATER_RELEASE_RESOURCE') {
    Check ($updaterBuild.Contains($needle)) "the release helper build uses $needle"
}
$packageText = [IO.File]::ReadAllText((Join-Path $root 'tools\package.ps1'))
Check ($packageText.Contains('build-updater.ps1') -and $packageText.Contains('trg-updater.debug')) 'the package uses the CLANG64 helper and keeps its debug outside'

Write-Host "$script:checks checks, $script:failures failed"
exit $(if ($script:failures) { 1 } else { 0 })
