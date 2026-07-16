#!/usr/bin/env pwsh
# Build GfxPluginMDI for Android (arm64-v8a by default).
#
# Requires:
#   - Android NDK (set ANDROID_NDK_ROOT, or override via -NdkRoot).
#   - cmake + ninja. If absent on PATH the script falls back to Unity's bundled
#     copies under <UnityEditor>\PlaybackEngines\AndroidPlayer\SDK\cmake\<ver>\bin.
#
# Usage:
#   pwsh -File build_android.ps1                       # arm64-v8a, Release
#   pwsh -File build_android.ps1 -Abi armeabi-v7a
#   pwsh -File build_android.ps1 -Abi arm64-v8a,armeabi-v7a -Config Release

param(
    [string[]] $Abi = @('arm64-v8a'),
    [string]   $Config = 'Release',
    [string]   $ApiLevel = '24',
    [string]   $NdkRoot = $env:ANDROID_NDK_ROOT,
    [string]   $CMake = $null,
    [string]   $Ninja = $null,
    [switch]   $Clean
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $NdkRoot -or -not (Test-Path $NdkRoot)) {
    throw "Android NDK not found. Set ANDROID_NDK_ROOT or pass -NdkRoot."
}
$toolchain = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path $toolchain)) {
    throw "Toolchain file not found: $toolchain"
}

function Resolve-Tool {
    param([string]$Name, [string]$Override)
    if ($Override) { return $Override }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # Fallback: Unity's bundled Android SDK ships cmake + ninja.
    $unityRoots = @()
    $hub = 'C:\Program Files\Unity\Hub\Editor'
    if (Test-Path $hub) {
        $unityRoots += Get-ChildItem $hub -Directory | Sort-Object Name -Descending | ForEach-Object { $_.FullName }
    }
    foreach ($root in $unityRoots) {
        $cmakeRoot = Join-Path $root 'Editor\Data\PlaybackEngines\AndroidPlayer\SDK\cmake'
        if (Test-Path $cmakeRoot) {
            $ver = Get-ChildItem $cmakeRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($ver) {
                $candidate = Join-Path $ver.FullName "bin\$Name.exe"
                if (Test-Path $candidate) { return $candidate }
            }
        }
    }
    throw "$Name not found. Install it or pass -$($Name.Substring(0,1).ToUpper() + $Name.Substring(1))."
}

$cmakeExe = Resolve-Tool -Name 'cmake' -Override $CMake
$ninjaExe = Resolve-Tool -Name 'ninja' -Override $Ninja

Write-Host "NDK:    $NdkRoot"
Write-Host "CMake:  $cmakeExe"
Write-Host "Ninja:  $ninjaExe"
Write-Host "Config: $Config | API: $ApiLevel"

foreach ($a in $Abi) {
    $buildDir = Join-Path $scriptDir "build/android-$a"
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "Cleaning $buildDir"
        Remove-Item $buildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    Write-Host "`n=== Configure: $a ==="
    & $cmakeExe -S $scriptDir -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$a" `
        "-DANDROID_PLATFORM=android-$ApiLevel" `
        "-DANDROID_STL=c++_shared" `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
        "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON" `
        "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384,-z,common-page-size=16384"
    if ($LASTEXITCODE -ne 0) { throw "Configure failed for $a" }

    Write-Host "`n=== Build: $a ==="
    & $cmakeExe --build $buildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $a" }
}

Write-Host "`nDone. Output: $(Resolve-Path (Join-Path $scriptDir '../Plugins/Android'))"
