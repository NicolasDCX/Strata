# Installs a project-local, admin-free Windows toolchain into .toolchain/:
#   Qt 6 (MinGW build), the matching MinGW-w64 GCC, CMake and Ninja.
# Nothing outside the repository is modified. Requires Python 3 and internet access.
#
#     .\scripts\bootstrap-toolchain.ps1 [-QtVersion 6.10.2]
#
param(
    [string]$QtVersion = '6.10.2'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolchain = Join-Path $root '.toolchain'
$venvPython = Join-Path $toolchain 'venv\Scripts\python.exe'
$out = Join-Path $toolchain 'Qt'

New-Item -ItemType Directory -Force $toolchain | Out-Null
if (-not (Test-Path $venvPython)) {
    python -m venv (Join-Path $toolchain 'venv')
}
& $venvPython -m pip install --quiet aqtinstall

# "python -m aqt" is used instead of aqt.exe so the download works even where
# unsigned launcher executables are not allowed to run.
& $venvPython -m aqt install-qt windows desktop $QtVersion win64_mingw --outputdir $out --archives qtbase
& $venvPython -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 --outputdir $out
& $venvPython -m aqt install-tool windows desktop tools_cmake qt.tools.cmake --outputdir $out
& $venvPython -m aqt install-tool windows desktop tools_ninja qt.tools.ninja --outputdir $out

Write-Host "Toolchain ready. Next: . .\scripts\env.ps1"
