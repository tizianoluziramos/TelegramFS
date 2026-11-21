param(
    [string]$WinFspInclude = "${env:ProgramFiles(x86)}\WinFsp\inc",
    [string]$TdLibBin = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

function Invoke-Native {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}

Invoke-Native cmake @("-S", $root, "-B", $build, "-A", "x64", "-DWINFSP_INCLUDE_DIR=$WinFspInclude", "-DTELEGRAMFS_TDLIB_BIN=$TdLibBin", "-DTELEGRAMFS_PACKAGE_RUNTIME=ON")
Invoke-Native cmake @("--build", $build, "--config", "Release", "--parallel")
Invoke-Native cmake @("--install", $build, "--config", "Release", "--prefix", "$build/dist")
