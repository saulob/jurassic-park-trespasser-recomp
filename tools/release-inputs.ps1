# What a release needs that is not in Git: the reviewed AppImage runtime, kept in cache\ (ignored, never mirrored or
# packaged), and the WSL2 distribution the AppImage is built in. Dot-sourced by tools\package-release.ps1.

$script:DefaultWslDistro = 'JptFocalBuild'
$script:UbuntuBaseName = 'ubuntu-base-20.04.5-base-amd64.tar.gz'
$script:UbuntuBaseUrl = "https://cdimage.ubuntu.com/ubuntu-base/releases/20.04/release/$script:UbuntuBaseName"
$script:UbuntuBaseSha256 = '60e216b60947653dc8989be3821380268315b99b2959c29882781541bfe5a426'
$script:WslBuildTools = @('gcc-13', 'g++-13', 'cmake', 'ninja', 'python3', 'git', 'patchelf', 'objcopy', 'readelf', 'strip',
    'curl', 'rsync', 'dpkg-query', 'desktop-file-validate', 'appstreamcli', 'unshare')

function Get-PinnedRuntimeSha256([string]$Root) {
    $baseline = Get-Content -LiteralPath (Join-Path $Root 'patches\appimage-runtime\baseline.env') -Raw
    if ($baseline -notmatch '(?m)^RUNTIME_SHA256=([0-9a-f]{64})\s*$') { throw 'RUNTIME_SHA256 not found in patches\appimage-runtime\baseline.env' }
    return $Matches[1]
}

