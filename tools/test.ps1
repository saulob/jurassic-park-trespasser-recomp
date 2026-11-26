<#
.SYNOPSIS
  The one validation command: builds the headless test programs incrementally and runs every suite of the project on
  Windows, then the Linux gate in WSL, then compares the two feature by feature. No game, launcher, window, controller,
  audio device or network is used.

.DESCRIPTION
  Suites are grouped in sections and each prints PASS, FAIL, SKIPPED or NOT RUN; a failing suite shows its command,
  exit code and real output. Every suite runs even after another failed. The exit code is 1 when any mandatory suite
  failed, a script or dependency (MSVC, CMake, Ninja, Python 3, git) is missing or the run changed the repository.

  Only test targets are compiled (ninja, incremental, EXCLUDE_FROM_ALL targets); the game, the launcher and the
  release package are never built, run or generated. The summary ends with the supported builds, the languages, the
  cheats and the features, each with what proves it. Where real game data is needed and absent the row says
  DATA UNAVAILABLE; a missing file is never turned into a PASS.

.PARAMETER List
  Lists the sections and suites and exits; nothing is built or run.

.PARAMETER Detail
  Shows the output of passing suites too.

.PARAMETER Filter
  Runs only the suites whose section, name or description contains this text (the run is marked partial).

.PARAMETER Release
  Also validates the release artifacts already in dist\ (folder and ZIP) with tools\verify-release.ps1. Nothing is
  packaged; absent artifacts fail.

.PARAMETER Full
  Runs water_tests without --quick (about five minutes more).

.PARAMETER NoBuild
  Runs the test programs already in the build folder without compiling them first.

.PARAMETER Build
  Builds the test programs (and the runtime, when nothing is filtered) incrementally and exits before any suite runs:
  a build-only run. The Linux half is not started. -NoBuild and -Build exclude each other.

.PARAMETER BuildDir
  Another x86 build folder (default build\cmake\msvc-x86, configured by tools\build.ps1).

.PARAMETER WindowsOnly
  Runs only the Windows half. The feature parity is then not evaluated and the summary says so.

.PARAMETER LinuxOnly
  Runs only the Linux half (WSL). The feature parity is then not evaluated.

.PARAMETER SkipLinux
  The same as -WindowsOnly.

.PARAMETER LinuxDistro
  The WSL distribution to use. Without it JptFocalBuild (Ubuntu 20.04, glibc 2.31) is preferred, then any other distribution
  with glibc 2.31, then a newer one for development only; the summary names the one used and whether the baseline is proven.

.PARAMETER GameIsoDir
  Folder of disc images to import headlessly before the Linux suites (default: TRESPASSER_ISO_DIR, the disc named by
  config\local.json, or a development downloads folder found on a drive). Existing installs are not replaced.

.PARAMETER Sanitize
  Runs only the Linux-specific suites of an AddressSanitizer + UndefinedBehaviorSanitizer build (a separate tree, slower); a
  partial run, never part of the default.
#>
[CmdletBinding()]
param(
    [switch]$List,
    [switch]$Detail,
    [string]$Filter = '',
    [switch]$Release,
    [switch]$Full,
    [switch]$NoBuild,
    [switch]$Build,
    [string]$BuildDir = '',
    [switch]$WindowsOnly,
    [switch]$LinuxOnly,
    [switch]$SkipLinux,
    [string]$LinuxDistro = '',
    [string]$GameIsoDir = '',
    [switch]$Sanitize
)

. (Join-Path $PSScriptRoot '_common.ps1')
$callerEnvironment = Get-ProcessEnvironment
$root = $script:RepoRoot
$buildDir = if (-not $BuildDir) { Join-Path $root 'build\cmake\msvc-x86' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$buildDir = [IO.Path]::GetFullPath($buildDir)
$buildDir64 = Join-Path $root 'build\cmake\msvc-x64'
$hostShell = (Get-Process -Id $PID).Path
if ($WindowsOnly -and $LinuxOnly) { Write-Host 'FAILED: -WindowsOnly and -LinuxOnly exclude each other.' -ForegroundColor Red; exit 1 }
if ($Build -and $NoBuild) { Write-Host 'FAILED: -Build (build only) and -NoBuild (run the programs already built) exclude each other.' -ForegroundColor Red; exit 1 }
if ($Build -and ($LinuxOnly -or $Sanitize)) { Write-Host 'FAILED: -Build builds the Windows test programs; it cannot be combined with -LinuxOnly or -Sanitize.' -ForegroundColor Red; exit 1 }
$runWindows = -not $LinuxOnly -and -not $Sanitize
$runLinux = -not ($WindowsOnly -or $SkipLinux)
$partial = [bool]$Filter -or [bool]$Sanitize
$script:ImportedVersionPaths = @()
. (Join-Path $PSScriptRoot '_gate_linux.ps1')

$suites = New-Object System.Collections.Generic.List[object]
$notRunSuites = New-Object System.Collections.Generic.List[object]
function Add-Suite([string]$Section, [string]$Kind, [string]$Name, [string]$Description, [hashtable]$Options = @{}) {
    $suites.Add([pscustomobject]@{ Section = $Section; Kind = $Kind; Name = $Name; Description = $Description; Options = $Options })
}
function Add-NotRun([string]$Section, [string]$Name, [string]$Reason) {
    $notRunSuites.Add([pscustomobject]@{ Section = $Section; Name = $Name; Status = 'NOT RUN'; Seconds = 0; Checks = 0; Summary = $Reason; Lines = @() })
}
function Add-Exes([string]$Section, [object[]]$Rows) {
    foreach ($row in $Rows) {
        $options = @{ MinChecks = $(if ($row.Count -gt 2) { $row[2] } else { 0 }) }
        if ($row.Count -gt 3) { foreach ($key in $row[3].Keys) { $options[$key] = $row[3][$key] } }
        Add-Suite $Section 'exe' $row[0] $row[1] $options
    }
}
function Add-Python([string]$Section, [object[]]$Rows) {
    foreach ($row in $Rows) { Add-Suite $Section 'python' "tests\$($row[0])" $row[1] @{ Background = ($row.Count -gt 2 -and $row[2]) } }
}
function Add-Tool([string]$Section, [string]$Command, [string]$Description) {
    $parts = -split $Command
    Add-Suite $Section 'tool' $Command $Description @{ Script = $parts[0]; Arguments = @($parts | Select-Object -Skip 1) }
}

function Find-GameIsoDirectory([string]$Requested) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Requested) { $candidates.Add($Requested) }
    if ($env:TRESPASSER_ISO_DIR) { $candidates.Add($env:TRESPASSER_ISO_DIR) }
    $local = Join-Path $root 'config\local.json'
    if (Test-Path -LiteralPath $local) {
        try {
            $config = [IO.File]::ReadAllText($local) | ConvertFrom-Json
            if ($config.iso_dir) { $candidates.Add([string]$config.iso_dir) }
            if ($config.iso) {
                $parent = Split-Path -Parent ([string]$config.iso)
                if ($parent) { $candidates.Add($parent); $candidates.Add((Join-Path $parent 'versoes')) }
            }
        } catch { }
    }
    $downloads = 'Decomp_' + 'Downloads'
    foreach ($drive in @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        try {
            $candidates.Add((Join-Path $drive.Root (Join-Path $downloads (Join-Path 'Jurassic Park' 'versoes'))))
            $candidates.Add((Join-Path $drive.Root (Join-Path $downloads 'Jurassic Park')))
        } catch { }
    }
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        try {
            if ((Test-Path -LiteralPath $candidate -PathType Container) -and
                (@(Get-ChildItem -LiteralPath $candidate -Filter '*.iso' -File -ErrorAction SilentlyContinue).Count)) {
                return (Get-Item -LiteralPath $candidate).FullName
            }
        } catch { }
    }
    return ''
}

# ---------------------------------------------------------------------------------------------------------------------
# The suites
# ---------------------------------------------------------------------------------------------------------------------
$s = 'Build and profiles'
Add-Tool $s 'tools\check_profiles.py' 'every supported build against its lift, profile, guest map, required overrides, offsets and ABI'
Add-Tool $s 'tools\lifts.py verify --content' 'lifts\*.tar.xz against lifts\manifest.json and each archive''s own metadata'
Add-Tool $s 'tools\gva_convert.py --check' 'no raw guest address in src\host: every one goes through the profile map'
Add-Tool $s 'tools\gen_guest_va.py --check' 'src\host\profiles is what the guest maps and profiles generate'
Add-Tool $s 'tools\host_offsets.py' 'config\host_offsets.json is current: every host structure offset proven in every build'
Add-Tool $s 'tools\gen_guest_layout.py' 'structure layouts per lift: no host define disagrees across builds'
Add-Tool $s 'tools\lift_hooks.py' 'every listed lift hook is in the generated lifts'
Add-Tool $s 'tools\stable_entries.py src\recomp\gen' 'every overridable function is a stable entry of the lift'
Add-Exes $s @(
    , @('profile_dispatch_tests', 'which lift each profile runs, its guest addresses, patch groups, layout; a stale lift is refused')
    , @('override_tests_incremental', 'host overrides through stable entries: direct, alias and dispatched calls, refusals (incremental link)')
    , @('override_tests_non_incremental', 'the same fixture linked /INCREMENTAL:NO, as Release links')
)
Add-Python $s @(
    , @('test_build_parity.py', 'supported-build parity: fingerprint, lift, addresses, offsets, overrides, patch sites, features, cheats, saves, launcher identification')
    , @('test_profiles.py', 'version table against profile files, guest maps and lifts')
    , @('test_layout.py', 'host structure offsets per profile; an unknown access fails')
    , @('test_lifts.py', 'the lifts as project artifacts, a clean checkout without any original executable')
    , @('test_lift_hooks.py', 'lift hook list and placement')
    , @('test_trescom_levels.py', 'the custom level catalog tool against a local server: downloads, resume, retries, refusals, relations, reports')
    , @('test_lift_staging.py', 'lift staging, publishing changed files only, cleanup')
    , @('test_stable_entries.py', 'stable-entry renaming and check')
    , @('test_guest_module_lift.py', 'lifted guest DLL (MSVCIRT): config and manifest facts, sorted prefixed table, every jump target a lifted entry, no symbol collision, every code export and every code address held in its data lifted, no TLS, no forwarders')
    , @('test_selfmod_lift.py', 'lifted SelfMod: range, 46 entries equal to the executable, dispatch coverage of every lifted caller, cross-range calls, 44 patch sites as guest loads, nothing unlifted')
    , @('test_linux_artifact.py', 'the Linux artifact: deterministic source fingerprint, self-check against sources and contract, atomic staging that keeps the last validated binary, one canonical path')
    , @('test_linux_native_rules.py', 'Linux host source rules: no native x86 execution or executable mapping, no guest address used as a host pointer')
    , @('test_override_installs.py', 'every literal override install is a listed stable entry')
    , @('test_single_host.py', 'one executable carries every lift')
    , @('test_no_original_executable.py', 'no original executable is needed by the build, launcher, runtime or release')
    , @('test_compile_commands.py', 'ninja -t commands of every test target: no contradicting flags, no warning suppression in project code, /W3 or higher, whether or not anything recompiles')
)
Add-Suite $s 'script' 'tests\build_script_tests.ps1' 'tools\build.ps1 statically: parameters, configurations, -Run arguments, user_data, SDL pin, test targets outside the default build' @{}
Add-Suite $s 'script' 'tests\link_recovery_tests.ps1' 'tools\_link_recovery.ps1 on fake builds: LNK1207 removes the target PDB/ILK and retries once, nothing else' @{}

$s = 'Platform'
Add-Exes $s @(
    , @('presentation_tests', 'window presentation: pending screen committed with the first frame of its generation, Auto window follows the game shape only then, fixed size never, mouse mapping, DPI, degenerate sizes', 100)
    , @('platform_tests', 'SDL 3.4.20 contract, single init and quit, window adoption, relative mouse policy, focus, recapture, DirectSound and A3D policy', 500)
    , @('app_identity_tests', 'SDL application metadata identifier and embedded 256x256 window icon on SDL dummy video driver', 10)
    , @('hotkey_tests', 'the port hotkey table both hosts read: F2 to F11, Alt+F4, repeats, modifiers and the Quick keys winning over the table', 40)
    , @('input_context_tests', 'the input context read from the game window stack (video, menu, loader, level, pause), input state, relative mouse and pointer decisions', 40)
    , @('host_tests', 'input and capture state machine, hotkeys, shutdown, Safe Mode, filesystem mapping, readbacks, D3D6 to D3D11 translation, shaders, hand and physics', 400000)
    , @('engine_heaps_tests', 'the lifted arrays and heaps full, past full and out of memory: the load ends before anything is written, the guard is never reached', 160)
    , @('x87_tests', 'the patched runtime: x87 ring, inline rounding, per-site call cache', 50)
    , @('x64_tests', 'x64 groundwork: guest address space, pointer marshalling, leak detection, COM handles', 0, @{ Arch = 'x64' })
)

$s = 'Input and controls'
Add-Exes $s @(
    , @('input_tests', 'mouse and gamepad through the lifted CInput read, frame by frame; invert; relative deltas', 300)
    , @('gamepad_tests', 'controller families, glyphs, bindings, deadzones, hotplug, disconnect and reconnect, menu focus, prompts', 20000)
    , @('fov_mouse_tests', 'field of view 60-110 and Hor+, mouse sensitivity 0.25-4.00 step 0.05, F7/F8/F5 hotkeys, Alt+Tab, Safe Mode', 700)
)
Add-Python $s @(, @('test_minimap_controls.py', 'F5 and L3 + R3 toggle the minimap; the Debug Menu is gone'))

$s = 'Core runtime'
Add-Exes $s @(
    , @('com_tests', 'DirectDraw and Direct3D 6 COM objects through their vtables, against stubs', 250)
    , @('log_tests', 'normal log without performance lines, --perf-log, independent F9 snapshot, failed_d3d_calls and exit code in the normal log', 15)
)
Add-Python $s @(, @('test_renderer_fixed.py', 'the renderer chosen at start stays until exit'))

$s = 'Memory, filesystem and saves'
Add-Python $s @(, @('test_linux_parity.py', 'application ID, portable folders, AppImage contract, launcher on Linux, setting options, written inventory'))
Add-Exes $s @(, @('user_data_tests', 'portable user_data root, --user_data_root, an unwritable root is an error, migration of the old folder', 150))
Add-Exes $s @(
    , @('vpath_tests', 'guest paths over a case-sensitive host: case-insensitive match, separators, containment, create mode, directory cache', 40)
    , @('reg_store_tests', 'registry value store: round trip, Windows-written file, fixed-width format', 25)
    , @('port_tests', 'port folder, AppImage folder, log folder and its fallback, restart command, application ID', 20)
    , @('sync_tests', 'events, semaphores, mutexes, waits, threads, critical sections, monotonic clock and timers over SDL3', 45)
)

$s = 'Graphics'
Add-Exes $s @(
    , @('ao_tests', 'ambient occlusion levels, configuration and the ASSAO pass on a WARP device', 150)
    , @('ao_fog_tests', 'ambient occlusion and fog through the real Direct3D 11 renderer on a WARP device: levels, vegetation, sky, translucency, gamma, MSAA, resize, device recreation', 150)
    , @('ce_gpu_tests', 'water refraction and enhanced sky shaders on a WARP device', 50)
    , @('video_tests', 'HD video presentation and GPU passes on a WARP device', 300)
    , @('video_focus_tests', 'video focus and pause policy', 20)
    , @('water_tests', 'host water, occlusion, polygon and bump kernels against the lifted functions, byte for byte', 0, @{ QuickArguments = @('--quick') })
    , @('frontend_tests', 'HD front end, original-art menus, loading screens, save pictures, DDF text, retail level previews', 25000)
)

