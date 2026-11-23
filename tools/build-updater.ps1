<#
.SYNOPSIS
  Builds the release update helper, trg-updater.exe, with MSYS2 CLANG64. Local only.
.DESCRIPTION
  Compiles the helper sources listed for the trg-updater target in CMakeLists.txt with the CLANG64 clang++ and LLD into a
  static x86-64 program that imports only Windows' own DLLs, including the Windows UCRT, like the release helpers of
  Dragon's Crown Pro PC, Odin Sphere Leifthrasir PC, 13 Sentinels and Alienation. The CLANG64 build compiles the helper's
  resource script (JPT_UPDATER_RELEASE_RESOURCE) with windres, so the program embeds the same VERSIONINFO and manifest;
  the debug information is split with llvm-objcopy into trg-updater.debug next to it, and the executable keeps only a
  .gnu_debuglink to it. tools\package.ps1 ships this build; the CMake target stays for development and the tests.
.PARAMETER OutDir
  The output folder (default: build\cmake\clang64-release\trg-updater-clang64). It is emptied first.
.EXAMPLE
  .\tools\build-updater.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'build\cmake\clang64-release\trg-updater-clang64' }
elseif (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $root $OutDir }

function Find-Clang64 {
    $roots = @($env:MSYS2_ROOT, 'C:\msys64', (Join-Path $env:USERPROFILE 'msys64')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    foreach ($msys in $roots) {
        $bin = Join-Path $msys 'clang64\bin'
        if (Test-Path -LiteralPath (Join-Path $bin 'clang++.exe')) { return $bin }
    }
    $clang = Get-Command 'clang++.exe' -ErrorAction SilentlyContinue
    if ($clang -and (& $clang.Source --version | Select-Object -First 1) -match 'x86_64-w64-windows-gnu') {
        return (Split-Path $clang.Source -Parent)
    }
    Stop-Tool 'MSYS2 CLANG64 was not found: install mingw-w64-clang-x86_64-toolchain (pacman -S) or set MSYS2_ROOT'
}

$bin = Find-Clang64
$cxx = Join-Path $bin 'clang++.exe'
$objcopy = Join-Path $bin 'llvm-objcopy.exe'
$objdump = Join-Path $bin 'llvm-objdump.exe'
$readobj = Join-Path $bin 'llvm-readobj.exe'
$windres = Join-Path $bin 'windres.exe'
foreach ($tool in $cxx, $objcopy, $objdump, $readobj, $windres, (Join-Path $bin 'ld.lld.exe')) {
    if (-not (Test-Path -LiteralPath $tool)) {
        Stop-Tool "Missing $tool (pacman -S mingw-w64-clang-x86_64-toolchain)"
    }
}

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(trespasser_recomp VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
$versionNumbers = "$($Matches[1]),$($Matches[2]),$($Matches[3]),0"
if ($cmake -notmatch '(?s)set\(JPT_UPDATER_HELPER_SOURCES\s+(.*?)\)') { Stop-Tool 'JPT_UPDATER_HELPER_SOURCES not found in CMakeLists.txt' }
$sources = @($Matches[1] -split '\s+' | Where-Object { $_ })
if ($cmake -notmatch '(?s)set\(JPT_UPDATER_RELEASE_RESOURCE\s+(.*?)\)') { Stop-Tool 'JPT_UPDATER_RELEASE_RESOURCE not found in CMakeLists.txt' }
$resource = @($Matches[1] -split '\s+' | Where-Object { $_ -like '*.rc' })
if ($sources.Count -lt 4 -or $resource.Count -ne 1) { Stop-Tool 'the trg-updater sources or its resource were not found in CMakeLists.txt' }

# The same header CMake writes from the project VERSION; the resource includes it by that name.
$generated = Join-Path $OutDir 'generated'
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'obj'), $generated | Out-Null
[IO.File]::WriteAllText((Join-Path $generated 'jpt_version.h'),
    "#define JPT_VERSION_NUMBERS $versionNumbers`n#define JPT_FILE_VERSION_STRING `"$version.0`"`n#define JPT_VERSION_STRING `"$version`"`n",
    (New-Object Text.UTF8Encoding($false)))

$rootForward = $root.Replace('\', '/')
$compileFlags = @('-O2', '-g', '-DNDEBUG', '-std=c++20', "-fmacro-prefix-map=$rootForward/=", '-Wall', '-Wextra',
    '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', "$rootForward/launcher", '-I', "$($generated.Replace('\', '/'))")
$linkFlags = @('-O2', '-g', '-DNDEBUG', '-mwindows', '-static', '-fuse-ld=lld', '-Wl,--no-insert-timestamp')
$libraries = @('-lshell32', '-lkernel32', '-luser32', '-lgdi32', '-lwinspool', '-lole32', '-loleaut32', '-luuid',
    '-lcomdlg32', '-ladvapi32')
# The helper runs alone from a temporary folder: it may load nothing but Windows itself.
$systemDlls = @('kernel32.dll', 'shell32.dll', 'user32.dll', 'advapi32.dll', 'ntdll.dll', 'shlwapi.dll', 'ole32.dll',
    'bcrypt.dll')

$savedPath = $env:PATH
Push-Location $OutDir
try {
    $env:PATH = "$bin;$env:PATH"
    Write-Step "Update helper: $((& $cxx --version | Select-Object -First 1))"
    $objects = foreach ($source in $sources) {
        $object = 'obj/' + (Split-Path $source -Leaf) + '.obj'
        & $cxx @compileFlags -c "$rootForward/$source" -o $object
        if ($LASTEXITCODE -ne 0) { Stop-Tool "Compiling $source failed" }
        $object
    }
    & $windres -O coff -I "$rootForward/launcher/updater" -I "$($generated.Replace('\', '/'))" -o 'obj/trg-updater.res.o' "$rootForward/$resource"
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Compiling trg-updater.rc failed' }
    & $cxx @linkFlags @objects 'obj/trg-updater.res.o' -o 'obj/trg-updater.exe' @libraries
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Linking trg-updater.exe failed' }
    & $objcopy --only-keep-debug 'obj/trg-updater.exe' 'trg-updater.debug'
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --only-keep-debug failed' }
    & $objcopy --strip-debug '--add-gnu-debuglink=trg-updater.debug' 'obj/trg-updater.exe' 'trg-updater.exe'
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --strip-debug failed' }

    $imports = @(& $objdump -p 'trg-updater.exe' | ForEach-Object { if ($_ -match 'DLL Name:\s*(\S+)') { $Matches[1] } })
    if ($LASTEXITCODE -ne 0 -or -not $imports.Count) { Stop-Tool 'llvm-objdump could not read the imports of trg-updater.exe' }
    $foreign = @($imports | Where-Object { $_.ToLowerInvariant() -notin $systemDlls -and $_ -notmatch '^api-ms-win-' })
    if ($foreign.Count) { Stop-Tool "trg-updater.exe imports non-system DLLs: $($foreign -join ', ')" }

    $sections = @(& $objdump -h 'trg-updater.exe' | ForEach-Object { if ($_ -match '^\s*\d+\s+(\S+)') { $Matches[1] } })
    if ($sections -match '^\.z?debug_') { Stop-Tool 'trg-updater.exe still holds debug sections' }
    if ($sections -notcontains '.gnu_debuglink') { Stop-Tool 'trg-updater.exe has no .gnu_debuglink' }

    $headers = & $readobj --file-headers 'trg-updater.exe' | Out-String
    if ($headers -notmatch 'IMAGE_FILE_MACHINE_AMD64') { Stop-Tool 'trg-updater.exe is not x86-64' }
    if ($headers -notmatch 'IMAGE_SUBSYSTEM_WINDOWS_GUI') { Stop-Tool 'trg-updater.exe is not a GUI program' }
    if ($headers -notmatch 'IMAGE_DLL_CHARACTERISTICS_NX_COMPAT' -or $headers -notmatch 'IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE') {
        Stop-Tool 'trg-updater.exe is missing NX_COMPAT or DYNAMICBASE'
    }

    $bytes = [IO.File]::ReadAllBytes((Join-Path $OutDir 'trg-updater.exe'))
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text = $latin1.GetString($bytes)
    foreach ($local in $root, $root.Replace('\', '/'), $env:USERPROFILE) {
        if (-not $local) { continue }
        if ($text.IndexOf($local, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $text.IndexOf($latin1.GetString([Text.Encoding]::Unicode.GetBytes($local)), [StringComparison]::Ordinal) -ge 0) {
            Stop-Tool "trg-updater.exe names this machine's folder: $local"
        }
    }
    if ($text.IndexOf('DEBUG_SAULO', [StringComparison]::Ordinal) -ge 0) { Stop-Tool 'trg-updater.exe carries DEBUG_SAULO diagnostics' }
    if (& $readobj --coff-debug-directory 'trg-updater.exe' | Select-String -Pattern 'PDBFileName: \S' -Quiet) {
        Stop-Tool 'trg-updater.exe names a PDB'
    }
} finally {
    $env:PATH = $savedPath
    Pop-Location
}

Write-Ok "$(Join-Path $OutDir 'trg-updater.exe') ($($imports.Count) system DLLs imported, no debug sections, x86-64 GUI)"
Write-Ok "$(Join-Path $OutDir 'trg-updater.debug') (not packaged)"
Write-Host "  trg-updater.exe SHA-256 $(Get-Sha256 (Join-Path $OutDir 'trg-updater.exe'))"