function Test-FileSha256([string]$File, [string]$Sha256) {
    try {
        (Test-Path -LiteralPath $File -PathType Leaf) -and (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256
    } catch { $false }
}

# cache\appimage-runtime\runtime-x86_64 when it has the pinned SHA-256. Otherwise $Import is asked to place a copy there
# (from a WSL packaging cache); a file with any other hash is deleted and $null returned, so nothing unreviewed is used.
function Resolve-ReviewedRuntime([string]$Root, [scriptblock]$Import) {
    $sha256 = Get-PinnedRuntimeSha256 $Root
    $file = Join-Path $Root 'cache\appimage-runtime\runtime-x86_64'
    if (Test-FileSha256 $file $sha256) { return $file }
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    if ($Import) {
        New-Item -ItemType Directory -Force (Split-Path $file -Parent) | Out-Null
        & $Import $file $sha256
        if (Test-FileSha256 $file $sha256) { return $file }
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
    if (Copy-RuntimeFromSiblingDistro $file $sha256) { return $file }
    return $null
}

# Read-only look into the other WSL distributions' packaging caches (the sibling ports pin the same runtime): a file is
# copied only when its SHA-256 is the pinned one, and nothing in those distributions is changed.
function Copy-RuntimeFromSiblingDistro([string]$Target, [string]$Sha256) {
    $names = @('alienation-pc-build\build\appimage\runtime-aln-x86_64', '13-sentinels-aegis-rim-pc-build\build\appimage\runtime-x86_64',
        'dragons-crown-pro-pc-build\build\appimage\runtime-dcp-x86_64', 'odin-sphere-leifthrasir-pc-build\build\appimage\runtime-osl-x86_64')
    foreach ($distro in @(Get-WslDistroNames)) {
        foreach ($name in $names) {
            $candidate = "\\wsl.localhost\$distro\root\.cache\$name"
            if (-not (Test-FileSha256 $candidate $Sha256)) { continue }
            New-Item -ItemType Directory -Force (Split-Path $Target -Parent) | Out-Null
            Copy-Item -LiteralPath $candidate -Destination "$Target.part" -Force
            if (Test-FileSha256 "$Target.part" $Sha256) {
                Move-Item -LiteralPath "$Target.part" $Target -Force
                Write-Host "AppImage runtime: the reviewed runtime copied from $candidate"
                return $true
            }
            Remove-Item -LiteralPath "$Target.part" -ErrorAction SilentlyContinue
        }
    }
    return $false
}

function Get-WslDistroNames {
    @(& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
}

# Creates the dedicated distribution from the pinned Ubuntu 20.04.5 base image, the way docs/LINUX.md describes.
function New-WslBuildDistro([string]$Root, [string]$Name) {
    $cache = Join-Path $Root 'cache\wsl'
    $image = Join-Path $cache $script:UbuntuBaseName
    if (-not (Test-FileSha256 $image $script:UbuntuBaseSha256)) {
        New-Item -ItemType Directory -Force $cache | Out-Null
        Remove-Item -LiteralPath $image -ErrorAction SilentlyContinue
        Invoke-WebRequest -Uri $script:UbuntuBaseUrl -OutFile "$image.part" -UseBasicParsing
        Move-Item -LiteralPath "$image.part" $image -Force
        if (-not (Test-FileSha256 $image $script:UbuntuBaseSha256)) {
            Remove-Item -LiteralPath $image -ErrorAction SilentlyContinue
            return "the Ubuntu base image from $script:UbuntuBaseUrl does not have the pinned SHA-256 $script:UbuntuBaseSha256"
        }
    }
    $folder = Join-Path $env:LOCALAPPDATA "WSL\$Name"
    if (Test-Path -LiteralPath $folder) { return "$folder exists but $Name is not a registered distribution; remove it or name another one with -WslDistro" }
    New-Item -ItemType Directory -Force $folder | Out-Null
    & wsl.exe --import $Name $folder $image --version 2 | Out-Host
    if ($LASTEXITCODE -ne 0) { return "wsl --import $Name failed (exit $LASTEXITCODE)" }
    return $null
}

# The tools of the release build that the distribution lacks; empty when it is prepared.
function Get-WslMissingTools([string]$Name) {
    $probe = 'for t in ' + ($script:WslBuildTools -join ' ') + '; do command -v "$t" >/dev/null || echo "$t"; done; ' +
        'test -f /opt/jpt-openssl/lib/libssl.a || echo openssl-static'
    @(& wsl.exe -d $Name --exec /bin/bash -c $probe 2>&1 | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
}

# Makes sure the dedicated distribution exists and is prepared by packaging/linux/ci-deps.sh. Only the default
# distribution is created or changed; another one named with -WslDistro is used as it is. Sets $script:RepoWsl, the
# checkout's path in the distribution. Returns a problem or $null.
function Initialize-WslBuildEnvironment([string]$Root, [string]$Name) {
    $dedicated = $Name -eq $script:DefaultWslDistro
    if ((Get-WslDistroNames) -notcontains $Name) {
        if (-not $dedicated) { return "WSL distribution $Name does not exist. Create an Ubuntu 20.04 one as docs/LINUX.md describes, or use the default $script:DefaultWslDistro." }
        Write-Step "Creating the WSL distribution $Name (Ubuntu 20.04.5 base image)"
        $problem = New-WslBuildDistro $Root $Name
        if ($problem) { return $problem }
    }
    $probe = & wsl.exe -d $Name --exec /bin/uname -sm 2>&1
    if ($LASTEXITCODE -ne 0 -or "$probe" -notmatch 'Linux x86_64') {
        return "WSL distribution $Name is not usable ($(("$probe" -replace '\0', '').Trim()))."
    }
    $RepoWsl = (& wsl.exe -d $Name --exec /usr/bin/wslpath -a -u $Root | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $RepoWsl) { return 'wslpath could not convert the repository path.' }
    $script:RepoWsl = $RepoWsl
    $missing = @(Get-WslMissingTools $Name)
    if ($missing.Count -eq 0) { return $null }
    if (-not $dedicated) {
        return "WSL distribution $Name lacks: $($missing -join ', '). Prepare it once with: wsl -d $Name -u root --exec /bin/bash $RepoWsl/packaging/linux/ci-deps.sh"
    }
    Write-Step "Preparing $Name with packaging/linux/ci-deps.sh (missing: $($missing -join ', '))"
    & wsl.exe -d $Name -u root --exec /bin/bash "$RepoWsl/packaging/linux/ci-deps.sh" | Out-Host
    if ($LASTEXITCODE -ne 0) { return "packaging/linux/ci-deps.sh failed in $Name (exit $LASTEXITCODE)" }
    $missing = @(Get-WslMissingTools $Name)
    if ($missing.Count -gt 0) { return "$Name still lacks after ci-deps.sh: $($missing -join ', ')" }
    return $null
}