$s = 'Gameplay, CE and Patch 1.1'
Add-Exes $s @(
    , @('ce_tests', 'Trespasser CE features: dual stow, quick save and load, object detail, sky, refraction, footsteps, legacy disk check', 500)
    , @('active_ai_tests', 'Active AI Original 3 and Enhanced 6, pool protection', 80)
    , @('patch117_tests', 'official Patch 1.1: policy, level fingerprints, terrain tolerances, bAddChild, jump, hand, box-box, LZ interception', 800)
)

$s = 'Cheats, menus and objectives'
Add-Exes $s @(, @('debug_menu_tests', 'F5 and F10 panels, cheats table, compass, minimap, objectives, overlay, menu text and layout, physics telemetry', 40000))
Add-Python $s @(
    , @('test_cheat_parity.py', 'every cheat of the table: row, F10 entry, handler, test, translations, addresses in every build')
    , @('test_objectives_retail.py', 'objective policy against the retail levels', $true)
    , @('test_minimap_bounds.py', 'minimap bounds against the retail levels', $true)
    , @('test_minimap_map.py', 'minimap geometry against the retail levels', $true)
)

$s = 'Localization and audio'
Add-Exes $s @(
    , @('l10n_tests', 'seven languages: game text, captions, interface keys, code pages, wrapping, Russian rendering, hard-coded English', 40000)
    , @('stream_tests', 'restored audio records, virtual Stream.tpa, caption timings per language', 4000)
    , @('video_subtitle_tests', 'video subtitle cues, timelines and retail fingerprints per language', 2000)
    , @('menu_audio_tests', 'menu click sound redirect')
    , @('audio_tests', 'SDL3 audio path without a device: DirectSound volume and frequency, menu click -6.02 dB, mixer, loops, 3D math, game PCM formats, no DirectSound or driver name in the backend', 200)
    , @('audio_pacer_tests', 'the queue policy between the mixer and the device on a virtual device: steady, bursts, jitter, drift, stalls, late and early producer, no stale replay, bounded latency; SDL 22050 Hz conversion keeps pitch and length', 60)
    , @('audio_portable_tests', 'platform-independent audio: SDL3 driver policy per platform and SDL_AUDIODRIVER override, start-up log line, PCM streams, voices over guest memory', 60)
    , @('audio_backend_tests', 'the real SDL3 backend on the dummy driver: explicit and headless driver, device thread pulling the mixer, rate change, reopen, unknown driver', 14)
    , @('dsound_compat_tests', 'portable DirectSound over guest vtables in non-host memory: init, buffers, Lock wrap, 3D, object table (forged and stale), threads, shutdown, no leaks', 380)
    , @('hammond_voiceover_tests', 'Hammond voice-over through the DirectSound layer: the real record format, streaming refills from the play cursor, -5 dB, 3D, frequency, one-shot, with music and effects', 100)
    , @('dsound_bridge_tests', '32-bit Windows binding: ABI against dsound.h, stdcall entry points, native calls through the vtable, DSound.dll import gate', 130)
    , @('smacker_tests', 'Smacker HLE: the 14 imported exports, Smack structure, frames, rectangles, 4x4 block rule, Y-interlace, clock, PCM into the shared mixer, binding by platform', 150)
    , @('smacker_ffmpeg_tests', 'FFmpeg Smacker backend boundary: versions, licences, decoders, refused files (DATA UNAVAILABLE without FFmpeg)', 1)
)
Add-Python $s @(
    , @('test_language_parity.py', 'every language has an interface file with English''s keys, game text, captions and a credit')
    , @('test_video_timelines.py', 'dubbed video cue tables', $true)
    , @('test_caption_counts.py', 'caption and restored-audio numbers derived from the data and stated correctly by README and LEGAL')
    , @('test_audio_portability.py', 'portable audio and Smacker layers: no Windows header, type, lock or loader, no pointer in 32 bits, source lists by platform')
)
Add-Suite $s 'check' 'linux audio probe' 'the portable audio and Smacker layers built (-Werror) and run natively on Linux with GCC and Clang, on SDL3 dummy audio (WSL; DATA UNAVAILABLE without it)' @{ Run = 'Test-LinuxAudioProbe' }

$s = 'Launcher'
Add-Exes $s @(, @('launcher_tests', 'pages, settings and migration, render sizes, Hor+, GPU choice, ISO reader, installer, play gate, translations', 9000))
Add-Exes $s @(, @('launcher_startup_tests', 'launcher start without graphics support: renderer order and fallback, --launcher-renderer, graphics card list failures, a failed launcher never starts the game', 120))
Add-Exes $s @(, @('sky_tests', 'custom skies: the file, the layers'' shapes, projection and clipping, package lookup, session', 1000))
Add-Exes $s @(, @('texhw_tests', 'custom level pictures (BMP, TGA, DDS) and their hardware texture overrides', 60))
Add-Exes $s @(, @('custom_level_tests', 'custom levels: ZIP, 7z and folder import, unsafe paths, links, limits, rollback, duplicates, listing, README, removal, and the file overlay of an active package on Windows and Linux paths; package analysis: fm.ini, tp_mod.ini, trespasser.ini, tpass.ini, readme rules, manifest and migration, added sound files, loading pictures, session settings', 400))
$s = 'Custom level session'
Add-Exes $s @(, @('custom_level_session_tests', 'an active custom level session as the game reads it: BMP layouts, ATX map pictures below the package''s MAPS (map actions, README, case, separators, traversal, absent or unreadable pictures, session end); Restart Level as a full level cycle over a fake engine and real packages (loose and fan mission, scenes 22, 25 and 26, own PID and SWP, game SPZ, texhw, MAPS, added TPA, custom to custom and custom to retail transitions, restart after a transition and twice, a save made before, the campaign afterwards); level guides: minimap and objectives made from a level''s world, the campaign''s only over the game''s own world, no information otherwise', 250))

$s = 'Restart lifecycle'
Add-Exes $s @(, @('level_actions_tests', 'the actions of ATX and Trespasser CE over a fake guest (speed, hint, controls, reload, map, teleport, particles, saves, session end) and the real Restart Level override over a fake engine: the game''s own restart keeps the world, a custom session purges and loads the level whole (new player, objects, triggers, AI, actions, world generation), after transitions and twice; the campaign''s restart unchanged', 600))

$s = 'Session logging'
Add-Exes $s @(, @('level_session_tests', 'the [session], [level] and [restart] lines: campaign and custom sessions, reasons new_game/initial/transition/restart/load_save, package or retail source, world generations, failed loads and restarts, no line without a lifecycle event', 40))

$s = 'CE scripts'
Add-Exes $s @(, @('script_vm_tests', 'the interpreter of Trespasser CE scripts (Squirrel): loading and calls, compile and runtime errors with file, line and function, an owner stopped until the level ends, the instruction budget (loops, recursion, callbacks, try and catch cannot keep a script running), only the math, string and blob libraries (no I/O, system, threads, debug hooks or error handler), print() and its cap, no log line forged by a script', 1500))

$s = 'Launch handoff'
Add-Exes $s @(, @('launch_handoff_tests', 'PLAY to the game window without opening one: campaign and custom, slow and fast starts, a created but not ready window, black frame and first present, failure before the window, no way back after PLAY, D3D11 and Vulkan alike; never a moment without one of our windows, no launcher input after PLAY', 200))

$s = 'Launcher'
Add-Python $s @(, @('test_setting_parity.py', 'launcher rows and runtime settings agree: parsers, defaults, ranges, lists, readers'))
Add-Python $s @(, @('test_player_polish.py', 'gamepad icon in the executable and both windows, numbered Level Select, Bug Report page and Worker, auto update wiring and release contract, Windows and Linux'))
Add-Exes $s @(
    , @('bug_report_tests', 'Bug Report page without a window or network: JSON, service address, log choice, privacy filter, report and preview, client against a stand-in transport, My Reports, nothing sent on its own', 500)
    , @('crash_dump_tests', 'Windows crash minidump naming, non-overwrite numbering and MDMP signature write', 15)
    , @('update_tests', 'auto update: versions, release JSON, SHA-256, ZIP safety and staging, jobs, install and rollback, address policy, protected paths and the release asset contract', 250)
    , @('update_network_tests', 'auto update failures against a scripted loopback HTTP server: 504, dropped connections, stalls, redirects, retries', 20)
    , @('update_helper_tests', 'the update helper as a real process: waits for its parent, replaces files, rolls back, restarts the program', 10)
)
Add-Suite $s 'script' 'tests\worker_script_tests.ps1' 'tools\worker.ps1 against a stand-in npx: arguments, dry run, missing folder, missing Wrangler configuration; nothing is deployed' @{}
Add-Suite $s 'check' 'bug report worker' 'server\trespasser-bug-reports: its own node test suite (API, limits, reports, admin, dashboard); DATA UNAVAILABLE without Node.js' @{ Run = 'Test-BugReportWorker' }

$s = 'Vulkan renderer'
Add-Exes $s @(
    , @('vulkan_policy_tests', 'Vulkan 1.2 policy: renderer/auto backend, GPU selection & stable IDs, capability fallbacks, present mode, swapchain stress, pipeline key canonicalization, barriers, geometry, SPIR-V bundle', 40)
    , @('vulkan_render_tests', 'Vulkan 1.2 headless offscreen parity fixtures: textures, alpha test, alpha blend, fog, Z-overlap, water refraction, enhanced sky, AO, readback, the last presented frame kept for the pause, GPU time of frames that submitted early, 0-leak resource lifetime', 20)
    , @('vulkan_dispatch_tests', 'Windows runtime Vulkan backend factory dispatch: requested/chosen/active semantics, explicit Vulkan no-silent-fallback, D3D11 guard, Vulkan/D3D11/Software/Auto vtable wiring', 10)
    , @('vulkan_runtime_tests', 'Windows Vulkan backend driven with the game''s Direct3D 6 sequence into an offscreen target: textures, texture-blend parity with the Direct3D reference, viewport, cull, depth, fog, dirty tracking, software polygons composited and read back, the pause background kept from the last presented frame, GPU perspective and bump polygons pixel for pixel, generated mipmap chains, the CPU time accounting and the backend overhead per draw; draws without textures, pipelines or draw calls fail', 40)
    , @('video_vulkan_tests', 'Vulkan video passes against the Direct3D 11 passes on shared synthetic frames (the images video_tests wrote): conversion and palette, smoothing, deblocking, newgame line reconstruction, stray row, side glow and the final Catmull-Rom filter, each within two levels and each failing without its stage; no validation errors', 40)
    , @('renderer_benchmark_d3d11_tests', 'Direct3D 11 backend on the shared synthetic Direct3D 6 command stream, offscreen: warm-up then 240 sampled frames', 200)
    , @('renderer_benchmark_vulkan_tests', 'Vulkan backend on the same stream, offscreen: warm-up then 240 sampled frames, submit and flip timed apart', 200)
)
Add-Python $s @(, @('test_vulkan_shaders.py', 'Vulkan 1.2 GLSL 450 shader sources, SPIR-V header/entry-point validation, bundle SHA-256 and glslangValidator/spirv-val parity'))
Add-Python $s @(, @('test_renderer_benchmark.py', 'Direct3D 11 against Vulkan on one command stream: same stream hash, median/p95/p99 per frame, ratios, bounded Vulkan overhead, a changed stream noticed'))
Add-Python $s @(, @('test_run_freshness.py', 'run.ps1 refuses an executable older than the sources; the Vulkan bridge logs its build provenance and the regional target sync'))

$s = 'Release contract'
Add-Suite $s 'check' 'release contract' 'version, licenses, README, forbidden files in the release checker, assets, no developer paths compiled into the program' @{ Run = 'Test-ReleaseContract' }
Add-Suite $s 'script' 'tools\test-release-check.ps1' 'verify-release.ps1 on synthetic folders and ZIPs: refuses user data, saves, logs, cache, game data, PDBs, temporary files, DEBUG_SAULO and developer paths' @{}
Add-Python $s @(, @('test_linux_packaging.py', 'Linux AppImage packaging: names from config/release.json and project VERSION, AppDir content, AppRun, desktop, icon, AppStream, runtime pin and FUSE fallback, ELF/RPATH/GLIBC audit, four release files, documentation'))
if ($Release) {
    Add-Suite 'Release artifacts' 'check' 'dist artifacts' 'the folder and the ZIP already in dist\ pass tools\verify-release.ps1 and hold the same files' @{ Run = 'Test-ReleaseArtifacts' }
} else {
    Add-NotRun 'Release artifacts' 'release artifacts in dist' 'only with -Release; nothing is packaged by this script'
}

$s = 'Repository and static gates'
Add-Suite $s 'script' 'tests\gate_tests.ps1' 'this script: parses, derives paths from the repository, -List, -Filter, failure and missing dependencies propagate, a forgotten test is detected' @{}
Add-Suite $s 'check' 'feature coverage' 'every feature of tests\feature_contract.json has a suite that exists and names it; every suite owns a feature' @{ Run = 'Test-FeatureContract' }
Add-Suite $s 'check' 'every test is wired in' 'each test target, script, source and checker tool is run by this script or excluded with a reason' @{ Run = 'Test-Coverage' }
Add-Suite $s 'check' 'repository hygiene' 'no markers in strings, local paths, credentials, game data, logs, saves, build output or conflict markers' @{ Run = 'Test-Hygiene' }
Add-Suite $s 'check' 'git diff --check' 'no whitespace errors or conflict markers in changes, staged or not, nor in new files' @{ Run = 'Test-GitWhitespace' }
Add-Suite $s 'check' 'working tree unchanged' 'no tracked, staged or untracked file changed, and game\ and user_data untouched' @{ Run = 'Test-TreeUnchanged'; Always = $true }

