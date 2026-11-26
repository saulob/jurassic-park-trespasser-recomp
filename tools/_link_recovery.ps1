# Recovery from MSVC link state left corrupted by an interrupted link (power loss): LNK1207 on a target's own PDB.
# Dot-sourced by tools\build.ps1; uses Write-Warn and Add-RunLog from _common.ps1.

$script:LinkPdbPattern = "LNK1207:\s*incompatible PDB format in\s+'?(?<pdb>[^';\r\n]+?\.pdb)'?\s*;"

# The PDB and ILK of a known target that LNK1207 names; nothing when the error is another one or names any other PDB.
function Get-CorruptLinkFiles([string[]]$Lines, [string]$BuildDir, [string[]]$Targets) {
    $expected = @{}
    foreach ($target in $Targets) {
        $pdb = [IO.Path]::GetFullPath((Join-Path $BuildDir "$target.pdb"))
        $expected[$pdb.ToLowerInvariant()] = $pdb
    }
    foreach ($line in $Lines) {
        $match = [regex]::Match("$line", $script:LinkPdbPattern)
        if (-not $match.Success) { continue }
        $named = $match.Groups['pdb'].Value.Trim().Replace('/', '\')
        if ($named.StartsWith('.\')) { $named = $named.Substring(2) }
        if ($named.Contains('..')) { continue }
        foreach ($key in $expected.Keys) {
            $same = if ([IO.Path]::IsPathRooted($named)) { [IO.Path]::GetFullPath($named).ToLowerInvariant() -eq $key } else { $key.EndsWith('\' + $named.ToLowerInvariant()) }
            if ($same) {
                $pdb = $expected[$key]
                return @($pdb, [IO.Path]::ChangeExtension($pdb, '.ilk'))
            }
        }
    }
    return @()
}

function Get-LinkErrorLines([string[]]$Lines) { @($Lines | Where-Object { "$_" -match 'LNK1207' }) }

# Runs $Build (returns @{ Code; Lines }) and, when it failed with a corrupted target PDB, removes that PDB and its ILK
# and runs it once more. The second result is final whatever it is.
function Invoke-BuildWithLinkRecovery([scriptblock]$Build, [string]$BuildDir, [string[]]$Targets) {
    $first = & $Build
    if ($first.Code -eq 0) { return $first.Code }
    $stale = @(Get-CorruptLinkFiles $first.Lines $BuildDir $Targets)
    if ($stale.Count -eq 0) { return $first.Code }
    Write-Warn 'MSVC linker state is corrupted, removing stale PDB/ILK and retrying once'
    foreach ($line in Get-LinkErrorLines $first.Lines) { Add-RunLog "original error: $line" }
    foreach ($file in $stale) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            Remove-Item -LiteralPath $file -Force
            Add-RunLog "removed $file"
        } else {
            Add-RunLog "not present: $file"
        }
    }
    Add-RunLog 'retrying the same build once'
    $second = & $Build
    $outcome = if ($second.Code -eq 0) { 'succeeded' } else { "failed with exit code $($second.Code)" }
    Write-Warn "The retry after removing the stale PDB/ILK $outcome"
    return $second.Code
}
