# build_mr.ps1 -- one-shot firmware build for music-radio.
#
# ASCII-only (PowerShell 5.1 decodes BOM-less .ps1 as GBK; UTF-8 comments
# corrupt the script, known-issues #20). Adapted from english-podcast's
# build_epd.ps1 -- all mitigations preserved:
#   1. move build/.ninja_deps aside before EVERY build (ninja /D:/ bug, #1),
#      never touch .ninja_log (#35). -KeepDeps skips the move for fast
#      re-runs after a FAILED build.
#   2. remove IDF_CCACHE_ENABLE (any non-empty value enables it; "0" counts).
#   3. IDF_PY_BUILD_JOBS=4 -- C++ link has a high memory peak.
#   4. clear leaked MSYS* env vars (IDF export fatals on MSYSTEM, #8).
#
# Environment pairing (known-issues #8): D:\esp-mo\esp-idf pairs with the
# DEFAULT %USERPROFILE%\.espressif tool dir. Never cross the pairs.
#
# Usage:  powershell -ExecutionPolicy Bypass -File projects\music-radio\build_mr.ps1
param(
    [switch]$Clean,      # ask for a manual build\ removal
    [switch]$KeepDeps    # skip the .ninja_deps move (incremental re-run)
)

$ErrorActionPreference = "Stop"
$workspace = "D:\esp-mo\esp-mosaico-vibe-main"
$project = "projects/music-radio"
$build = "$workspace\projects\music-radio\build"
$idf = "D:\esp-mo\esp-idf"
$script = "$workspace\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if ($Clean) {
    Write-Host "[build] -Clean requested: please delete $build yourself in the IDE,"
    Write-Host "[build] then re-run without -Clean. (fullclean is blocked by the same guard.)"
    exit 2
}

# (2) remove, never set: any non-empty value enables ccache.
Remove-Item Env:IDF_CCACHE_ENABLE -ErrorAction SilentlyContinue
# (3) bounded parallelism: the C++ link step needs the memory.
$env:IDF_PY_BUILD_JOBS = "4"

# (4) leaked MSYS environment breaks the IDF export step.
foreach ($leak in @("MSYSTEM", "MINGW_PREFIX", "MINGW_CHOST", "MSYS")) {
    if (Test-Path "Env:$leak") {
        Write-Host "[build] clearing leaked $leak"
        Remove-Item "Env:$leak" -ErrorAction SilentlyContinue
    }
}

# (1) stale ninja dependency index -- move aside, never delete.
if (Test-Path "$build\.ninja_deps") {
    if ($KeepDeps) {
        Write-Host "[build] -KeepDeps: leaving build\.ninja_deps in place (incremental)"
    } else {
        Write-Host "[build] moving aside stale .ninja_deps (ninja /D:/ path bug)"
        Move-Item "$build\.ninja_deps" "$build\.ninja_deps.stale.$(Get-Date -Format yyyyMMddHHmmss)" -Force
    }
}

Write-Host "[build] building $project ..."
Push-Location $workspace
try {
    $env:IDF_PATH = $idf
    Write-Host "[build] IDF_PATH=$env:IDF_PATH  jobs=$env:IDF_PY_BUILD_JOBS"
    python $script --project $project build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK -- payload: $build\music_radio.bin"
Write-Host "[build] next   : python mosaico.py iris system-update --project $project"