$excludedTargets = @{}
$helperTestFiles = @{
    'tests\renderer_workload.h' = 'command stream shared by renderer_benchmark_d3d11_tests and renderer_benchmark_vulkan_tests'
    'tests\video_fixtures.h' = 'synthetic video frames shared by video_tests and video_vulkan_tests'
    'tests\test_profile.h' ='shared header: activates en-116 for the host-code tests'
    'tests\override_fixture.c' = 'fixture linked into both override_tests targets'
    'tests\override_fixture_list.h' = 'fixture list included by override_fixture.c'
    'tests\lift_resolution_check.cmake' = 'CMake script run by the build to check lift resolution'
    'tests\feature_contract.json' = 'the feature contract read by the feature coverage check'
    'tests\dsound_guest_harness.h' = 'shared harness of dsound_compat_tests and hammond_voiceover_tests'
    'tests\dsound_abi_pairs.h' = 'list of ABI values compared by dsound_bridge_tests and dsound_windows_abi.c'
    'tests\linux_fake_guest.h' = 'guest functions in the lifter style used by the Linux call spine tests'
    'tests\linux_win32_harness.h' = 'shared set-up of the Linux user32, gdi32 and ddraw suites'
    'tests\fault_guard.h' = 'shared helper of x64_tests: access-violation probes on Windows and POSIX'
    'tests\linux_suites.json' = 'classification of the suites for the Linux gate (tools/linux_gate.py)'
    'tests\smk_fixture.h' = 'shared generator of a synthetic Smacker file: smacker_ffmpeg_tests'
    'tests\level_fixtures.h' = 'shared generator of synthetic scenes, worlds and texture sets: custom_level_tests and custom_level_session_tests'
    'tests\texhw_fixture.h' = 'shared generator of synthetic texhw pictures: texhw_tests'
}
$toolRole = @{
    'tools\_common.ps1' = 'helper dot-sourced by the tool scripts'
    'tools\_link_recovery.ps1' = 'dot-sourced by tools\build.ps1; exercised by tests\link_recovery_tests.ps1'
    'tools\_gate_linux.ps1' = 'the Linux half of this script: WSL discovery, the Linux gate run, progress and the feature parity; dot-sourced'
    'tools\linux_gate.py' = 'the Linux gate runner: run in WSL by tools/test-linux.sh'
    'tools\linux_probe.sh' = 'what a WSL distribution offers the Linux gate: run in WSL by this script'
    'tools\build.ps1' = 'builds the game: never run by the gate; checked statically by tests\build_script_tests.ps1'
    'tools\run.ps1' = 'starts the game: never run by the gate'
    'tools\generate-lift.ps1' = 'maintainer: regenerates a lift from an original executable'
    'tools\verify-release.ps1' = 'release checker: exercised by tools\test-release-check.ps1'
    'tools\test.ps1' = 'this script'
    'tools\abi_check.py' = 'maintainer: needs the original executables'
    'tools\analyze.py' = 'maintainer: regenerates analysis\ from the original executable'
    'tools\class_layout.py' = 'maintainer: needs the original executables'
    'tools\common.py' = 'helper module of the python tools'
    'tools\compile_vk_shaders.py' = 'compiles src/vulkan/shaders/*.{vert,frag} to Vulkan 1.2 SPIR-V and validates with spirv-val; checked by tests\test_vulkan_shaders.py'
    'tools\gate_support.py' = 'helper module of the contract suites'
    'tools\extract_game.py' = 'maintainer: extracts a disc into game\'
    'tools\fetch_pcrecomp.py' = 'maintainer and build: fetches pcrecomp (network)'
    'tools\guest_map.py' = 'maintainer: needs the original executables'
    'tools\gva_convert.py' = 'run with --check'
    'tools\iso9660.py' = 'library of the disc tools'
    'tools\layout_diff.py' = 'maintainer: needs the original executables'
    'tools\lift_staging.py' = 'library: tested by tests\test_lift_staging.py'
    'tools\lifts.py' = 'run with verify'
    'tools\make-header-art.py' = 'maintainer: regenerates a launcher asset'
    'tools\make_game_manifests.py' = 'maintainer: reads the player''s disc images'
    'tools\make_profile.py' = 'maintainer: needs an original executable'
    'tools\minimap_bounds.py' = 'generator: its output is checked by tests\test_minimap_bounds.py'
    'tools\minimap_map.py' = 'generator: its output is checked by tests\test_minimap_map.py'
    'tools\run_lift.py' = 'maintainer: lifts an executable'
    'tools\scn_triggers.py' = 'maintainer: dumps level triggers'
    'tools\smk_inventory.py' = 'maintainer: inventories installed videos'
    'tools\smk_speech.py' = 'maintainer: decodes video audio with ffmpeg'
    'tools\x64_selfmod_scan.py' = 'maintainer: reads the original executable'
    'tools\stream_fix_pack.cpp' = 'developer tool target, not a test'
    'tools\translation_pack.cpp' = 'developer tool target, not a test'
    'tools\extract_lifted.cmake' = 'build helper'
    'tools\linux_audio_probe.sh' = 'run in WSL by the linux audio probe check'
    'tools\build-linux.sh' = 'builds the Linux executable: Linux only, never run by this gate'
    'tools\source_fingerprint.py' = 'the deterministic source fingerprint embedded in the Linux executable; run by the Linux gate, checked by tests\test_linux_artifact.py'
    'tools\gen_build_info.py' = 'writes the Linux executable provenance header during its build; checked by tests\test_linux_artifact.py'
    'tools\linux_artifact.py' = 'the Linux artifact: self-check against sources and contract, atomic staging; run by tools/linux_gate.py and this script, checked by tests\test_linux_artifact.py'
    'tools\package-linux.sh' = 'packaging entry: verifies the canonical Linux Release artifact and writes the AppImage; run by tools/package-release-linux.sh, never by this gate'
    'tools\package-release-linux.sh' = 'maintainer: mirrors the sources into the WSL build folder and runs package-linux.sh; started by package-release.ps1, never by this gate'
    'tools\build-appimage-runtime.sh' = 'pinned AppImage Type 2 runtime with the FUSE fallback patch; run by package-linux.sh, needs root or passwordless sudo'
    'tools\test-linux.sh' = 'the Linux gate: Linux only, run in WSL by this script or on Linux by hand'
    'tools\test-linux-parity.sh' = 'the Linux parity gate: the static contracts of tests\test_linux_parity.py, then tools/test-linux.sh; Linux only, run in WSL or by hand'
    'tools\linux_parity_report.py' = 'writes and checks docs\LINUX_PARITY.md from the feature contract; checked by tests\test_linux_parity.py'
    'tools\fetch_launcher_deps.py' = 'maintainer and Linux build: fetches SDL3, Dear ImGui, TRG Launcher and Squirrel at their pinned commits (network), like Sync-LauncherDependencies of tools\build.ps1'
    'tools\make-app-icon.py' = 'maintainer: renders packaging\icons\app-icon.svg into the PNGs and the ICO (--check verifies them); checked by tests\test_app_icon.py'
    'tools\build-updater.ps1' = 'maintainer: builds the CLANG64 x64 release helper; run by tools/package.ps1, never by this gate'
    'tools\build-clang-cl.ps1' = 'maintainer: builds the clang-cl x86 Release executable; never run by this gate'
    'tools\package.ps1' = 'maintainer: builds the clang-cl Release and the CLANG64 helper, stages the Windows release ZIP: never run by the gate; checked statically by tests\test_player_polish.py and through tools\verify-release.ps1'
    'tools\package-release.ps1' = 'maintainer: package.ps1 plus the Linux AppImage through WSL and the final gates on both: never run by the gate; checked by tests\test_player_polish.py'
    'tools\worker.ps1' = 'maintainer: deploys the bug report Worker with Wrangler (network); exercised by tests\worker_script_tests.ps1 with a stand-in npx'
    'tools\ffmpeg-bootstrap.sh' = 'a pinned LGPL-only FFmpeg for the Smacker tests, built once into the cache: run by tools/test-linux.sh when the machine has none'
    'tools\openssl-bootstrap.sh' = 'a pinned static OpenSSL for the Bug Report HTTPS, built once into the cache: run by tools/build-linux.sh when the machine has no development files'
    'tools\linux_elf_check.py' = 'static checks of a Linux executable, run by tools/test-linux.sh'
    'tools\linux_symbolize.sh' = 'maintainer: symbolizes the host addresses of a Linux crash report'
    'tools\custom_level_scan.cpp' = 'developer tool target (custom_level_scan): the headless report of a folder of custom level packages, not a test'
    'tools\analyze-custom-level.ps1' = 'maintainer: prints what the import makes of one package; never run by the gate'
    'tools\custom-level-compatibility.ps1' = 'maintainer: runs custom_level_scan over a folder of packages; never run by the gate'
    'tools\download-trescom-levels.ps1' = 'maintainer: starts tools\download-trescom-levels.py (network); never run by the gate'
    'tools\download-trescom-levels.py' = 'maintainer: catalogs and downloads a TresCom category (network); its modules are tested by tests\test_trescom_levels.py against a local server'
    'tools\trescom_site.py' = 'library of download-trescom-levels.py: tested by tests\test_trescom_levels.py'
    'tools\trescom_fetch.py' = 'library of download-trescom-levels.py: tested by tests\test_trescom_levels.py'
    'tools\trescom_catalog.py' = 'library of download-trescom-levels.py: tested by tests\test_trescom_levels.py'
    'tools\trescom_report.py' = 'library of download-trescom-levels.py: tested by tests\test_trescom_levels.py'
    'tools\trescom_corpus.py' = 'library of trescom_compatibility.py: tested by tests\test_trescom_levels.py'
    'tools\trescom_compatibility.py' = 'maintainer: the compatibility report of a downloaded corpus through custom_level_scan; its rules are tested by tests\test_trescom_levels.py'
    'tools\linux_inventory.py' = 'maintainer: classifies the guest imports for the Linux port'
    'tools\linux_lifted_link_report.py' = 'maintainer: what the lifted guest needs from a Linux host, run by tools/test-linux.sh'
    'tools\import-appimage-runtime.sh' = 'packaging helper: copies the pinned AppImage runtime from a WSL packaging cache; run by tools\package-release.ps1 in WSL, checked by tests\test_linux_packaging.py, never by this gate'
    'tools\release-inputs.ps1' = 'packaging helper dot-sourced by tools\package-release.ps1: the reviewed AppImage runtime and the WSL build environment; checked by tests\test_linux_packaging.py, never run by this gate'
    'tools\linux_suite_scan.py' = 'maintainer: which suites compile on Linux'
    'tools\lift_module.py' = 'maintainer: lifts the guest DLLs of config/guest_modules.json; run by tools/run_lift.py, checked by tests\test_guest_module_lift.py'
    'tools\lift_selfmod.py' = 'maintainer: lifts the SelfMod section; run by tools/run_lift.py, checked by tests\test_selfmod_lift.py'
}

function Group-InOrder([object[]]$Items) {
    $order = New-Object System.Collections.Generic.List[string]
    $map = @{}
    foreach ($item in $Items) {
        if (-not $map.ContainsKey($item.Section)) { $map[$item.Section] = New-Object System.Collections.Generic.List[object]; $order.Add($item.Section) }
        $map[$item.Section].Add($item)
    }
    foreach ($name in $order) { [pscustomobject]@{ Name = $name; Group = $map[$name].ToArray(); Count = $map[$name].Count } }
}

$allSuites = $suites.ToArray()
if ($Filter) {
    $suites = New-Object System.Collections.Generic.List[object] (, @($allSuites | Where-Object { $_.Options['Always'] -or "$($_.Section) $($_.Name) $($_.Description)" -like "*$Filter*" }))
}

if ($List) {
    foreach ($group in (Group-InOrder $suites)) {
        Write-Host "==> $($group.Name)" -ForegroundColor Cyan
        foreach ($suite in $group.Group) { Write-Host ('  {0,-44} {1}' -f $suite.Name, $suite.Description) }
    }
    if (-not $Filter) { foreach ($entry in $notRunSuites) { Write-Host ('  {0,-44} NOT RUN: {1}' -f $entry.Name, $entry.Summary) -ForegroundColor Yellow } }
    Write-Host ('{0} suites in {1} sections' -f $suites.Count, @(Group-InOrder $suites).Count)
    foreach ($name in @(Get-LinuxSuiteNames | Where-Object { -not $Filter -or $_ -like "*$Filter*" })) { Write-Host ("    linux  {0}" -f $name) -ForegroundColor DarkGray }
    exit 0
}

if ($Filter -and @($suites | Where-Object { -not $_.Options['Always'] }).Count -eq 0 -and
    -not ($runLinux -and @(Get-LinuxSuiteNames | Where-Object { $_ -like "*$Filter*" }).Count)) {
    Write-Host "FAILED: no suite matches '$Filter' (tools\test.ps1 -List shows them)" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------------------------------------------------
# Running things
# ---------------------------------------------------------------------------------------------------------------------
$tempDir =Join-Path ([IO.Path]::GetTempPath()) "jpt-test-$PID"
$reportDir = Join-Path $tempDir 'report'

function ConvertTo-CmdArg([string]$Text) {
    if ($Text -eq '' -or $Text -match '[\s"&|<>^()]') { return '"' + ($Text -replace '"', '""') + '"' }
    return $Text
}

function Start-Proc([string]$File, [string[]]$Arguments = @(), [string]$WorkDir = $root, [int]$TimeoutSec = 900) {
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    $out = Join-Path $tempDir ('out-{0}.txt' -f [guid]::NewGuid().ToString('N'))
    $command = (@($File) + $Arguments | ForEach-Object { ConvertTo-CmdArg $_ }) -join ' '
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = '/d /s /c "{0} < nul > "{1}" 2>&1"' -f $command, $out
    $info.WorkingDirectory = $WorkDir
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    # cmd.exe truncates a PATH over 8191 characters and then cannot find wsl.exe: child processes get it de-duplicated.
    $info.EnvironmentVariables['PATH'] = Get-CleanPathValue $env:PATH
    $process = [System.Diagnostics.Process]::Start($info)
    return [pscustomobject]@{ Process = $process; Out = $out; Command = $command; WorkDir = $WorkDir; Timeout = $TimeoutSec; Clock = [System.Diagnostics.Stopwatch]::StartNew() }
}

function Wait-Proc($Handle) {
    $remaining = [Math]::Max(1, $Handle.Timeout * 1000 - [int]$Handle.Clock.ElapsedMilliseconds)
    $timedOut = -not $Handle.Process.WaitForExit($remaining)
    if ($timedOut) {
        & taskkill.exe /PID $Handle.Process.Id /T /F 2>&1 | Out-Null
        $Handle.Process.WaitForExit()
    }
    $code = $Handle.Process.ExitCode
    $seconds = if ($timedOut) { $Handle.Clock.Elapsed.TotalSeconds } else { ($Handle.Process.ExitTime - $Handle.Process.StartTime).TotalSeconds }
    $Handle.Process.Dispose()
    $lines = @()
    if (Test-Path -LiteralPath $Handle.Out) {
        $text = [IO.File]::ReadAllText($Handle.Out, [Text.Encoding]::UTF8)
        if ($text.Length -gt 0) { $lines = @($text -split "\r?\n") }
        if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = @($lines[0..($lines.Count - 2)]) }
        Remove-Item -LiteralPath $Handle.Out -Force -ErrorAction SilentlyContinue
    }
    if ($timedOut) { $lines += "TIMEOUT: stopped after $($Handle.Timeout) s"; $code = 124 }
    return [pscustomobject]@{ Code = $code; Lines = $lines; Command = $Handle.Command; WorkDir = $Handle.WorkDir; Seconds = $seconds }
}

function Invoke-Process([string]$File, [string[]]$Arguments = @(), [string]$WorkDir = $root, [int]$TimeoutSec = 900) {
    return Wait-Proc (Start-Proc $File $Arguments $WorkDir $TimeoutSec)
}

function New-Result([int]$Code, [string[]]$Lines) { return [pscustomobject]@{ Code = $Code; Lines = @($Lines); Command = '' } }

function Get-GitLines([string[]]$Arguments) { return Invoke-Process 'git' (@('-C', $root) + $Arguments) $root 120 }

$script:Python = $false
function Get-PythonProblem {
    if ($script:Python -eq $false) { $script:Python = Get-Python }
    if (-not $script:Python) { return 'Python 3 was not found (the py launcher or python on PATH); install it from python.org' }
    return $null
}

function Get-MsvcProblem {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return 'Visual Studio with "Desktop development with C++" was not found (vswhere.exe); the test programs cannot be built' }
    $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if (-not $found) { $found = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
    if (-not $found) { return 'no Visual Studio with the MSVC x86/x64 tools was found; the test programs cannot be built' }
    return $null
}

