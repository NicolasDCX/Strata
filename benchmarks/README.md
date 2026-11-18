# Benchmark results record

`results.csv` keeps one line per headline number, so a later experiment can be compared with the exact source it was measured on. The full write-ups (tables, method, caveats) stay in the main README; this file is the index.

Columns:

| column | meaning |
|---|---|
| `date` | day of the run |
| `commit` | short hash of the source that produced `result` (`pre-git`: measured before version control) |
| `experiment` | short slug, matching a README section |
| `mesh`, `helpers` | triangle count (21M = `--subdivisions 10`, 84M = 11) and helper threads |
| `case`, `metric` | what was measured |
| `baseline`, `result` | before and after; `;` separates interleaved rounds |
| `unit`, `config` | unit, and the `se_bench` / `SculptEngine` invocation |
| `notes` | anything needed to read the number |

Adding results:
1. Commit (or at least note `git rev-parse --short HEAD`) before measuring; a dirty tree is recorded as `<hash>-dirty`.
2. Copy the binaries out of `build/` and run interleaved: `tools/Bench/scripts/interleave.sh` (see its header), then `python tools/Bench/scripts/summarize.py <out-dir>`.
3. Append the headline lines here. Keep raw outputs out of git (`benchmarks/raw/` and `bench-out/` are ignored).

Machine for all current lines: Windows 11, Ryzen 9 7900X (12 cores), 31 GB RAM, RTX 4060 Ti 8 GB, GCC 13.1 `-O3`, Qt 6.10.2, release preset.
