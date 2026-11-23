<#
.SYNOPSIS
  Builds the Windows Release host (clang-cl x86 + lld-link). Shorthand for tools\build.ps1 -Config Release.
.DESCRIPTION
  Forwards to tools\build.ps1 with -Config Release -Toolchain clang-cl, in build\cmake\clangcl-x86-release. The
  development build (tools\build.ps1) stays MSVC RelWithDebInfo; tools\build.ps1 -Config Release -Toolchain msvc keeps
  an MSVC Release for diagnosis.
.PARAMETER Clean
  Deletes the clang-cl build folder first.
.PARAMETER Jobs
  Parallel compile jobs (default: Ninja's choice).
.PARAMETER Run
  Starts the game after a successful build.
.EXAMPLE
  .\tools\build-clang-cl.ps1
  .\tools\build-clang-cl.ps1 -Clean
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Clean,
    [int]$Jobs = 0,
    [switch]$Run
)

$arguments = @{ Config = 'Release'; Toolchain = 'clang-cl' }
foreach ($name in 'Clean', 'Jobs', 'Run') {
    if ($PSBoundParameters.ContainsKey($name)) { $arguments[$name] = $PSBoundParameters[$name] }
}
& (Join-Path $PSScriptRoot 'build.ps1') @arguments
exit $LASTEXITCODE