function Get-TestTargetsOfCMake {
    $text = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
    return @([regex]::Matches($text, 'add_executable\(\s*([A-Za-z0-9_]+_tests)\b') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
}

# --- checks ------------------------------------------------------------------------------------------------------

function Get-CommentFreeText([string]$Text) {
    $Text = [regex]::Replace($Text, '/\*.*?\*/', ' ', 'Singleline')
    return [regex]::Replace($Text, '//[^\r\n]*', ' ')
}

function Get-RepositoryFiles {
    $tracked = @((Get-GitLines @('ls-files')).Lines | Where-Object { $_ })
    $untracked = @((Get-GitLines @('ls-files', '--others', '--exclude-standard')).Lines | Where-Object { $_ })
    return @($tracked + $untracked | Sort-Object -Unique)
}

$testExtensions = '.c', '.h', '.cpp', '.inc', '.py', '.ps1', '.cmake', '.md', '.txt', '.json', '.yml', '.in'
$sourceExtensions = '.c', '.h', '.cpp', '.inc'
$hygieneNamedBy = 'tools/test.ps1', 'tests/gate_tests.ps1', 'tools/verify-release.ps1', 'tools/test-release-check.ps1',
    'tools/package-linux.sh', 'tests/test_linux_packaging.py', 'tools/build-updater.ps1', 'tools/build-clang-cl.ps1',
    'docs/user-data.md', 'tests/feature_contract.json', 'tests/build_script_tests.ps1', 'tests/stream_tests.c',
    'tests/test_no_original_executable.py', 'tests/launcher_tests.cpp', 'tests/patch117_tests.c', 'tests/video_subtitle_tests.c',
    'tests/bug_report_tests.cpp'

function Test-Hygiene {
    $problems = New-Object System.Collections.Generic.List[string]
    $files = Get-RepositoryFiles
    if (-not $files.Count) { return New-Result 1 @('git listed no files: is this a repository?') }
    $forbidden = '(?i)^(game|user|user_data|cache|logs|build|dist|out|external)/|(^|/)SAVEGAME\.|\.(log|iso|tpa|scn|grf|smk|wtd|spz|pdb|ilk|dmp|obj|pid|bak|tmp|7z|sav|pfx|pem)$|(^|/)(trespass\.exe|guest\.img|recomp-registry\.txt|trespasser\.ini)$'
    foreach ($relative in $files) { if ($relative -match $forbidden) { $problems.Add("${relative}: game data, logs, saves, build output or tool state do not belong in the repository") } }
    $absolute = '(?i)\b[A-Z]:[\\/]+(Users|VisualStudio)[\\/]|' + ('Decomp_' + 'Downloads')
    $secret = 'ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{20,}|sk-[A-Za-z0-9]{32,}|-----BEGIN [A-Z ]*PRIVATE KEY-----'
    $checked = 0
    $markers = 0
    $markerFiles = New-Object System.Collections.Generic.HashSet[string]
    foreach ($relative in $files) {
        $extension = [IO.Path]::GetExtension($relative).ToLowerInvariant()
        if ($testExtensions -notcontains $extension) { continue }
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $checked++
        $named = $hygieneNamedBy -contains $relative
        $text = [IO.File]::ReadAllText($path)
        $number = 0
        foreach ($line in ($text -split "\r?\n")) {
            $number++
            if (-not $named) {
                if ($line -match $absolute) { $problems.Add("${relative}:${number}: a path of one machine: $($Matches[0])") }
                if ($line -match '^(<<<<<<<|>>>>>>>) ') { $problems.Add("${relative}:${number}: conflict marker") }
            }
            if ($line -match $secret) { $problems.Add("${relative}:${number}: looks like a credential") }
        }
        if (-not $named -and $text.Contains('DEBUG_SAULO')) {
            if ($sourceExtensions -contains $extension) {
                $code = Get-CommentFreeText $text
                if ($code.Contains('DEBUG_SAULO')) { $problems.Add("${relative}: DEBUG_SAULO outside a comment (a string or code compiled into the program)") }
                else { $markers += ([regex]::Matches($text, 'DEBUG_SAULO')).Count; [void]$markerFiles.Add($relative) }
            } else {
                $problems.Add("${relative}: DEBUG_SAULO in a file that is not source")
            }
        }
    }
    if ($problems.Count) { return New-Result 1 @($problems | Select-Object -First 40) }
    $lines = @("$checked text files and $($files.Count) paths clean")
    if ($markers) { $lines += "skipped: $markers temporary DEBUG_SAULO comment markers remain in $($markerFiles.Count) source files (comments only; none in strings, code or assets)" }
    return New-Result 0 $lines
}

function Test-GitWhitespace {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return New-Result 1 @('git was not found: it is needed for git diff --check') }
    $lines = New-Object System.Collections.Generic.List[string]
    $code = 0
    foreach ($arguments in @('-c', 'core.whitespace=cr-at-eol', 'diff', '--check'), @('-c', 'core.whitespace=cr-at-eol', 'diff', '--cached', '--check')) {
        $result = Get-GitLines $arguments
        if ($result.Code -ne 0) { $code = 1; $lines.Add("git $($arguments[2..($arguments.Count - 1)] -join ' ') reported problems:"); $result.Lines | ForEach-Object { $lines.Add($_) } }
    }
    $checked = 0
    foreach ($relative in @((Get-GitLines @('ls-files', '--others', '--exclude-standard')).Lines | Where-Object { $_ })) {
        if ($testExtensions -notcontains [IO.Path]::GetExtension($relative).ToLowerInvariant()) { continue }
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $checked++
        $number = 0
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            $number++
            if ($line -match '[ \t]$') { $code = 1; $lines.Add("${relative}:${number}: trailing whitespace (new file)") }
            elseif ($line -match '^(<<<<<<<|>>>>>>>) ') { $code = 1; $lines.Add("${relative}:${number}: conflict marker (new file)") }
        }
    }
    if ($code -eq 0) { $lines.Add("clean, plus $checked new text files") }
    return New-Result $code @($lines)
}

function Invoke-GitCapture([string[]]$Arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = 'git'
    $info.Arguments = ((@('-C', $root, '--no-optional-locks') + $Arguments) | ForEach-Object { ConvertTo-CmdArg $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['GIT_OPTIONAL_LOCKS'] = '0'
    $process = [System.Diagnostics.Process]::Start($info)
    $errors = $process.StandardError.ReadToEndAsync()
    $buffer = New-Object System.IO.MemoryStream
    $process.StandardOutput.BaseStream.CopyTo($buffer)
    $process.WaitForExit()
    $code = $process.ExitCode
    $process.Dispose()
    return [pscustomobject]@{ Code = $code; Bytes = $buffer.ToArray(); Errors = $errors.Result.Trim() }
}

function Get-GitStdout([string[]]$Arguments) {
    for ($attempt = 1; $attempt -le 4; $attempt++) {
        $result = Invoke-GitCapture $Arguments
        if ($result.Code -eq 0) { return $result }
        Start-Sleep -Milliseconds 250
    }
    throw "git $($Arguments -join ' ') failed with exit code $($result.Code): $($result.Errors)"
}

function Get-BytesSha256([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() } finally { $sha.Dispose() }
}

# Files up to 8 MB are compared by content; larger ones by size and modification time, and reported as unverified.
function Get-FileSignature([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return 'missing' }
    if ($item.Length -le 8MB) { return "$($item.Length) bytes, sha256 $((Get-Sha256 $Path).Substring(0, 16))" }
    return "$($item.Length) bytes, large file, modified $($item.LastWriteTimeUtc.Ticks)"
}

function Get-TreeSnapshot {
    $files = @{}
    $status = (Get-GitStdout @('status', '--porcelain=v1', '-uall')).Bytes
    foreach ($line in ([Text.Encoding]::UTF8.GetString($status) -split "\r?\n")) {
        if ($line.Length -lt 4) { continue }
        $relative = $line.Substring(3).Trim('"') -replace '/', '\'
        $files["git\$relative"] = "status $($line.Substring(0, 2)); $(Get-FileSignature (Join-Path $root $relative))"
    }
    $files['git diff'] = Get-BytesSha256 (Get-GitStdout @('diff', '--binary')).Bytes
    $files['git diff --cached'] = Get-BytesSha256 (Get-GitStdout @('diff', '--cached', '--binary')).Bytes
    foreach ($folder in 'game', 'user_data', 'build\user_data') {
        $path = Join-Path $root $folder
        if (-not (Test-Path -LiteralPath $path)) { $files["$folder\"] = 'absent'; continue }
        foreach ($item in Get-ChildItem -LiteralPath $path -Recurse -Force -File -ErrorAction SilentlyContinue) {
            $files[$item.FullName.Substring($root.Length + 1)] = Get-FileSignature $item.FullName
        }
    }
    return $files
}

function Compare-TreeSnapshots($Before, $After) {
    $changes = New-Object System.Collections.Generic.List[string]
    foreach ($name in (@($Before.Keys) + @($After.Keys) | Sort-Object -Unique)) {
        $was = $Before[$name]
        $now = $After[$name]
        if ($was -eq $now) { continue }
        $kind = if ($null -eq $was) { 'appeared' } elseif ($null -eq $now) { 'disappeared' } else { 'changed' }
        $changes.Add("${name}: $kind; before: $(if ($null -eq $was) { '-' } else { $was }); after: $(if ($null -eq $now) { '-' } else { $now })")
    }
    return @($changes)
}

function Get-DescendantProcesses {
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | ForEach-Object { [pscustomobject]@{ Name = [string]$_.Name; ProcessId = [int]$_.ProcessId; ParentId = [int]$_.ParentProcessId } })
    $found = @()
    $frontier = @([int]$PID)
    while ($frontier.Count) {
        $children = @($all | Where-Object { $frontier -contains $_.ParentId -and $_.ProcessId -ne $PID })
        $found += $children
        $frontier = @($children | ForEach-Object { $_.ProcessId })
    }
    return $found
}

function Test-TreeUnchanged {
    $lines = @()
    $code = 0
    $changes = @(Compare-TreeSnapshots $script:SnapshotBefore (Get-TreeSnapshot))
    if ($script:ImportedVersionPaths.Count) {
        $changes = @($changes | Where-Object {
            $path = ($_ -split ':', 2)[0].ToLowerInvariant()
            -not (@($script:ImportedVersionPaths | Where-Object { $path.StartsWith($_.ToLowerInvariant()) }).Count)
        })
    }
    if ($changes.Count) {
        $code = 1
        $lines += "$($changes.Count) file(s) differ from the snapshot taken before the first suite (git status -uall, git diff --binary, git diff --cached --binary, game\, user_data, build\user_data):"
        $lines += @($changes | Select-Object -First 40 | ForEach-Object { "  $_" })
    }
    $running = @(Get-DescendantProcesses | Where-Object { $_.Name -notin 'conhost.exe' })
    if ($running.Count) { $code = 1; $lines += "processes started by the gate are still running: $((@($running | ForEach-Object { "$($_.Name) ($($_.ProcessId))" })) -join ', ')" }
    if ($code -eq 0) { $lines = @('git state, game\ and user_data identical before and after; no test process left running') }
    return New-Result $code $lines
}

function Test-Coverage {
    $problems = New-Object System.Collections.Generic.List[string]
    $runTargets = @($allSuites | Where-Object { $_.Kind -eq 'exe' } | ForEach-Object { $_.Name })
    $known = @(Get-TestTargetsOfCMake)
    $notes = @()
    $ninjaListed = $false
    if (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) {
        $ninja = Get-Command ninja -ErrorAction SilentlyContinue
        if ($ninja) {
            $listed = Invoke-Process 'ninja' @('-C', $buildDir, '-t', 'targets', 'all') $root 120
            if ($listed.Code -eq 0) {
                $ninjaListed = $true
                $known = @($known + @($listed.Lines | ForEach-Object { if ($_ -match '^(?!cmake_)([A-Za-z0-9_]*_tests[A-Za-z0-9_]*):') { $Matches[1] } }) | Sort-Object -Unique)
            }
        } else { $notes += 'skipped: ninja is not on PATH, so only the targets named in CMakeLists.txt were compared' }
    } else { $notes += 'skipped: the build folder is not configured, so only the targets named in CMakeLists.txt were compared' }
    foreach ($target in $known) {
        if ($runTargets -notcontains $target -and -not $excludedTargets.ContainsKey($target)) { $problems.Add("test target $target is not run by tools\test.ps1 (add it, or exclude it with a reason)") }
    }
    if ($ninjaListed) { foreach ($name in $runTargets) { if ($known -notcontains $name) { $problems.Add("suite $name names no CMake test target") } } }
    $cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
    foreach ($module in @(Get-ChildItem -LiteralPath (Join-Path $root 'cmake') -Filter '*.cmake' -File -ErrorAction SilentlyContinue)) { $cmake += "`n" + [IO.File]::ReadAllText($module.FullName) }
    $listedNames = @($allSuites | ForEach-Object { $_.Name })
    $testsDir = Join-Path $root 'tests'
    foreach ($file in @(Get-ChildItem -LiteralPath $testsDir -File -ErrorAction SilentlyContinue)) {
        $relative = "tests\$($file.Name)"
        $ext = $file.Extension.ToLowerInvariant()
        if ($ext -in '.py', '.ps1') {
            if ($listedNames -notcontains $relative -and -not $helperTestFiles.ContainsKey($relative)) { $problems.Add("$relative is not run by tools\test.ps1") }
        } elseif ($ext -in '.c', '.cpp') {
            if (-not $cmake.Contains("tests/$($file.Name)")) { $problems.Add("$relative belongs to no CMake test target") }
        } elseif (-not $helperTestFiles.ContainsKey($relative)) {
            $problems.Add("$relative is neither a test, a source of a test target nor a listed helper")
        }
    }
    foreach ($suite in @($allSuites | Where-Object { $_.Kind -in 'python', 'script' })) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $suite.Name))) { $problems.Add("suite $($suite.Name): the file does not exist") }
    }
    $toolsDir = Join-Path $root 'tools'
    $toolSuiteScripts = @($allSuites | Where-Object { $_.Kind -eq 'tool' } | ForEach-Object { $_.Options['Script'] })
    foreach ($file in @(Get-ChildItem -LiteralPath $toolsDir -File -ErrorAction SilentlyContinue)) {
        $relative = "tools\$($file.Name)"
        if ($toolSuiteScripts -contains $relative -or $listedNames -contains $relative -or $toolRole.ContainsKey($relative)) { continue }
        $problems.Add("$relative is neither run by tools\test.ps1 nor listed with its role")
    }
    foreach ($text in @($cmake -split "\r?\n" | Where-Object { $_ -match '_tests' -and $_ -match '/wd\d+' })) { $problems.Add("a test target masks warnings: $($text.Trim())") }
    if ($problems.Count) { return New-Result 1 @($problems) }
    $excluded = @($known | Where-Object { $excludedTargets.ContainsKey($_) })
    $summary = "$($known.Count - $excluded.Count) test targets, $(@($allSuites | Where-Object { $_.Kind -in 'python', 'script' -and $_.Name -like 'tests\*' }).Count) test scripts and every tool classified; all run"
    return New-Result 0 (@($summary) + $notes + ($excluded | ForEach-Object { "excluded $_ : $($excludedTargets[$_])" }))
}

function Test-BugReportWorker {
    $node = Get-Command node -ErrorAction SilentlyContinue
    $dir = Join-Path $root 'server\trespasser-bug-reports'
    if (-not $node) { return New-Result 0 @('bug report worker: skipped - DATA UNAVAILABLE (Node.js is not installed)') }
    $run = Invoke-Process 'node' @('--test', 'test/*.test.js') $dir 300
    if ($run.Code -ne 0) { return New-Result 1 @($run.Lines) }
    return New-Result 0 @($run.Lines | Select-Object -Last 8)
}

