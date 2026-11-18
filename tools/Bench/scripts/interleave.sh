#!/usr/bin/env bash
# Interleaved A/B benchmark runner for se_bench (Git Bash on Windows, or any POSIX shell).
#
#   tools/Bench/scripts/interleave.sh <out-dir> <rounds> <se_bench.exe> <variant>=<extra flags> ... -- <run>=<se_bench args> ...
#
# Every round runs every <run> once per <variant>, alternating variants, each in a
# fresh process. Output goes to <out-dir>/<run>_<variant>_r<round>.txt. On Windows each
# process is wrapped by peakrun.ps1, which appends peak working set, peak commit and the
# lowest free RAM. Copy se_bench.exe (with its DLLs) out of build/ first if you plan to
# rebuild while it runs.
#
# Example: serial vs parallel edit region at 21M and 84M with 11 helpers:
#   tools/Bench/scripts/interleave.sh bench-out/region 2 "$PWD/build/release/bin/se_bench.exe" \
#       ser= par=pregion -- \
#       adj10="adjacency 10 11 pgather pmark dense icones creuse" \
#       adj11="adjacency 11 11 pgather pmark dense icones creuse" \
#       hist11="history 11 11 2 pgather pmark dense icones creuse"
# Then: python tools/Bench/scripts/summarize.py bench-out/region
set -u
out="$1"; rounds="$2"; exe="$3"; shift 3
variants=(); runs=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do variants+=("$1"); shift; done
[ $# -gt 0 ] && shift
while [ $# -gt 0 ]; do runs+=("$1"); shift; done
mkdir -p "$out"
here="$(cd "$(dirname "$0")" && pwd)"
for r in $(seq 1 "$rounds"); do
  for run in "${runs[@]}"; do
    name="${run%%=*}"; args="${run#*=}"
    for v in "${variants[@]}"; do
      vname="${v%%=*}"; vflags="${v#*=}"
      file="$out/${name}_${vname}_r$r.txt"
      if command -v powershell >/dev/null 2>&1; then
        powershell -NoProfile -ExecutionPolicy Bypass -File "$here/peakrun.ps1" -Exe "$exe" -ArgLine "$args $vflags" -Out "$file"
      else
        "$exe" $args $vflags > "$file" 2>&1; echo "PROCESS exit $?" >> "$file"
      fi
      echo "$(basename "$file" .txt) | $(tail -1 "$file") $(date +%T)"
    done
  done
done
echo ALL_DONE
