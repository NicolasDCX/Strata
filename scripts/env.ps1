# Dot-source this file to put the project-local toolchain (installed by
# scripts/bootstrap-toolchain.ps1 into .toolchain/) on PATH for this shell:
#
#     . .\scripts\env.ps1
#
$root = Split-Path -Parent $PSScriptRoot
$qt = Join-Path $root '.toolchain\Qt'
$qtKit = Get-ChildItem -Path $qt -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object Name -Descending | Select-Object -First 1

if (-not $qtKit) {
    Write-Error "No Qt kit under $qt. Run scripts\bootstrap-toolchain.ps1 first."
    return
}

$mingw = Get-ChildItem -Path (Join-Path $qt 'Tools') -Directory -Filter 'mingw*_64' |
    Sort-Object Name -Descending | Select-Object -First 1

$env:CMAKE_PREFIX_PATH = Join-Path $qtKit.FullName 'mingw_64'
$env:PATH = @(
    (Join-Path $mingw.FullName 'bin'),
    (Join-Path $qt 'Tools\CMake_64\bin'),
    (Join-Path $qt 'Tools\Ninja'),
    (Join-Path $qtKit.FullName 'mingw_64\bin'),
    $env:PATH
) -join ';'

Write-Host "SculptEngine toolchain: Qt $($qtKit.Name) (MinGW), $($mingw.Name)"