function Test-LinuxAudioProbe {
    if (-not $runLinux) { return New-Result 0 @('linux audio probe: skipped - DATA UNAVAILABLE (Linux validation was not requested)') }
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return New-Result 0 @('linux audio probe: skipped - DATA UNAVAILABLE (wsl.exe is not installed)') }
    $clean = { param($lines) @($lines | ForEach-Object { ($_ -replace "`0", '').TrimEnd() } | Where-Object { $_ -ne '' }) }
    $probe = Invoke-Process 'wsl.exe' @('--', 'bash', '-c', 'command -v cmake >/dev/null && command -v ninja >/dev/null && (command -v gcc >/dev/null || command -v clang >/dev/null) && echo toolchain-ready') $root 120
    if (-not (@(& $clean $probe.Lines) -contains 'toolchain-ready')) { return New-Result 0 @('linux audio probe: skipped - DATA UNAVAILABLE (no WSL distribution with cmake, ninja and gcc or clang)') }
    $script = @(& $clean (Invoke-Process 'wsl.exe' @('wslpath', '-a', ((Join-Path $root 'tools\linux_audio_probe.sh') -replace '\\', '/')) $root 60).Lines)
    $repo = @(& $clean (Invoke-Process 'wsl.exe' @('wslpath', '-a', ($root -replace '\\', '/')) $root 60).Lines)
    if ($script.Count -ne 1 -or $repo.Count -ne 1) { return New-Result 1 @('linux audio probe: wslpath did not answer') }
    $run = Invoke-Process 'wsl.exe' @('--', 'bash', $script[0], $repo[0]) $root 1500
    $lines = @(& $clean $run.Lines)
    if ($run.Code -ne 0) { return [pscustomobject]@{ Code = 1; Lines = $lines; Command = $run.Command } }
    $summary = @($lines | Where-Object { $_ -match '^linux audio probe: ' })
    return New-Result 0 (@($lines | Where-Object { $_ -match 'skipped|DATA UNAVAILABLE|note:' }) + @($summary[-1]))
}

function Test-FeatureContract {
    $path = Join-Path $root 'tests\feature_contract.json'
    if (-not (Test-Path -LiteralPath $path)) { return New-Result 1 @('tests\feature_contract.json is missing') }
    try { $contract = [IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { return New-Result 1 @("tests\feature_contract.json is not valid JSON: $($_.Exception.Message)") }
    $problems = New-Object System.Collections.Generic.List[string]
    if ($contract.schema -ne 2) { return New-Result 1 @('tests\feature_contract.json: schema 2 is expected') }
    $windowsNames = @($allSuites | ForEach-Object { $_.Name })
    $linuxNames = @(Get-LinuxSuiteNames)
    $ownedWindows = New-Object System.Collections.Generic.HashSet[string]
    $ownedLinux = New-Object System.Collections.Generic.HashSet[string]
    $ids = New-Object System.Collections.Generic.HashSet[string]
    $count = 0; $global = 0; $platformOnly = 0
    $checkEvidence = {
        param($feature, $item, [string]$where, $names, $owned)
        if ($names -notcontains $item.suite) { $problems.Add("feature $($feature.id): $where evidence $($item.suite) is not a registered suite"); return }
        [void]$owned.Add($item.suite)
        if ($item.file) {
            $file = Join-Path $root $item.file
            if (-not (Test-Path -LiteralPath $file)) { $problems.Add("feature $($feature.id): $($item.file) does not exist"); return }
            if (-not [regex]::IsMatch([IO.File]::ReadAllText($file), $item.pattern)) { $problems.Add("feature $($feature.id): $($item.file) no longer contains /$($item.pattern)/, so $($item.suite) may no longer cover it") }
        }
    }
    foreach ($feature in @($contract.features)) {
        $count++
        if (-not $ids.Add([string]$feature.id)) { $problems.Add("feature $($feature.id) appears twice"); continue }
        if ($feature.scope -notin 'global', 'windows', 'linux', 'planned') { $problems.Add("feature $($feature.id): scope must be global, windows, linux or planned"); continue }
        if ($feature.scope -eq 'planned') {
            foreach ($platform in 'windows', 'linux') { if ($feature.$platform.status -ne 'BLOCKED' -or -not (Get-Prop $feature.$platform 'reason')) { $problems.Add("feature $($feature.id) is planned: $platform must be BLOCKED with a reason") } }
            continue
        }
        if ($feature.scope -eq 'global') { $global++ } else { $platformOnly++ }
        foreach ($platform in 'windows', 'linux') {
            $section = $feature.$platform
            if (-not $section -or $section.status -notin 'PASS', 'BLOCKED', 'PLATFORM_ONLY') { $problems.Add("feature $($feature.id): $platform status must be PASS, BLOCKED or PLATFORM_ONLY"); continue }
            if ($section.status -eq 'PLATFORM_ONLY' -and $feature.scope -eq 'global') { $problems.Add("feature $($feature.id) is global: $platform cannot be PLATFORM_ONLY (it must be PASS or BLOCKED with a reason)") }
            if ($section.status -eq 'PLATFORM_ONLY' -and $feature.scope -ne 'global' -and $feature.scope -eq $platform) { $problems.Add("feature $($feature.id) is $platform only: $platform cannot be PLATFORM_ONLY") }
            if ($section.status -in 'PLATFORM_ONLY', 'BLOCKED' -and -not (Get-Prop $section 'reason')) { $problems.Add("feature $($feature.id): $platform $($section.status) needs a reason") }
            if ($feature.scope -ne 'global' -and $feature.scope -ne $platform -and $section.status -ne 'PLATFORM_ONLY') { $problems.Add("feature $($feature.id) is $($feature.scope) only: $platform must be PLATFORM_ONLY") }
            if ($feature.scope -eq $platform -and $section.status -eq 'BLOCKED' -and $feature.id -ne 'vulkan-renderer') { }
            if ($section.status -eq 'PASS' -and @(Get-Prop $feature 'shared' @()).Count + @(Get-Prop $section 'evidence' @()).Count -eq 0) { $problems.Add("feature $($feature.id): $platform is PASS but the contract lists no evidence") }
        }
        foreach ($item in @(Get-Prop $feature 'shared' @())) {
            & $checkEvidence $feature $item 'shared (windows)' $windowsNames $ownedWindows
            & $checkEvidence $feature $item 'shared (linux)' $linuxNames $ownedLinux
        }
        foreach ($item in @(Get-Prop $feature.windows 'evidence' @())) { & $checkEvidence $feature $item 'windows' $windowsNames $ownedWindows }
        foreach ($item in @(Get-Prop $feature.linux 'evidence' @())) { & $checkEvidence $feature $item 'linux' $linuxNames $ownedLinux }
    }
    $infrastructure = @($contract.infrastructure | ForEach-Object { $_.suite })
    foreach ($name in $windowsNames) {
        if (-not $ownedWindows.Contains($name) -and $infrastructure -notcontains $name -and $name -ne 'dist artifacts') { $problems.Add("Windows suite $name owns no feature (add it to a feature of tests\feature_contract.json, or to infrastructure with a reason)") }
    }
    foreach ($name in $linuxNames) {
        if (-not $ownedLinux.Contains($name) -and $infrastructure -notcontains $name) { $problems.Add("Linux suite $name owns no feature (add it to a feature of tests\feature_contract.json)") }
    }
    foreach ($entry in @($contract.infrastructure)) { if ($windowsNames -notcontains $entry.suite -and $entry.suite -ne 'linux feature coverage') { $problems.Add("infrastructure suite $($entry.suite) is not registered") } }
    if ($problems.Count) { return New-Result 1 @($problems | Select-Object -First 60) }
    return New-Result 0 @("$count features ($global global, $platformOnly on one platform), $($ownedWindows.Count) Windows suites and $($ownedLinux.Count) Linux suites own them")
}

function Get-ProjectVersion {
    $text = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
    if ($text -match '(?m)^project\(\w+ VERSION (\d+\.\d+\.\d+)') { return $Matches[1] }
    return $null
}

function Test-ReleaseContract {
    $problems = New-Object System.Collections.Generic.List[string]
    $lines = @()
    $version = Get-ProjectVersion
    if (-not $version) { $problems.Add('CMakeLists.txt: project VERSION is not major.minor.patch') } else { $lines += "version $version" }
    $cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
    if ($cmake -notmatch 'JPT_PORT_VERSION="\$\{PROJECT_VERSION\}"') { $problems.Add('the launcher does not take its version from the project VERSION') }
    foreach ($file in 'README.md', 'docs\LEGAL.md', 'docs\user-data.md', 'docs\LINUX.md', 'docs\RELEASE_SECURITY.md', 'packaging\README-linux.txt',
                      'packaging\licenses\THIRD-PARTY-LICENSES.txt', 'packaging\licenses\LICENSE.txt',
                      'packaging\licenses\FFmpeg-LGPL-2.1.txt', 'packaging\licenses\FFMPEG-SOURCE.txt',
                      'packaging\licenses\OpenSSL-LICENSE.txt', 'packaging\licenses\Apache-2.0.txt',
                      'packaging\linux\AppRun', 'packaging\linux\io.github.saulob.jurassic-park-trespasser.desktop',
                      'packaging\linux\io.github.saulob.jurassic-park-trespasser.appdata.xml',
                      'patches\appimage-runtime\baseline.env', 'patches\appimage-runtime\0001-extract-and-run-when-fuse-fails.patch',
                      'assets\launcher\header-art.png', 'third_party\assao\LICENSE', 'launcher\third_party\nanosvg\LICENSE.txt') {
        $path = Join-Path $root $file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $problems.Add("$file is missing") } elseif ((Get-Item -LiteralPath $path).Length -eq 0) { $problems.Add("$file is empty") }
    }
    $licensesPath = Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt'
    if (Test-Path -LiteralPath $licensesPath) {
        $licenses = [IO.File]::ReadAllText($licensesPath)
        foreach ($name in 'pcrecomp', 'Dear ImGui', 'SDL', 'ASSAO', 'nanosvg', 'OpenSSL', 'FFmpeg', 'Vulkan', 'Squirrel') { if ($licenses -notmatch [regex]::Escape($name)) { $problems.Add("THIRD-PARTY-LICENSES.txt does not name $name") } }
    }
    $deps = [IO.File]::ReadAllText((Join-Path $root 'config\launcher_deps.txt'))
    if ($deps -notmatch 'SDL is release-3\.4\.20' -or $deps -notmatch 'external/SDL\s+\S+\s+[0-9a-f]{40}') { $problems.Add('config\launcher_deps.txt does not pin SDL 3.4.20 to a commit') }
    $ignore = [IO.File]::ReadAllText((Join-Path $root '.gitignore'))
    foreach ($entry in 'build/', 'game/', 'user_data/', 'logs/', 'dist/', '*.iso', '*.exe', '*.tpa', '*.scn', 'external/') { if (-not $ignore.Contains($entry)) { $problems.Add(".gitignore does not ignore $entry") } }
    $verify = [IO.File]::ReadAllText((Join-Path $root 'tools\verify-release.ps1'))
    foreach ($name in 'user_data', 'saves', 'cache', 'logs', 'game', 'trespass.exe', 'recomp-registry.txt', '*.pdb', '*.tmp', '*.log', '*.iso', 'DEBUG_SAULO') {
        if (-not $verify.Contains("'$name'") -and -not $verify.Contains($name)) { $problems.Add("tools\verify-release.ps1 no longer refuses $name") }
    }
    foreach ($language in @('en', 'pt-BR', 'fr', 'de', 'es', 'ru', 'it')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root "assets\translations\$language.interface.txt"))) { $problems.Add("assets\translations\$language.interface.txt is missing from the release assets") }
    }
    foreach ($dir in 'assets\restored_audio', 'assets\frontend') {
        if (@(Get-ChildItem -LiteralPath (Join-Path $root $dir) -Recurse -File -ErrorAction SilentlyContinue).Count -eq 0) { $problems.Add("$dir holds no files") }
    }
    $shipped = 'trespasser_recomp', 'jpt_launcher', 'jpt_launcher_core', 'jpt_common'
    foreach ($name in $shipped) {
        foreach ($block in [regex]::Matches($cmake, "target_compile_definitions\($name\b[^)]*\)")) {
            foreach ($line in ($block.Value -split "\r?\n")) {
                if ($line -match 'CMAKE_(SOURCE|BINARY)_DIR|JPT_SOURCE_DIR' -and $line -notmatch '\$<\$<NOT:\$<CONFIG:Release>>:') {
                    $problems.Add("$name compiles a build-machine path into a Release program: $($line.Trim())")
                }
            }
        }
    }
    if ($problems.Count) { return New-Result 1 @($problems) }
    return New-Result 0 (@("version $version; licenses, README, assets, forbidden-file list, SDL pin and ignore rules present") )
}

