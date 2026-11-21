param(
    [string]$WinFspInclude = "${env:ProgramFiles(x86)}\WinFsp\inc",
    [string]$VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

function Invoke-Native {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}

if (-not $VcpkgRoot) { throw "Set VCPKG_INSTALLATION_ROOT or pass -VcpkgRoot" }
$toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"

Invoke-Native cmake @(
    "-S", $root, "-B", $build, "-A", "x64",
    "-DWINFSP_INCLUDE_DIR=$WinFspInclude",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
)
Invoke-Native cmake @("--build", $build, "--config", "Release", "--parallel")
Invoke-Native cmake @("--install", $build, "--config", "Release", "--prefix", "$build/dist")