function Test-ReleaseArtifacts {
    $dist = Join-Path $root 'dist'
    $folders = @(); $zips = @()
    if (Test-Path -LiteralPath $dist) {
        $folders = @(Get-ChildItem -LiteralPath $dist -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.verify' -and $_.Name -notlike '*-symbols' })
        $zips = @(Get-ChildItem -LiteralPath $dist -Filter '*.zip' -File -ErrorAction SilentlyContinue)
    }
    if ($folders.Count -eq 0 -and $zips.Count -eq 0) { return New-Result 1 @("no release artifacts in ${dist}: package the release first (this script never packages), or run without -Release") }
    $lines = New-Object System.Collections.Generic.List[string]
    $code = 0
    foreach ($artifact in @($folders) + @($zips)) {
        $run = Invoke-Process $hostShell @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\verify-release.ps1'), '-Path', $artifact.FullName) $root 600
        $run.Lines | ForEach-Object { $lines.Add($_) }
        if ($run.Code -ne 0) { $code = 1 }
    }
    if ($folders.Count -eq 1 -and $zips.Count -eq 1) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zips[0].FullName)
        try { $entries = @($archive.Entries | Where-Object { $_.Name } | ForEach-Object { ($_.FullName -replace '^[^/\\]+[/\\]', '').Replace('/', '\') } | Sort-Object) } finally { $archive.Dispose() }
        $staged = @(Get-ChildItem -LiteralPath $folders[0].FullName -Recurse -File | ForEach-Object { $_.FullName.Substring($folders[0].FullName.Length + 1) } | Sort-Object)
        if (@(Compare-Object $entries $staged).Count) { $code = 1; $lines.Add('the ZIP does not hold exactly the verified folder') }
    }
    return New-Result $code @($lines)
}

# --- building ----------------------------------------------------------------------------------------------------

function Show-BuildWarnings([string[]]$Lines) {
    return @($Lines | Where-Object { $_ -match ':\s*(warning|Warning)\s+[A-Z]+\d+' -and $_ -notmatch '[\\/](external|third_party)[\\/]' -and $_ -notmatch 'pcrecomp' })
}

function Invoke-Configure([string]$Folder, [string]$Arch) {
    $arguments = @('-S', $root, '-B', $Folder, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DTRESPASSER_GEN_OPT=/Od', '-DTRESPASSER_TRACE=OFF')
    return Invoke-Process 'cmake' $arguments $root 1800
}

function Build-TestTargets([string]$Folder, [string]$Arch, [string[]]$Targets) {
    if (-not $Targets.Count) { return $null }
    $problem = Get-MsvcProblem
    if ($problem) { return $problem }
    $null = Enter-Msvc $Arch
    if (-not (Test-Path -LiteralPath (Join-Path $Folder 'build.ninja'))) {
        Write-Host "  configuring $Folder (first use)" -ForegroundColor DarkGray
        $configure = Invoke-Configure $Folder $Arch
        if ($configure.Code -ne 0) { return "CMake could not configure $Folder (exit $($configure.Code)); run tools\build.ps1 once to prepare the sources:`n$($configure.Lines -join "`n")" }
    }
    $buildRun = Invoke-Process 'ninja' (@('-C', $Folder, '-k', '0') + $Targets) $root 3600
    $warnings = @(Show-BuildWarnings $buildRun.Lines)
    if ($buildRun.Code -ne 0) { return "ninja exit code $($buildRun.Code):`n$(($buildRun.Lines | Select-Object -Last 60) -join "`n")" }
    if ($warnings.Count) { return "$($warnings.Count) compiler warning(s) in project sources:`n$($warnings -join "`n")" }
    $steps = @($buildRun.Lines | Where-Object { $_ -match '^\[\d+/\d+\]' }).Count
    return [pscustomobject]@{ Steps = $steps; Targets = $Targets.Count }
}

function Export-CompileCommands {
    $exported = @()
    $failure = $null
    $problem = Get-MsvcProblem
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        if ($problem) { return @{ Files = @(); Failure = "ninja is needed to list the compile commands: $problem" } }
        $null = Enter-Msvc 'x86'
    }
    $plans = @(@{ Folder = $buildDir; Name = 'x86'; Targets = @($allSuites | Where-Object { $_.Kind -eq 'exe' -and $_.Options['Arch'] -ne 'x64' } | ForEach-Object { $_.Name } | Sort-Object -Unique); Required = $true },
        @{ Folder = $buildDir64; Name = 'x64'; Targets = @($allSuites | Where-Object { $_.Kind -eq 'exe' -and $_.Options['Arch'] -eq 'x64' } | ForEach-Object { $_.Name }); Required = $false })
    foreach ($plan in $plans) {
        if (-not (Test-Path -LiteralPath (Join-Path $plan.Folder 'build.ninja'))) {
            if ($plan.Required) { $failure = "$($plan.Folder) is not configured: no compile commands to check" }
            continue
        }
        $run = Invoke-Process 'ninja' (@('-C', $plan.Folder, '-t', 'commands') + $plan.Targets) $root 300
        if ($run.Code -ne 0) { $failure = "ninja -t commands failed for $($plan.Name): $($run.Lines -join ' ')"; continue }
        $file = Join-Path $reportDir "commands-$($plan.Name).txt"
        [IO.File]::WriteAllLines($file, [string[]]$run.Lines)
        $exported += $file
    }
    return @{ Files = $exported; Failure = $failure }
}

# --- suites ------------------------------------------------------------------------------------------------------

function Start-Suite($Suite) {
    switch ($Suite.Kind) {
        'exe' {
            $folder = if ($Suite.Options['Arch'] -eq 'x64') { $buildDir64 } else { $buildDir }
            $exe = Join-Path $folder "$($Suite.Name).exe"
            if ($script:BuildProblems.ContainsKey($Suite.Options['Arch'] + '')) { return New-Result 127 @("not built: $($script:BuildProblems[$Suite.Options['Arch'] + ''])") }
            if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
                $how = if ($NoBuild) { 'run .\tools\test.ps1 without -NoBuild to build it' } else { 'the target did not build; see the build output above' }
                return New-Result 127 @("no test program $exe ($how)")
            }
            $arguments = @()
            if ($Suite.Options.ContainsKey('QuickArguments') -and -not $Full) { $arguments = $Suite.Options['QuickArguments'] }
            return Start-Proc $exe $arguments $root 900
        }
        'python' {
            $problem = Get-PythonProblem
            if ($problem) { return New-Result 1 @($problem) }
            $script = Join-Path $root $Suite.Name
            if (-not (Test-Path -LiteralPath $script -PathType Leaf)) { return New-Result 127 @("missing script: $script") }
            $arguments = @($script:Python.Args) + @('-X', 'utf8', '-I', '-B', '-m', 'unittest', 'discover', '-s', 'tests', '-p', (Split-Path $Suite.Name -Leaf), '-v')
            return Start-Proc $script:Python.Exe $arguments $root 900
        }
        'tool' {
            $problem = Get-PythonProblem
            if ($problem) { return New-Result 1 @($problem) }
            $script = Join-Path $root $Suite.Options['Script']
            if (-not (Test-Path -LiteralPath $script -PathType Leaf)) { return New-Result 127 @("missing script: $script") }
            return Start-Proc $script:Python.Exe (@($script:Python.Args) + @('-X', 'utf8', '-B', $script) + @($Suite.Options['Arguments'])) $root 900
        }
        'script' {
            $script = Join-Path $root $Suite.Name
            if (-not (Test-Path -LiteralPath $script -PathType Leaf)) { return New-Result 127 @("missing script: $script") }
            return Start-Proc $hostShell @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) $root 900
        }
        default { return $null }
    }
}

function Complete-Suite($Suite, $Started) {
    if ($Suite.Kind -eq 'check') { return & $Suite.Options['Run'] }
    if ($Started -is [pscustomobject] -and $Started.PSObject.Properties['Process']) { return Wait-Proc $Started }
    return $Started
}

function Get-Counts([object[]]$Lines) {
    $checks = 0
    foreach ($line in $Lines) {
        if ($line -match '(\d+) checks') { $checks = [int]$Matches[1] }
        elseif ($line -match '^Ran (\d+) tests?') { $checks = [int]$Matches[1] }
    }
    return $checks
}

function Get-SkipNotes([string]$Label, [object[]]$Lines) {
    $notes = @()
    foreach ($line in $Lines) {
        if ($line -match "\.\.\. skipped '(.+)'") { $notes += "${Label}: skipped - $($Matches[1])" }
        elseif ($line -match '^\s*skipped\b' -or $line -match ':\s*skipped\b' -or $line -match '\bskipped\)\s*$' -or $line -match '\(retail checks skipped\)') { $notes += "${Label}: $($line.Trim())" }
        elseif ($line -match '\bSKIP\b') { $notes += "${Label}: $($line.Trim())" }
    }
    return $notes
}

# ---------------------------------------------------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------------------------------------------------
$environment = [ordered]@{ SDL_VIDEODRIVER = 'dummy'; SDL_AUDIODRIVER = 'dummy'; PYTHONDONTWRITEBYTECODE = '1'; JPT_GATE_REPORT = $reportDir; JPT_GATE_COMMANDS = $null; TRESPASSER_TEST_LIFT = $null }

$results = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
$script:BuildProblems = @{}
$script:Started = Get-Date
$gateStopped = $false
$clock = [System.Diagnostics.Stopwatch]::StartNew()
if (-not $runWindows) { $suites = New-Object System.Collections.Generic.List[object] }
$linuxPlanned = @(Get-LinuxSuiteNames | Where-Object { -not $Filter -or $_ -like "*$Filter*" })
$gameIsoDir = if ($runLinux) { Find-GameIsoDirectory $GameIsoDir } else { '' }
$importRequested = [bool]$gameIsoDir -and (-not $Filter -or $Filter -match 'linux_boot_tests|game imports')
$missingVersionPaths = @()
foreach ($id in 'en-116', 'ru-116', 'es-117', 'ptbr-117') {
    if (-not (Test-Path -LiteralPath (Join-Path $root "game\versions\$id\guest.img"))) { $missingVersionPaths += "game\versions\$id" }
}
if ($runLinux -and $importRequested) { $linuxPlanned = @('game imports') + $linuxPlanned }
if ($runLinux -and -not $Filter -and -not $Sanitize) { $linuxPlanned = @('linux build') + @($linuxPlanned | Where-Object { $_ -ne 'linux build' }) }
if ($Sanitize) { $linuxPlanned = @($linuxPlanned | Where-Object { $_ -match '^[a-z0-9_]+_tests$' }) }
Import-TestHistory
if ($runWindows) {
    if (-not $Filter -and -not $NoBuild) { $script:ProgressPlan.Add('windows runtime (trespasser_recomp)') }
    $script:ProgressPlan.Add('x86 test targets'); $script:ProgressPlan.Add('x64 test targets')
    foreach ($suite in $suites) { $script:ProgressPlan.Add($suite.Name) }
}
if ($runLinux) { foreach ($name in $linuxPlanned) { $script:ProgressPlan.Add($name) } }

try {
    Remove-DuplicatePathEntries
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $script:SnapshotBefore = Get-TreeSnapshot

    $x86Targets = @($suites | Where-Object { $_.Kind -eq 'exe' -and $_.Options['Arch'] -ne 'x64' } | ForEach-Object { $_.Name } | Sort-Object -Unique)
    $x64Targets = @($suites | Where-Object { $_.Kind -eq 'exe' -and $_.Options['Arch'] -eq 'x64' } | ForEach-Object { $_.Name } | Sort-Object -Unique)
    $winRuntimeArtifact = $null
    if ($runWindows) { Write-Host '==> Windows focused gate' -ForegroundColor Cyan }
    if (-not $NoBuild -and ($x86Targets.Count + $x64Targets.Count)) {
        Write-Host '==> Build (runtime + test targets, incremental)' -ForegroundColor Cyan
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $buildPlans = New-Object System.Collections.Generic.List[object]
        if (-not $Filter) {
            $buildPlans.Add(@{ Arch = ''; Folder = $buildDir; Targets = @('trespasser_recomp'); Label = 'windows runtime (trespasser_recomp)'; IsRuntime = $true })
        }
        $buildPlans.Add(@{ Arch = ''; Folder = $buildDir; Targets = $x86Targets; Label = 'x86 test targets'; IsRuntime = $false })
        $buildPlans.Add(@{ Arch = 'x64'; Folder = $buildDir64; Targets = $x64Targets; Label = 'x64 test targets'; IsRuntime = $false })
        foreach ($plan in $buildPlans) {
            if (-not $plan.Targets.Count) { continue }
            $archName = if ($plan.Arch) { $plan.Arch } else { 'x86' }
            $outcome = Build-TestTargets $plan.Folder $archName $plan.Targets
            if ($outcome -is [string]) {
                if (-not $plan.IsRuntime) { $script:BuildProblems[$plan.Arch] = $outcome.Split("`n")[0] }
                Step-Progress
                Write-Host ('{0} FAIL  {1,-46}' -f (Get-ProgressPrefix), $plan.Label) -ForegroundColor Red
                Write-Host "        $outcome" -ForegroundColor Red
                $results.Add([pscustomobject]@{ Section = 'Build'; Platform = 'windows'; Name = $plan.Label; Status = 'FAIL'; Seconds = $watch.Elapsed.TotalSeconds; Checks = 0; Summary = 'build failed'; Lines = @() })
            } else {
                Step-Progress
                Write-Host ('{0} PASS  {1,-46} {2,6:N1} s  {3} targets, {4} compile or link step(s), no warnings' -f (Get-ProgressPrefix), $plan.Label, $watch.Elapsed.TotalSeconds, $outcome.Targets, $outcome.Steps) -ForegroundColor Green
                $results.Add([pscustomobject]@{ Section = 'Build'; Platform = 'windows'; Name = $plan.Label; Status = 'PASS'; Seconds = $watch.Elapsed.TotalSeconds; Checks = 0; Summary = "$($outcome.Steps) steps"; Lines = @() })
                if ($plan.IsRuntime) {
                    $exePath = Join-Path $plan.Folder 'trespasser_recomp.exe'
                    if (Test-Path -LiteralPath $exePath -PathType Leaf) {
                        $clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
                        $winRuntimeArtifact = [pscustomobject]@{
                            Path = $exePath
                            Size = (Get-Item -LiteralPath $exePath).Length
                            Config = 'RelWithDebInfo (x86)'
                            Compiler = if ($clCmd) { $clCmd.Source } else { 'MSVC cl.exe' }
                        }
                    } else {
                        Write-Host ('        expected runtime executable missing: {0}' -f $exePath) -ForegroundColor Red
                        $results[$results.Count - 1].Status = 'FAIL'
                        $results[$results.Count - 1].Summary = 'trespasser_recomp.exe missing after build'
                    }
                }
            }
            $watch.Restart()
        }
    } elseif ($NoBuild) {
        $notes.Add('test programs were NOT rebuilt (-NoBuild): the results describe the programs already in the build folder')
    }

    if ($Build) {
        $buildRows = @($results | Where-Object { $_.Section -eq 'Build' })
        $buildFailed = @($buildRows | Where-Object { $_.Status -eq 'FAIL' })
        if ($buildFailed.Count) {
            Write-Host ('FAILED: build only: {0} build step(s) failed.' -f $buildFailed.Count) -ForegroundColor Red
        } elseif (-not $buildRows.Count) {
            Write-Host 'FAILED: build only: nothing was built (check -Filter and -BuildDir).' -ForegroundColor Red
        } else {
            Write-Host ('BUILD ONLY: {0} build step(s) passed; no suite ran and the Linux half was not started.' -f $buildRows.Count) -ForegroundColor Green
        }
        if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
        exit $(if ($buildRows.Count -and -not $buildFailed.Count) { 0 } else { 1 })
    }

    if (@($suites | Where-Object { $_.Name -eq 'tests\test_compile_commands.py' }).Count) {
        $commands = Export-CompileCommands
        $names = if ($commands.Failure) { Join-Path $reportDir 'commands-unavailable.txt' } else { $commands.Files -join ';' }
        [Environment]::SetEnvironmentVariable('JPT_GATE_COMMANDS', $names)
        if ($commands.Failure) { $notes.Add("compile commands: $($commands.Failure)") }
    }

    $pending = @{}
    foreach ($suite in $suites) {
        if ($suite.Options['Background']) { $pending[$suite.Name] = Start-Suite $suite }
    }

    $section = ''
    foreach ($suite in $suites) {
        if ($suite.Section -ne $section) { $section = $suite.Section; Write-Host "==> $section" -ForegroundColor Cyan }
        $label = $suite.Name
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $started = if ($pending.ContainsKey($suite.Name)) { $pending[$suite.Name] } else { Start-Suite $suite }
            $run = Complete-Suite $suite $started
            if ($pending.ContainsKey($suite.Name)) { $pending.Remove($suite.Name) }
        } catch { $run = New-Result 1 @("the suite could not run: $($_.Exception.Message)") }
        $watch.Stop()
        $seconds = if ($run.PSObject.Properties['Seconds']) { $run.Seconds } else { $watch.Elapsed.TotalSeconds }

        $lines = @($run.Lines)
        $status = if ($run.Code -eq 0) { 'PASS' } else { 'FAIL' }
        $problem = $null
        $checks = Get-Counts $lines
        if ($status -eq 'PASS' -and @($lines | Where-Object { $_ -match '^\s*(FAIL|FAILED)\b' }).Count) { $status = 'FAIL'; $problem = 'exit code 0, but the output reports FAIL' }
        if ($status -eq 'PASS' -and $suite.Options['MinChecks'] -gt 0 -and $checks -lt $suite.Options['MinChecks']) {
            $status = 'FAIL'
            $problem = "only $checks checks ran; this suite has at least $($suite.Options['MinChecks']) (coverage shrank)"
        }
        if ($status -eq 'PASS' -and $suite.Kind -in 'python') {
            if (-not (@($lines | Where-Object { $_ -match '^(OK|OK \(skipped=\d+\))$' }).Count)) { $status = 'FAIL'; $problem = 'unittest did not report OK' }
        }
        if ($status -eq 'PASS' -and $pending.Count -eq 0 -and $suite.Name -ne 'working tree unchanged') {
            $stray = @(Get-DescendantProcesses | Where-Object { $_.Name -ne 'conhost.exe' })
            if ($stray.Count) { $status = 'FAIL'; $problem = "the suite left process(es) of the gate running: $((@($stray | ForEach-Object { "$($_.Name) ($($_.ProcessId))" })) -join ', ')" }
        }
        $summary = ''
        $last = @($lines | Where-Object { $_ -match '(\d+ checks?|\bOK\b|\bpassed\b|0 failures|0 failed|0 problem)' })
        if ($last.Count) { $summary = $last[-1].Trim() }
        else {
            $text = @($lines | Where-Object { $_.Trim() })
            if ($text.Count) { $summary = if ($suite.Kind -eq 'check') { $text[0].Trim() } else { $text[-1].Trim() } }
        }
        if ($suite.Kind -eq 'python' -and $checks) { $summary = "$checks tests, $($summary)" }
        if ($summary.Length -gt 96) { $summary = $summary.Substring(0, 93) + '...' }
        $skipNotes = @(Get-SkipNotes $label $lines)
        foreach ($note in $skipNotes) { $notes.Add($note) }

        $unavailable = ($status -eq 'PASS' -and $checks -le 2 -and @($lines | Where-Object { $_ -match 'DATA UNAVAILABLE' }).Count -gt 0)
        $results.Add([pscustomobject]@{ Section = $section; Platform = 'windows'; Name = $label; Status = $status; Seconds = $seconds; Checks = $checks; Summary = $summary; Lines = $lines; Skipped = $skipNotes.Count; Unavailable = $unavailable })
        Step-Progress
        if ($status -eq 'PASS') {
            $tail = if ($skipNotes.Count) { "$summary  [$($skipNotes.Count) part(s) skipped]" } else { $summary }
            if ($unavailable) { $tail = "DATA UNAVAILABLE: $tail" }
            Write-Host ('{0} PASS  {1,-46} {2,6:N1} s  {3}' -f (Get-ProgressPrefix), $label, $seconds, $tail) -ForegroundColor $(if ($unavailable) { 'Yellow' } else { 'Green' })
            if ($Detail) { $lines | ForEach-Object { Write-Host "        $_" } }
        } else {
            Write-Host ('{0} FAIL  {1,-46} {2,6:N1} s  exit code {3}' -f (Get-ProgressPrefix), $label, $seconds, $run.Code) -ForegroundColor Red
            if ($problem) { Write-Host "        $problem" -ForegroundColor Red }
            if ($run.Command) { Write-Host "        command: $($run.Command)" -ForegroundColor Red }
            if ($run.PSObject.Properties['WorkDir'] -and $run.WorkDir) { Write-Host "        working directory: $($run.WorkDir)" -ForegroundColor Red }
            if ($suite.Name -ne 'working tree unchanged') {
                $moved = @(Compare-TreeSnapshots $script:SnapshotBefore (Get-TreeSnapshot))
                if ($moved.Count) { Write-Host "        files that differ from the snapshot taken before the run: $($moved.Count)" -ForegroundColor Red; $moved | Select-Object -First 20 | ForEach-Object { Write-Host "          $_" -ForegroundColor Red } }
            }
            Write-Host "  ----- output of $label -----" -ForegroundColor Red
            $lines | ForEach-Object { Write-Host $_ }
            Write-Host '  ----- end of output -----' -ForegroundColor Red
        }
    }
} catch {
    Write-Host "FAILED: the gate itself stopped: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  at $($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber): $("$($_.InvocationInfo.Line)".Trim())" -ForegroundColor Red
    $gateStopped = $true
} finally {
    Restore-ProcessEnvironment $callerEnvironment
}
if ($gateStopped) {
    if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
    exit 1
}

# ---------------------------------------------------------------------------------------------------------------------
# The Linux half: PowerShell -> wsl.exe -> wslpath -> bash -> tools/test-linux.sh -> tools/linux_gate.py
# ---------------------------------------------------------------------------------------------------------------------
$linux = [ordered]@{ Requested = $runLinux -and ($linuxPlanned.Count -gt 0); Ran = $false; Environment = $null; Why = ''; Info = @{}; Seconds = 0.0 }
$versionReportPath = Join-Path $root 'build\logs\version_parity.json'
if ($runLinux) { Remove-Item -LiteralPath $versionReportPath -Force -ErrorAction SilentlyContinue }
if ($linux.Requested) {
    Write-Host '==> Linux gate (WSL)' -ForegroundColor Cyan
    $linux.Environment = Find-LinuxEnvironment $LinuxDistro
    if (-not $linux.Environment.Found) {
        $linux.Why = "BLOCKED: WSL unavailable ($($linux.Environment.Reason))"
        Write-Host "  $($linux.Why)" -ForegroundColor Red
        Write-Host '  Linux is NOT validated: no Linux row can pass and the feature parity reports it.' -ForegroundColor Red
        $results.Add([pscustomobject]@{ Section = 'Linux (WSL)'; Platform = 'linux'; Name = 'wsl'; Status = 'FAIL'; Seconds = 0; Checks = 0; Summary = $linux.Why; Lines = @(); Skipped = 0; Unavailable = $false })
    } else {
        $linuxEnv = $linux.Environment
        Write-Host ('  distribution : {0}  ({1}; glibc {2}; {3})' -f $linuxEnv.Name, $linuxEnv.Os, $linuxEnv.Glibc, $linuxEnv.Why)
        Write-Host ('  repository   : {0}  (converted with wslpath)' -f $linuxEnv.Repo)
        if ($importRequested) { Write-Host ('  game images  : {0}  (imported headlessly; existing installs are kept)' -f $gameIsoDir) }
        Write-Host ('  command      : wsl.exe -d {0} --exec bash {1}/tools/test-linux.sh --native-build --machine --cc {2}{3}{4}' -f $linuxEnv.Name, $linuxEnv.Repo, $linuxEnv.Cc, $(if ($NoBuild) { ' --no-build' } else { '' }), $(if ($importRequested) { ' --import-dir <images>' } else { '' }))
        if (-not $linuxEnv.Baseline) { Write-Host '  NOTE: the glibc 2.31 baseline is not proven by this run (no Ubuntu 20.04 environment was usable).' -ForegroundColor Yellow }
        if (-not $linuxEnv.Ffmpeg) { Write-Host "  NOTE: no FFmpeg development files in $($linuxEnv.Name): the Linux gate builds a pinned LGPL-only FFmpeg into ~/.cache/jpt-recomp/ffmpeg (tools/ffmpeg-bootstrap.sh; offline, the Smacker suites report DATA UNAVAILABLE)." -ForegroundColor Yellow }
        $print = {
            param($row)
            Step-Progress
            $blocked = $row.Status -eq 'PASS' -and $row.Unavailable
            $tail = $row.Summary
            if ($row.Notes.Count -and $row.Status -eq 'PASS') { $tail = "$tail  [$($row.Notes.Count) note(s)]" }
            if ($row.Status -eq 'PASS') {
                if ($blocked) { $tail = "DATA UNAVAILABLE: $tail" }
                Write-Host ('{0} PASS  {1,-46} {2,6:N1} s  {3}' -f (Get-ProgressPrefix), $row.Name, $row.Seconds, $tail) -ForegroundColor $(if ($blocked) { 'Yellow' } else { 'Green' })
                if ($Detail) { $row.Lines | ForEach-Object { Write-Host "        $_" } }
            } else {
                Write-Host ('{0} FAIL  {1,-46} {2,6:N1} s  {3}' -f (Get-ProgressPrefix), $row.Name, $row.Seconds, $row.Summary) -ForegroundColor Red
                Write-Host "  ----- output of $($row.Name) (Linux) -----" -ForegroundColor Red
                $row.Lines | ForEach-Object { Write-Host "        $_" }
                Write-Host '  ----- end of output -----' -ForegroundColor Red
            }
        }
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $outcome = Invoke-LinuxGate $linuxEnv $Filter ([bool]$NoBuild) ([bool]$Sanitize) $(if ($importRequested) { $gameIsoDir } else { '' }) $print
        $linux.Seconds = $watch.Elapsed.TotalSeconds
        $linux.Info = $outcome.Info
        $linux.Ran = $true
        $importRow = @($outcome.Rows | Where-Object { $_.Name -eq 'game imports' -and $_.Status -eq 'PASS' })
        if ($importRow.Count -and $missingVersionPaths.Count) { $script:ImportedVersionPaths = $missingVersionPaths }
        foreach ($row in $outcome.Rows) {
            $skip = @($row.Notes)
            foreach ($note in $skip) { $notes.Add("Linux $($row.Name): $note") }
            $results.Add([pscustomobject]@{ Section = 'Linux (WSL)'; Platform = 'linux'; Name = $row.Name; Status = $row.Status; Seconds = $row.Seconds; Checks = $row.Checks; Summary = $row.Summary; Lines = $row.Lines; Skipped = 0; Unavailable = $row.Unavailable })
        }
        if ($outcome.Info.Count) { Write-Host ('  Linux gate: glibc {0} on this system, executable needs at most glibc {1}, compiler {2}' -f $outcome.Info['glibc'], $(if ($outcome.Info['glibc_max']) { $outcome.Info['glibc_max'] } else { 'unknown' }), $outcome.Info['compiler']) -ForegroundColor DarkGray }
    }
} elseif ($runLinux -and $Filter) {
    $notes.Add("Linux: no row matches the filter '$Filter'; the Linux half did not run")
}

# ---------------------------------------------------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------------------------------------------------
$clock.Stop()
if (-not $Filter -and $runWindows) { foreach ($entry in $notRunSuites) { $results.Add($entry) } }
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
$passed = @($results | Where-Object { $_.Status -eq 'PASS' })
$notRun = @($results | Where-Object { $_.Status -eq 'NOT RUN' })
$checkTotal = ($results | Measure-Object -Property Checks -Sum).Sum

function Read-Report([string]$Name) {
    $path = Join-Path $reportDir "$Name.json"
    if (Test-Path -LiteralPath $path) { try { return [IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { return $null } }
    return $null
}

function Get-Status([string]$SuiteName) {
    $match = @($results | Where-Object { $_.Name -eq $SuiteName -and (Get-Prop $_ 'Platform') -ne 'linux' })
    if ($match.Count -eq 0) { return 'NOT RUN' }
    return $match[0].Status
}

Write-Host '==> Summary' -ForegroundColor Cyan
foreach ($group in (Group-InOrder $results)) {
    $bad = @($group.Group | Where-Object { $_.Status -eq 'FAIL' }).Count
    $skipped = @($group.Group | Where-Object { $_.Status -eq 'NOT RUN' }).Count
    $text = if ($bad) { "$bad FAILED" } elseif ($skipped) { "$skipped NOT RUN" } else { 'all passed' }
    Write-Host ('  {0,-30} {1,3} suites  {2}' -f $group.Name, $group.Count, $text) -ForegroundColor $(if ($bad) { 'Red' } elseif ($skipped) { 'Yellow' } else { 'Gray' })
}

$parity = Read-Report 'build_parity'
if ($parity) {
    Write-Host '==> Supported builds' -ForegroundColor Cyan
    $parityStatus = Get-Status 'tests\test_build_parity.py'
    Write-Host ('  {0,-9} {1,-13} {2,-6} {3,-12} {4,-8} {5,-10} {6}' -f 'BUILD', 'LIFT', 'LANG', 'FINGERPRINT', 'STATUS', 'EVIDENCE', 'REAL EXECUTABLE')
    foreach ($buildRow in @($parity.builds)) {
        $rowStatus = if ($parityStatus -ne 'PASS' -or @($buildRow.problems).Count) { 'FAIL' } else { 'PASS' }
        Write-Host ('  {0,-9} {1,-13} {2,-6} {3,-12} {4,-8} {5,-10} {6}' -f $buildRow.id, $buildRow.lift, $buildRow.language, $buildRow.facts.fingerprint, $rowStatus, $buildRow.evidence, $buildRow.real) -ForegroundColor $(if ($rowStatus -eq 'PASS') { 'Gray' } else { 'Red' })
        Write-Host ('            addresses: {0}; offsets: {1}; overrides {2}; cheats {3}; Patch 1.1: {4}' -f $buildRow.facts.addresses, $buildRow.facts.offsets, $buildRow.facts.required_overrides, $buildRow.facts.cheat_addresses, $buildRow.facts.patch117) -ForegroundColor DarkGray
        foreach ($item in @($buildRow.problems)) { Write-Host "            $item" -ForegroundColor Red }
    }
}

$l10n = @($results | Where-Object { $_.Name -eq 'l10n_tests' -and (Get-Prop $_ 'Platform') -ne 'linux' })
$languageReport = Read-Report 'language_parity'
if ($languageReport) {
    Write-Host '==> Languages' -ForegroundColor Cyan
    $languageStatus = Get-Status 'tests\test_language_parity.py'
    foreach ($language in @($languageReport.languages)) {
        $text = ''
        $lineOf = @()
        if ($l10n.Count) { $lineOf = @($l10n[0].Lines | Where-Object { $_ -match "^(GameStrings|captions|interface|text) $([regex]::Escape($language.code))\s*:" }) }
        $complete = ($language.code -eq 'en') -or (@($lineOf).Count -ge 4 -and @($lineOf | Where-Object { $_ -match 'missing [1-9]|[1-9]\d* with English left' }).Count -eq 0)
        $rowStatus = if ($languageStatus -eq 'PASS' -and @($language.problems).Count -eq 0 -and $complete -and (Get-Status 'l10n_tests') -eq 'PASS') { 'PASS' } else { 'FAIL' }
        Write-Host ('  {0,-22} {1,-6} {2}' -f $language.name, $rowStatus, $(if ($language.code -eq 'en') { 'the source language (game strings are the executable''s own)' } else { "$(@($lineOf).Count) l10n_tests coverage lines, interface keys equal to English" })) -ForegroundColor $(if ($rowStatus -eq 'PASS') { 'Gray' } else { 'Red' })
        foreach ($item in @($language.problems)) { Write-Host "            $item" -ForegroundColor Red }
    }
}

$cheatReport = Read-Report 'cheat_parity'
if ($cheatReport) {
    Write-Host '==> Cheats' -ForegroundColor Cyan
    $cheatStatus = Get-Status 'tests\test_cheat_parity.py'
    $untested = [int]$cheatReport.discovered - [int]$cheatReport.tested
    Write-Host ('  discovered {0}, named by tests {1}, saved by the launcher {2}, verified in {3} builds: {4}' -f $cheatReport.discovered, $cheatReport.tested, $cheatReport.saved, @($cheatReport.builds).Count, $(if ($cheatStatus -eq 'PASS' -and $untested -eq 0) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($cheatStatus -eq 'PASS' -and $untested -eq 0) { 'Gray' } else { 'Red' })
}

$settingReport = Read-Report 'setting_parity'
if ($settingReport) {
    Write-Host ('==> Settings: {0} runtime settings, {1} launcher rows, {2} shown in the launcher: {3}' -f $settingReport.settings, $settingReport.launcher_rows, $settingReport.shown_in_launcher, (Get-Status 'tests\test_setting_parity.py')) -ForegroundColor Cyan
}

$parityFailed = $false
$contractPath = Join-Path $root 'tests\feature_contract.json'
Write-Host '==> Windows/Linux feature parity' -ForegroundColor Cyan
if ($partial -or -not $runWindows -or -not $runLinux) {
    $reason = if ($partial) { 'a partial run' } elseif (-not $runWindows) { 'the Windows half was not run' } else { 'the Linux half was not requested (-WindowsOnly or -SkipLinux)' }
    Write-Host "  NOT EVALUATED: $reason" -ForegroundColor Yellow
} elseif (Test-Path -LiteralPath $contractPath) {
    try {
        $contract = [IO.File]::ReadAllText($contractPath) | ConvertFrom-Json
        $windowsMap = @{}; $linuxMap = @{}
        foreach ($result in $results) { if ((Get-Prop $result 'Platform') -eq 'linux') { $linuxMap[$result.Name] = $result } elseif ($result.Name) { $windowsMap[$result.Name] = $result } }
        $linuxWhy = if ($linux.Why) { $linux.Why } else { 'the Linux gate did not run' }
        $parityRows = @(Get-FeatureParity $contract $windowsMap $linuxMap $true ([bool]$linux.Ran) $linuxWhy)
        foreach ($row in $parityRows) {
            if ($Detail) {
                Write-Host ('  FEATURE|{0}|windows|{1}' -f $row.Id, $row.Windows)
                Write-Host ('  FEATURE|{0}|linux|{1}' -f $row.Id, $row.Linux)
            }
            if ($Detail -or $row.Parity -ne 'PASS') {
                $color = switch ($row.Parity) { 'PASS' { 'Green' } 'BLOCKED' { 'Yellow' } default { 'Red' } }
                Write-Host ('  FEATURE_PARITY|{0}|{1}{2}' -f $row.Id, $row.Parity, $(if ($row.Parity -ne 'PASS') { "|windows=$($row.Windows)|linux=$($row.Linux)|$($row.Reason)" } else { '' })) -ForegroundColor $color
            }
        }
        $planned = @($parityRows | Where-Object { $_.Scope -eq 'planned' })
        $global = @($parityRows | Where-Object { $_.Scope -eq 'global' })
        $one = @($parityRows | Where-Object { $_.Scope -in 'windows', 'linux' })
        $count = { param($rows, $key, $value) @($rows | Where-Object { $_.$key -eq $value }).Count }
        Write-Host ''
        Write-Host ('  features: {0} ({1} global, {2} on one platform with the other PLATFORM_ONLY, {3} planned and not counted: {4})' -f $parityRows.Count, $global.Count, $one.Count, $planned.Count, (($planned | ForEach-Object { $_.Id }) -join ', '))
        Write-Host ('  Windows : PASS {0}  FAIL {1}  BLOCKED {2}  PLATFORM_ONLY {3}' -f (& $count $parityRows 'Windows' 'PASS'), (& $count $parityRows 'Windows' 'FAIL'), (& $count $parityRows 'Windows' 'BLOCKED'), (& $count $parityRows 'Windows' 'PLATFORM_ONLY'))
        Write-Host ('  Linux   : PASS {0}  FAIL {1}  BLOCKED {2}  PLATFORM_ONLY {3}' -f (& $count $parityRows 'Linux' 'PASS'), (& $count $parityRows 'Linux' 'FAIL'), (& $count $parityRows 'Linux' 'BLOCKED'), (& $count $parityRows 'Linux' 'PLATFORM_ONLY'))
        $gPass = & $count $global 'Parity' 'PASS'; $gFail = & $count $global 'Parity' 'FAIL'; $gBlocked = & $count $global 'Parity' 'BLOCKED'
        Write-Host ('  PASS  {0}/{1} global features' -f $gPass, $global.Count) -ForegroundColor $(if ($gPass -eq $global.Count) { 'Green' } else { 'Gray' })
        Write-Host ('  FAIL  {0}' -f $gFail) -ForegroundColor $(if ($gFail) { 'Red' } else { 'Gray' })
        Write-Host ('  BLOCKED {0}' -f $gBlocked) -ForegroundColor $(if ($gBlocked) { 'Yellow' } else { 'Gray' })
        $legit = @($one | Where-Object { $_.Parity -eq 'PASS' })
        if ($legit.Count) { Write-Host ('  one-platform features, each justified in the contract: {0}' -f (($legit | ForEach-Object { $_.Id }) -join ', ')) -ForegroundColor DarkGray }
        $onePlatformBad = @($one | Where-Object { $_.Parity -ne 'PASS' })
        $parityFailed = ($gFail + $gBlocked + $onePlatformBad.Count) -gt 0
        $parityVerdict = if ($parityFailed) { 'FAIL' } else { 'PASS' }
        Write-Host ('  Windows/Linux feature parity: {0}' -f $parityVerdict) -ForegroundColor $(if ($parityFailed) { 'Red' } else { 'Green' })
        $results.Add([pscustomobject]@{ Section = 'Feature parity'; Platform = 'parity'; Name = 'windows linux feature parity'; Status = $(if ($parityFailed) { 'FAIL' } else { 'PASS' }); Seconds = 0; Checks = $parityRows.Count; Summary = "$gPass/$($global.Count) global features"; Lines = @(); Skipped = 0; Unavailable = $false })
        $failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
    } catch {
        Write-Host "  the parity could not be computed: $($_.Exception.Message)" -ForegroundColor Red
        $parityFailed = $true
    }
}

if (-not (Test-Path variable:gPass)) { $gPass = 0; $global = @() }
if (-not $partial -and $runWindows -and -not $NoBuild) {
    Write-Host '==> Windows runtime artifact' -ForegroundColor Cyan
    if ($winRuntimeArtifact) {
        Write-Host '  PASS' -ForegroundColor Green
        Write-Host ('  path     : {0}' -f $winRuntimeArtifact.Path)
        Write-Host ('  size     : {0:N0} bytes ({1:N1} MB)' -f $winRuntimeArtifact.Size, ($winRuntimeArtifact.Size / 1MB))
        Write-Host ('  config   : {0}' -f $winRuntimeArtifact.Config)
        Write-Host ('  compiler : {0}' -f $winRuntimeArtifact.Compiler)
    } else {
        Write-Host '  FAIL (trespasser_recomp.exe did not build)' -ForegroundColor Red
    }
}
if (-not $partial -and $runWindows -and $linux.Ran -and -not $failed.Count -and -not $parityFailed -and $linux.Info['build_dir']) {
    Write-Host '==> Linux runtime artifact' -ForegroundColor Cyan
    $linuxRows = @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'linux' })
    $windowsRows = @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'windows' })
    $gateRecord = [ordered]@{
        windows = [ordered]@{ suites = $windowsRows.Count; checks = [int](Measure-Sum $windowsRows 'Checks'); failed = @($windowsRows | Where-Object { $_.Status -eq 'FAIL' }).Count }
        linux = [ordered]@{ suites = $linuxRows.Count; checks = [int](Measure-Sum $linuxRows 'Checks'); failed = @($linuxRows | Where-Object { $_.Status -eq 'FAIL' }).Count; distribution = $linuxEnv.Name; glibc_max = $linux.Info['glibc_max'] }
        parity = [ordered]@{ global_features = "$gPass/$($global.Count)"; verdict = 'PASS' }
    }
    $recordPath = Join-Path $root 'build\logs\linux_artifact_gate.json'
    New-Item -ItemType Directory -Force -Path (Split-Path $recordPath) | Out-Null
    [IO.File]::WriteAllText($recordPath, ($gateRecord | ConvertTo-Json -Depth 5))
    $repoLinux = $linuxEnv.Repo
    $stageArguments = @('-d', $linuxEnv.Name, '--exec', 'python3', '-I', "$repoLinux/tools/linux_artifact.py", 'stage', '--elf', "$($linux.Info['build_dir'])/bin/trespasser_recomp", '--dest-dir', "$repoLinux/build/linux/bin", '--result-json', "$repoLinux/build/logs/linux_artifact_gate.json")
    $stage = Invoke-Process 'wsl.exe' $stageArguments $root 900
    $stageLines = @(ConvertFrom-WslText $stage.Lines)
    $manifestPath = Join-Path $root 'build\linux\bin\trespasser_recomp.build.json'
    if ($stage.Code -eq 0 -and (Test-Path -LiteralPath $manifestPath)) {
        $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        $registry = $manifest.self_check.registry
        Write-Host '  PASS' -ForegroundColor Green
        Write-Host ('  path               : {0}' -f (Join-Path $root 'build\linux\bin\trespasser_recomp'))
        Write-Host ('  SHA-256            : {0}' -f $manifest.elf_sha256)
        Write-Host ('  source fingerprint : {0}' -f $manifest.source_fingerprint)
        Write-Host ('  self-check         : PASS, {0} capabilities, overrides {1} listed / {2} applicable on Linux / {3} with default settings, {4} lift hooks' -f $manifest.self_check.capabilities, $registry.override_registry, $registry.overrides_linux_applicable, $registry.overrides_installed_default_settings, $registry.lift_hooks)
        Write-Host ('  feature contract   : PASS (sha {0})' -f $manifest.feature_contract_sha256.Substring(0, 16))
        Write-Host ('  GLIBC max          : {0} (baseline {1})' -f $manifest.glibc_max, $manifest.build.glibc_baseline)
        Write-Host ('  FFmpeg             : {0}' -f $(if ($manifest.ffmpeg.enabled) { $(if ($manifest.ffmpeg.lgpl_only) { 'enabled, LGPL-only' } else { 'enabled, NOT LGPL-only (development)' }) } else { 'not compiled in' }))
        $results.Add([pscustomobject]@{ Section = 'Linux runtime artifact'; Platform = 'artifact'; Name = 'linux runtime artifact'; Status = 'PASS'; Seconds = 0; Checks = 1; Summary = $manifest.elf_sha256.Substring(0, 16); Lines = @(); Skipped = 0; Unavailable = $false })
    } else {
        Write-Host '  FAIL' -ForegroundColor Red
        $stageLines | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        $results.Add([pscustomobject]@{ Section = 'Linux runtime artifact'; Platform = 'artifact'; Name = 'linux runtime artifact'; Status = 'FAIL'; Seconds = 0; Checks = 0; Summary = "staging exited with code $($stage.Code)"; Lines = $stageLines; Skipped = 0; Unavailable = $false })
        $failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
    }
} elseif (-not $partial -and $runWindows -and $linux.Ran) {
    Write-Host '==> Linux runtime artifact: NOT STAGED (a row failed; build\linux\bin keeps the last validated binary)' -ForegroundColor Yellow
}

$versionReport = $null
if ($linux.Ran -and (Test-Path -LiteralPath $versionReportPath)) {
    try { $versionReport = [IO.File]::ReadAllText($versionReportPath) | ConvertFrom-Json } catch { $versionReport = $null }
}
$versionIds = @('en-116', 'ru-116', 'es-117', 'ptbr-117')
$versionNames = @{ 'en-116' = 'English 1.0.116'; 'ru-116' = 'Russian 1.0.116'; 'es-117' = 'Spanish 1.1.117'; 'ptbr-117' = 'Brazilian PT 1.1.117' }
$windowsBuilds = if ($parity) { @($parity.builds) } else { @() }
$bootResult = @($results | Where-Object { $_.Name -eq 'linux_boot_tests' -and (Get-Prop $_ 'Platform') -eq 'linux' })
$versionCovered = $linux.Ran -and $bootResult.Count -gt 0
$versionFailed = $false
Write-Host '==> Game version parity' -ForegroundColor Cyan
Write-Host ('  {0,-22} {1,-9} {2,-12} {3,-13} {4,-9}' -f 'VERSION', 'WINDOWS', 'LINUX BUILD', 'LINUX STATIC', 'RUNTIME')
foreach ($id in $versionIds) {
    $row = if ($versionReport) { @($versionReport.versions | Where-Object { $_.id -eq $id }) | Select-Object -First 1 } else { $null }
    $winBuild = @($windowsBuilds | Where-Object { $_.id -eq $id }) | Select-Object -First 1
    $windowsState = if (-not $winBuild) { 'NOT RUN' } elseif (@($winBuild.problems).Count -or -not $winBuild.playable) { 'FAIL' } else { 'PASS' }
    $linuxBuild = if (-not $versionCovered) { 'NOT RUN' } elseif ($row) { [string]$row.build } else { 'FAIL' }
    $linuxStatic = if (-not $versionCovered) { 'NOT RUN' } elseif ($row) { [string]$row.static } else { 'FAIL' }
    $runtime = if (-not $versionCovered) { 'NOT RUN' } elseif ($row) { [string]$row.runtime } else { 'BLOCKED' }
    $states = @($windowsState, $linuxBuild, $linuxStatic, $runtime)
    $color = if ($states -contains 'FAIL') { 'Red' } elseif ($states -contains 'BLOCKED') { 'Yellow' } elseif ($states -contains 'NOT RUN') { 'DarkGray' } else { 'Green' }
    Write-Host ('  {0,-22} {1,-9} {2,-12} {3,-13} {4,-9}' -f $versionNames[$id], $windowsState, $linuxBuild, $linuxStatic, $runtime) -ForegroundColor $color
    if ($row -and $row.detail) { Write-Host ("    " + $row.detail) -ForegroundColor DarkGray }
    if ($states -contains 'FAIL') { $versionFailed = $true }
}
$versionStatus = if (-not $versionCovered) { 'NOT RUN' } elseif ($versionFailed -or -not $versionReport) { 'FAIL' } else { 'PASS' }
$versionSummary = if (-not $versionCovered) { 'the Linux version parity suite did not run' } elseif ($versionFailed) { 'a version failed' } elseif (-not $versionReport) { 'the Linux version parity report is missing' } else { 'four version profiles; runtime BLOCKED where data is absent' }
$results.Add([pscustomobject]@{ Section = 'Game versions'; Platform = 'parity'; Name = 'game version parity'; Status = $versionStatus; Seconds = 0; Checks = $versionIds.Count; Summary = $versionSummary; Lines = @(); Skipped = 0; Unavailable = $false })
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
$passed = @($results | Where-Object { $_.Status -eq 'PASS' })
$notRun = @($results | Where-Object { $_.Status -eq 'NOT RUN' })
$checkTotal = ($results | Measure-Object -Property Checks -Sum).Sum

$slowest = @($results | Where-Object { $_.Status -in 'PASS', 'FAIL' -and $_.Seconds -gt 0 } | Sort-Object Seconds -Descending | Select-Object -First 5)
$partlySkipped = @($results | Where-Object { $_.PSObject.Properties['Skipped'] -and $_.Skipped -gt 0 })
$windowsSeconds = Measure-Sum @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'windows' }) 'Seconds'
$linuxSeconds = Measure-Sum @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'linux' }) 'Seconds'
$windowsChecks = Measure-Sum @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'windows' }) 'Checks'
$linuxChecks = Measure-Sum @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'linux' }) 'Checks'
Write-Host ('  PASS: {0}   FAIL: {1}   SKIPPED parts in {2} suites   NOT RUN: {3}   checks counted: {4:N0}   time: {5:N0} s' -f $passed.Count, $failed.Count, $partlySkipped.Count, $notRun.Count, $checkTotal, $clock.Elapsed.TotalSeconds)
Write-Host ('  Test timing: Windows {0}  ({1} suites, {2:N0} checks)   Linux {3}  ({4} rows, {5:N0} checks)   total {6}' -f (Format-Clock $windowsSeconds), @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'windows' }).Count, $windowsChecks, (Format-Clock $linuxSeconds), @($results | Where-Object { (Get-Prop $_ 'Platform') -eq 'linux' }).Count, $linuxChecks, (Format-Clock $clock.Elapsed.TotalSeconds)) -ForegroundColor DarkGray
Export-TestHistory @($results | Where-Object { (Get-Prop $_ 'Platform') -in 'windows', 'linux' })
Write-Host ('  slowest: ' + (($slowest | ForEach-Object { '{0} {1:N1} s' -f $_.Name, $_.Seconds }) -join ', ')) -ForegroundColor DarkGray
if ($notRun.Count) {
    Write-Host '  NOT RUN (not failures; each says why):' -ForegroundColor Yellow
    foreach ($entry in $notRun) { Write-Host "    $($entry.Name): $($entry.Summary)" -ForegroundColor Yellow }
}
if ($notes.Count) {
    Write-Host '  parts the suites skipped or data unavailable (not failures, not proven here):' -ForegroundColor Yellow
    $notes | Select-Object -Unique | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}
if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
if ($Filter) { Write-Host "  PARTIAL RUN: only suites matching '$Filter' ran." -ForegroundColor Yellow }
if ($failed.Count) {
    Write-Host ('FAILED: {0}' -f (($failed | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Red
    exit 1
}
if ($parityFailed) { Write-Host 'FAILED: the Windows/Linux feature parity (global features that differ or are BLOCKED are listed above).' -ForegroundColor Red; exit 1 }
if ($linux.Requested -and -not $linux.Ran) { Write-Host 'FAILED: Linux was not validated.' -ForegroundColor Red; exit 1 }
if ($passed.Count -eq 0) { Write-Host 'FAILED: no suite ran.' -ForegroundColor Red; exit 1 }
if ($partial) { Write-Host 'PASSED (partial run).' -ForegroundColor Green; exit 0 }
Write-Host 'ALL TESTS PASSED' -ForegroundColor Green
exit 0
