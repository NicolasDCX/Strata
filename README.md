# SculptEngine

First working prototype of a native 3D sculpting application: C++23, Vulkan viewport, Qt 6 editor.
It opens on a sphere that you can orbit around and sculpt with Draw, Smooth and Move brushes, with undo/redo.

This is a foundation, not a finished product. See [Known limitations](#known-limitations).

## What it does

- Qt 6 main window with an embedded Vulkan viewport, toolbar and status bar
- Procedural icosphere (81,920 triangles by default, configurable)
- Orbit / pan / zoom camera
- Mouse picking through a BVH, with a brush cursor drawn on the surface
- Draw, Smooth and Move brushes with adjustable radius and strength
- Undo / redo, one step per stroke, stored as sparse per-vertex deltas
- Edit cost proportional to the brush footprint: incremental BVH refit and page-based GPU uploads
- Sculpting on its own thread: input, undo and redo are queued and never block the interface
- Per-vertex work spread over helper threads for large brushes, with bit-identical results
- Smooth brush that adapts to the mesh density
- Paged geometry: vertices and triangles live in fixed-capacity pages with stable ids
- Spatial chunks with a two-level BVH (top level over chunks, local tree per chunk), built in parallel
- GPU uploads by GPU page, with page sizes independent of the geometry's
- Per-chunk frustum culling and back-facing chunk culling (normal cones kept up to date by the sculpt thread) against the current, deformation-aware chunk bounds (identical images, verified)
- Optional GPU memory budget: vertex pages and chunk indices are made resident on demand, nearest chunks first, with LRU eviction that is safe with frames in flight; the CPU copy stays complete and authoritative
- 151 unit tests that run without a GPU, plus scripted end-to-end self-tests of the real window (functional, and a memory-pressure scenario)

## Controls

| Input | Action |
|---|---|
| Left mouse on the model | Sculpt with the active brush |
| Ctrl + left mouse | Sculpt with the direction inverted (Draw carves) |
| Shift + left mouse | Smooth, whatever brush is selected |
| Left mouse on the background, right mouse, or Alt + left mouse | Orbit |
| Middle mouse, or Shift + right mouse | Pan |
| Mouse wheel | Zoom |
| `D` / `S` / `M` | Draw / Smooth / Move brush |
| `[` / `]` | Smaller / larger brush |
| `X` | Toggle Invert |
| `F` | Frame the model |
| `C` | Toggle chunk culling (for comparison; on by default) |
| Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) | Undo / Redo |

Each brush remembers its own radius and strength. Radius is in world units (the sphere has radius 1).

## Requirements

| Component | Version used for development | Notes |
|---|---|---|
| C++23 compiler | GCC 13.1 (MinGW-w64) | needs `<expected>` and `<format>`; MSVC 19.38+ and Clang 17+ should work but were not tested |
| CMake | 3.30 (minimum 3.28) | |
| Ninja | 1.12 | |
| Qt | 6.10.2 (minimum 6.5), modules Core, Gui, Widgets | must be built with Vulkan support (the official binaries are) |
| GPU driver | Vulkan 1.0 capable | no Vulkan SDK needed to build or run |
| Git | any | CMake downloads the dependencies below |

Fetched automatically by CMake when not already installed: GLM 1.0.1, Vulkan-Headers, glslang (shader compiler, built as a host tool), GoogleTest 1.15.2.
If a Vulkan SDK is installed (`VULKAN_SDK` set), its headers and `glslangValidator` are used instead.

## Build on Windows

There are two ways to get a toolchain. Both produce the same build.

**A. Project-local toolchain, no administrator rights** (what this repository was developed with).
Needs Python 3 and Git on `PATH`. Downloads about 0.9 GB into `.toolchain/` and changes nothing else on the machine.

```powershell
.\scripts\bootstrap-toolchain.ps1
. .\scripts\env.ps1
cmake --preset release
cmake --build --preset release
```

**B. Your own toolchain.** Install Qt 6 (MinGW or MSVC kit), CMake and Ninja, open a shell where the compiler is available, then:

```powershell
cmake --preset release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\mingw_64
cmake --build --preset release
```

The build copies the Qt runtime next to the executable (`windeployqt`), so `build\release\bin\SculptEngine.exe` starts from Explorer.

> **Smart App Control.** Windows 11 Smart App Control in enforcing mode blocks every unsigned executable,
> including MinGW's compiler and anything you build yourself. Native development on such a machine requires
> turning it off in *Windows Security > App & browser control*, or building inside WSL.

## Build on Linux

Not tested yet (the prototype was built and run on Windows only); the code uses no platform-specific API.

```bash
# Debian/Ubuntu example. Qt >= 6.5 is required; use the Qt online installer or aqtinstall if your distribution is older.
sudo apt install build-essential cmake ninja-build git qt6-base-dev libvulkan-dev libgl1-mesa-dev
cmake --preset release
cmake --build --preset release
```

## Run

```powershell
.\build\release\bin\SculptEngine.exe                   # 81,920 triangles
.\build\release\bin\SculptEngine.exe --subdivisions 7  # 327,680 triangles
```

| Option | Meaning |
|---|---|
| `--subdivisions <n>` | Icosphere level, 20 x 4^n triangles. 5 = 20k, 6 = 82k (default), 7 = 328k, 8 = 1.3M, 9 = 5.2M, 10 = 21M, 11 = 84M (maximum; about 8 GB of RAM and 2 GiB of GPU memory, see "Scaling checkpoint") |
| `--vertex-page <n>` | Vertices per geometry page, a power of two. Default 1024 |
| `--chunk <n>` | Triangles per spatial chunk, a power of two. Default 4096 |
| `--adjacency <compact\|wide>` | Storage of the per-vertex neighbour and incident-triangle lists. `compact` (default): 16-bit offsets from a per-page anchor, 58% of the wide size at 5.2M triangles. `wide`: u32 ids. Same results bit for bit; see "Compact adjacency" |
| `--neighbors <stored\|derived>` | Vertex neighbour lists. `stored` (default): a table built at startup. `derived` (opt-in): no table, each ring rebuilt from the incident triangles when read; 40 MiB less at 5.2M triangles, same results bit for bit, Smooth 3 to 6% slower per edit; see "Derived neighbours" |
| `--gather <parallel\|serial>` | Vertex gather of each dab on the helper threads (`parallel`, default) or on the sculpt thread (`serial`). Same results bit for bit; see "Parallel gather" |
| `--marking <parallel\|serial>` | BVH leaf marking of each dab on the helper threads, one chunk per task (`parallel`, default), or on the sculpt thread (`serial`). Same results bit for bit; see "Parallel BVH leaf marking" |
| `--box-refit <dense\|incremental>` | BVH box refit: a chunk whose refit touches at least half of its leaves sweeps its whole tree in node order (`dense`, default), or only the touched nodes level by level (`incremental`). Same boxes bit for bit; see "Dense box refit" |
| `--cones <incremental\|exact>` | Normal cones after an edit: a chunk with at most half of its leaves changed widens its cone by the new normals and gets it rebuilt exactly in the background (`incremental`, default), or every touched chunk recomputes its cone (`exact`). Culling stays conservative either way; see "Incremental normal cones" |
| `--region <parallel\|serial>` | Edit region walk of each dab, undo and redo on the helper threads when at least 16K vertices moved (`parallel`, default; a 4-byte stamp per vertex, 160 MiB at 84M), or on the sculpt thread (`serial`). Same lists in the same order, same results bit for bit; see "Parallel edit region" |
| `--dab-prepare <overlap\|serial>` | Undo record, average normal and page staging of each dab of 16K vertices or more: at the same time on the helper threads (`overlap`, default), or one after the other (`serial`). Same results bit for bit; see "Gather profile at 84M and overlapped dab preparation" |
| `--move-updates <latest\|all>` | Move updates queued behind the one being applied: only the newest that changes anything is applied (`latest`, default), or every one (`all`). Same final mesh, undo record, dirty pages and (once idle) BVH and cones; only intermediate frames are skipped. See "Latest-wins Move updates" |
| `--region-overlap <on\|off>` | With `--dab-prepare overlap`: walk each dab's edit region and queue its BVH leaves at the same time as the undo record, average normal and page staging (`on`), or after the brush (`off`, default). Same results bit for bit; see "Large-brush Draw and Smooth at 84M" |
| `--smooth-kernel <quadratic\|trilinear>` | Blend of Smooth's dense-mode kernel cells: quadratic B-spline over 3x3x3 cells (`quadratic`, default) or the former trilinear blend over 2x2x2 cells (`trilinear`), which creases the surface along the cell planes and leaves angular bands along strokes; see "Smooth bands: kernel-cell creases" |
| `--smooth-dense <n>` | Average vertices per kernel cell from which a Smooth dab uses the dense-mode filter instead of Laplacian passes (default 4; 12 before "Smooth strength across mesh resolutions") |
| `--smooth-binning <parallel\|serial>` | Smooth's kernel grid for dabs of 32K vertices or more: binned on the helper threads, each cell summed in list order (`parallel`, default; 4 bytes per dab vertex), or on the sculpt thread (`serial`). Same results bit for bit; see "Smooth profile at 84M and parallel kernel-grid binning" |
| `--cone-normals <reuse\|recompute>` | Exact normal cones keep each face normal of their first pass in a per-thread scratch and reuse it in the second (`reuse`, default; 64 KiB per thread), or compute it again (`recompute`). Same cones bit for bit; see "Face normals reused in exact cones" |
| `--history-normals <before\|under>` | Undo and redo compute the new normals before taking the document lock, from the history entry's positions staged over the mesh (`before`, opt-in), or under it (`under`, default). Same results bit for bit; shorter lock hold, longer total. See "Undo and redo: normals before the lock" |
| `--threads <n>` | Helper threads for the sculpt thread. 0 = none. Default: about one per physical core |
| `--validation` | Enable `VK_LAYER_KHRONOS_validation` when it is installed (needs the Vulkan SDK) |
| `--cull <mode>` | Chunk culling: `on` (default: view frustum + chunks facing entirely away), `frustum` (view frustum only), `off`, `unmerged` (as `on`, one draw call per chunk instead of one per run of adjacent index slots) |
| `--normal-cones <on\|off>` | Keep a normal cone per chunk, updated by the sculpt thread's BVH refit (default `on`). With `off` no chunk is skipped as back-facing |
| `--lod` | Opt-in: draw a coarse, crack-free stand-in for visible chunks whose detailed pages are not on the GPU. Allocated only with a budget too small for the whole mesh; its memory comes out of that budget |
| `--lod-group-chunks <n>` | With `--lod`: chunks simplified and drawn coarse as one unit, 1 (default), 2, 4 or 8. Larger groups need less memory but follow strong deformation worse |
| `--lod-ratio <r>` | With `--lod`: fraction of the triangles the stand-ins keep. Default 1/16 (0.0625) |
| `--lod-draws <mode>` | With `--lod`: `indirect` (default; all coarse groups in one multi-draw-indirect call per pass, when the device supports it) or `direct` (one draw per group) |
| `--lod-always` | Testing: with `--lod`, allocate the fallback even when the pools hold the whole mesh (seam checks) |
| `--gpu-budget <MiB>` | Limit the GPU memory for mesh data: pages are made resident on demand and evicted when the pools are full. Without this option (or the next) the pools are sized for the whole mesh plus spare slots for edits, and every page stays resident |
| `--gpu-budget-fraction <f>` | The same budget given as a fraction of what one complete copy of the mesh needs, e.g. `0.25` |
| `--max-upload <MiB>` | Most mesh data copied to the GPU in one frame (the initial fill of whole-mesh pools is not limited). Default 32 |
| `--selftest <dir>` | Run the scripted smoke test, write screenshots and `report.txt` to `<dir>`, exit 0 on success |
| `--frame-on-commit on\|off` | Render a frame as soon as an edit is published (`on`), or request one and let Qt deliver the request from its timer (`off`, default). Measured: no net latency gain; the frames then wait for the swap chain and delay input ("Input to frame in the Vulkan viewport at 84M") |
| `--selftest-scenario <name>` | `standard` (default), `pressure` (fixed views, camera motion, live sculpting, undo/redo; meant to run under a small budget) `history` (repeated undo/redo of typical and strong strokes while frames render back to back; reports the renderer's lock wait, the commands' hold and total, and the scratch), `move`, `smooth`, `latency` (one Draw and one Smooth stroke at radius 0.25 with 120 Hz scripted input; each dab-producing input is linked to its commit and to the first frame that took it; writes `trace_draw.csv` and `trace_smooth.csv`) or `smoothfeel` (Smooth along half of a bumpy Draw ridge from world-space rays, so the same stroke at every level; grazing-light close-ups before, after one pass and after five, with a crest height profile and Smooth's dense/sparse dab count) |
| `--selftest-reference <dir>` | Compare the deterministic captures of this run with an earlier run in `<dir>` (only where every visible chunk is drawn) |

## Tests

```powershell
ctest --preset release
```

Runs the Geometry and Sculpt/Application test suites. They do not create a window or touch the GPU.
To build only the engine libraries, the tests and the benchmark (no Qt, no Vulkan):

```powershell
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
```

End-to-end test of the real window and renderer (needs a display and a Vulkan GPU):

```powershell
.\build\release\bin\SculptEngine.exe --selftest selftest-output
```

Memory pressure: a reference run with the default (whole-mesh) pools, then the same script under a budget, compared with it:

```powershell
.\build\release\bin\SculptEngine.exe --subdivisions 9 --selftest-scenario pressure --selftest pressure-ref
.\build\release\bin\SculptEngine.exe --subdivisions 9 --selftest-scenario pressure --gpu-budget-fraction 0.9 --selftest-reference pressure-ref --selftest pressure-90
.\build\release\bin\SculptEngine.exe --subdivisions 9 --selftest-scenario pressure --gpu-budget-fraction 0.25 --selftest-reference pressure-ref --selftest pressure-25
```

Headless performance measurements:

```powershell
.\build\release\bin\se_bench.exe 6 8
```

Lock hold, reader wait, dab latency and input backlog of live strokes through `AppController` (level, helpers, rounds):

```powershell
.\build\release\bin\se_bench.exe live 9 11 2
```

The same with the parallel gather and the parallel BVH leaf marking (`se_bench` runs both serially unless given `pgather` and `pmark`, so they can be compared; the application's default is parallel for both):

```powershell
.\build\release\bin\se_bench.exe live 9 11 2 pgather pmark
```

## Verified status

Everything below was actually run on the development machine
(Windows 11, Ryzen 9 7900X, GeForce RTX 4060 Ti, GCC 13.1 `-O3`, Qt 6.10.2).

| Check | Result |
|---|---|
| Release build, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` | builds with no warnings in project code |
| Unit tests | 207 / 207 pass in the release preset (126 geometry, 81 sculpt), including 2 latest-wins Move tests (a 28-step script of overloaded, reversing, zero-drag, rejected-ray, immediate-release, mixed-brush, settings-change and undo/redo strokes at 0, 1, 3 and 11 helpers: positions, normals, undo and redo records, dirty pages, boxes, cones, stale flags, top level and picks equal to applying every update; cones conservative while strokes run; Draw and Smooth never dropped). Before them: 205 / 205 (126 geometry, 79 sculpt), including an input-sample test (Draw, Smooth and Move strokes through the controller: one sample per input in order, consistent queued/started/finished times, inputs processed one after the other, Move one application per update and none at begin, dabs adding up to the stroke's dab count). Before it: 204 / 204 (126 geometry, 78 sculpt), including 2 Smooth-binning tests (one dense dab with its list ascending, shuffled and reversed, and Smooth strokes of 4 radii and strengths: positions, every dab's hash and undo deltas bit-identical to serial binning at 0, 1, 3 and 11 helpers; reversing the block order fails both). Before them: 202 / 202 (126 geometry, 76 sculpt), including 2 overlapped-preparation tests (Draw, Smooth and Move strokes overlapped at 0, 1, 3 and 11 helpers against serial preparation: every dab's hash, undo deltas bit for bit, BVH boxes and cones, picks; undo/redo through the controller). Before them: 200 / 200 (126 geometry, 74 sculpt), with the one-pass edit region and two one-ring-heavy moved sets added to the list-order test (every third vertex of a patch, a random 30% of the mesh). Before: 200 / 200, including 3 parallel edit-region tests (triangle and vertex lists equal to the serial walk element for element for 3 layouts, shuffled, sorted, repeated, scattered and whole-mesh moved lists and sizes around the threshold, at 1, 3 and 11 helpers; Draw, Smooth and Move strokes and undo/redo through the controller bit-identical to the serial walk). Before them: 197 / 197 (126 geometry, 71 sculpt), including 3 exact-size BVH build tests (node count against the build recursion for every count up to 200,000 and up to 100M; size, capacity and nodes of fresh and reused trees around every split boundary; partial last chunks, serial and threaded, with rays against brute force). Before them: 194 / 194 (123 geometry, 71 sculpt), including 2 face-normal reuse tests (cones bit-identical to `computeNormalCone` for smooth, crumpled, folded and degenerate meshes of 1 to 8,192 triangles; chunk cones exact and no visible triangle culled through repeated folds, serial and with 3 helpers) and reuse runs of the controller cone test. Before them: 192 / 192 (121 geometry, 71 sculpt), including 3 incremental-cone tests (after every edit, partial rebuild and optimize step, every cone holds every normal, no rejected chunk has a triangle facing any of 20 eyes, boxes and picks equal exact cones, cones that are not stale are exact; after the rebuild everything is bit-identical to exact cones; through the controller at 0, 3 and 11 helpers, identical once idle) and a third, incremental-cone variant of the dab and undo/redo concurrent-reader tests. Before them: 187 / 187 (121 geometry, 66 sculpt), including 2 dense-refit tests (every node box, cone, pick and optimize step identical to the incremental refit and to a full refit for edits of any size and chunks of 64 to 4,096 triangles; strokes and undo/redo through the controller identical at 0, 3 and 11 helpers) and the dab and undo/redo concurrent-reader tests run with both refits. Before them: 183 / 183 (121 geometry, 62 sculpt), including 2 history-staging tests (undo/redo with the normals computed before the lock: hash, boxes and cones identical to computing them under it after every step at 0, 1, 3 and 11 helpers; a concurrent reader sees only states between commands, a dirty-page mirror equal to the mesh, and consistent normals, boxes and cones). Before them: 181 / 181 (121 geometry, 60 sculpt), including 3 parallel-marking tests (boxes, cones, picking and every background optimize step identical to serial marking at 0, 1, 3 and 11 helpers, for a shuffled list with duplicates and through Draw, Smooth and Move strokes; undo and redo through the controller, marked before the lock, identical at every step and exact). Before them: 178 / 178 (121 geometry, 57 sculpt), including 2 parallel-gather tests (serial and parallel gather bit-identical at 0, 1, 3 and 11 helpers: every dab's positions and normals, undo deltas, exact undo/redo; switching modes between strokes). Before them: 176 / 176 (121 geometry, 55 sculpt), including 3 prepare/commit tests (preparing a dab changes nothing readers see; after every commit normals, BVH boxes and cones match the positions bit for bit; a concurrent reader only sees states between dabs and a dirty-page mirror equal to the mesh; exact undo/redo through the controller). Before them: 173 / 173 (121 geometry, 52 sculpt), including 5 derived-neighbour geometry tests (rings identical to the stored rows for both layouts, page sizes 16 to 65,536, shuffled ids, valence 300, isolated vertices, flipped, degenerate and shared-vertex fans) and 4 derived-neighbour sculpt tests (Draw, Smooth and Move bit-identical to stored neighbours, serial, parallel and with the wide layout; exact undo/redo with 64-vertex pages and 128-triangle chunks). Before them: 164 / 164 (116 geometry, 48 sculpt), including 3 edit-region tests (the shared per-dab triangle and vertex lists equal the two separate walks, for both layouts and any vertex order; refit from the list bit-identical to refit from the moved vertices, serial and parallel), 5 compact-adjacency geometry tests and 4 compact-adjacency sculpt tests (bit-identical to wide, serial and parallel, exact undo/redo). Before the edit region: 161 / 161 (113 geometry, 48 sculpt). Before compact adjacency: 152 / 152 (108 geometry, 44 sculpt), including 5 normal-cone tests. Earlier: 143 / 143 in the release, debug and headless presets. Removing the old fully resident path removed its 5 page-tracker tests; the dirty-page mapping they covered and the new index-range merging have their own tests |
| Single storage path, 1.3M and 5.2M | standard self-test: the 10 scripted captures (sculpting, undo, redo, camera moves) are pixel-identical to the old fully resident path, and every capture has full coverage (all visible chunks drawn, no stale page). Pressure self-test with the default pools: full, zoomed, after-motion and undo captures identical to the old path, full coverage throughout, 0 evictions |
| Self-test, 82k triangles (default) | pass: rendering, picking, 3 brushes, undo/redo, camera, real mouse/keyboard events, window resize, on-screen presentation |
| Self-test, 328k triangles | pass |
| Self-test, 1.3M triangles | pass |
| Self-test, 5.2M triangles | pass |
| Self-test, 21M triangles (`--subdivisions 10`), prepare/commit build and the build before it | pass, exact undo/redo; renderer worst lock wait 16.7 ms before, 0.001 ms after ("Prepare and commit") |
| Standard self-test at 84M, serial and parallel Smooth binning, back to back, 2 rounds | pass, 237 checks each, picking, exact undo/redo of all strokes, every cone holds its normals; Smooth worst input latency 34.9 / 33.4 → 32.5 / 31.3 ms; dab latency (all brushes) within noise; memory unchanged ("Smooth profile at 84M and parallel kernel-grid binning") |
| Standard self-test at 84M, serial and overlapped dab preparation, back to back, 2 rounds | pass, 237 checks each, picking, exact undo/redo of all strokes, every cone holds its normals; dab latency median 18.3 / 17.6 → 17.0 / 16.5 ms, p95 23.9 / 23.8 → 21.7 / 21.6 ms; memory unchanged ("Gather profile at 84M and overlapped dab preparation") |
| Standard self-test at 84M, one-pass edit region against f71cb46, back to back | pass, 237 checks each, picking, exact undo/redo of all strokes, every cone holds its normals; dab latency median 18.9 → 17.9 ms, p95 25.2 → 23.5 ms; edit scratch 480 → 438 MiB ("Dab profile at 84M and the one-pass edit region") |
| Standard self-test at 84M with the parallel and the serial edit region, and at 21M with the parallel one and with the new default | pass, 237 checks each, picking, exact undo/redo of all strokes, every cone holds its normals; 84M dab latency median 27.3 → 16.5 ms, p95 36.3 → 22.6 ms, lock hold median 2.7 → 2.3 ms; peak commit 11.84 → 12.05 GiB ("Parallel edit region") |
| Standard self-test at 84M with the exact-size BVH build | pass, 237 checks, picking, exact undo/redo of all strokes, every cone holds its normals; startup 50.0 → 36.2 s ("Exact-size chunk BVH build") |
| Standard self-test at 84M and 21M, history self-test at 84M (`--subdivisions 11`) | pass, 237 checks each, exact undo/redo, every visible chunk drawn; 84M live-stroke dab latency median 28.9 ms, 155 inputs queued at release ("Scaling checkpoint: 84M triangles") |
| History self-test, 21M triangles, face normals reused and recomputed in exact cones, 0 and 11 helpers, 2 runs each; standard self-test at 21M with reuse | pass (237 checks for the standard one), exact undo/redo, cones hold every normal; same triangles drawn per frame within 0.03%; undo hold 50.3 → 47.2 ms without helpers, 9.65 → 9.17 ms with 11 ("Face normals reused in exact cones") |
| History self-test, 21M triangles, exact and incremental cones, 0 and 11 helpers, 2 runs each; standard self-test at 21M with both | pass (238 checks for the standard one), exact undo/redo, cones hold every normal; same triangles drawn per frame within 0.02%; undo hold 52.0 → 50.6 ms without helpers, unchanged with 11 ("Incremental normal cones") |
| History self-test, 21M triangles, dense and incremental box refit, 0 and 11 helpers, 2 runs each; standard self-test at 21M with both and with the new default | pass (237 checks for the standard one), exact undo/redo; undo hold 11.8 → 9.7 ms with 11 helpers, 70 → 52 ms without ("Dense box refit") |
| Move self-test (`--selftest-scenario move`), 84M triangles, `--move-updates all` 3 runs and `latest` 4 runs | 6 of 7 pass all 29 checks; the 7th (latest) counted 620 inputs for 500 sent in one stroke, likely stray pointer input, and three reruns counted exactly. Same mesh hashes in every run and mode; undo exact; undone and redone meshes pixel-identical. Large-brush drain 0.33–27.5 s → 15–122 ms ("Latest-wins Move updates") |
| History self-test (`--selftest-scenario history`), 21M triangles, both normal modes, 0 and 11 helpers, 2 runs each | pass: frames overlap every command, 4 cycles of undo all + redo all end exactly where they began, cones hold every normal, the undone mesh renders pixel-identical to the initial one ("Rendering overlapped with undo/redo") |
| Self-test, 21M triangles with `--history-normals before`, with it off, and with the build before it; 5.2M with it on | pass, 238 checks each, exact undo/redo ("Undo and redo: normals before the lock") |
| Self-test, 21M and 5.2M triangles, parallel BVH leaf marking (default), and 21M with the build before it | pass, 237 checks each, exact undo/redo; 21M dab latency median 9.4 → 8.1 ms ("Parallel BVH leaf marking") |
| Self-test, 21M and 5.2M triangles, parallel gather (default), and 21M with the prepare/commit build before it | pass, 237 checks each, exact undo/redo; 21M dab latency median 11.3 → 8.1 ms ("Parallel gather") |
| Self-test, 328k triangles with `--vertex-page 4096 --chunk 16384` | pass |
| Live stroke in the window (frames rendered while the sculpt thread works) | pass at all four sizes |
| Culling: images with culling off, frustum only, on (frustum + back-facing chunks), and one draw per chunk, in 4 views (full, zoomed, zoomed + orbit, only the limb visible) and in 4 more views after strong deformations, after undoing them and after redoing them | pixel-identical, at 1.3M and 5.2M triangles; every published normal cone holds its chunk's normals after each stage |
| Back-facing culling against a run without normal cones (`--normal-cones off`) | standard self-test: all 22 scripted captures pixel-identical at 1.3M and 5.2M. Pressure self-test: identical wherever every visible chunk is drawn, except 1 to 5 pixels after redo under a 90% budget, which the runs without cones show identically (Known limitations) |
| Self-test with a budget large enough for the whole mesh (`--gpu-budget-fraction 1.2`), 5.2M triangles | pass, including the culling comparisons |
| Pressure self-test, 1.3M and 5.2M, default pools / 90% / 25% budget | pass. At 90% the zoomed captures before and after the camera motion and after undo are pixel-identical to the reference; at 25% the view does not fit and the nearest chunks are drawn |
| Edits under memory pressure | every edited page reaches the GPU once the renderer settles (0 stale pages), undo and redo restore the picture exactly; thousands of evictions during camera motion at 25% |
| Start, close window, with `--gpu-budget 10 --cull off` (1.3M) | exits with code 0 |

Not verified: Linux, MSVC, Clang, other GPUs, and the Vulkan validation layer (not installed on the machine, so API usage has not been machine-checked). The threaded code was not run under a race detector (ThreadSanitizer is not available with this MinGW toolchain); its correctness rests on the design and on the tests described in docs/Architecture.md.

All figures: Release build, brush radius 0.25. "Phase 3" is the version with one flat mesh array and a single BVH; "Phase 4" the paged, chunked version before culling and residency. The tables on memory, edit cost and responsiveness below are from Phase 4; sculpt-side code did not change in Phase 5 and `se_bench` gives the same figures (Draw edit, 1 thread: 0.85 to 0.91 ms at 1.3M, 3.4 to 3.8 ms at 5.2M).

### Memory and construction

| Mesh | Mesh memory | Acceleration structure | Mesh construction | BVH build, 1 thread / 11 helpers | Total startup |
|---|---|---|---|---|---|
| 82k triangles | 4.1 MiB | 2.3 MiB | 19 ms | 17 / 2 ms | 27 ms |
| 328k triangles | 16.3 MiB | 9.2 MiB | 82 ms | 67 / 8 ms | 93 ms |
| 1.3M triangles | 65.1 MiB | 36.7 MiB (Phase 3: 52.0) | 363 ms (Phase 3: 88) | 269 / 27 ms (Phase 3: 321) | 407 ms (Phase 3: about 410) |
| 5.2M triangles | 260.3 MiB | 146.8 MiB | 1,760 ms (Phase 3: 563) | 1,090 / 133 ms (Phase 3: 1,418) | 1,948 ms (Phase 3: about 1,980) |

Mesh memory is unchanged by paging (rounding to whole pages and the page tables cost under 1%). More than half of it is adjacency. These figures are with the wide adjacency layout; the compact layout, now the default, brings 5.2M to 201.9 MiB and 1.3M to 50.4 MiB (see "Compact adjacency").
Mesh construction is three times slower than before because the spatial ordering is now a recursive median split; the BVH, built per chunk in parallel, is ten times faster. Startup time is the same overall.

### Edit cost

Measured with `se_bench`, each size in a separate run: one Draw stroke. An edit is gather + deform + normals + BVH refit.

| Mesh | Vertices per edit | Phase 3, 1 thread | Now, 1 thread | Phase 3, 11 helpers | Now, 11 helpers | Uploaded per frame (Phase 3) | Raycast (Phase 3) |
|---|---|---|---|---|---|---|---|
| 82k triangles | ~550 | 0.06 ms | 0.06 ms | 0.06 ms | 0.06 ms | 9.5% of mesh (13.5%) | 0.65 us (0.59) |
| 328k triangles | ~2,200 | 0.21 ms | 0.22 ms | 0.21 ms | 0.22 ms | 4.1% (5.5%) | 0.78 us (0.64) |
| 1.3M triangles | ~8,800 | 0.87 ms | 0.89 ms | 0.80 ms | 0.80 ms | 2.9% (3.0%) | 1.37 us (1.18) |
| 5.2M triangles | ~35,400 | 3.6 ms | 3.7 to 4.1 ms | 2.9 ms | 3.2 to 4.1 ms | 2.1% (2.2%) | 1.89 us (1.57) |

- Edit latency is at Phase 3 level up to 1.3M triangles and up to about 10% slower at 5.2M on one thread. With helpers at 5.2M it is 10 to 40% slower and varies between runs.
- Raycasts are 10 to 20% slower: two tree levels instead of one, plus a page-table lookup per vertex.
- Uploads are smaller at every size.
- The first paged version was 20 to 55% slower per edit. That was traced to paged adjacency lookups and out-of-line calls in the BVH refit and fixed; the remaining difference is the page-table lookup itself.

### Page and chunk size

`se_bench 8 8 11 sweep`, 1.3M triangles, Draw stroke, best of three runs:

| Vertex page | Edit, 1 thread | Uploaded per frame | | Chunk | Edit, 1 thread | BVH tidy-up after stroke |
|---|---|---|---|---|---|---|
| 256 | 1.18 ms | 2.1% of mesh | | 512 triangles | 1.05 ms | 21 ms |
| 1,024 (default) | 1.11 ms | 2.9% | | 1,024 | 1.02 ms | 24 ms |
| 4,096 | 1.24 ms | 3.9% | | 4,096 (default) | 1.05 ms | 31 ms |
| 16,384 | 1.15 ms | 9.1% | | 16,384 | 1.07 ms | 32 ms |
| 65,536 | 1.03 ms | 29.3% | | 65,536 | 1.03 ms | 32 ms |

Edit time does not depend measurably on either size (differences are within run-to-run noise). The vertex page size decides how much is uploaded, so small pages win; 1,024 was kept as a balance between upload volume and page count. Chunk size only affects the background tidy-up. (These runs were taken before the last two optimisations, so absolute edit times are about 15% higher than in the table above.)

### Responsiveness

Sculpting runs on its own thread. Two measurements:

**Headless**, through the same `AppController` the editor uses: a 600-sample stroke is queued at once while a second thread takes the mesh lock every 4 ms, as the renderer does once per frame.

| Mesh | Queuing one input | Worst wait for the mesh lock (Phase 3) | BVH tidy-up after the stroke (Phase 3) |
|---|---|---|---|
| 328k triangles | 0.0001 ms | 0.24 ms | under 10 ms, in the background |
| 1.3M triangles | 0.0001 ms | 0.86 ms (0.65) | 33 ms (19), in the background |
| 5.2M triangles | 0.0001 ms | 3.4 ms (3.3) | 140 ms (about 100), in the background |

**In the application**: the self-test draws a stroke of 500 inputs at 2 ms intervals while the window renders normally, and records every frame.

| Mesh | Frames during the stroke | Frame interval, median / p95 / max | Frame CPU time, median / max | Wait for the mesh lock, p95 / max | Vertices uploaded per frame, median |
|---|---|---|---|---|---|
| 82k triangles | 145 | 7.0 / 29.3 / 40.8 ms | 0.02 / 0.18 ms | 0.00 / 0.15 ms | 3,072 |
| 1.3M triangles | 154 | 16.7 / 17.0 / 33.3 ms | 0.03 / 0.07 ms | 0.00 / 0.00 ms | 14,336 |
| 5.2M triangles | 146 | 7.5 / 27.0 / 59.4 ms | 0.11 / 2.6 ms | 1.4 / 2.5 ms | 41,984 |

Frame intervals are set by the display and by when redraws are requested, not by the mesh size: the renderer's own work stays under 3 ms even at 5.2M triangles. No input was left in the queue when the stroke was released at any size.
The tidy-up re-partitions more triangles than in Phase 3 (whole chunks of 4,096 instead of blocks of 2,048), which is why it takes longer in total; it runs in sub-millisecond steps and blocks neither input nor rendering.
### Chunk culling (Phase 5)

From the self-test: 90 presented frames per row, after the renderer settled. GPU time is measured with timestamp queries around the render pass; CPU time is the renderer's work per frame on the UI thread. Old fully resident storage (as in Phase 4). Draw calls: with culling, consecutive visible chunks are adjacent in the index buffer and are drawn as one run. Current numbers, at full GPU clock and with the single storage path, are in "One GPU storage path" below.

| View | Mesh | Chunks drawn | Triangles submitted | Draw calls | GPU ms, median (p95) | CPU ms, median |
|---|---|---|---|---|---|---|
| Fully visible, culling off | 5.2M | 1,280 / 1,280 | 5.24M | 1 | 6.71 (6.95) | 0.021 |
| Fully visible, culling on | 5.2M | 1,280 | 5.24M | 1 | 6.72 (6.92) | 0.038 |
| Fully visible, one draw per chunk | 5.2M | 1,280 | 5.24M | 1,280 | 6.71 (7.29) | 0.065 |
| Zoomed, culling off | 5.2M | 1,280 | 5.24M | 1 | 6.71 (7.01) | 0.018 |
| Zoomed, culling on | 5.2M | 678 | 2.78M | 62 | 3.61 (3.62) | 0.043 |
| Only the limb in view, culling off | 5.2M | 1,280 | 5.24M | 1 | 6.70 (6.72) | 0.019 |
| Only the limb in view, culling on | 5.2M | 536 | 2.20M | 39 | 2.84 (2.85) | 0.036 |
| Fully visible, culling off / on | 1.3M | 320 | 1.31M | 1 | 1.93 (3.38) / 2.02 (2.70) | 0.019 / 0.024 |
| Zoomed, culling off / on | 1.3M | 320 / 207 | 1.31M / 0.85M | 1 / 21 | 2.69 (3.69) / 2.74 (4.27) | 0.017 / 0.022 |
| Only the limb in view, culling off / on | 1.3M | 320 / 152 | 1.31M / 0.62M | 1 / 18 | 2.29 (3.08) / 1.67 (2.81) | 0.016 / 0.023 |

- **Overhead when everything is visible:** none measurable on the GPU. Testing 1,280 boxes costs 0.02 ms of CPU; one draw call per chunk instead of merged runs adds another 0.03 ms.
- **Benefit:** at 5.2M triangles GPU time falls in proportion to the triangles submitted, from 6.7 ms to 3.6 ms zoomed and 2.8 ms when only the limb is visible. At 1.3M the GPU runs at low clocks and the medians are within noise, except in the limb view.
- Frustum culling cannot remove the far side of the model behind the visible surface, so a zoomed view still submits about half of the mesh.
- Frame intervals (not in the table) are set by presentation, not by rendering: median 6 to 17 ms and p95 about 29 ms in every row.
- **This table was measured with the fully resident vertex buffers in host memory.** Rendering was then bound by reading vertices over PCIe, which is why GPU time followed the triangle count so closely. It is kept as the record of that configuration.

### One GPU storage path

The renderer now has a single storage path: the page pools that Phase 5 introduced for budgets. By default they are sized for the whole mesh plus spare vertex slots for copy on write (an eighth of the pages, at least 64), filled once at startup in chunk order, and every page stays resident. An explicit budget (`--gpu-budget`, `--gpu-budget-fraction`) still gives smaller pools with on-demand residency. The old fully resident path (`MeshGpuBuffers`: one vertex buffer per frame slot, static index buffer, vertex attributes) was removed after the comparison below. Index ranges in adjacent pool slots are merged into one draw call. With whole-mesh pools chunk *c* sits in slot *c*, so draw-call counts are the same as with the old static index buffer.

**How GPU time is measured.** With vertex data in GPU memory a single pass is too light for the GPU to leave its power-saving clocks (210 to 360 MHz seen in nvidia-smi), and GPU time at light load is a floor of about 4 to 6 ms at 5.2M that does not follow the triangle count. The self-test therefore also draws every row 64 times per frame (`RenderSettings::drawRepeat`, measurement only) and reports frame GPU time / 64. nvidia-smi showed 2,745 MHz throughout those rows. (A first attempt with 8 passes gave about 5 ms of work per 16.7 ms frame, which was still not enough: numbers varied by 2x between runs, and those numbers have been withdrawn.) Later passes are mostly depth-rejected, so this measures mainly vertex cost.

All three paths in the same build, same views (the old ones selected by a temporary switch that was then removed). "Old budgeted" is the Phase 5 budgeted behaviour with a budget that holds the whole mesh (`--gpu-budget-fraction 1.2`): nearest-first order, one draw per chunk, no initial fill.

| View, culling | Mesh | Triangles | Draw calls: old resident / old budgeted / unified | GPU ms per pass at full clock: old resident / old budgeted / unified | Renderer CPU ms per frame, median: old resident / old budgeted / unified |
|---|---|---|---|---|---|
| Full, off | 5.2M | 5.24M | 1 / 1,280 / 1 | 0.619 / 0.634 / 0.610 | 0.018 / 0.124 / 0.073 |
| Full, on | 5.2M | 5.24M | 1 / 1,280 / 1 | 0.633 / 0.633 / 0.609 | 0.030 / 0.138 / 0.089 |
| Full, one draw per chunk | 5.2M | 5.24M | 1,280 / 1,280 / 1,280 | 0.638 / 0.632 / 0.609 | 0.068 / 0.135 / 0.098 |
| Zoomed, off | 5.2M | 5.24M | 1 / 1,280 / 1 | 0.560 / 0.550 / 0.554 | 0.020 / 0.120 / 0.068 |
| Zoomed, on | 5.2M | 2.78M | 62 / 678 / 62 | 0.347 / 0.343 / 0.352 | 0.040 / 0.088 / 0.065 |
| Limb only, off | 5.2M | 5.24M | 1 / 1,280 / 1 | 0.547 / 0.519 / 0.543 | 0.015 / 0.124 / 0.068 |
| Limb only, on | 5.2M | 2.20M | 39 / 536 / 42 | 0.290 / 0.289 / 0.292 | 0.028 / 0.070 / 0.058 |
| Full, off / on | 1.3M | 1.31M | 1 / 320 / 1 | 0.185 / 0.186 / 0.184 | 0.014-0.018 / 0.043-0.046 / 0.024-0.031 |
| Zoomed, on | 1.3M | 0.85M | 21 / 210 / 21 | 0.117 / 0.122 / 0.116 | 0.018 / 0.038 / 0.028 |
| Limb only, on | 1.3M | 0.62M | 18 / 154 / 18 | 0.082 / 0.084 / 0.082 | 0.018 / 0.034 / 0.026 |

GPU memory for the mesh: old resident 45.0 / 180.0 MiB, old budgeted (1.2) 33.0 / 129.0 MiB, unified 31.9 / 127.5 MiB at 1.3M / 5.2M. The initial fill takes 7 to 8 ms at 1.3M and 25 ms at 5.2M.

- **GPU time is the same in all three paths** (within 3%, run-to-run noise). Vertex pulling through the page table costs nothing measurable against fixed-function vertex attributes on this GPU.
- **Culling pays in proportion to triangles** once the GPU runs at full clock: at 5.2M, 0.55 to 0.35 ms per pass zoomed and 0.54 to 0.29 ms when only the limb is visible. (Light-load measurements had hidden this.)
- **Draw calls:** merging adjacent slots brings the pools back to the old path's counts (1 for a full view instead of 1,280). With that and no nearest-first sort, the unified path's CPU time is about half the old budgeted path's (one draw per chunk alone costs it 0.025 ms more at 5.2M). A small difference remains after edits elsewhere in the run (42 vs 39 runs in one view, because more chunks were visible after the live stroke). Indirect draws were not needed and were not evaluated.
- **Remaining regression: CPU per frame.** At 5.2M the unified path spends about 0.05 ms more per frame than the old resident path in the same run (0.06 to 0.10 ms against 0.02 to 0.07 ms); another run of the unified path measured 0.11 to 0.18 ms, so run-to-run spread is as large as the difference. At 1.3M the difference is about 0.01 ms. The extra work is requesting every visible chunk's pages from the pool and rewriting the page table each frame; it was not profiled further. Against a 16.7 ms frame it is not visible in frame intervals.
- **Memory:** 29% less than the old resident path, because one pool replaces one vertex buffer per frame in flight.

### Back-facing chunk culling (normal cones)

`--cull on` (the default) now also skips chunks that face entirely away from the viewer. Each chunk keeps a cone that holds every one of its triangle normals. The sculpt thread recomputes it in the same BVH refit pass that updates the chunk's bounds, and the renderer reads both under the same lock. A chunk is skipped only when the cone proves that the rasterizer's back-face culling would discard every one of its triangles. A chunk is always drawn when its cone is unusable (wider than 84 degrees) or when its GPU copy has an edit that hasn't been uploaded yet. `--cull frustum` keeps frustum culling only; `--normal-cones off` also stops the sculpt thread from computing cones. A prototype that recomputed cones lazily on the render thread was measured first (notes below); this version replaces it.

Measured with 22 self-test runs: standard and pressure scenarios at 1.3M and 5.2M triangles, cones off (`--normal-cones off`) against cones on, with the default pools and with 90% and 25% budgets, plus `--lod` runs. The cones-off run with the default pools is the reference for every other run.

**Images.**
- In every run, the standard self-test renders 16 views with culling off, frustum only, on, and on with one draw per chunk, and every comparison is pixel-identical. Eight of those views come after strong deformations, undo and redo: large Draw and Move strokes from both sides that fold the surface and leave 10 (1.3M) or 29 (5.2M) chunks without a usable cone.
- The standard run with cones is pixel-identical to the run without cones in all 22 scripted captures, at both sizes.
- After every stage, every published cone contained all of its chunk's triangle normals (0 violations).
- The pressure scenario was compared with the reference wherever every visible chunk was drawn. The full, zoomed, after-motion and undo captures, and all deformed and undone captures, are identical with cones on at the default and 90% budgets.
- After redo, at 90% (cones on), 1 to 5 pixels in a 1920x1121 frame differ from the reference. The 90% runs with cones off show the same pixels with the same values, and so do the cones-off `--lod` runs. The cause is in the budget path, not in culling; it is listed under Known limitations.

**Missing chunks and image differences under pressure** are reported separately. "Missing" counts visible chunks that could not be drawn; "pixels lost" counts surface pixels of the reference that show background.

| Mesh, budget | Full view: drawn / visible, cones off -> on | Pixels lost, off -> on | Zoomed view: drawn / visible, off -> on | Camera motion: missing chunks per frame (median), off -> on | Upload during motion, off -> on |
|---|---|---|---|---|---|
| 1.3M, default | 320/320 -> 143/143 | 0 -> 0 | 166/166 -> 18/18 | 0 -> 0 | 0 -> 0 MiB |
| 1.3M, 90% | 229/320 -> 143/143 | 0 -> 0 | 166/166 -> 18/18 | 0 -> 0 | 9.4 -> 6.7 MiB, 345 -> 0 evictions |
| 1.3M, 25% | 54/320 -> 54/143 | 111,794 -> 111,794 | 30/166 -> 18/18 (identical image) | 142 -> 0 | 49.2 -> 14.4 MiB |
| 5.2M, default | 1,280/1,280 -> 509/509 | 0 -> 0 | 561/561 -> 34/34 | 0 -> 0 | 0 -> 0 MiB |
| 5.2M, 90% | 966/1,280 -> 509/509 | 0 -> 0 | 561/561 -> 34/34 | 0 -> 0 | 19.1 -> 16.8 MiB, 626 -> 0 evictions |
| 5.2M, 25% | 249/1,280 -> 249/509 | 72,468 -> 72,468 | 196/561 -> 34/34 (identical image) | 371 -> 0 | 214.7 -> 31.0 MiB |

During the live stroke at 25%, 135 (1.3M) and 365 (5.2M) chunks were missing in every frame with cones off, and none with cones on. The wide view at 25% still does not fit with cones on: the front-facing half needs more than the pool holds. It loses exactly the same pixels as without cones.

**GPU time per pass**, with the GPU fully loaded (64 passes per frame, standard self-test, default pools, cones on). Triangles in millions.

| View | Mesh | Off | Frustum | On (frustum + back-facing) | Chunks skipped as back-facing |
|---|---|---|---|---|---|
| Full | 1.3M | 0.177 ms, 1.31 | 0.177 ms, 1.31 | **0.126 ms**, 0.69 | 152 |
| Zoomed | 1.3M | 0.172 ms, 1.31 | 0.133 ms, 0.88 | **0.075 ms**, 0.27 | 149 |
| Limb only | 1.3M | 0.151 ms, 1.31 | 0.083 ms, 0.63 | **0.077 ms**, 0.18 | 110 |
| Full | 5.2M | 0.610 ms, 5.24 | 0.612 ms, 5.24 | **0.365 ms**, 2.30 | 719 |
| Zoomed | 5.2M | 0.559 ms, 5.24 | 0.373 ms, 3.00 | **0.165 ms**, 0.58 | 590 |
| Limb only | 5.2M | 0.543 ms, 5.24 | 0.296 ms, 2.29 | **0.144 ms**, 0.53 | 430 |

Selecting chunks (frustum + cone tests for every chunk) took 0.005 to 0.007 ms per frame at 1.3M and 0.017 to 0.029 ms at 5.2M. In the pressure runs, loaded GPU time per pass in the full view went from 0.179 to 0.134 ms (1.3M) and from 0.609 to 0.346 ms (5.2M) with the default pools.

**Where the cost went.** The cones are computed on the sculpt thread. Each touched chunk's cone is recomputed from its 4,096 triangles, in parallel with the local BVH refit. All times below are on the sculpt thread, cones off -> on.

| Stroke | Mesh | BVH refit ms per dab | Triangles visited for cones per dab | Time per input p95 / max | Undo / redo median |
|---|---|---|---|---|---|
| Strong strokes (radius 0.3 to 0.5, 245 dabs) | 1.3M | 0.205 -> 0.233 | 35,900 | 2.10 / 7.24 -> 2.32 / 8.26 ms | 0.93 / 1.08 -> 0.97 / 0.93 ms |
| Standard live stroke (radius 0.25) | 1.3M | 0.309 -> 0.318 | 38,100 | 0.98 / 1.89 -> 0.98 / 1.71 ms | |
| Strong strokes (467 dabs) | 5.2M | 0.464 -> 0.504 | 82,400 | 10.18 / 24.25 -> 10.23 / 26.34 ms | 2.78 / 2.23 -> 2.70 / 2.39 ms |
| Standard live stroke | 5.2M | 0.912 -> 0.939 | 104,400 | 3.18 / 5.15 -> 3.43 / 5.22 ms | |
| Pressure live stroke (radius 0.04, 99 dabs) | 1.3M | 0.016 to 0.018 -> 0.059 to 0.086 | 6,200 | 0.06 to 0.07 -> 0.11 to 0.22 ms | |
| Pressure live stroke | 5.2M | 0.049 to 0.051 -> 0.092 to 0.121 | 11,000 | 0.17 to 0.19 -> 0.20 to 0.28 ms | |

- **Sculpting:** cones add about 0.03 to 0.04 ms per dab for large brushes, a few percent of the edit time. For a small brush they add 0.04 to 0.07 ms per dab, because a dab that touches a few hundred vertices still recomputes whole chunks. That is 3 to 5 times the refit alone, but still well under 0.3 ms per input. Undo and redo are unchanged within noise.
- **Rendering:** the prototype recomputed cones on the render thread and spent 0.21 to 0.33 ms of renderer CPU per stroke frame doing it. Now renderer CPU during strokes is back to 0.03 to 0.06 ms per frame (median), at or below the cones-off runs. The cost moved to the sculpt thread, where it is smaller because it runs in parallel with the refit.
- **Upload:** edits upload the same pages as before (stroke p95 72 KiB at 1.3M, 144 KiB at 5.2M). Camera motion uploads less, because back-facing chunks no longer claim pool slots (table above).
- **Memory:** 20 bytes per chunk (26 KiB at 5.2M).

Run-to-run spread is about 10% for these timings. Frame intervals are set by presentation and show no difference.

### GPU memory budget (Phase 5)

The pressure self-test: a full view, a zoomed view, 240 steps of camera motion (a full orbit plus a vertical sweep, one step every 4 ms), a live Draw stroke in the zoomed view (500 inputs at 2 ms), undo, redo. "Phase 4 storage" is the old fully resident path (removed since); "whole-mesh pools" is the current default. GPU times here are light-load times (see "One GPU storage path").

| Mesh, storage | GPU memory for the mesh | Zoomed view drawn | Camera motion: frame interval median / p95 / max | GPU ms median (p95) | Upload per frame median / p95 / max | Uploaded / evictions during the motion |
|---|---|---|---|---|---|---|
| 1.3M, Phase 4 storage | 45.0 MiB | 166 / 166 chunks | 8.1 / 29.0 / 42.7 ms | 0.98 (1.07) | 0 | 0 / 0 |
| 1.3M, whole-mesh pools | 31.9 MiB | 166 / 166, identical image | 8.0 / 29.1 / 37.2 ms | 0.26 (0.29) | 0 | 0 / 0 |
| 1.3M, budget 90% | 27.0 MiB | 166 / 166, identical image | 8.0 / 28.7 / 44.9 ms | 0.27 (0.29) | 72 / 264 / 312 KiB | 9.7 MiB / 358 |
| 1.3M, budget 25% | 7.5 MiB | 30 / 166 (needs 418 pages, pool holds 160) | 16.7 / 16.8 / 33.4 ms | 0.23 (0.23) | 432 / 744 / 1,056 KiB | 48 MiB / 1,647 |
| 5.2M, Phase 4 storage | 180.0 MiB | 561 / 561 | 8.0 / 29.0 / 44.6 ms | 3.05 (3.11) | 0 | 0 / 0 |
| 5.2M, whole-mesh pools | 127.5 MiB | 561 / 561, identical image | 8.0 / 28.7 / 41.4 ms | 0.45 (0.49) | 0 | 0 / 0 |
| 5.2M, budget 90% | 108.0 MiB | 561 / 561, identical image | 8.0 / 28.9 / 37.0 ms | 0.46 (0.49) | 72 / 672 / 888 KiB | 19 MiB / 622 |
| 5.2M, budget 25% | 30.0 MiB | 196 / 561 (needs 1,264 pages, pool holds 640) | 8.0 / 28.6 / 37.2 ms | 0.31 (0.32) | 1.9 / 2.7 / 3.1 MiB | 214 MiB / 6,965 |

Live stroke in the same runs:

| Mesh, storage | Frame interval median / p95 / max | Frame CPU median / p95 | GPU ms median (p95) | Upload per frame median / p95 | Stale pages after settling |
|---|---|---|---|---|---|
| 1.3M, Phase 4 storage | 6.0 / 28.9 / 43.1 ms | 0.03 / 0.05 ms | 1.30 (1.69) | 48 / 72 KiB | 0 |
| 1.3M, whole-mesh pools | 16.7 / 17.6 / 33.2 ms | 0.03 / 0.04 ms | 0.80 (1.70) | 48 / 72 KiB | 0 |
| 1.3M, budget 25% | 16.7 / 17.0 / 33.3 ms | 0.04 / 0.06 ms | 0.55 (0.72) | 48 / 72 KiB | 0 |
| 5.2M, Phase 4 storage | 6.2 / 29.0 / 44.3 ms | 0.05 / 0.18 ms | 3.14 (3.49) | 96 / 144 KiB | 0 |
| 5.2M, whole-mesh pools | 6.4 / 29.0 / 43.0 ms | 0.07 / 0.10 ms | 1.37 (1.88) | 72 / 144 KiB | 0 |
| 5.2M, budget 90% | 6.5 / 28.8 / 43.9 ms | 0.12 / 0.24 ms | 1.15 (1.41) | 72 / 144 KiB | 0 |
| 5.2M, budget 25% | 6.5 / 28.9 / 39.9 ms | 0.10 / 0.15 ms | 0.83 (1.83) | 72 / 144 KiB | 0 |

With the standard self-test's larger brush at 5.2M (89 dabs, about 35,000 vertices each): Phase 4 storage uploads a median 1,080 KiB per frame, frame interval 8.2 / 42.6 / 64.3 ms, mesh-lock wait p95 0.96 ms; a budget that holds the whole mesh (`--gpu-budget-fraction 1.2`, 129 MiB) uploads 936 KiB, frame interval 6.8 / 29.0 / 53.8 ms, lock wait p95 2.4 ms. Phase 4 measured 7.5 / 27.0 / 59.4 ms and 2.5 ms worst lock wait for the same stroke.

What the numbers say:

- **Memory:** one complete copy of the mesh in the pool layout is 30 MiB at 1.3M and 120 MiB at 5.2M; whole-mesh pools add the spare slots (31.9 and 127.5 MiB). The old fully resident path used 45 and 180 MiB because it kept one vertex buffer per frame in flight.
- **GPU time in these tables** is light-load time and partly reflects clock state. The "Phase 4 storage" rows were measured with host-memory vertex buffers, which made them 3 to 7x slower than the pool rows. Forcing the pools into host memory raised the full view at 5.2M to 10.6 ms; that fallback was exercised and the pressure runs passed with it.
- **Upload volume** follows the view. With 90% of the memory, moving the camera re-uploads 19 MiB over 240 steps; at 25% the visible set never fits and pages are streamed continuously (214 MiB). Edits upload the same pages in every mode.
- **Frame pacing** is unchanged: intervals are set by presentation in every configuration. During camera motion the renderer's CPU time per frame is at most 0.42 ms with a small budget (choosing chunks, requesting pages, copying) and at most 0.15 ms with whole-mesh pools; during strokes it also includes waiting for the mesh lock.
- **When the view does not fit**, the nearest chunks are drawn and the rest are missing. In a full view the missing ones are on the far side and hidden; in a close view they show as holes at the edges of the drawn area.
### Compact adjacency (default)

The compact layout (`geo::AdjacencyLayout::Compact`, the default; `--adjacency wide` selects the u32 layout) stores each adjacency row as signed 16-bit offsets from a per-page anchor (the median id of the page), with one u32 header per vertex (row start, count, "u32 row" flag). A row with any id outside 16-bit reach keeps u32 ids, so every id stays exact. Rows still decode to the same ids in the same order as the wide layout, so every result is bit-identical. Construction builds the compact pages one at a time and frees the flat triangle table before it builds the neighbours.

Measured with `se_bench adjacency <level> <helpers> [wide|compact]` (compact when omitted), each run a fresh process, 5 interleaved rounds per configuration; median [min-max]. "before" is the build before compact adjacency, "wide" the wide layout after it. The two tables below are from before the shared edit region (next section), when compact was opt-in.

| | 1.3M before | 1.3M compact | 5.2M before | 5.2M compact |
|---|---|---|---|---|
| Mesh memory | 65.1 MiB | 50.4 MiB (-23%) | 260.3 MiB | 201.9 MiB (-22%) |
| Neighbours / incident triangles | 17.5 / 17.5 MiB | 10.2 / 10.2 MiB | 70.1 / 70.1 MiB | 40.6 / 41.2 MiB |
| Rows kept as u32 ids (both tables) | - | 2.3% | - | 2.7% |
| Peak heap, `PagedMesh` from flat arrays | 122.6 MiB | 80.3 MiB | 490.2 MiB | 321.3 MiB |
| Peak heap, `makeIcosphere` (generate + order + page) | 137.8 MiB | 95.5 MiB | 550.3 MiB | 381.4 MiB |
| `PagedMesh` construction | 96 [95-99] ms | 105 [104-106] ms | 390 [385-396] ms | 427 [423-433] ms |
| Startup (mesh + BVH, 11 helpers) | 398 [375-421] ms | 408 [399-410] ms | 1,883 [1,799-1,891] ms | 1,897 [1,814-1,940] ms |

Per edit, 200-sample strokes (ms). Round 1 was a quiet machine; the final binaries were run again on a busier one (round 2), where every configuration was 5 to 10% slower and ranges were wider:

| Stroke | helpers | before (round 1) | compact (round 1) | before (round 2) | wide (round 2) | compact (round 2) |
|---|---|---|---|---|---|---|
| 1.3M Draw | 0 | 1.288 [1.257-1.400] | 1.284 [1.268-1.354] | 1.320 | 1.343 | 1.371 |
| 1.3M Smooth | 0 | 1.565 [1.536-1.669] | 1.589 [1.542-1.626] | 1.735 | 1.597 | 1.681 |
| 1.3M Move | 0 | 0.839 [0.833-0.840] | 0.866 [0.857-0.881] | 0.910 | 0.888 | 0.941 |
| 1.3M Draw | 11 | 0.897 [0.841-0.919] | 0.894 [0.892-0.923] | 0.987 | 1.048 | 0.978 |
| 1.3M Smooth | 11 | 1.048 [1.018-1.090] | 1.081 [1.062-1.113] | 1.134 | 1.282 | 1.176 |
| 1.3M Move | 11 | 0.469 [0.457-0.475] | 0.497 [0.490-0.500] | 0.527 | 0.526 | 0.549 |
| 5.2M Draw | 0 | 4.691 [4.635-5.344] | 4.738 [4.662-7.255] | 5.322 | 5.373 | 5.394 |
| 5.2M Smooth | 0 | 6.153 [5.881-6.952] | 6.045 [5.942-6.186] | 6.443 | 7.127 | 7.200 |
| 5.2M Move | 0 | 3.285 [3.103-3.327] | 3.270 [3.204-3.524] | 3.437 | 3.476 | 3.902 |
| 5.2M Draw | 11 | 3.169 [2.993-3.324] | 3.143 [3.113-3.391] | 3.490 | 3.620 | 3.509 |
| 5.2M Smooth | 11 | 3.482 [3.144-3.525] | 3.403 [3.290-3.431] | 3.663 | 3.768 | 3.854 |
| 5.2M Move | 11 | 1.623 [1.527-1.655] | 1.658 [1.650-1.802] | 1.835 | 1.869 | 1.900 |

- **Exactness:** positions and normals hash the same after every stroke, before and after, wide and compact, with 0 or 11 helpers. Unit tests check the rows against the wide layout and check strokes bit for bit (serial and parallel) and exact undo/redo. Live at 5.2M, the standard and pressure self-tests pass with compact, and all 16 deterministic pressure captures are pixel-identical to a wide reference run.
- **Cost:** Draw and Smooth are within run-to-run variation. Move is 2 to 6% slower per edit, above that variation: it does little besides the serial adjacency walks (normal-update queue, BVH triangle marking), where a compact row costs about 3 ns more to read than a wide one. Construction is about 10% slower; startup 1 to 3%.
- **Why it was not the default at first:** the saving is meaningful (58 MiB, 22% of the mesh, at 5.2M; peak construction memory -35%), but the Move cost exceeded the measured variation. The shared edit region removed that cost (next section). A first delta-coded version (each entry relative to the previous one) was as small but 5 to 18% slower in normals; independent 16-bit offsets fixed most of that.
- **Default path:** the wide layout is read through the same `AdjacencyRow` type. After a fix (an explicit storage flag instead of a null test, so the compiler drops the unused branch) wide is equal to before: Move 0.507 vs 0.507 ms median in 12 interleaved runs at 1.3M with 11 helpers.

#### Shared edit region: one adjacency walk per dab

Profiling Move at 5.2M (11 helpers, 1.6 ms per edit) showed that 70% of an edit was two serial walks over the same rows: the normal update queued the moved vertices and their neighbours (0.50 ms, a neighbour row per moved vertex, de-duplicated with a u32 stamp per vertex), and the BVH refit marked the triangles of every moved vertex (0.64 ms, each triangle reached once per moved corner, about 6 marks per vertex). Decoding was a small part; most of the time was the per-entry checks. The compact gap (0.035 ms) was almost all in these walks.

`geo::EditRegion` now collects, once per dab (and per undo or redo step), every triangle around the moved vertices (each once, in first-occurrence order) and the moved vertices plus the other corners of those triangles, which is the same set as moved + neighbours. `NormalUpdater::updateVertices` and `ChunkedBvh::refitTriangles` both read these lists, so the neighbour rows are no longer read while editing and the incident rows once instead of twice. The triangle claim is branchless (whether a triangle is new follows no pattern); the corner claim keeps its branch (most corners are already claimed moved vertices, so it predicts well; the branchless form was 2.6x slower there). Scratch is two bitsets, one bit per vertex and per triangle, and the two lists; bits are cleared by walking the lists, so a dab neither allocates nor clears whole arrays once the lists have grown.

Exactness: the refit marks the same triangles in the same order, so its result is bit-identical; normals are computed independently per vertex, so their order does not matter. Positions and normals hash the same as before at 1.3M and 5.2M, wide and compact, 0 and 11 helpers. Unit tests compare the lists with the two separate walks (both layouts, shuffled vertex order, reused scratch) and the refit from the list with the refit from the moved vertices (serial and parallel); the compact-vs-wide stroke and undo/redo tests run through the new path.

Per edit, ms, median [min-max] of 8 interleaved rounds (each a fresh process; wide and compact of the builds before and after this change alternate):

| Stroke | helpers | wide before | compact before | wide after | compact after |
|---|---|---|---|---|---|
| 1.3M Draw | 0 | 1.287 [1.233-1.362] | 1.299 [1.262-1.405] | 1.203 [1.182-1.251] | 1.204 [1.194-1.243] |
| 1.3M Smooth | 0 | 1.527 [1.499-1.697] | 1.566 [1.538-1.663] | 1.509 [1.479-1.546] | 1.519 [1.490-1.535] |
| 1.3M Move | 0 | 0.845 [0.834-0.861] | 0.872 [0.858-0.909] | 0.804 [0.797-0.830] | 0.810 [0.803-0.838] |
| 1.3M Draw | 11 | 0.902 [0.876-0.975] | 0.901 [0.889-0.917] | 0.839 [0.822-0.866] | 0.855 [0.833-0.876] |
| 1.3M Smooth | 11 | 1.052 [1.016-1.086] | 1.071 [1.042-1.193] | 1.040 [1.014-1.084] | 1.044 [1.011-1.093] |
| 1.3M Move | 11 | 0.476 [0.473-0.532] | 0.499 [0.484-0.558] | 0.466 [0.459-0.511] | 0.489 [0.468-0.509] |
| 5.2M Draw | 0 | 4.609 [4.530-5.033] | 4.766 [4.592-4.942] | 4.451 [4.294-4.844] | 4.505 [4.316-4.949] |
| 5.2M Smooth | 0 | 5.806 [5.663-6.274] | 6.003 [5.805-6.168] | 5.822 [5.614-5.984] | 5.824 [5.575-6.067] |
| 5.2M Move | 0 | 3.165 [3.044-4.074] | 3.345 [3.141-3.554] | 3.094 [2.925-3.309] | 3.108 [2.972-3.229] |
| 5.2M Draw | 11 | 3.109 [3.051-3.575] | 3.156 [3.068-3.412] | 2.909 [2.838-3.139] | 2.899 [2.845-3.006] |
| 5.2M Smooth | 11 | 3.264 [3.208-3.392] | 3.366 [3.269-3.579] | 3.156 [3.103-3.384] | 3.166 [3.102-3.275] |
| 5.2M Move | 11 | 1.570 [1.548-1.658] | 1.667 [1.621-1.734] | 1.492 [1.459-1.619] | 1.493 [1.479-1.530] |

- **Move, absolute:** 5 to 10% faster per edit at 5.2M (11 helpers: wide 1.570 to 1.492 ms, compact 1.667 to 1.493 ms), 2 to 7% at 1.3M.
- **Compact vs wide gap:** at 5.2M from +6.2% to +0.1% (11 helpers) and from +5.7% to +0.5% (0 helpers); at 1.3M with 0 helpers from +3.1% to +0.7%. At 1.3M with 11 helpers the 8-round medians still differ by 5%, but the ranges overlap; 16 further alternating rounds of just that case gave 0.471 vs 0.477 ms (+1.3%), interquartile ranges 0.462-0.482 and 0.468-0.503, within the variation. What remains is the row decode in the single walk (about 0.004 ms per edit at 1.3M).
- **Draw and Smooth:** no regression; both are equal or faster in every configuration (they share the same edit path).
- **Where Move's time goes now** (5.2M, 11 helpers, about 1.47 ms, measured with temporary timers): the shared walk 0.39 ms (incident rows, triangle claims), one-ring corners 0.12 ms, clearing the bits 0.04 ms, normal recompute (parallel) 0.17 ms, BVH leaf queueing 0.42 ms (per unique triangle: leaf lookup, leaf stamp, ancestor marks; no adjacency read), BVH local refits + cones + top level 0.24 ms, brush 0.05 ms. The leaf queueing is now the largest serial part and costs the same with both layouts.
- **Scratch memory:** 3.0 MiB at 5.2M (bitsets 0.9 MiB, lists 2.1 MiB at their grown size), 0.56 MiB at 1.3M. The normal updater's u32 stamp per vertex (10 MiB at 5.2M, 2.5 MiB at 1.3M) is no longer allocated on the edit paths, so scratch is 7 MiB smaller overall at 5.2M.
- **Live, 5.2M:** the standard self-test passes with compact, with the same inputs, dabs and undo/redo results as wide (339 inputs, 467 dabs, 9 exact undo and redo steps). The pressure self-test passes with compact, and all 16 deterministic captures are pixel-identical to a wide reference run made with the same build. Two runs (one compact, one wide) failed when stray input reached the full-screen window during the scripted strokes; their input and dab counts differed from every undisturbed run (329 inputs / 698 dabs and 339 / 254). Every undisturbed run passed: compact 4 of 4 (the last with default settings, no `--adjacency` option), wide 1 of 1.
- **Default:** compact is now the default (`MeshLayout::adjacency`, `--adjacency`, `se_bench adjacency`); `--adjacency wide` keeps the u32 layout.

### Derived neighbours (opt-in)

`--neighbors derived` (`geo::NeighborStorage::Derived`) builds no neighbour table. A vertex's ring is rebuilt from its incident-triangle row when it is read.

**Who reads neighbour rows.** Since the shared edit region, the only production reader is Smooth's Laplacian. It sums the neighbours' positions in float in row order (ascending ids) and divides by the count, so a derived ring must be the same sorted, unique set, in the same order. The other reader is the old `NormalUpdater::update`, which no longer runs on any edit path and does not depend on order. Draw, Move, normals, the BVH, undo/redo and the renderer read incident triangles or nothing.

**How a ring is derived** (`PagedMesh::forEachNeighbor`, no allocation):
- **Regular fan** (a closed, consistently wound disk of at most 8 triangles). Each neighbour is the corner after the vertex in exactly one incident triangle. The vertex's corner is picked with masks, not branches, and the k ids are sorted with a branchless 19-comparator network. One bit per vertex records regularity, checked once at construction: the "next" corners must be all different and equal the "prev" corners, and the vertex must be exactly one corner of each triangle. 0.31 MiB at 5.2M.
- **Every other vertex** (borders, flipped or degenerate triangles, non-manifold or high-valence fans) takes the general path: all other corners, sorted and deduplicated by insertion. The icosphere has none.
- `neighbors(v)` returns a `NeighborRing` value (a view of the stored row, or the derived ids held inline) for tests and tools.

**Exactness.** Stroke hashes are identical to stored neighbours at 1.3M and 5.2M, 0 and 11 helpers. Unit tests compare every ring with the stored rows over many meshes, and check Draw, Smooth (dense kernel, Laplacian-only and full-strength settings) and Move bit for bit, plus exact undo/redo across small pages and chunks. Live at 5.2M, the standard self-test passes with `--neighbors derived`.

Memory and construction (8 interleaved rounds, medians; "base" is the build before this change):

| | 1.3M stored | 1.3M derived | 5.2M stored | 5.2M derived |
|---|---|---|---|---|
| Mesh memory | 50.4 MiB | 40.4 MiB (-20%) | 201.9 MiB | 161.6 MiB (-20%) |
| Adjacency (neighbours + incident triangles) | 20.4 MiB | 10.3 MiB | 81.7 MiB | 41.5 MiB |
| Peak heap, `PagedMesh` from flat arrays / `makeIcosphere` | 80.3 / 95.5 MiB | same | 321.3 / 381.4 MiB | same |
| `PagedMesh` construction | 106 ms | 64 ms (-40%) | 424 ms | 260 ms (-39%) |
| Startup (mesh + BVH, 11 helpers) | 408 ms | 364 ms | 1,877 ms | 1,712 ms |

The construction peak does not change: it is reached while the incident-triangle table is encoded, before neighbours were built.

Per edit, ms. The focused run is 10 alternating rounds of stored and derived from the same binary; median and interquartile range:

| Stroke | helpers | stored | derived | change |
|---|---|---|---|---|
| 1.3M Smooth | 0 | 1.576 (1.553-1.600) | 1.623 (1.619-1.647) | +3.0% |
| 1.3M Smooth | 11 | 1.055 (1.043-1.067) | 1.103 (1.088-1.114) | +4.5% |
| 5.2M Smooth | 0 | 5.888 (5.860-5.929) | 6.258 (6.237-6.280) | +6.3% |
| 5.2M Smooth | 11 | 3.195 (3.189-3.230) | 3.251 (3.225-3.306) | +1.8% |
| 1.3M Draw / Move | 0 | 1.229 / 0.809 | 1.210 / 0.812 | within variation |
| 1.3M Draw / Move | 11 | 0.849 / 0.475 | 0.859 / 0.477 | within variation |
| 5.2M Draw / Move | 0 | 4.494 / 3.086 | 4.526 / 3.118 | within variation |
| 5.2M Draw / Move | 11 | 2.946 / 1.585 | 2.931 / 1.503 | within variation |

The 8-round run against the previous build gave the same picture: Smooth +6.5% and +5.5% with 0 helpers, with non-overlapping ranges, and +2.0% and +1.1% with 11 helpers. Draw and Move were within variation, and the stored path was equal to the previous build.

- **Where Smooth's time goes.** The Smooth brush stage is 10 to 21% slower. Reading a stored compact row costs about 7 ns per vertex; deriving a ring costs about 44 ns: the row decode, six 12-byte triangle reads and the sort. A first version with insertion sort only took 130 ns. The triangle reads are the floor, so a ring cannot be made as cheap as a stored row. Caching rings per dab would not help either: a dab runs 1 Laplacian pass at 5.2M and 2 at 1.3M with the benchmark settings.
- **Why it is not the default.** The saving is meaningful (40 MiB, 20% of the mesh at 5.2M; construction 39% faster), but Smooth is 3 to 6% slower per edit, outside the measured variation. Draw and Move do not change.

### Parallel BVH leaf marking (default with helper threads)

Before the commit, every dab queues the BVH leaves of its affected triangles (`ChunkedBvh::markTriangles`, part of prepare). Serially this was the largest single serial step left in a big dab: 2.5 ms of a 10 ms 21M Draw dab with 11 helpers.

**Profile** (per dab, 11 helpers): 74K triangles in 28 chunks at 5.2M; 296K triangles in 91 chunks at 21M, where the list changes chunk about 11,000 times. Finding each triangle's chunk is 0.03 / 0.11 ms; the marking itself (a leaf stamp, a level list, the ancestors flagged) is 0.47 / 2.0 ms, random accesses into each chunk's own tree. Chunks share nothing, so the marking is split by chunk: fixed 8,192-triangle blocks list the chunks they reach, a short serial pass puts the chunks in the serial loop's first-occurrence order and gives each a range, a stable counting sort groups the triangles by chunk in list order, and one task per chunk marks them (docs/Architecture.md, "Parallel leaf marking"). Lists under 32K triangles and runs without helpers mark serially. Undo and redo use the same path and mark before taking the lock ("Undo and redo" below). `--marking serial` turns it off.

**Exactness.** Hashes unchanged in all 36 benchmark runs (Draw, Smooth, Move at 5.2M and 21M, 0 and 11 helpers). `ParallelMarkingTests.cpp` (2 tests): boxes, cones, cone triangle counts and 625 picks, before and after every 512-triangle background optimize step, identical to serial marking at 0, 1, 3 and 11 helpers, for chunks of 256 and 4,096 triangles and a shuffled list with duplicates; and the same through Draw, Smooth and Move strokes with exact undo/redo. Reversing the chunk order or dropping one chunk's marks fails it.

**Stroke processor alone** (`se_bench adjacency <level> <helpers> pgather [pmark]`; 3 interleaved rounds of the baseline (prepare/commit with parallel gather), the new build with serial marking and with parallel marking; median, range in brackets):

| | marking, baseline → parallel | dab, baseline | dab, serial (new build) | dab, parallel |
|---|---|---|---|---|
| 5.2M Draw, 11 | 0.58 → 0.20 ms | 2.68 (2.60–2.80) | 2.72 (2.70–2.77) | 2.34 (2.32–2.40) |
| 5.2M Smooth, 11 | 0.46 → 0.16 ms | 2.88 (2.86–3.02) | 2.91 (2.87–3.04) | 2.63 (2.59–2.80) |
| 5.2M Move, 11 | 0.43 → 0.19 ms | 1.71 (1.68–1.81) | 1.76 (1.65–1.78) | 1.46 (1.46–1.61) |
| 21M Draw, 11 | 2.50 → 0.54 ms | 10.2 (10.0–11.8) | 11.4 (10.4–11.6) | 9.3 (8.0–9.4) |
| 21M Smooth, 11 | 1.98 → 0.42 ms | 10.5 (10.2–11.9) | 11.8 (11.0–11.9) | 10.3 (8.7–10.6) |
| 21M Move, 11 | 1.85 → 0.41 ms | 6.26 (6.25–7.49) | 7.35 (6.07–7.40) | 5.77 (4.72–6.11) |
| 21M Draw, 0 | 2.28 → 2.26 ms (serial) | 21.3 (20.7–21.6) | 21.0 (20.4–23.3) | 20.9 (20.7–21.9) |
| 5.2M Draw, 0 | 0.48 → 0.48 ms (serial) | 4.59 (4.54–4.68) | 4.61 (4.51–4.66) | 4.62 (4.52–4.65) |

The marking time never overlaps between serial and parallel. At 21M with 11 helpers the dab times of this bench fall into two groups about 1.3 ms apart from run to run, in every build; the serial new build landed in the slower group twice out of three (its marking time equals the baseline's), and its range overlaps the baseline's.

**Live, headless** (`se_bench live <level> <helpers> 2 pgather [pmark]`, 400 inputs every 2 ms, a reader locking every 4 ms; 3 interleaved processes × 2 rounds, medians of the 6 strokes):

| | dab latency p50 / p95 / max | inputs queued max / at release | lock hold per dab p50 / p95 / max | reader wait p95 / max |
|---|---|---|---|---|
| 5.2M Draw, 11, baseline | 2.94 / 3.46 / 4.57 | 2 / 1 | 0.45 / 0.55 / 0.61 | 0.10 / 0.68 |
| 5.2M Draw, 11, parallel | 2.40 / 2.86 / 4.24 | 2 / 1 | 0.41 / 0.49 / 0.62 | 0.14 / 0.43 |
| 5.2M Smooth, 11, baseline | 3.47 / 4.11 / 4.46 | 2 / 1 | 0.44 / 0.55 / 0.64 | 0.00 / 0.67 |
| 5.2M Smooth, 11, parallel | 2.95 / 3.39 / 3.75 | 1 / 1 | 0.38 / 0.48 / 0.57 | 0.14 / 0.60 |
| 5.2M Move, 11, baseline | 1.71 / 1.93 / 2.33 | 1 / 1 (worst 36) | 0.41 / 0.48 / 0.62 | 0.02 / 0.66 |
| 5.2M Move, 11, parallel | 1.58 / 1.84 / 2.22 | 1 / 1 (worst 2) | 0.42 / 0.49 / 0.64 | 0.00 / 0.64 |
| 21M Draw, 11, baseline | 9.73 / 11.6 / 18.1 | 19 / 16 | 1.36 / 1.71 / 1.93 | 0.82 / 1.53 |
| 21M Draw, 11, serial (new build) | 9.97 / 11.8 / 17.2 | 28 / 26 | 1.39 / 1.67 / 1.92 | 0.77 / 1.44 |
| 21M Draw, 11, parallel | 8.14 / 9.76 / 15.4 | 8 / 1 | 1.26 / 1.50 / 1.88 | 0.73 / 1.09 |
| 21M Smooth, 11, baseline | 10.8 / 12.8 / 14.1 | 13 / 8 | 1.23 / 1.40 / 1.56 | 0.69 / 1.21 |
| 21M Smooth, 11, parallel | 9.16 / 11.0 / 12.1 | 6 / 3 | 1.12 / 1.30 / 1.37 | 0.57 / 1.17 |
| 21M Move, 11, baseline | 6.41 / 7.22 / 8.24 | 274 / 274 | 1.18 / 1.34 / 1.49 | 0.87 / 1.29 |
| 21M Move, 11, serial (new build) | 6.39 / 7.47 / 8.53 | 274 / 274 | 1.18 / 1.33 / 1.50 | 0.82 / 1.30 |
| 21M Move, 11, parallel | 4.94 / 5.64 / 6.85 | 241 / 241 | 1.08 / 1.19 / 1.34 | 0.82 / 1.16 |
| 21M Draw, 0, baseline / serial / parallel (p50) | 19.2 / 19.0 / 19.5 | 208 / 204 / 210 | 6.90 / 6.88 / 6.95 | 6.05 / 5.87 / 5.94 |

Per-process dab p50 with 11 helpers, 21M: Draw 8.7–11.5 ms baseline, 8.8–11.7 serial, 7.1–9.5 parallel; Smooth 9.4–12.2, 9.6–12.4, 8.0–10.6; Move 5.7–7.4, 5.4–7.4, 4.0–5.9. At 5.2M: Draw 2.37–3.23, 2.41–2.99, 2.17–2.49; Smooth 2.60–3.68, 2.90–3.45, 2.57–3.43; Move 1.31–2.18, 1.31–1.85, 1.33–1.68. Dabs are 15 to 23% shorter at 21M and 7 to 18% at 5.2M; Move at 5.2M gains in the bench above but not beyond the spread here. The lock hold falls slightly (the commit's refit finds the trees warmer in cache) and the reader wait is unchanged: marking is part of prepare. Undo/redo holds are the same (21M, 11 helpers: undo 7.7 / 7.8 ms, redo 6.6 / 6.4 ms). Without helpers the three builds agree within their spread.

**Undo and redo.** Since then, undo and redo also mark their leaves before taking the document lock, with the same parallel path. They collect the edit region from the history entry's vertices (`UndoStack::undoVertices` / `redoVertices`, which do not move the cursor) and call `markTriangles` outside the lock. Under the lock they restore the positions, update the normals and run `finishMarkedRefit`. `se_bench live <level> <helpers> 4 pgather [pmark]`, 3 interleaved processes × 4 rounds, 12 undos and 12 redos per row (the last stroke of each round is Move):

| | undo lock hold p50 / p95 / max | undo total p50 | redo lock hold p50 / p95 / max | redo total p50 |
|---|---|---|---|---|
| 5.2M, 11 helpers, before | 1.43 / 2.64 / 2.64 ms | 1.43 ms | 1.73 / 2.21 / 2.21 ms | 1.73 ms |
| 5.2M, 11 helpers, serial marking outside the lock | 0.71 / 0.94 / 0.94 | 1.67 | 0.69 / 1.01 / 1.01 | 1.50 |
| 5.2M, 11 helpers, parallel marking outside the lock | 0.60 / 0.80 / 0.80 | 1.27 | 0.61 / 0.79 / 0.79 | 1.35 |
| 21M, 11 helpers, before | 4.89 / 10.0 / 10.0 | 4.89 | 5.19 / 7.38 / 7.38 | 5.19 |
| 21M, 11 helpers, serial marking outside the lock | 1.73 / 2.62 / 2.62 | 5.18 | 1.71 / 2.61 / 2.61 | 5.09 |
| 21M, 11 helpers, parallel marking outside the lock | 1.52 / 2.51 / 2.51 | 3.37 | 1.58 / 2.33 / 2.33 | 3.96 |
| 21M, 0 helpers, before | 10.1 / 21.8 / 21.8 | 10.1 | 9.59 / 16.6 / 16.6 | 9.59 |
| 21M, 0 helpers, after | 6.60 / 15.5 / 15.5 | 9.51 | 6.51 / 13.1 / 13.1 | 9.36 |

Most of the shorter hold comes from moving the marking out of the lock; the parallel marking then shortens the total, which is what bounds back-to-back undos. The dab metrics of the same runs are unchanged (21M Draw dab p50 6.90 / 6.66 ms, lock hold 1.05 / 1.02 ms, before / after). Vulkan self-test at 21M (both PASS, 237 checks): strong-stroke undo median 16.2 → 11.4 ms, max 74 → 53 ms; redo median 15.9 → 11.9 ms. 5.2M PASS. `ParallelMarkingTests.UndoRedoThroughControllerMatchSerialMarking` runs four strokes and undoes and redoes all of them through the controller with parallel marking at 0, 1, 3 and 11 helpers, against serial marking: the same hash, picks, boxes and cones after every step, and exact undo and redo. Dropping one chunk's marks fails it.

**Undo and redo: normals before the lock (opt-in, `--history-normals before`).** Profile of the remaining hold (`se_bench history <level> <helpers> 3 pgather pmark`). It undoes and redoes 3 typical strokes (Draw, Smooth, Move at radius 0.25) and then 5 strong strokes like the self-test's (radius 0.3 to 0.5, strength 1). Each undo or redo restores about 460K vertices at 21M. At 21M with 11 helpers the hold is 8.0 ms (p50, typical strokes). It splits into positions 1.4 ms, normals 2.9 ms (35%), dirty pages 0.35 ms, refit + cones 3.3 ms (42%) and top level 0.05 ms. Without helpers the hold is 58 ms: normals 23 ms (40%), refit + cones 33 ms (57%).

The prototype moves the normals out of the lock. Before taking it, undo and redo:
1. stage the history entry's positions in the dab staging pages (`PositionStaging`, shared with `StrokeProcessor`), so moved vertices read the entry and all others read the mesh;
2. compute the affected vertices' normals from that view on the helpers;
3. release the staging.

Under the lock they write positions, dirty pages, the precomputed normals, then refit, cones and the top level, as one update. Interleaved, 3 processes per row, 3 cycles of undo all + redo all each. Times are hold / total (sculpt thread) / renderer wait, p50 · p95 · max in ms:

| | undo hold | undo total | redo hold | redo total | renderer wait | scratch |
|---|---|---|---|---|---|---|
| 5.2M, 11 helpers, typical, under the lock | 2.13 · 2.45 · 2.45 | 6.62 · 7.90 · 7.90 | 2.08 · 2.52 · 2.52 | 5.97 · 7.13 · 7.13 | 0.25 · 0.72 · 1.85 | 8.1 MiB |
| 5.2M, 11 helpers, typical, before the lock | 1.39 · 1.85 · 1.85 | 7.23 · 8.98 · 8.98 | 1.44 · 1.82 · 1.82 | 6.83 · 7.99 · 7.99 | 0.28 · 0.70 · 1.25 | 13.1 MiB |
| 5.2M, 11 helpers, strong, under | 2.07 · 4.08 · 4.33 | 5.37 · 11.8 · 12.7 | 2.11 · 3.86 · 3.91 | 6.44 · 11.2 · 11.9 | 0.28 · 0.74 · 3.53 | 16.7 MiB |
| 5.2M, 11 helpers, strong, before | 1.72 · 3.11 · 3.13 | 6.52 · 13.0 · 14.6 | 1.48 · 3.05 · 3.06 | 7.18 · 12.7 · 13.1 | 0.27 · 0.73 · 2.24 | 21.5 MiB |
| 5.2M, 0 helpers, typical, under | 10.9 · 13.4 · 13.4 | 15.8 · 20.3 · 20.3 | 10.7 · 12.1 · 12.1 | 15.6 · 17.6 · 17.6 | 0.22 · 0.75 · 11.2 | 4.7 MiB |
| 5.2M, 0 helpers, typical, before | 7.68 · 8.73 · 8.73 | 18.6 · 21.3 · 21.3 | 7.50 · 8.49 · 8.49 | 16.8 · 18.4 · 18.4 | 0.23 · 0.76 · 7.53 | 9.6 MiB |
| 5.2M, 0 helpers, strong, under | 9.46 · 25.0 · 25.0 | 14.3 · 36.6 · 37.1 | 11.8 · 23.4 · 27.3 | 17.8 · 33.9 · 38.2 | 0.21 · 0.75 · 24.7 | 10.3 MiB |
| 5.2M, 0 helpers, strong, before | 7.20 · 17.4 · 17.5 | 15.7 · 39.2 · 39.6 | 7.88 · 16.6 · 17.3 | 18.4 · 36.3 · 38.6 | 0.21 · 0.74 · 16.5 | 15.1 MiB |
| 21M, 11 helpers, typical, under | 7.97 · 8.72 · 8.72 | 24.6 · 29.8 · 29.8 | 8.00 · 8.69 · 8.69 | 24.2 · 26.1 · 26.1 | 0.32 · 0.72 · 8.20 | 31.7 MiB |
| 21M, 11 helpers, typical, before | 5.69 · 6.46 · 6.46 | 27.6 · 34.1 · 34.1 | 5.75 · 6.40 · 6.40 | 27.7 · 29.5 · 29.5 | 0.33 · 0.72 · 5.63 | 51.1 MiB |
| 21M, 11 helpers, strong, under | 7.41 · 14.5 · 14.8 | 22.7 · 43.4 · 46.1 | 7.09 · 14.5 · 15.0 | 22.5 · 43.3 · 43.6 | 0.34 · 0.72 · 14.2 | 65.4 MiB |
| 21M, 11 helpers, strong, before | 5.49 · 10.7 · 10.7 | 24.9 · 49.0 · 53.9 | 5.08 · 10.5 · 10.6 | 25.0 · 48.8 · 48.8 | 0.33 · 0.73 · 10.6 | 84.5 MiB |
| 21M, 0 helpers, typical, under | 57.7 · 62.4 · 62.4 | 83.2 · 89.5 · 89.5 | 56.1 · 61.7 · 61.7 | 80.6 · 87.5 · 87.5 | 0.24 · 0.73 · 61.0 | 18.2 MiB |
| 21M, 0 helpers, typical, before | 37.8 · 40.5 · 40.5 | 86.7 · 97.5 · 97.5 | 37.0 · 39.0 · 39.0 | 85.7 · 91.5 · 91.5 | 0.23 · 0.74 · 40.0 | 37.5 MiB |
| 21M, 0 helpers, strong, under | 53.2 · 110 · 110 | 75.7 · 159 · 159 | 53.3 · 107 · 107 | 76.4 · 153 · 153 | 0.24 · 0.72 · 107 | 40.3 MiB |
| 21M, 0 helpers, strong, before | 34.3 · 70.4 · 70.5 | 79.0 · 161 · 165 | 34.1 · 68.2 · 68.6 | 78.8 · 159 · 160 | 0.23 · 0.72 · 70.2 | 59.4 MiB |

The renderer's p50 and p95 waits come from frames that don't overlap a command; its max is the one that does, and it follows the hold.
- **Hold.** The normals inside the hold drop from 2.9 to 0.4 ms at 21M with 11 helpers, and from 23 to 1.3 ms without. The hold shrinks 23 to 35% in every row.
- **Cost.** Total latency rises 3 to 13%: staging copies the entry's vertex pages, and the normals run before the lock instead of inside it, at the same cost. The staging copies and the normal buffer add 5 MiB of retained scratch at 5.2M and 19 MiB at 21M.
- **What is left.** Refit + cones is now 55 to 90% of what remains (3.4 of 5.7 ms at 21M with 11 helpers, 35 of 38 ms without), and positions 1.4 ms. They were not changed.

**Exactness.** The hash after the cycles is identical with and without the option in all 24 runs. The flag-off build matches the baseline build within noise in every stage. `HistoryStagingTests.cpp` has 2 tests:
- Through the controller, with the option on at 0, 1, 3 and 11 helpers, the hash and the BVH boxes and cones after every one of 10 undo/redo steps equal the option off. Undo and redo are exact.
- A thread locking the document during queued undos and redos only ever sees states the option-off run passes through between commands. Its dirty-page mirror equals the mesh at every read, and normals, boxes and cones match the positions.

Writing the mesh's positions instead of the entry's into the staging fails both tests.

**Vulkan self-test** (standard script, 11 helpers; all PASS, 238 checks, exact undo/redo; the baseline build, the new build with the option off, and with it on at 21M, then on at 5.2M). Strong-stroke undo totals, median / max: 11.2 / 53.3 ms (baseline), 11.5 / 58.8 (off), 12.7 / 68.6 (on). Redo: 11.2 / 47.4, 11.7 / 50.8, 13.0 / 56.0. At 5.2M (on) undo is 2.5 / 17.3 and redo 2.4 / 14.2. Scratch at 21M is 66.1 → 84.9 MiB. The self-test reports only total time, which grows as in `se_bench`. The renderer's worst lock wait is 0.001 ms in every run, because no frame overlapped an undo there.

**Decision.** Off by default. It shortens the hold the renderer can wait on, but every undo and redo finishes later and keeps up to 19 MiB more scratch.

**Rendering overlapped with undo/redo** (`--selftest-scenario history`, 21M, 2 runs per row). The scenario makes 3 typical strokes (radius 0.25) and the 9 strong strokes of the standard self-test. For each set it runs 4 cycles of undo all + redo all, queueing each command as soon as the previous one is published. Meanwhile a 1 ms timer requests frames back to back, so every command overlaps several frames: 24 to 77 frames per run waited 0.05 ms or more. Each run then checks:
- the mesh ends exactly where it began;
- the cones hold every normal;
- after undoing everything, the frame is pixel-identical to the initial one.

All 8 runs PASS. Renderer wait is per frame; hold and total are per undo (redo within a few %). Values are p50 · p95 · max in ms, two runs separated by "/":

| | renderer wait | undo hold | undo total | retained scratch |
|---|---|---|---|---|
| typical, 0 helpers, under | 9.5 · 66.8 · 72.0 / 5.3 · 63.0 · 66.6 | 70.3 · 74.9 / 70.3 · 75.9 | 97.8 · 109 · 109 / 98.1 · 110 · 110 | 29.3 MiB |
| typical, 0 helpers, before | 0.00 · 39.1 · 46.9 / 0.00 · 45.2 · 66.0 | 45.3 · 49.0 / 44.6 · 49.1 | 104 · 116 · 116 / 103 · 118 · 118 | 43.2 MiB |
| typical, 11 helpers, under | 0.50 · 5.19 · 6.97 / 0.49 · 2.90 · 6.32 | 11.8 · 12.4 / 11.8 · 12.5 | 32.1 · 37.7 · 37.7 / 32.6 · 40.1 · 40.1 | 43.2 MiB |
| typical, 11 helpers, before | 0.27 · 6.36 · 7.51 / 0.34 · 5.73 · 7.51 | 8.25 · 9.21 / 8.21 · 8.66 | 36.3 · 47.0 · 47.0 / 38.6 · 46.5 · 46.5 | 57.0 MiB |
| strong, 0 helpers, under | 0.48 · 92.6 · 112 / 0.63 · 93.9 · 110 | 18.7 · 111 / 18.6 · 112 | 26.2 · 156 · 164 / 26.1 · 156 · 164 | 41.1 MiB |
| strong, 0 helpers, before | 0.11 · 64.8 · 68.4 / 0.12 · 52.6 · 72.5 | 13.3 · 69.6 / 15.0 · 72.8 | 28.1 · 161 · 175 / 31.8 · 172 · 180 | 59.7 MiB |
| strong, 11 helpers, under | 0.35 · 6.72 · 17.0 / 0.32 · 16.6 · 17.4 | 3.51 · 18.2 / 3.37 · 17.6 | 9.0 · 56.1 · 59.4 / 9.1 · 48.9 · 56.1 | 65.5 MiB |
| strong, 11 helpers, before | 0.25 · 0.74 · 8.14 / 0.32 · 7.72 · 8.93 | 2.54 · 12.2 / 2.45 · 12.2 | 10.4 · 54.8 · 65.7 / 11.4 · 54.3 · 63.3 | 84.2 MiB |

The scenario's typical steps are larger than `se_bench`'s, because the strokes cross more of the model. With normals before the lock, the renderer's worst wait:
- halves for strong strokes with 11 helpers (17 → 8–9 ms);
- drops about a third without helpers (110 → 70 ms; typical 67–72 → 47–66 ms);
- does not improve for typical strokes with 11 helpers (6.3–7.0 → 7.5 ms). A frame there waits for only part of a hold, and the hold is already short.

Total latency is 3 to 20% longer in every row, and scratch is 14 to 19 MiB higher.

**Where the refit + cones time goes** (`se_bench history`, which now times each chunk's box refit and its cone in the same parallel loop, p50 per undo). Times are summed over the threads; without helpers they add up to the wall time:

| | chunks / triangles refitted / cone triangles per step | refit + cones (wall) | boxes (CPU) | cones (CPU) | boxes share |
|---|---|---|---|---|---|
| 21M, 0 helpers, typical | 259 / 919K / 1.06M | 33.4 ms | 23.3 | 10.1 | 70% |
| 21M, 0 helpers, strong | 298 / 1.05M / 1.22M | 29.8 | 20.5 | 9.3 | 69% |
| 21M, 11 helpers, typical | 259 / 919K / 1.06M | 4.25 | 32.7 | 17.0 | 66% |
| 21M, 11 helpers, strong | 298 / 1.05M / 1.22M | 3.89 | 29.8 | 15.6 | 66% |
| 5.2M, 0 helpers, typical | 73 / 230K / 300K | 6.45 | 3.42 | 2.92 | 54% |
| 5.2M, 11 helpers, typical | 73 / 230K / 300K | 0.92 | 6.29 | 3.59 | 64% |

**The box refit dominates**: two thirds of the stage at 21M and about half to two thirds at 5.2M. This holds in both normal modes (normals before the lock: 35.0 vs 16.4 ms CPU at 21M with 11 helpers). With helpers both stages cost 40 to 70% more CPU than alone, so they share memory bandwidth. Neither stage has been changed.

The stage timer has no measurable cost: 4 interleaved pairs at 21M with 11 helpers give a hold of 9.06–9.48 ms against 9.22–9.49 for the build without it, and refit + cones 4.21–4.37 against 4.20–4.35. Hashes are unchanged.

### Dense box refit (default)

**Profile of the box refit** (incremental refit, `se_bench history`, p50 per undo of the typical strokes). `Bvh::finishRefit` now times each level. A full 4,096-triangle chunk has all its leaves at depth 10, so leaf boxes and propagation through inner nodes are timed apart. Queueing the parents of changed nodes is timed on its own. CPU ms are summed over the chunks:

| | box refit (CPU) | leaf boxes | inner nodes | queue parents | leaves / inner nodes recomputed | slowest / mean chunk |
|---|---|---|---|---|---|---|
| 21M, 0 helpers | 23.5 ms | 17.5 (75%) | 2.1 (9%) | 3.4 (14%) | 230K / 231K | 0.107 / 0.069 ms |
| 21M, 11 helpers | 34.2 | 26.2 (77%) | 2.9 | 4.4 | 230K / 231K | 0.146 / 0.102 |
| 21M, 11 helpers, strong | 31.3 | 24.0 (77%) | 2.6 | 4.0 | 264K / 265K | 0.157 / 0.103 |
| 5.2M, 0 helpers | 3.59 | 2.37 (66%) | 0.39 | 0.67 | 58K / 59K | 0.068 / 0.036 |
| 5.2M, 11 helpers | 6.08 | 4.34 (71%) | 0.54 | 1.00 | 58K / 59K | 0.135 / 0.065 |

What explains the cost:
- **Not uneven chunk workloads.** The slowest chunk is 1.5 to 2 times the mean, and with 259 chunks over 12 threads the wall time is close to the CPU sum divided by 12.
- **Not repeated work in the tree.** Only 0.2% of the recomputed leaves and 0.06% of the inner nodes come out unchanged. An undo touches about 87% of the leaves of each affected chunk, so nearly all of them must be recomputed anyway.
- **Memory traffic.** The queued leaves are visited in the order the triangles were marked, not in node order. Each leaf reads its 4 triangle ids, 4 triangles and 12 positions through the page tables, and each position is read about 6 times (once per incident triangle). With 11 helpers the same work costs 40 to 70% more CPU time than on one thread, so the threads compete for memory bandwidth. Queueing the parents of changed nodes adds 14% on top.

**The change.** A chunk whose refit queued at least half of its leaves sweeps its whole tree in node order instead (`Bvh::finishRefit`):
1. every leaf in index order;
2. every inner node from the last index down (children have larger indices than their parent).

Unqueued leaves and inner nodes get the box they already have, because a box is the min/max of the same values. So the result equals the incremental refit bit for bit. The stamps, the optimize state and everything a reader sees are unchanged. Chunks below the threshold, every standalone `Bvh`, and calls that refit a tree with a job system keep the level-by-level refit. `--box-refit incremental` turns it off.

**Interleaved results** (`se_bench history`, 3 processes per row, p50 with p95 in brackets, ms). "Box refit" is the wall time; with 11 helpers it is estimated from the refit + cones wall time and the two stages' CPU shares:

| | box refit | refit + cones | undo hold | undo total | renderer max wait | scratch |
|---|---|---|---|---|---|---|
| 21M, 11 helpers, typical | 2.82 → 1.25 | 4.28 → 2.74 | 9.38 → 7.67 | 25.7 → 24.1 | 8.96 → 7.54 | 31.7 MiB, unchanged |
| 21M, 11 helpers, strong | 2.61 → 1.16 | 3.97 → 2.55 | 8.66 (17.2) → 7.08 (14.4) | 23.9 (47.2) → 22.2 (44.4) | 16.4 → 13.2 | 65.4 MiB, unchanged |
| 21M, 0 helpers, typical | 23.4 → 8.85 | 33.5 → 19.0 | 57.9 → 43.7 | 83.7 → 69.0 | 62.4 → 45.7 | 18.2 MiB, unchanged |
| 21M, 0 helpers, strong | 21.1 → 8.55 | 30.3 → 18.0 | 52.3 (111) → 39.8 (83.8) | 75.3 (159) → 62.9 (130) | 111 → 82.4 | 40.3 MiB, unchanged |
| 5.2M, 11 helpers, typical | 0.53 → 0.27 | 0.85 → 0.62 | 2.00 → 1.82 | 6.37 → 6.45 | 2.12 → 1.66 | 8.1 MiB, unchanged |
| 5.2M, 11 helpers, strong | 0.44 → 0.31 | 0.85 → 0.76 | 1.85 (3.94) → 1.83 (3.67) | 5.25 (12.0) → 5.67 (11.9) | 3.02 → 3.46 | 16.7 MiB, unchanged |
| 5.2M, 0 helpers, typical | 3.35 → 1.84 | 6.28 → 4.74 | 10.3 → 8.76 | 15.7 → 14.1 | 11.2 → 9.47 | 4.7 MiB, unchanged |
| 5.2M, 0 helpers, strong | 3.04 → 1.61 | 5.81 → 4.39 | 9.34 (24.7) → 8.04 (18.6) | 14.2 (35.8) → 13.3 (30.1) | 24.5 → 18.0 | 10.3 MiB, unchanged |

Redo moves the same way. The cones' CPU time is unchanged in every row, and so is the new build with the option off (third column of each run).
- **At 21M** the box refit is 2.3 to 2.7 times faster, and the hold 15 to 25% shorter.
- **At 5.2M with 11 helpers** the hold gains 1 to 9%, which is within noise for strong strokes. The total there ranges 5.21 to 5.67 ms across the base, off and dense builds.

The hash after the cycles is identical in all 36 runs.

**Draw, Smooth and Move** (they share the refit; `se_bench adjacency <level> <helpers> pgather pmark [dense]`, 3 interleaved processes, median per dab, hashes identical in all runs):

| | Draw ms/dab | Smooth | Move | local refits + cones per dab |
|---|---|---|---|---|
| 1.3M, 11 helpers | 0.927 → 0.856 | 1.096 → 1.074 | 0.503 → 0.470 | −14 to −18% |
| 1.3M, 0 helpers | 1.256 → 1.208 | 1.558 → 1.537 | 0.824 → 0.798 | −5 to −6% |
| 5.2M, 11 helpers | 2.329 → 2.290 | 2.628 → 2.560 | 1.465 → 1.458 | −14 to −17% |
| 5.2M, 0 helpers | 4.633 → 4.459 | 5.941 → 5.920 | 3.160 → 2.985 | −9 to −12% |
| 21M, 11 helpers | 8.211 → 7.977 | 9.009 → 8.692 | 4.997 → 4.845 | −15 to −25% |
| 21M, 0 helpers | 20.85 → 18.39 | 26.40 → 25.22 | 13.58 → 12.35 | −23 to −29% |

`se_bench live` (2 interleaved processes) agrees. At 21M with 11 helpers:
- Draw: lock hold per dab 1.12 → 0.90 ms, dab latency 7.30 → 6.55 ms;
- Smooth: 1.01 → 0.82, 8.33 → 7.54;
- Move: 0.94 → 0.78, 4.30 → 3.60;
- undo: 3.67 → 3.36 ms (hold 1.65 → 1.43).

No row is slower outside run-to-run spread, and dabs smaller than half a chunk take the unchanged incremental path.

**Vulkan history scenario at 21M** (`--selftest-scenario history`, 2 runs per row, "/" separates them; all PASS; normals under the lock as by default). Values are p50 · p95 · max in ms:

| | renderer wait (every frame) | undo hold p50 (p95) | undo total p50 (p95) | scratch |
|---|---|---|---|---|
| typical, 11 helpers, incremental | 0.40 · 2.99 · 3.97 / 0.38 · 4.16 · 6.05 | 11.7 / 11.9 | 32.1 / 32.3 | 43.2 MiB |
| typical, 11 helpers, dense | 0.42 · 5.91 · 6.56 / 0.16 · 1.22 · 3.89 | 9.6 / 9.9 | 29.6 / 30.4 | 43.2 MiB |
| strong, 11 helpers, incremental | 0.36 · 6.62 · 17.5 / 0.34 · 6.24 · 16.8 | 3.37 (18.0) / 3.58 (17.8) | 9.2 (49.2) / 10.7 (50.2) | 65.5 MiB |
| strong, 11 helpers, dense | 0.44 · 5.44 · 15.0 / 0.31 · 4.47 · 14.1 | 3.02 (15.1) / 3.08 (15.0) | 8.8 (47.5) / 9.0 (46.2) | 65.5 MiB |
| typical, 0 helpers, incremental | 8.37 · 66.8 · 71.8 / 0.57 · 64.5 · 72.1 | 70.1 / 69.0 | 97.7 / 96.3 | 29.3 MiB |
| typical, 0 helpers, dense | 0.51 · 48.2 · 52.2 / 0.69 · 50.6 · 53.7 | 52.8 / 52.0 | 79.8 / 79.9 | 29.3 MiB |
| strong, 0 helpers, incremental | 0.46 · 105 · 112 / 0.56 · 96.0 · 114 | 18.3 (114) / 18.5 (112) | 25.7 (161) / 26.1 (157) | 41.1 MiB |
| strong, 0 helpers, dense | 0.36 · 77.4 · 104 / 0.43 · 75.6 · 84.8 | 13.8 (86.5) / 14.6 (86.6) | 21.6 (140) / 22.6 (130) | 41.1 MiB |

With 11 helpers and typical strokes, the renderer's waits are a few ms either way and move with run-to-run noise. Elsewhere its worst wait drops with the hold.

Standard 21M self-test: both builds PASS (237 checks) with exact undo/redo, and the strong-stroke undo median is 11.7 → 11.5 ms. Per dab, the lock hold is 1.21 → 1.01 ms and the latency 7.82 → 7.53 ms.

**Exactness.** `DenseRefitTests.cpp` has 2 tests:
- Edits from a few triangles to the whole sphere, with chunks of 64, 256 and 4,096 triangles, give every node box, cone, top level and pick identical to the incremental refit, and to a full refit. The background optimize steps that follow, interleaved with later edits, also stay identical. Chunks both above and below the threshold are exercised.
- Draw, Smooth, Move and a small Draw through the controller, followed by undo and redo, give the same hash, boxes and picks after every step at 0, 3 and 11 helpers, and undo/redo stays exact.

The concurrent-reader tests for dabs (`PrepareCommitReader`) and for undo/redo (`HistoryStagingReader`) now run with both refits. Sweeping the inner nodes in the wrong order fails all four of these tests.

**Decision.** The dense refit is the default (`AppConfig::denseBoxRefit`). Every 21M row improves repeatably, and so does every dab size measured from 1.3M up. The 5.2M undo rows with helpers stay within noise. Exactness, picking, undo/redo and consistent publication are unchanged. Since the box refit was the largest part of refit + cones, the cones are now about half of what remains (16 of 31 ms CPU at 21M with 11 helpers).

**Vulkan self-test** (standard script, 11 helpers, 500 inputs at 2 ms; 21M interleaved with the baseline; all PASS, 237 checks, exact undo/redo):

| | dab latency median / p95 / max | sculpt ms per input p95 / max | BVH per dab (marking + refit + cones) | lock hold per dab median / p95 / max | renderer wait p95 / max | frame CPU p95 | queued at release |
|---|---|---|---|---|---|---|---|
| 21M, baseline | 9.38 / 12.4 / 14.0 ms | 10.9 / 14.0 | 2.87 ms | 1.33 / 1.60 / 1.67 | 0.000 / 0.001 | 0.79 | 0 |
| 21M, parallel marking | 8.12 / 10.2 / 10.7 ms | 9.2 / 10.7 | 1.40 ms | 1.22 / 1.44 / 1.48 | 0.000 / 0.84 | 0.79 | 0 |
| 5.2M, parallel marking | 2.86 / 3.74 / 4.11 ms | 3.3 / 4.1 | 0.61 ms | 0.50 / 0.67 / 0.74 | 0.000 / 0.001 | 0.21 | 0 |

The 0.84 ms renderer wait at 21M is one frame (p95 0.000); it is within the commit hold, which is shorter than before. Strong strokes at 21M: sculpt ms per input p95 34.8 (baseline 38.7), BVH per dab 1.11 ms (2.03).

**Scratch.** The bucket array (one u32 per marked triangle), the per-block chunk lists and a u32 per chunk add 0.6 MiB at 5.2M (5.9 → 6.5 MiB) and 2.5 MiB at 21M (22.5 → 25.0 MiB) in `se_bench`, 5 MiB after the self-test's larger brushes at 21M (57.3 → 62.4 MiB). They are kept in the BVH, grow to the largest dab and are reused.

**Decision.** Parallel marking is the default: with helpers the marking is 2.3 to 4.6 times faster in every run, the dab is repeatably shorter for all three brushes at 21M and for Draw and Smooth at 5.2M, and the serial fallback stays within the baseline's spread. What remains serial in a 21M Draw dab with 11 helpers: the edit-region walk and dirty marking inside "normals", the undo record (0.34 ms), the average normal (0.3 ms) and the commit's top level (0.01 ms).

### Incremental normal cones (default)

Before this change, every refit recomputed the normal cone of every chunk it touched: two passes over all 4,096 triangles, even when a dab moved a few hundred of them. With the dense box refit, the cones were about half of what remained of undo/redo's refit (16 of 31 ms CPU at 21M with 11 helpers).

**The change** (`ChunkedBvh::setIncrementalCones`, `AppConfig::incrementalCones`, `--cones incremental|exact`). For each chunk, in the same parallel pass as its box refit and under the same lock:
- **Fallback.** If the refit queued more than half of the chunk's leaves, the cone is recomputed exactly, as before.
- **Widening.** Otherwise the cone keeps its axis and is widened to the new face normals of the triangles in the queued leaves. That is one pass over the changed triangles. Every triangle with a moved vertex is in one of those leaves, and the normals of the others did not change, so the old cone still holds them. Each normal is tested with the same arithmetic and the same 1e-4 margin as `computeNormalCone`, so the widened cone holds every normal of the chunk (`widenNormalCone`).
- **Culling off.** If the old cone was unusable, or the widened one would be wider than about 84 degrees, the chunk gets an unusable cone, which rejects nothing.
- **Rebuild.** Widened and disabled chunks are marked stale. When its queue is empty (also between the inputs of a stroke), the sculpt thread computes exact cones for up to 4 chunks per thread. It does this outside the document lock, which is safe because it is the only writer, then publishes them under the lock. This runs before the BVH optimize steps, and `waitUntilIdle` waits for it. Once every stale chunk is rebuilt, the cones equal exact mode bit for bit.

The publication hold is at most 0.002 ms. A rebuild step computes for at most 0.43 ms of wall time before the next input is taken.

**Undo and redo** (`se_bench history … dense [icones] [cfb=100]`, p50 in ms, median of 3 interleaved processes). The columns give the baseline (frozen dense build) → incremental. "No fallback" (`cfb=100`) widens every chunk, whatever its share of changed leaves. Cone CPU is summed over threads. Extra triangles are measured right after each command, before its rebuild, from 14 fixed eyes, against exact cones. The rebuild is compute wall time per command.

| | undo hold | refit + cones (wall) | cones (CPU) | cones per step: exact / widened / off | extra triangles per eye | rebuild per command | no fallback: hold, extra triangles, rebuild |
|---|---|---|---|---|---|---|---|
| 21M, 11 helpers, typical | 7.67 → 7.61 | 2.71 → 2.61 | 17.1 → 15.0 | 226 / 31 / 2 | 4.6K (0.05%) | 0.24 ms | 7.13 · 114K (1.3%) · 1.71 ms |
| 21M, 11 helpers, strong | 7.30 → 7.21 | 2.52 → 2.37 | 15.7 → 13.7 | 257 / 36 / 4 | 3.5K (0.04%) | 0.30 | 6.54 · 206K (2.2%) · 1.79 |
| 21M, 0 helpers, typical | 43.4 → 42.1 | 19.2 → 17.7 | 10.2 → 8.7 | 226 / 31 / 2 | 4.6K (0.05%) | 1.11 | 38.3 · 114K · 7.97 |
| 21M, 0 helpers, strong | 38.8 → 37.0 | 17.5 → 16.0 | 9.3 → 7.9 | 257 / 36 / 4 | 3.5K (0.04%) | 1.38 | 34.3 · 206K · 9.58 |
| 5.2M, 11 helpers, typical | 1.76 → 1.70 | 0.67 → 0.58 | 3.7 → 2.9 | 57 / 15 / 1 | 3.0K (0.13%) | 0.12 | 1.56 · 29K (1.2%) · 0.39 |
| 5.2M, 11 helpers, strong | 1.55 → 1.55 | 0.71 → 0.59 | 4.8 → 3.4 | 63 / 20 / 4 | 2.8K (0.11%) | 0.18 | 1.43 · 52K (2.1%) · 0.52 |
| 5.2M, 0 helpers, typical | 8.78 → 8.21 | 4.79 → 4.15 | 2.9 → 2.2 | 57 / 15 / 1 | 3.0K (0.13%) | 0.51 | 7.14 · 29K · 2.18 |
| 5.2M, 0 helpers, strong | 7.96 → 7.25 | 4.37 → 3.58 | 2.7 → 1.9 | 63 / 20 / 4 | 2.8K (0.11%) | 0.72 | 6.14 · 52K · 2.52 |

Redo moves the same way. Per run:
- **Without helpers,** the undo hold is 3 to 9% shorter in all 12 runs.
- **With 11 helpers,** it is the same as the baseline within noise.
- **Most undo chunks fall back.** Undo changes about 87% of the leaves of most affected chunks, which keeps the cone CPU gain to 10 to 30%.
- **No fallback** shortens the hold by 5 to 12% in every run. In exchange it lets through 1 to 2% more triangles until the rebuild, and its rebuild costs 6 to 7 times more. It is not adopted.

The same build with exact cones matches the baseline within noise. The hash after the cycles is identical in all runs.

**Dabs** (`se_bench adjacency … dense [icones] r=R`, median ms per dab of 3 interleaved processes; the 21M small-dab row with helpers is from a second set of 5 rounds, because the first was bimodal in every mode). The bench calls the stroke processor directly, so no rebuild happens during a stroke. "Refit + cones" is the part of the commit that holds the document lock.

| | Draw | Smooth | Move | Draw refit + cones |
|---|---|---|---|---|
| 5.2M, small (r 0.05), 0 helpers | 0.276 → 0.178 | 0.339 → 0.244 | 0.191 → 0.106 | 0.148 → 0.050 |
| 5.2M, small, 11 helpers | 0.201 → 0.169 | 0.267 → 0.233 | 0.133 → 0.099 | 0.067 → 0.038 |
| 21M, small, 0 helpers | 0.848 → 0.732 | 1.115 → 1.006 | 0.552 → 0.474 | 0.330 → 0.212 |
| 21M, small, 11 helpers | 0.730 → 0.711 | 1.004 → 0.965 | 0.420 → 0.415 | 0.119 → 0.105 |
| 5.2M, r 0.25, 0 helpers | 4.38 → 4.11 | 5.76 → 5.45 | 2.94 → 2.75 | 1.38 → 1.10 |
| 5.2M, r 0.25, 11 helpers | 2.33 → 2.30 | 2.60 → 2.54 | 1.45 → 1.41 | 0.228 → 0.193 |
| 21M, r 0.25, 0 helpers | 18.4 → 18.3 | 23.8 → 23.4 | 12.0 → 11.5 | 4.83 → 4.33 |
| 21M, r 0.25, 11 helpers | 7.71 → 7.95 | 8.78 → 8.40 | 4.53 → 4.66 | 0.788 → 0.702 |

The refit + cones time drops in every row. The 21M rows with 11 helpers and radius 0.25 move both ways within the run-to-run spread. The hashes are identical.
- **Worst case, at the end of a 200-dab stroke** with no rebuild during it: the widened cones let through up to 0.7% more triangles (Draw, small dabs at 5.2M; 0.23% at 21M).
- **No fallback** lets through up to 3.2% more.

**Live, through the controller** (`se_bench live … dense [icones] r=R`, 11 helpers, 400 inputs every 2 ms, 2 interleaved runs, lock hold per dab p50 in ms). Here the rebuild runs between inputs:
- **5.2M, small dabs:** Draw 0.082 / 0.087 → 0.045 / 0.052, Smooth 0.115 / 0.128 → 0.068 / 0.057, Move 0.138 / 0.140 → 0.072 / 0.056. Dab latency p50 is 0.22 to 0.40 → 0.14 to 0.31.
- **21M, small dabs:** Draw 0.196 / 0.184 → 0.181 / 0.164, Move 0.215 / 0.207 → 0.174 / 0.170. Move latency is 0.58 / 0.60 → 0.47 / 0.46.
- **21M, radius 0.25:** Draw 1.14 / 1.05 → 1.01 / 0.95, Smooth 1.07 / 0.98 → 0.98 / 0.90, Move 1.03 / 0.94 → 0.97 / 0.89. Latency changes by 0 to 4%.
- **5.2M, radius 0.25:** within noise.
- **Rebuild work:** about 1.3K to 1.5K chunks, 9.5 to 11 ms of compute, per 21M Draw stroke at radius 0.25; 27 to 35 ms per small-dab stroke. All of it runs in idle time.
- **Extra triangles at stroke end:** at most 585 per eye (0.024%) at 5.2M and 293 (0.003%) at 21M.

**Vulkan history scenario at 21M** (`--selftest-scenario history`, 2 runs per mode, all PASS: exact end state, every cone holds its normals, the undone mesh pixel-identical to the initial one):

| | undo hold p50 | renderer wait p50 · p95 · max | triangles per frame, mean (typical / strong) | frame GPU p50 | rebuild per scenario |
|---|---|---|---|---|---|
| typical, 0 helpers | 52.0 → 50.6 | 0.68 · 51.0 · 60.5 / 0.05 · 44.1 · 47.7 → 1.83 · 42.0 · 49.1 / 0.00 · 45.1 · 50.9 | 7,827,630 → 7,827,721 | 10.4 / 15.0 → 14.8 / 14.6 | 111 to 145 steps, 21 to 27 ms, worst step 0.29 ms |
| strong, 0 helpers | 13.97 → 13.13 | max 82.5 / 83.0 → 80.5 / 84.4 | 9,284,993 → 9,284,888 | 8.3 / 2.1 → 2.4 / 2.3 | 565 steps, 96 ms, worst 0.31 ms |
| typical, 11 helpers | 9.71 → 9.69 | 0.37 · 1.80 · 4.79 / 0.23 · 2.84 · 5.00 → 0.36 · 2.34 · 3.16 / 0.22 · 2.40 · 3.71 | 7,828,391 → 7,829,732 | 5.5 / 5.3 → 5.1 / 2.3 | 32 steps, 7.6 ms, worst 0.39 ms |
| strong, 11 helpers | 3.06 → 2.81 | max 13.7 / 12.9 → 14.5 / 14.9 | 9,285,515 → 9,286,548 | 1.7 / 1.7 → 1.7 / 1.7 | 88 steps, 19 ms, worst 0.43 ms |

The triangles drawn per frame are the same within ±0.02% (identical p50 and max), and the back-facing chunks skipped are the same (p50 3,207 to 3,208 and 2,844 to 2,859). GPU time at this light load moves with clock state in both modes. Standard 21M self-test: PASS with `--cones incremental` and with `--cones exact` (238 checks each), including the culling comparisons that require identical images with culling off, frustum only and on, in 16 views after strong deformation, undo and redo.

**Correctness tests** (`IncrementalConeTests.cpp`, plus a third variant of both concurrent-reader tests):
- **Edits from 0.01 to 2.5 radii,** with chunks of 64, 256 and 4,096 triangles and fallbacks of 50% and none, after every edit and every partial rebuild or optimize step:
  - every cone holds every normal;
  - brute force over 20 eyes finds no rejected chunk with a triangle facing the eye;
  - boxes and picks equal exact mode;
  - every cone that is not stale equals the exact one.
  
  After the rebuild, everything equals exact mode and a full refit bit for bit.
- **A folded triangle** turns its chunk's culling off.
- **Through the controller,** strokes and undo/redo at 0, 3 and 11 helpers give the same hash, boxes, cones and picks as exact mode once idle, and idle means no stale cone remains.
- **Concurrent readers** (`PrepareCommitReader`, `HistoryStagingReader`) see, under the lock, boxes that match the positions and cones that hold every normal; every cone that is not stale is exact.

Two mutations were checked: skipping the widening fails 4 of these tests, and never marking a chunk stale fails 5.

**Decision.** Incremental cones are the default (`AppConfig::incrementalCones`), with the fallback above half of a chunk's leaves. The dab lock hold drops repeatably, by 10 to 40% for small dabs and about 10% for radius 0.25 at 21M. The undo/redo hold drops 3 to 9% without helpers in every run. The wider cones add no rendering cost that can be measured: under 0.15% more triangles for the frame or two before the rebuild, and the same triangles and back-facing skips in the Vulkan scenario. At 21M with 11 helpers the undo/redo hold does not change, because most undo chunks fall back. What remains there is the box refit and the exact cones of chunks that changed almost entirely.

### Exact-size chunk BVH build (default)

`Bvh::build` used to reserve `2n` nodes for its node, parent and depth arrays, build into them, and then call `shrink_to_fit`. With thousands of chunks built at once, every chunk made large allocations and copying reallocations on the shared process heap. That made the build superlinear, and with helpers slower than on one thread (see "Scaling checkpoint: 84M triangles").

**The change.** The build splits each node at the exact median, so a tree's shape, and therefore its node count, depends only on its triangle count. `Bvh::nodeCountFor(n)` computes that count in O(log n): at every depth the subtree sizes are two adjacent values. `build` reserves exactly that many nodes, keeps the arrays when a rebuild has the same count, and no longer shrinks them. The trees are the same, so results are bit-identical.

**Tests** (`BvhTests.cpp`):
- `NodeCountForMatchesTheBuildRecursion`: every count from 0 to 200,000 and up to 100M against the recursion written out.
- `BuildReservesExactlyItsNodesForAnyRange`: counts around every split boundary (1 to 70, `2^k ± 1`, `5·2^k ± 1` up to 8,192) at several offsets. Each must have size and capacity equal to `nodeCountFor`, and a reused `Bvh` must give the same nodes and leaves, byte for byte, as a fresh one.
- `PartialLastChunkHasExactNodeCountAndCorrectQueries`: meshes whose last chunk is partial (4,096·k ± small), serial and with 3 helpers. Each chunk must have its exact count, and rays must match brute force.

Putting back the `2n` reserve fails 2 of them. 197 / 197 tests pass.

**Interleaved runs** (`se_bench adjacency … pgather pmark dense icones creuse`, 2 rounds; old → new, round 1 / round 2):

| | BVH build | startup (icosphere + BVH) | peak commit |
|---|---|---|---|
| 21M, 0 helpers | 5.03 / 4.80 → 4.51 / 4.33 s | 12.9 / 12.4 → 12.3 / 11.9 s | 1.73 → 1.73 GiB |
| 21M, 11 helpers | 0.84 / 0.75 → 0.54 / 0.40 s | 8.66 / 8.30 → 8.39 / 7.99 s | 1.73 → 1.73 GiB |
| 84M, 0 helpers | 34.6 / 34.3 → 18.1 / 18.0 s | 68.6 / 68.3 → 52.1 / 51.9 s | 6.94 → 6.94 GiB |
| 84M, 11 helpers | 16.3 / 16.1 → 1.62 / 2.16 s | 50.2 / 50.1 → 35.0 / 36.1 s | 6.94 → 6.94 GiB |

- **Startup through `AppController`** (`live`, 11 helpers): 8.7 → 8.4 s at 21M and 50.0 → 36.2 s at 84M.
- **Same results.** Every hash (initial mesh, and after the Draw, Smooth and Move strokes) and the BVH size (41,963,519 nodes, 2,331.6 MiB at 84M) are identical.
- **Peak commit** is set by the sphere generator, not by the BVH, so it does not change.
- **84M Vulkan standard self-test:** PASS, 237 checks. Picking hits at exactly the camera distance minus the radius and misses past the edge, undo and redo restore every vertex exactly (including the strong strokes), and every cone holds its normals. The editor's peak commit is 11.8 GiB (working set 9.5 GiB).

### Normal recomputation at 84M (profile)

A dab's normals stage, outside the lock, has three parts:
1. `EditRegion::collect` walks the moved vertices' incident triangles, then their one-ring. It is serial.
2. `computeNormals` gives each affected vertex the sum of its incident triangles' face normals. It runs in parallel blocks of 4,096 vertices, and each face normal is computed once per corner.
3. The affected vertices are marked dirty, serially.

Undo and redo run the region walk before the lock and `recomputeNormals` (the same sum, written into the mesh) in parallel under it. The stroke stats now time each part (`StrokeStats::normals`, printed by `se_bench adjacency`), and the history profile has the normals' CPU time (`HistoryProfile::normalsCpuMs`).

**Dabs** (`se_bench adjacency`, radius 0.25, per edit, Draw; Smooth and Move are proportional):

| | normals | region (serial) | compute: wall, CPU, CPU per vertex | dirty | moved / affected vertices |
|---|---|---|---|---|---|
| 21M, 0 helpers | 6.19 ms | 2.78 | 3.33, 3.32, 21 ns | 0.09 | 155K / 156K |
| 21M, 11 helpers | 4.49 | 3.58 | 0.79, 7.75, 50 ns | 0.11 | 155K / 156K |
| 84M, 0 helpers | 36.1 | 14.5 | 21.3, 21.3, 34 ns | 0.35 | 620K / 622K |
| 84M, 11 helpers | 19.4 | 15.8 | 3.27, 35.1, 56 ns | 0.36 | 620K / 622K |
| 84M, 11 helpers, radius 0.125 | 4.03 | 3.16 | 0.76, 7.54, 48 ns | 0.11 | 157K / 158K |

- **The serial region walk is the bottleneck.** With helpers it is 81% of the normals stage and 46% of the whole Draw dab at 84M (15.8 of 34.6 ms).
- **It is linear in the brush,** at 20 to 25 ns per moved vertex at both sizes. At 84M with radius 0.125, which moves as many vertices as 21M at radius 0.25, the whole dab costs the same as at 21M.
- **The parallel compute already scales:** 6.5 times faster with 11 helpers.
- **CPU per vertex grows with helpers** (34 → 56 ns at 84M), the same pattern as the cones: shared caches and memory bandwidth.

**Undo** (`se_bench history`, typical strokes, p50 per step, 84M, 1.84M affected vertices per step):

| | lock hold | normals in the hold: wall, CPU, per vertex | positions | refit + cones |
|---|---|---|---|---|
| 0 helpers | 167 ms | 96.7, 96.7, 53 ns | 5.3 | 63.2 |
| 11 helpers | 28.6 | 12.7, 147, 80 ns | 5.5 | 8.9 |
| 21M, 11 helpers (461K affected) | 7.4 | 3.5, 38.4, 83 ns | 1.3 | 2.2 |

Under the lock the normals are already parallel, at 7.6 times the single-thread speed. They cost more per vertex than in a dab (80 against 56 ns), because the undo of three strokes touches a larger, more scattered set of pages.

**Next optimizations, in order:**
1. **Parallel edit region** (dabs; adopted since, see "Parallel edit region": region 15.8 → 6.6 ms, Draw dab 33 → 25.5 ms). `EditRegion::collect` could run like the parallel gather. Fixed blocks of moved vertices would claim triangles and one-ring vertices with atomic bit operations and keep the first occurrence by an atomic minimum. A prefix sum would then lay out the lists in today's order, which the BVH refit relies on for identical results. Expected at 84M with 11 helpers: region 15.8 → about 2 ms, Draw dab 34.6 → about 21 ms. The same walk runs before the undo lock, where it shortens the total.
2. **Undo normals** (the hold). Three options:
   - store each entry's final normals with its positions, so undo and redo copy them like positions (5.5 ms for 1.84M vertices against 12.7 ms; about 22 MB more per typical history step at 84M);
   - make `--history-normals before` the default (shorter hold, longer total);
   - compute each face normal once per edit instead of once per corner (1.24M instead of 3.7M cross products for an 84M Draw dab).

### Parallel edit region (default with helper threads)

The serial `EditRegion::collect` was 46% of a Draw dab at 84M with 11 helpers (15.8 of 34.6 ms, "Normal recomputation at 84M"), and the same walk runs before every undo and redo lock.

**The change** (`EditRegion::collect(mesh, moved, jobs)`, `StrokeProcessor::setParallelRegion`, `AppConfig::parallelRegion`, `--region parallel|serial`). With helper threads and at least 16,384 moved vertices, the walk runs in parallel and produces the same two lists in the same order:
1. Each moved vertex gets the index of its first occurrence in the moved list, by an atomic minimum in a per-vertex stamp (one `u32` per vertex, flagged as moved).
2. Blocks of 2,048 moved vertices keep the vertices whose stamp is their own index, and the incident triangles whose earliest moved corner is that vertex, in row order. That is exactly where the serial walk first meets each triangle.
3. A prefix sum over the blocks lays out the triangle list.
4. Each corner that did not move gets its first corner position in the triangle list (`3·triangle + corner`) by an atomic minimum.
5. Blocks of 4,096 triangles keep the one-ring vertices whose stamp is that corner, and a second prefix sum lays out the vertex list.
6. The stamps are reset by walking the lists, so a dab neither allocates nor clears whole arrays.

The stamp array is `thread_local`, grows to the vertex count and is kept; only the sculpt thread walks regions, so there is one. Smaller edits and runs without helpers keep the serial walk. Undo and redo use the same path before the lock.

**Tests** (`ParallelRegionTests.cpp`):
- `ListsEqualTheSerialWalkInOrder`: both lists equal the serial walk element for element, for 3 layouts and moved lists that are shuffled, sorted, repeated, interleaved distant patches, every vertex, and sizes around the threshold, at 1, 3 and 11 helpers; serial and parallel calls alternate on one object.
- `StrokesMatchTheSerialWalkAtEveryThreadCount`: Draw, Smooth and Move strokes give the same hash after every dab, the same undo deltas, boxes and picks.
- `UndoRedoThroughControllerMatchTheSerialWalk`: undo and redo through `AppController` give the same hash, picks and trees after every step.

Two mutations each fail the list test: storing the corner instead of its atomic minimum, and choosing a triangle's owner by the largest stamp. 200 / 200 tests pass.

**Interleaved runs**, one binary, serial → parallel, 2 rounds (round 1 / round 2). `se_bench adjacency|live|history <level> <helpers> … pgather pmark dense icones creuse [pregion]`, radius 0.25 unless noted. Without helpers both use the serial walk; the differences there (up to 4%) are run-to-run noise.

Dabs (`adjacency`, per edit):

| | Draw: dab / region | Smooth: dab / region | Move: dab / region |
|---|---|---|---|
| 21M, 11 helpers | 7.17 / 8.58 → 6.36 / 6.51 ms; 3.06 / 3.72 → 1.60 / 1.67 | 8.33 / 9.60 → 7.83 / 7.79; 2.71 / 3.13 → 1.42 / 1.39 | 4.45 / 5.42 → 3.68 / 3.68; 2.39 / 3.02 → 1.46 / 1.43 |
| 84M, 11 helpers | 33.9 / 31.9 → 25.5 / 25.5; 15.8 / 14.9 → 6.75 / 6.43 | 38.0 / 36.2 → 30.9 / 29.9; 13.8 / 13.3 → 5.51 / 5.29 | 24.0 / 23.2 → 14.9 / 14.7; 14.8 / 14.8 → 5.74 / 5.60 |
| 84M, 11 helpers, radius 0.125 (same footprint as 21M) | 8.59 / 7.22 → 6.60 / 5.57; 3.69 / 3.03 → 1.81 / 1.48 | 10.1 / 8.74 → 8.21 / 7.03; 3.29 / 2.85 → 1.52 / 1.20 | 5.75 / 4.60 → 3.81 / 3.23; 3.24 / 2.53 → 1.53 / 1.23 |
| 21M / 84M, 0 helpers | serial both ways: 17.8 / 17.1 vs 16.9 / 16.7; 94.4 / 93.1 vs 90.4 / 89.7 | | |

Live strokes (`live`, 400 inputs every 2 ms, p50 of each dab, lock hold p50, reader wait p95 / max over every frame):

| | dab latency Draw / Smooth / Move | inputs queued max (Draw / Smooth / Move) | lock hold per dab | reader wait | undo, redo (lock hold) |
|---|---|---|---|---|---|
| 21M, 11 helpers | 8.8 / 10.2 / 5.5 → 6.0 / 7.4 / 3.4 ms (round 2: 8.7 / 9.8 / 5.1 → 6.6 / 8.4 / 3.7) | 7 / 6 / 256 → 7 / 5 / 170 (r2 6 / 5 / 247 → 7 / 5 / 186) | 0.93 → 0.83 (r2 0.88 → 0.91) | ≤ 0.6 / 1.0 both | 6.5, 5.7 (1.9) → 4.2, 3.5 (1.5); r2 6.6, 4.9 → 4.7, 4.2 |
| 84M, 11 helpers | 35.2 / 39.6 / 24.1 → 25.4 / 31.9 / 14.8 (r2 35.0 / 39.1 / 24.1 → 24.1 / 30.0 / 13.9) | 297 / 291 / 367 → 253 / 257 / 347 (r2 → 248 / 252 / 343) | 3.49 → 3.48 (r2 3.48 → 3.46) | ≤ 2.6 / 3.7 both | 28.9, 23.3 (7.2) → 17.2, 14.5 (7.0); r2 28.4, 23.3 → 16.9, 14.6 |
| 84M, 11 helpers, radius 0.125 | 8.3 / 10.1 / 5.0 → 5.4 / 7.3 / 3.2 (r2 9.1 / 11.0 / 5.7 → 6.7 / 8.8 / 3.9) | 121 / 156 / 248 → 16 / 69 / 154 (r2 148 / 172 / 261 → 73 / 120 / 193) | 0.83 → 0.79 (r2 0.96 → 0.96) | ≤ 0.7 / 1.2 both | 9.0 / 8.3 → 4.4 / 4.9 (hold 2.1 / 1.9 → 1.7 / 1.8) |

The lock hold and reader wait do not change: the walk runs before the lock. At 84M radius 0.25 the dab still takes longer than the 2 ms input interval, so inputs still queue, but fewer.

Undo and redo (`history`, typical strokes, p50 per step; 84M: 1.84M moved vertices per step):

| | undo: region / total / lock hold | redo: region / total / lock hold |
|---|---|---|
| 21M, 11 helpers | 15.9 / 16.2 → 6.5 / 6.1 ms; 25.1 / 25.3 → 15.4 / 15.2; 7.2 / 7.3 → 7.3 / 7.2 | 15.6 / 15.5 → 6.0 / 5.9; 24.5 / 24.4 → 14.6 / 14.5; 7.3 / 7.4 → 7.3 / 7.1 |
| 84M, 11 helpers | 60.5 / 62.3 → 24.7 / 26.6; 90.6 / 97.9 → 61.0 / 63.1; 24.1 / 29.2 → 28.7 / 29.1 | 60.2 / 60.4 → 23.4 / 24.2; 90.5 / 93.9 → 57.5 / 59.0; 25.5 / 28.6 → 29.3 / 29.5 |

Every hash is identical between the two walks: after every Draw, Smooth and Move stroke at both sizes and radii, and after the undo/redo cycles.

**Why not the predicted 2 ms.** The walk is 2.2 to 2.5 times faster with 11 helpers, not 8. Steps 1 and 4 are atomic minimums at random addresses in a 160 MiB stamp array, and steps 2 and 5 read it again at random; each is bound by memory latency and bandwidth, like the other parallel stages at this size ("Normal recomputation at 84M": CPU per vertex grows with helpers).

**Memory.** The stamps are 4 bytes per vertex, kept once allocated: 40 MiB at 21M and 160 MiB at 84M (sculpt thread only). The per-block lists add a few MiB, and the serial bitsets are no longer grown, so the stroke's own scratch is slightly smaller (`live`: 25.1 → 24.2 MiB at 21M, 98.4 → 94.6 MiB at 84M). `AppController::stats().editScratchBytes` now includes the stamps. Peak commit does not change in dab runs, because the sphere generator sets it (1.73 / 6.94 GiB); in the history runs it grows by the stamps, 1.79 → 1.84 GiB at 21M and 7.12 → 7.34 GiB at 84M. The 84M editor peaked at 11.8 GiB commit with 9.75 GB of RAM free, so 160 MiB fits.

**Vulkan standard self-test** (`--selftest`, which includes the live strokes in the window), one binary with `--region parallel` and `--region serial`:
- **84M:** both PASS, 237 checks, none failed. Picking hits at exactly the camera distance minus the radius, undo and redo restore every vertex exactly (including the strong strokes), and every cone holds its normals. Serial → parallel: dab latency median 27.3 → 16.5 ms, p95 36.3 → 22.6 ms; lock hold per dab median 2.75 → 2.26 ms. Peak commit 11.84 → 12.05 GiB, peak working set 9.46 → 9.63 GiB, lowest free RAM 9.67 → 9.29 GiB. The edit scratch after the self-test's large brushes is 261 → 320 MiB without the stamps: the per-block lists are kept at the largest dab's size.
- **21M:** PASS, 237 checks, with `--region parallel` and with the new default; dab latency median 6.0 ms, peak commit 3.28 GiB.

**Default.** The speedup repeats in both rounds at 21M and 84M, for all three brushes and for undo and redo, and the memory fits, so the parallel walk is the default when helper threads exist. `--region serial` turns it off.

### Dab profile at 84M and the one-pass edit region (default)

**Profile** (`se_bench adjacency … pgather pmark dense icones creuse pregion`, editor defaults, radius 0.25, per edit, commit f71cb46, 11 helpers):

| | dab | gather: query + select + undo record + average normal | edit region | normals compute + dirty | BVH: leaf marking + refits and cones + top | publish + stage | brush |
|---|---|---|---|---|---|---|---|
| 84M Draw | 25.5 ms | 9.5 = 2.5 + 3.6 + 1.5 + 1.9 | 6.4 | 2.9 + 0.4 | 3.6 = 1.3 + 2.2 + 0.05 | 1.1 + 1.0 | 0.4 |
| 84M Smooth | 29.9 | 8.3 = 1.9 + 3.4 + 1.4 + 1.6 | 5.3 | 2.5 + 0.3 | 3.3 | 0.9 + 0.8 | 8.4 |
| 84M Move | 14.7 | none (Move keeps its stroke's vertices) | 5.6 | 2.7 + 0.4 | 3.5 | 1.0 + 1.0 | 0.5 |
| 84M Draw, radius 0.125 | 5.6 | 2.0 | 1.5 | 0.6 + 0.1 | 1.0 | 0.2 + 0.2 | 0.07 |
| 21M Draw | 6.5 | 2.4 | 1.7 | 0.7 + 0.1 | 1.1 | 0.2 + 0.3 | 0.1 |

- **The edit region is the largest single stage** of Draw and Move, and the second of Smooth after its brush.
- **It is a brush-size cost, not a mesh-size cost:** at 84M with radius 0.125 (the same vertex footprint as 21M at 0.25) every stage costs what it costs at 21M.
- **Inside the walk** (84M Draw, per walk): stamping the moved vertices 0.25 ms, the row pass 3.1, joining the triangles 0.5, then a second pass over the whole triangle list for the one-ring (stamp 0.9, collect 0.5, join 0.1), and the reset 0.08.
- **The row pass, split** (a temporary switch that skipped parts of it): decoding the incident rows 0.8 ms, plus fetching each triangle's corners 1.0, plus reading their stamps and appending 1.7.
- **The marker array does not dominate.** Vertex ids follow the spatial order, so a dab's stamps sit in a small range of the 160 MiB array: stamping and resetting take 0.3 ms together. The cost is the triangles: each region triangle is fetched once per moved corner in the row pass, and once more in the one-ring pass.

**The change** (`EditRegion::collectParallel`, same lists in the same order):
1. **The row pass also finds the one-ring.** When a row owns a triangle, its unmoved corners are already in hand. Each one's stamp is lowered by atomic minimum to its row index, and the corner is recorded as a candidate in order. The serial walk lists a one-ring vertex at its first corner in the triangle list, which is in the earliest row that owns one of its triangles: the first candidate of that row. The join keeps exactly that candidate, the only one whose row matches the stamp, and marks the vertex taken so that later candidates of the same row are dropped. The second pass over the triangle list is gone.
2. **Two stamp reads per triangle instead of three**, with a branchless append. The corner that is the row's own vertex is known to hold the row's index. Moved stamps are now indices below 2^31 and one-ring stamps carry the high bit, so "the two other corners hold this index or more" is the ownership test, whichever one-ring stamps other rows are lowering at the same time.

**Tests.** `ListsEqualTheSerialWalkInOrder` gains two moved sets in which most triangles have one-ring corners, owned by rows in different blocks: every third vertex of a large patch, and a random 30% of the mesh. Two mutations each fail it: not marking the vertex taken, and recording the two corners in the wrong order. 200 / 200 tests pass.

**Interleaved runs**, f71cb46 → this change, fresh processes, 2 rounds (round 1 / round 2). Without helpers both builds take the serial walk, which this change does not touch; those runs differ only by noise (up to ±5%, and one baseline outlier at 125 ms where the other three runs were 98–106 ms). The 21M rows differ by about as much as the two rounds of one build.

Dabs (`adjacency`, ms per edit, 11 helpers):

| | Draw: dab / region | Smooth: dab / region | Move: dab / region |
|---|---|---|---|
| 84M | 25.1 / 25.9 → 23.3 / 24.4; 6.43 / 6.74 → 4.58 / 4.63 | 30.1 / 31.1 → 28.3 / 30.6; 5.28 / 5.41 → 3.55 / 3.74 | 14.5 / 14.9 → 12.6 / 13.6; 5.58 / 5.78 → 3.69 / 3.99 |
| 84M, radius 0.125 | 6.65 / 6.78 → 6.23 / 6.58; 1.80 / 1.82 → 1.35 / 1.40 | 8.43 / 8.38 → 7.87 / 8.16; 1.61 / 1.53 → 1.12 / 1.16 | 3.95 / 3.81 → 3.41 / 3.33; 1.54 / 1.50 → 1.10 / 1.09 |
| 21M | 5.73 / 6.67 → 5.67 / 5.94; 1.49 / 1.72 → 1.22 / 1.21 | 6.88 / 7.68 → 7.21 / 7.32; 1.27 / 1.44 → 1.04 / 1.05 | 2.95 / 3.53 → 3.22 / 3.23; 1.22 / 1.43 → 1.06 / 1.06 |

Live strokes (`live`, 400 inputs every 2 ms, 11 helpers; dab latency p50 / p95 / max, inputs queued at most):

| | Draw | Smooth | Move |
|---|---|---|---|
| 84M | 24.4 / 32.2 / 82.1, 264 queued → 23.1 / 27.4 / 53.2, 240 (r2: 25.8 / 30.7 / 60.1, 262 → 23.9 / 28.3 / 54.5, 247) | 29.9 / 37.4 / 44.4, 249 → 31.0 / 37.9 / 41.5, 263 (r2: 31.8 / 37.6 / 45.1, 263 → 30.4 / 35.8 / 39.2, 254) | 13.6 / 15.2 / 18.3, 344 → 12.3 / 14.2 / 15.4, 337 (r2: 14.8 / 17.0 / 19.0, 347 → 13.1 / 14.9 / 15.9, 340) |
| 84M, radius 0.125 | 6.55 / 7.85 / 28.1, 65 → 5.96 / 7.31 / 26.1, 37 (r2: 6.49 / 7.67 / 28.4, 65 → 6.26 / 7.52 / 27.3, 54) | 8.36 / 9.59 / 12.3, 112 → 7.89 / 9.45 / 12.9, 97 (r2: 8.69 / 10.2 / 13.4, 121 → 7.85 / 9.02 / 12.5, 97) | 3.58 / 4.09 / 5.09, 180 → 3.28 / 3.83 / 4.87, 159 (r2: 3.83 / 4.41 / 4.87, 195 → 3.24 / 3.66 / 3.98, 156) |
| 21M | 5.37 / 6.11 / 13.8, 6 → 5.77 / 6.42 / 14.6, 7 (r2: 6.53 / 7.85 / 17.2, 8 → 6.12 / 7.91 / 15.0, 7) | 6.67 / 7.76 / 9.27, 4 → 7.23 / 8.64 / 10.4, 5 (r2: 8.02 / 10.7 / 11.3, 5 → 7.83 / 10.2 / 11.6, 5) | 2.93 / 3.09 / 3.49, 131 → 3.00 / 3.34 / 3.71, 136 (r2: 3.52 / 4.05 / 4.42, 176 → 3.47 / 3.98 / 4.58, 168) |

- **Lock hold and renderer wait are unchanged:** the walk runs before the lock. Lock hold p50 is 3.3–3.5 ms per dab at 84M and 0.7–0.9 ms at 21M and at 84M radius 0.125. Reader wait p95 is ≤ 2.6 ms and max ≤ 3.9 ms at 84M, ≤ 1.2 ms at the smaller footprint, in both builds.
- **The Draw maximum** at 84M is a single outlier dab per stroke (82 / 60 → 53 / 54 ms, against a p95 near 30); its cause was not isolated.
- **Smooth** is set by its brush (8.4 ms); its region saving (1.7 ms) is within the run-to-run spread of the live stroke.

Undo and redo (`history`, 84M, 11 helpers, typical strokes, p50 per step): the region before the lock 26.0 / 25.4 → 15.4 / 16.5 ms (redo 23.9 / 24.1 → 15.8 / 16.1); undo total 61.6 / 60.7 → 50.7 / 53.9 ms, redo total 57.7 / 57.8 → 50.0 / 53.6 ms; lock hold unchanged (27.9 / 29.8 → 28.3 / 30.8).

**Same results.** Every stroke hash (Draw, Smooth and Move, 21M and 84M, both radii, 0 and 11 helpers) and the hash after the undo/redo cycles are identical between the two builds.

**Memory.** The stamps are unchanged (160 MiB at 84M, 40 MiB at 21M). The per-block one-ring lists of triangle blocks are gone, so the retained edit scratch (stamps included) is smaller: `live` 254.7 → 234.2 MiB at 84M and 64.2 → 59.1 MiB at 21M. Peak commit is unchanged in dab runs (6.94 / 1.73 GiB, set by the sphere generator) and lower in the 84M history run (7.34 → 7.23 GiB).

**Remaining in the walk** (84M Draw, per walk): stamping 0.29 ms, the row pass 2.75, joining the triangles and the one-ring 0.33, joining the vertices 0.20, reset 0.13. The row pass is now row decoding and the corner fetch for each triangle once per moved corner. The next costs of a large dab are the gather (9.5 ms at 84M, its vertex selection 3.6) and, for Smooth, the brush.

**84M Vulkan standard self-test**, the two editors back to back: both PASS, 237 checks, none failed. Picking hits at the camera distance minus the radius, undo and redo restore every vertex exactly (including the strong strokes), and every cone holds its normals. Live strokes in the window, f71cb46 → this change: dab latency median 18.9 → 17.9 ms, p95 25.2 → 23.5 ms, max 29.4 → 27.9 ms; lock hold per dab median 2.78 → 2.81 ms; edit scratch 480.1 → 437.5 MiB; peak commit 12.05 → 11.94 GiB, peak working set 9.64 → 9.57 GiB. An earlier single run of the new build gave 17.8 / 23.6 / 32.0 ms.

**Adopted.** The parallel walk is replaced in place (no option): with 11 helpers the region is 1.4 times faster at 84M in both rounds and for all three brushes and undo/redo, results are bit-identical, and memory goes down. At 21M the dab difference is within the noise between rounds. Without helpers nothing changes.

### Vertex gather: test only each block's candidates (adopted)

Measured source: `b1de3fb` plus the uncommitted work.

**Where selection spends its time** (parallel gather, 84M, radius 0.25, `se_bench adjacency 11 11 pgather pmark pregion pprep nooreg`, ms per dab). The gather is query 1.9–2.4, select 3.0–3.6, and an overlapped preparation of 2.3–2.5. Selection has three steps:
1. **First corners, 1.0–1.2 ms.** This finds each vertex's first occurrence: an atomic minimum of the corner index per vertex over the ~3.6M corners of about 1.2M triangles. It is about a third of the selection.
2. **Test, 1.5–1.8 ms, the largest step.** It walks all the corners again, re-reading each triangle and each vertex's slot, only to find the corners that are first occurrences. It then tests their distance.
3. **Join and reset, 0.4–0.7 ms.**

**Change.** The first step now runs block by block (2,048 triangles). When a block lowers a vertex's slot, it keeps that vertex and the corner as a candidate.
- Within a block corners only grow, so a block lowers each slot at most once, and its candidates come out in corner order.
- The block that holds a vertex's first corner always keeps it.
- The test step then checks only those candidates. A candidate is a first occurrence if the slot still holds its corner.
- So the test step no longer re-reads the triangles. The vertex order, the distance arithmetic and the falloff weights are unchanged. The serial path (fewer than 32,768 triangles) is unchanged.

**Exactness.**
- Draw, Smooth and Move hashes are identical to the baseline in every run: 6 headless runs at 84M, 12 at 21M and 6 viewport runs.
- The parallel-gather tests pass: bit-identical to the serial gather at 0, 1, 3 and 11 helpers, with undo and redo. All 211 tests pass.
- `se_bench repeat 9 11 draw|smooth 0.25 2`: after each undo, 0 positions and 0 normals differ from startup.

**Headless screen** (84M, radius 0.25, 3 alternating rounds, base / candidate, ms per dab):

| | round 1 | round 2 | round 3 |
|---|---|---|---|
| Draw select (first, test) | 3.56 (1.08, 1.83) / 2.64 (1.21, 0.92) | 3.56 (1.09, 1.81) / 2.77 (1.22, 0.94) | 3.05 (1.02, 1.50) / 2.96 (1.29, 1.03) |
| Smooth select (first, test) | 3.45 (1.16, 1.74) / 2.40 (1.26, 0.78) | 3.49 (1.18, 1.79) / 2.67 (1.33, 0.88) | 3.04 (1.11, 1.55) / 2.75 (1.33, 0.88) |
| Draw / Smooth gather | 8.59 / 7.88 → 7.58 / 6.66 | 8.57 / 7.90 → 7.94 / 7.12 | 8.10 / 7.30 → 8.11 / 7.13 |

- **The test step halves in every round** (1.5–1.8 → 0.8–1.0 ms).
- **The first-corner step costs 0.1–0.3 ms more**, for pushing the candidates.
- **Net selection gain: 0.8–1.05 ms in rounds 1–2.** In round 3 the baseline ran in the machine's faster state, and the gain was 0.1–0.3 ms.
- **Whole-dab totals (23–30 ms) stay within run-to-run variation.**

**Viewport trace** (84M, radius 0.25, input posted from a producer thread at 120 Hz, `--frame-on-commit off`, runs in the order base, cand, cand, base, base, cand). Dab-producing inputs, press excluded: Draw 44 per run, Smooth 33. All checks pass in all runs. The stroke hashes are the same in every run: Draw `68f2e6a3acf6dc6e`, Smooth `84e5593ed31cd301`. Endpoint: submission.

| p50 / p95 ms | Draw queued → commit | Draw posted → submission | Smooth queued → commit | Smooth posted → submission |
|---|---|---|---|---|
| base (runs 1, 4, 5) | 20.93 / 24.10 · 20.77 / 23.96 · 20.63 / 23.61 | 28.19 / 38.75 · 28.70 / 42.00 · 28.02 / 40.51 | 33.13 / 37.82 · 33.27 / 37.97 · 33.11 / 37.60 | 43.06 / 53.57 · 41.50 / 56.14 · 42.25 / 51.21 |
| candidate (runs 2, 3, 6) | 20.48 / 23.71 · 20.45 / 23.29 · 20.22 / 23.23 | 29.00 / 45.40 · 29.20 / 40.24 · 29.47 / 38.23 | 31.88 / 37.48 · 32.29 / 36.00 · 31.75 / 36.32 | 40.90 / 53.70 · 39.51 / 49.83 · 40.52 / 50.97 |
| gather per dab (stage timers) | Draw 8.51–8.56 → 7.89–8.01 | | Smooth 9.36–9.55 → 8.69–8.81 | |

- **Gather and commit are consistently earlier.** The viewport gather is 0.6–0.7 ms shorter in every run.
  - Draw: queued → commit is 0.3–0.5 ms lower at p50, and lower at p95 in every pairing.
  - Smooth: queued → commit is 1.0–1.4 ms lower at p50.
- **Visible effect is mixed.**
  - Smooth's posted → submission p50 is 1.5–2 ms lower.
  - Draw's p50 is 0.3–1.4 ms higher, which is noise from the 0–16 ms wait for the next frame. A 0.4 ms earlier commit can't show through that.
  - Neither brush is visibly faster at p95.

**21M, smaller brushes** (`adjacency 10 11 ... r=0.125` and `r=0.0625`, 3 alternating rounds; hashes identical in all).
- **Radius 0.125** takes the changed parallel path (about 82K triangles). The machine switched state during the runs, so compare runs in the same state:
  - Fast state: select 0.298–0.306 → 0.259 ms.
  - Slow state: 0.433 → 0.343–0.386 ms.
  - No regression.
- **Radius 0.0625** queries about 20K triangles and takes the unchanged serial path: select 0.150–0.187 ms in both versions.

**Extra scratch.** The candidate lists keep their capacity, which `StrokeProcessor::gatherCandidateBytes()` reports and `se_bench adjacency` prints:
- 84M, radius 0.25: 10.2 MiB, out of 26.8 MiB of gather scratch.
- 21M, radius 0.125: 0.67 MiB.

**Verdict: adopted** as the parallel gather's selection, with no switch. Results are identical and the commit is consistently earlier. The cost is about 10 MiB at 84M. The visible gain is below the frame-wait noise.

### Reusing face normals within a dab (measured, reverted)

Measured source: `b1de3fb` plus the uncommitted work.

**Idea.** The normal update sums, for each affected vertex (the moved vertices and their one-ring), the cross product of every incident triangle. So each triangle's face normal is computed once per corner, about 3 times. The prototype computed each face normal once and reused it:
1. Mark the triangle pages that hold the dab's triangles (those with a moved corner) and give each page a slot.
2. Compute every such triangle's face normal once, from the staged positions, with the same expression. Store it in the slot, and set a bit per triangle.
3. For each affected vertex, sum its incident triangles in the existing row order. Take the stored normal where its bit is set. Compute faces with no moved corner (the brush boundary) in place, as before.

**Exactness.** Results were bit-identical:
- Draw, Smooth and Move headless hashes at 84M were identical to the baseline in all 6 runs (`738f223307025da3`, `7e12edabb0689a9e`, `585eba3231cbc28f`).
- Two temporary tests passed:
  - Per dab: positions and normals, plus the undo records, were bit-identical with reuse on and off, at 0, 1, 3 and 11 helpers.
  - Undo and redo: the mesh hash after every stroke, undo and redo through `AppController` was identical.

**Headless A/B** (84M, radius 0.25, `se_bench adjacency 11 11 pgather pmark pregion pprep nooreg [facereuse]`, same binary, 3 alternating rounds, current / prototype, ms per dab):

| | round 1 | round 2 | round 3 |
|---|---|---|---|
| Draw normal compute (CPU) | 2.37 (25.1) / 3.96 (38.6) | 2.48 (26.2) / 4.82 (48.1) | 3.08 (33.4) / 4.83 (48.2) |
| Smooth normal compute (CPU) | 2.02 (21.9) / 3.33 (33.6) | 2.08 (22.5) / 4.12 (42.3) | 2.66 (29.3) / 4.10 (42.2) |
| Draw whole dab | 19.97 / 21.63 | 20.43 / 24.63 | 22.67 / 24.81 |
| Smooth whole dab | 23.32 / 25.21 | 23.85 / 28.77 | 27.03 / 28.86 |

- **The prototype is slower in every round.**
  - Normal compute is 1.3–2.3 ms more wall time (per vertex 40–54 → 60–77 ns of CPU).
  - The whole dab is 1.7–4.9 ms more, well beyond run-to-run variation.
  - Move, which shares the path, is 1.4–1.9 ms slower in normal compute.
- **Extra scratch: 34.1 MiB** at 84M. That is the face-normal cache for about 1.2M triangles over the pages they touch, the per-triangle bits and the 80 KiB page-slot table.
- **Why it loses (inferred, not profiled):**
  - The per-corner cross product is cheap. Its three positions are the vertex's neighbours, already in cache from the one-ring walk.
  - The prototype replaces it with an extra pass that stores 12 bytes and sets an atomic bit per triangle. Each later lookup then reads a page slot, a bit word and the stored normal, from a cache spread across about 34 MiB.
  - That memory traffic costs more than the arithmetic it saves.

**Verdict: reverted.** By the experiment's rule, the viewport trace was not run. The per-corner computation stays.

### Input to frame in the Vulkan viewport at 84M (measured)

Measured source: `cb4bf67` plus the uncommitted work. Command: `SculptEngine --subdivisions 11 --region-overlap off --selftest-scenario latency --selftest <dir>`, 3 runs.

**Scenario.** One Draw stroke and one Smooth stroke at radius 0.25 go through the real viewport (`ViewportWindow::pointerMove`). Each stroke is a press plus 240 moves on a 120 Hz schedule, along a circle at 0.45 of the sphere's screen radius (about 2.8 units of surface). Each stroke is undone afterwards. Brush behaviour and defaults are unchanged.

**What is stamped.** All stamps use `steady_clock`.
- **Inputs:** enqueue time (`AppController::enqueue`).
- **Dabs:** each dab's commit gets a sequence number and a time in the commit hook, under the document lock and just before it is released (`AppController::commitSequence()`, `InputSample::commitSeq` and `committedAtMs`).
- **Frames:** the time the window receives each `UpdateRequest`; when `startNextFrame` is entered; when it takes the pending vertex updates under the mesh lock, together with the commit sequence it saw then; and when `frameReady()` returns.
- **Matching:** an input's frame is the first one whose consumed sequence covers that input's last dab, with no stale pages and no held-back uploads.
- **No presentation timing.** QVulkanWindow uses FIFO and no present-timing extension is used. The last observable event is therefore `frameReady()` returning: QVulkanWindow has issued the queue submit and `vkQueuePresentKHR` by then. Display scan-out comes later and is not measured.

**Measured:** every dab-producing input. Draw: 45 per run, 135 in all. Smooth: 34 per run, 102 in all. All reached a frame.

| ms, pooled over 3 runs (p50 / p95) | Draw (135 inputs) | Smooth (102 inputs) |
|---|---|---|
| input queued → commit (queue wait 0.01) | 21.0 / 24.1 | 32.9 / 38.4 |
| commit → frame took the update | 10.1 / 12.9 | 9.0 / 16.4 |
| ↳ of which notification → `UpdateRequest` | 10.0 / 12.8 | 8.8 / 16.2 |
| ↳ of which `UpdateRequest` → `startNextFrame` | 0.05 / 0.06 | 0.04 / 0.07 |
| frame took it → submit + present request | 2.8 / 4.1 | 2.9 / 4.1 |
| commit → submit + present request | 13.0 / 16.1 | 11.9 / 19.5 |
| **input queued → submit + present request** | **33.8 / 35.9** | **48.7 / 50.8** |

The Draw maximum, 55.7–61.4 ms queued → commit, is the press's dab every time (first touch).

**Example timelines** (run 1, ms from the press):
- **Draw, input 23:**
  1. queued 200.63, dab started 200.64
  2. committed 222.16 (seq 5); the GUI thread is notified at 222.21
  3. `UpdateRequest` and frame 9 start at 231.59; the frame takes seq 5 at 231.60
  4. submit + present request at 234.40

  Totals: 21.5 ms to commit, 9.4 ms waiting for the frame, 2.8 ms to record and submit. No frame was submitted between the commit and frame 9.
- **Smooth, input 22:**
  1. queued 187.81
  2. committed 222.56 (seq 49)
  3. frame 10 starts at 234.59 and takes seq 49
  4. submitted at 237.50

  Totals: 34.8 ms + 12.0 ms + 2.9 ms.
- **Smooth, input 15:** frame 7 had started at 173.24, during the dab's commit. It waited 2.2 ms for the mesh lock, took the update at 175.43 (0.02 ms after the commit) and was submitted at 178.72. This happened for 6–9 of the 34 Smooth inputs per run and none of the Draw inputs.

**Findings:**
- **No frame shows older geometry after a commit.** Over the 237 inputs, no frame was submitted after a commit while still holding older geometry, and no frame had stale pages or held-back uploads. A completed edit never waits an extra frame. It waits for the next frame to be requested.
- **Before commit is most of the delay:** 62% for Draw and 68% for Smooth at p50. The renderer's own part is 2.8–2.9 ms of recording, submit and present request.
- **Commit to frame is a wait for the next `UpdateRequest`.**
  - The GUI thread hears of the commit 0.1 ms later and calls `requestUpdate()`.
  - Qt then delivers `UpdateRequest` on a fixed grid of about 15.6 ms (frame intervals p50 15.5–15.8 ms, sometimes 31 ms). Once it arrives, `startNextFrame` runs 0.05 ms later, so acquire and fence waits are negligible.
  - The `UpdateRequest` times and the scripted input timer share the same phase modulo 15.625 ms. This is consistent with both being Qt timers on Windows' default 15.625 ms timer tick, rather than with vsync. That cause is inferred, not verified.
  - The wait for this tick is 0–16 ms, uniform. That is the p50 of about 10 ms and the p95 of about 13–16 ms.
- **Harness limitation (these runs; since fixed).** The scripted input ran on the same Qt timer. It reached `pointerMove` on that grid, 7.5 ms p50 (14.5 p95) after its 120 Hz schedule, often two moves at once. The table starts at enqueue, so this lag is not in it. The scenario now posts its input from a producer thread instead; see "Input independent of Qt's timer" below.

**Stage timers per dab in the viewport** (`InteractionStats::strokeStages`, runs 2 and 3, ms):

| | gather (query, select, prepare wall) | stage | brush | normals (region, compute, dirty) | publish | BVH |
|---|---|---|---|---|---|---|
| Draw | 8.5–8.6 (2.4, 3.7, 2.5) | 1.0 | 0.4 | 8.2 (4.8, 3.1, 0.4) | 1.0 | 3.6 |
| Smooth | 9.5 (2.6, 4.0, 2.8) | 1.0 | 10.8–11.0 | 8.1–8.2 (4.4–4.5, 3.3, 0.4) | 1.1 | 3.8 |

**Next experiment.** Most of the delay is before commit, so the CPU stage to target is **Smooth's brush stage**: 10.8–11.0 ms per dab, the largest single stage of either brush and a third of Smooth's input-to-commit time.
1. Split the brush stage into its binning, kernel and apply steps, in this live trace and in `se_bench adjacency` on the same stroke. The earlier bench profile had 7.6 ms (kernel 4.5), so first find which step grows by about 3 ms in the viewport.
2. Then shorten that step, with bit-identical results.

Draw's largest stages are the gather (8.5 ms) and the serial region walk (4.8 ms), and overlapping the region walk with the preparation was already within run-to-run variation. The renderer-side 0–16 ms wait for the next `UpdateRequest` (about 10 ms p50 for both brushes) is the next candidate after that. To test it: post the `UpdateRequest` directly when a commit arrives, or raise the timer resolution, and check that commit → frame drops to the 3 ms the renderer needs.

**Smooth's brush stage split** (follow-up, 2 alternating rounds at 84M).
- **Runs:** `se_bench adjacency 11 11 pgather pmark pregion pprep nooreg`, which matches the editor's defaults, alternated with the latency scenario. The scenario now notes the per-step timers that `SmoothBrush::steps()` keeps, and the vertices per dab.
- **Both paths are the same:** every dab is dense, binned in parallel, with one Laplacian pass.
- **Dab size differs:** the viewport's Smooth dabs move 650,695 vertices each. That is the full cap of a 0.25 sphere on the plain unit sphere (π r² of the surface). The bench's move 555,467, because its Smooth stroke follows its own path. Hence the per-vertex column.

| per dab | bench (2 runs) | viewport (2 runs) | bench ns/vertex | viewport ns/vertex |
|---|---|---|---|---|
| bin the vertices into the grid | 2.12 / 1.70 ms | 3.08 / 2.83 ms | 3.4 | 4.5 |
| kernel targets (27 cells per vertex) | 4.68 / 4.53 | 5.39 / 5.30 | 8.3 | 8.2 |
| kernel commit | 0.28 / 0.14 | 0.34 / 0.35 | 0.4 | 0.5 |
| Laplacian targets | 1.39 / 1.21 | 1.69 / 1.69 | 2.3 | 2.6 |
| Laplacian commit | 0.29 / 0.11 | 0.33 / 0.33 | 0.4 | 0.5 |
| **brush stage** | **8.77 / 7.69** | **10.83 / 10.49** | **14.8** | **16.4** |

Where the roughly 2.4 ms by which the viewport exceeds the bench goes:
- **Dab size, about 1.4 ms:** the viewport's dabs hold 17% more vertices.
- **Binning, about 0.7 ms:** it costs 32% more per vertex in the viewport. Binning streams positions and normals for every vertex, so it is the step most likely to suffer from memory or core contention. The renderer and the driver are running at the same time, but that cause is not isolated.
- **The kernel pass is not affected:** its per-vertex cost is the same in both. It is the largest step, half of the brush stage (5.3 ms per dab, 8.2 ns per vertex), and already parallel.

**Kernel pass: two variants tried, neither kept** (`se_bench adjacency 10 11 pgather pmark pregion pprep nooreg`, 21M, 3 alternating rounds; kernel ms per dab):

| variant | results | kernel per dab | vs baseline |
|---|---|---|---|
| baseline | — | 1.23 / 1.18 / 1.24 | — |
| cells packed into one array, clamped row offsets once per vertex | bit-identical (Draw, Smooth and Move hashes unchanged) | 1.16 / 1.23 / 1.17 (1.18 / 1.16 / 1.17 in a first set) | −3 to −5%, within run-to-run variation |
| plus each cell's average normal computed once (no per-tap division) | **not** bit-identical (Smooth hash changes) | 1.12 / 1.07 / 1.07 | about −9% more |

- **No memory bottleneck.** The grid has about 1,000 cells (about 28 KB) and stays in cache. Packing it into one array and indexing it more cheaply changes nothing measurable, so the pass is bound by its arithmetic (27 taps per vertex).
- **Per-cell values change the results.** The only per-cell quantity the taps could share is the average normal, and moving the division into the dot product rounds differently. I was wrong earlier to say this could stay bit-identical.
- **Small gain.** At 84M the per-cell average would save about 0.5 ms of a 5.3 ms kernel pass, about 1% of Smooth's 49 ms from input to submit.
- **Both variants were reverted.** The kernel code, the defaults and the results are as before.

**What this leaves.** The kernel pass can't be made meaningfully shorter without changing its arithmetic. A real reduction would mean fewer taps per vertex, for example a separable or 2x2x2 blend, which is a change in the brush's behaviour and needs the band checks redone. Two things remain without changing results: the binning slowdown in the viewport (0.7 ms) and the 0–16 ms wait for the next `UpdateRequest`. The second is larger and applies to every brush.

**Rendering on commit** (`--frame-on-commit on|off`, default off; 2 alternating rounds at 84M, latency scenario).
- **What `on` does:** when the GUI thread hears of a published edit, it sends the `UpdateRequest` to the window itself, so QVulkanWindow renders at once instead of at Qt's next timer delivery.
- **Measures:**
  - *Scheduled → submit* runs from an input's place on the 120 Hz schedule to the submit and present request. It includes the time the GUI thread takes to receive the input.
  - The press is left out.

| p50 / p95 ms, run 1 · run 2 | Draw off | Draw on | Smooth off | Smooth on |
|---|---|---|---|---|
| commit → frame took the edit | 9.3 / 28.4 · 9.8 / 12.4 | 6.5 / 26.3 · 4.4 / 23.0 | 6.5 / 20.3 · 9.3 / 16.0 | 0.3 / 19.3 · 1.0 / 11.7 |
| `UpdateRequest` → `startNextFrame`, all frames | 0.06 / 27.5 · 0.05 / 0.06 | 2.1 / 27.0 · 2.3 / 26.4 | 2.3 / 34.3 · 0.04 / 0.07 | 4.2 / 26.1 · 11.9 / 15.4 |
| input sent late vs its schedule (GUI thread busy) | 4.1 / 22.6 · 7.7 / 14.5 | 9.5 / 29.0 · 10.2 / 29.4 | 3.2 / 27.6 · 7.6 / 14.5 | 9.0 / 26.7 · 9.5 / 26.1 |
| queued → submit | 33.4 / 52.1 · 34.1 / 35.4 | 30.0 / 51.2 · 30.1 / 48.0 | 43.2 / 59.6 · 49.2 / 50.8 | 37.0 / 53.2 · 37.2 / 49.6 |
| **scheduled → submit** | **45.6 / 70.9 · 41.5 / 48.5** | **46.8 / 63.7 · 42.8 / 73.9** | **52.2 / 72.8 · 51.9 / 65.0** | **51.4 / 73.3 · 49.5 / 65.1** |

- **The edit reaches a frame sooner.** Commit → frame p50 drops by 3–5 ms for Draw and 6–9 ms for Smooth.
- **But the GUI thread blocks.** The extra frames now wait inside QVulkanWindow's `beginFrame`, before `startNextFrame`, for a frame slot or swap-chain image. That wait is the fence and acquire under FIFO. With `off` this wait is 0.05 ms, because Qt's ~15.6 ms timer spaces the frames out.
- **Input arrives later.** While the GUI thread is blocked it takes in no input, so inputs reach the controller 2–3 ms later at p50 and twice as late at p95 (about 27–29 ms against 14.5 ms).
- **No net gain.** From the input schedule to submit, the two modes are the same: Draw about 42–47 ms p50, Smooth about 50–52 ms p50, with p95 within run-to-run variation.
- **Run-to-run variation is large.** In the first `off` run, frames already waited in `beginFrame` (27–34 ms p95), and the p95s are much worse than in the second.
- **Verdict:** default stays off. `on` only moves the wait from Qt's timer to the swap chain.

To shorten commit → frame without blocking input, a frame must not be started while every swap-chain image is still queued for presentation. Options:
- render on commit only when the previous frame has been presented;
- use a present mode that does not block (mailbox), if the device offers one;
- move rendering off the GUI thread.

Each needs real presentation feedback (`VK_KHR_present_wait` or similar), which is not wired in.

**Input independent of Qt's timer** (producer thread; 84M; 3 runs per mode in the order off, on, on, off, off, on).
- **How the input is produced now.** The latency scenario's input comes from a worker thread:
  - It sleeps on a high-resolution waitable timer until each 120 Hz slot.
  - It then posts a timestamped pointer event to the viewport through Qt's event queue (`QCoreApplication::postEvent`): the press, 240 moves, then the release.
  - On the GUI thread, an event filter on the viewport records when each event is handled, then calls the viewport's pointer handler.
  - During the stroke the filter also swallows and counts the window system's own mouse events, so only scripted input reaches the stroke.
- **Three times per input:** scheduled, posted (just before `postEvent`) and handled.
  - Scheduled → posted is the producer's jitter.
  - Posted → handled is how long the event waited for the GUI thread.
- **Endpoint: submission.** That is `frameReady()` returning, by which point the queue submit and present request have been issued. It is not presentation.
- **Checks, all passing in all 6 runs:**
  - 241 input samples per stroke (press + 240 moves).
  - All 242 posted events handled once, in order; 0 window-system mouse events swallowed.
  - Every dab-producing input reached a frame: Draw 45 per run (44 without the press), Smooth 34 (33).
  - The same mesh hash after each stroke in every run and mode: Draw `68f2e6a3acf6dc6e`, Smooth `84e5593ed31cd301`.

| dab-producing inputs, press excluded, p50 / p95 ms (pooled, 3 runs per mode) | Draw off (132) | Draw on (132) | Smooth off (99) | Smooth on (99) |
|---|---|---|---|---|
| scheduled → posted (producer jitter) | 0.25 / 0.48 | 0.26 / 0.51 | 0.23 / 0.51 | 0.21 / 0.51 |
| **posted → GUI handler** | **0.04 / 3.03** | **14.89 / 31.92** | **0.04 / 0.89** | **11.84 / 27.43** |
| input queued → commit | 20.84 / 24.27 | 20.83 / 25.82 | 33.02 / 37.70 | 32.00 / 38.40 |
| commit → frame took the edit | 4.00 / 12.14 | 9.94 / 12.79 | 4.81 / 16.03 | 1.98 / 11.61 |
| frame took it → submission | 2.80 / 3.04 | 2.84 / 3.51 | 2.91 / 3.16 | 2.88 / 3.18 |
| **posted → submission** | **28.73 / 37.68** | **49.24 / 67.54** | **41.32 / 52.51** | **53.67 / 69.35** |

Per run, posted → submission p50 (p95):
- **Draw off:** 28.4 (37.3), 29.0 (37.2), 27.8 (40.4). **Draw on:** 49.1 (65.0), 51.6 (68.6), 43.0 (67.4).
- **Smooth off:** 40.5 (52.0), 41.8 (52.7), 43.2 (51.2). **Smooth on:** 53.9 (67.7), 53.7 (79.4), 49.4 (65.5).

- **Immediate rendering hurts consistently.** In every `on` run, posted → submission is 12–23 ms worse at p50 and 13–28 ms worse at p95 than every `off` run.
- **The cause is the blocked GUI thread.** Each commit's frame waits in `beginFrame` for a swap-chain image, 11–25 ms p50. During that wait, posted input sits in the queue: posted → handler goes from 0.04 ms to 12–15 ms at p50. That costs more than the shorter commit → frame time saves, and for Draw commit → frame is not even shorter.
- **The producer is not the problem.** Its jitter is under 0.5 ms at p95 in both modes.
- **With `off`, the frame side is small.** Input that arrives independently of Qt's timer no longer lines up with Qt's frame ticks, so commit → frame is 4.0–4.8 ms p50 (12–16 ms p95), against about 10 ms p50 with the timer-driven harness. Including recording and submission, the frame side is about 7 ms p50 out of 29 ms (Draw) or 41 ms (Smooth). The rest is before commit: 21 and 33 ms.

**Verdict on frame scheduling.** It does not deserve another experiment now. A non-blocking scheduler could at best remove the 4–5 ms p50 (12–16 ms p95) commit → frame wait. Such a scheduler would render on commit only when a swap-chain image is free, and it needs presentation feedback first. Blocking the GUI thread, as `--frame-on-commit on` does, makes latency worse. Default stays off. Draw's gather and normals and Smooth's brush and gather are 20–33 ms before commit, which is where the next gains are.

### Rates repeatability and what "visible" measures (benchmark fix)

Measured source: `cb4bf67` plus the uncommitted work.

**Symptom.** Repeats of the same Draw or Smooth stroke in one `se_bench rates` run give different mesh hashes, with `--region-overlap` off as well as on. Move repeats give the same hash.

**Not the input.** The k-th stroke of a run has the same hash in every process: 10 runs at 84M and 8 at 21M, overlap off and on, despite live 120 Hz pacing. Draw and Smooth inputs are never dropped and are applied in order, so input timing does not reach the geometry.

**Smallest case.** `se_bench repeat 6 11 draw|smooth 0.25 3` replays the rates stroke (the same 241 rays, each applied before the next, no timing) 3 times through `AppController`, undoing after each as rates does. At level 6 (82K triangles):
- **Undo restores the mesh.** After every undo, positions and normals equal the startup state bit for bit.
- **Same dabs and vertices.** Every repeat emits the same dabs from the same inputs (Draw 59 from 59 inputs, Smooth 48 from 48), and its undo record holds the same vertices with the same before-positions.
- **Different order.** The record lists those vertices in a different order. The final positions then differ by up to 1.2e-6 on about 4,200 vertices, and dab centres differ from about input 21–26 on.
- **Overlap doesn't matter.** Overlap off and on give the same numbers.
- **Fresh controllers agree.** With `fresh` (each repeat in a new controller), every repeat is bit-identical to the first: same dab centres, vertex ids and order, positions and normals, with overlap off and on. Identical input on an identical document gives identical geometry.

**Root cause.** The BVH's triangle order is not part of what undo restores. After a stroke, the background `ChunkedBvh::optimize` re-partitions the touched blocks of each chunk from the sculpted positions (`Bvh::optimize` → `buildNode`, an unstable median split). After the undo it re-partitions them again from the restored positions, starting from the previous order. About 15K of 82K triangle slots end up in a different order than at startup, and they keep changing with each stroke and undo.

The gather lists vertices in the order the sphere query visits the leaves. Draw's average normal and Smooth's binning sum over that list in order, so a different order changes the last bits. The first dab is affected straight away, and later dabs pick hits on a surface that differs by 1e-6. Move's result per vertex does not depend on the order, so its repeats agree.

This is not a correctness bug: undo and redo restore positions exactly, and a given history replays identically. But a stroke's exact result depends on the acceleration structure's history as well as on the mesh.

**Benchmark fix** (`tools/Bench/main.cpp`):
- RATE lines carry `seq=`, the stroke's place in the run. Hashes are comparable between runs only at the same `seq`, and `mall` against `mlatest` and `nooreg` against `oreg` compare that way. For bit-identical repeats inside one run, use `repeat ... fresh`.
- `repeat` mode as above.
- `dump=DIR` writes each stroke's input samples and reader ticks to CSV.
- A `commit` latency, from input queued to its last dab committed, for applied inputs with dabs.

**What "visible" is.** `rates` has no renderer. A reader thread takes the document lock every 16.67 ms, as the renderer does to copy dirty pages, and records when it got it. "Visible" is the first such tick after an input's dab committed, minus the time the input was queued. It is a lower bound on a presented frame: no render or present time is included, and the ticks jitter by about ±3 ms.

**One 84M Draw stroke** (radius 0.25, `dump=`):
- The rays advance 0.0125 per input at 120 Hz, and dabs are 0.0625 apart (0.25 × radius). Draw's own bulge lengthens the path the rays hit, so a dab comes every 4th input (53 times) or 5th (5 times): 59 of 241 inputs emit a dab. For Smooth it is every 5th input, 48 of 241.
- Queue wait for those inputs is 0.01 ms, and the dab takes 22 ms.
- Inputs come every 8.33 ms, half the reader period, so a dab input arrives either about 8.3 ms after a tick or on one. Arriving 8.3 ms after a tick, the next ticks are 8.3, 25 and 41.7 ms away: a 22 ms dab is visible at 25 ms. Arriving on a tick, they are 16.7 and 33.3 ms away: the same dab is visible at 33.3 ms. Tick jitter and the reader's 16.60 ms average period add values around 17–20 ms.
- One baseline stroke: 19–20 ms ×4, 25–26 ×30, 33–34 ×25. p50 25.5, p95 33.6.

So visible p95 moves in steps of 8.33 ms, one input interval, not one frame. The "≈8 ms shift described as one frame" in "Large-brush Draw and Smooth at 84M" was one such step. Which step the p95 lands on depends on the dab time against the tick thresholds and on how many dab inputs arrive on a tick, which varies with the reader's start offset and drift. `commit` does not have these steps.

**Rerun of the overlap A/B** (84M, radius 0.25, 120 Hz, 3 alternating rounds × 3 strokes, round orders base-cand, cand-base, base-cand; medians per round):

| | baseline (`nooreg`) | overlapped region (`oreg`) |
|---|---|---|
| Draw commit p50 (ms) | 21.8 / 21.8 / 18.9 | 19.6 / 20.5 / 19.3 |
| Draw visible p95 (ms) | 33.9 / 33.8 / 33.5 | 33.8 / 33.7 / 33.6 |
| Smooth commit p50 (ms) | 32.5 / 32.9 (27.0 on the 3rd stroke) / 27.3 | 29.3 / 31.5 / 29.0 |
| Smooth visible p95 (ms) | 42.2 / 42.2 / 42.1 | 42.1 / 42.0 / 42.1 |

Hashes are the same per `seq` in both modes. The machine changed speed during the run: baseline Smooth dropped from 32.9 to 27.0 ms in the middle of round 2 and stayed there for round 3. In the faster state the baseline beat the overlap (Draw 18.9 vs 19.3 ms, Smooth 27.3 vs 29.0 ms). In the slower state the overlap won by 1.2–3.4 ms, as in the earlier 5 rounds.

**What still holds:**
- **Smooth band fix:** the images, crease measures and undo/redo checks don't use rates hashes. Its timings came from deterministic `adjacency` runs.
- **Latest-wins Move:** its equal-hash checks compared the same stroke positions between modes, and Move does not depend on gather order.
- **Overlapped region walk:** it is bit-identical (tests; `adjacency` and per-`seq` hashes).

**What changes:**
- The claim that the overlap lowered visible latency by "one frame" in rounds 1–2 is withdrawn. Those were 8.33 ms steps of a phase-dependent metric. In the rerun, visible p95 did not move in any stroke.
- The per-dab gain (about 2 ms in the slower machine state) did not hold in the faster state, so it is within run-to-run variation. Default stays off.

### Large-brush Draw and Smooth at 84M: region walk overlapped with the preparation (measured, default unchanged)

Measured source: `cb4bf67` plus the uncommitted work (the Smooth band fix included). Both modes ran from one build, chosen with `oreg` / `nooreg`. Every other setting is the editor's default, with 11 helper threads.

**Strokes.** `se_bench rates <L> 11 hz=120 rad=<R> reps=3` makes the checkpoint's 2 s strokes along the equator at 120 Hz: 240 inputs; at 84M, Draw makes 59 dabs and Smooth 48. Each stroke is undone afterwards. A reader takes the document lock every 16.67 ms, so "visible" is the first such frame after an input's dab commits. The `only=draw|smooth`, `hz=`, `rad=` and `reps=` filters are new. Stage timers: `se_bench adjacency <L> 11` with the editor's flags.

**Baseline profile, 84M, radius 0.25** (`adjacency`, ms per dab):

| | Draw (566K verts) | Smooth (572K verts) |
|---|---|---|
| dab | 20.7 | 25.6 |
| gather | 8.1: query 2.4, select 3.0, undo record + average normal + staging 2.6 wall (overlapped; each is a serial loop) | 7.2 |
| brush | 0.4 | 7.6: kernel 4.5, binning 1.6, Laplacian 1.2 |
| affected triangles and one-ring (region walk) | 4.4 (rows 3.1) | 3.5 |
| normals | 2.9 (31 ms CPU) | 2.7 |
| dirty pages | 0.4 | 0.3 |
| BVH | 3.6: leaf marking 1.3, refits and cones 2.2 | 3.4 |
| publish | 1.0 | 0.9 |

At 120 Hz neither stroke falls behind: no inputs are queued at release and the drain after release is 0 ms. Release to visible is 0.5 or 16.5 ms, depending only on where the release falls in the 60 Hz frame. Input-to-visible p95 is about 34 ms for Draw (21 ms dab + wait for the frame) and 42 ms for Smooth (31 ms dab).

**Candidate** (`StrokeProcessor::setOverlapRegion`, `AppConfig::overlapRegion`, `--region-overlap on`, `se_bench oreg`). The three preparation steps overlapped at the start of each large dab are serial loops: undo record, average normal and page staging. They keep 3 threads busy for about 2.6 ms while 9 helpers wait. The region walk (affected triangles and one-ring) and the BVH leaf marking need only the gathered vertex list and the mesh's topology, so they now run as a fourth step in the same overlap, using the idle helpers through nested parallel loops.

The walk's per-vertex stamp array is now one array for the process, guarded by a mutex, instead of one per thread. The walk may run on a helper, and a per-thread array would grow a 160 MiB copy on every thread that ran it. Memory is unchanged.

Every dab, its order, the lists and their order are unchanged. Final positions, normals, undo records, boxes, cones and picks are bit-identical:
- `OverlappedPrepare.OverlappedRegion*` (strokes at 0, 1, 3 and 11 helpers; undo/redo through the controller at 0, 3 and 11).
- `adjacency` hashes at 84M (Draw `738f2233`, Smooth `7e12edab`, Move `585eba32`, same in all 6 runs) and at 21M.
- `history 10 11 3`: the same hashes after the undo/redo cycles.

**Results, 84M, radius 0.25, 120 Hz**: 5 alternating rounds of 3 strokes per brush and mode, round order base-cand, cand-base, base-cand, cand-base, base-cand. Each cell is the median of the round's 3 strokes, rounds 1–5.

| | baseline | overlapped region |
|---|---|---|
| Draw dab p50 (ms) | 19.0 / 18.4 / 21.3 / 21.8 / 21.0 | 17.4 / 16.9 / 19.4 / 19.2 / 19.1 |
| Draw visible p95 (ms) | 33.6 / 33.5 / 33.9 / 33.9 / 33.9 | 25.5 / 25.5 / 33.5 / 33.6 / 33.6 |
| Smooth dab p50 (ms) | 27.1 / 27.3 / 31.1 / 32.9 / 30.9 | 25.1 / 25.2 / 29.6 / 28.9 / 29.2 |
| Smooth visible p95 (ms) | 41.5 / 41.8 / 42.2 / 49.9 / 42.2 | 33.8 / 33.8 / 42.1 / 42.2 / 42.1 |
| drain after release / release to visible | 0 ms / 0.5 or 16.5 ms | same |

- **Per-dab time is lower in every round and in both orders.** Draw is 1.6–2.6 ms (8–12%) faster and Smooth 1.5–4.1 ms (6–12%). In `adjacency`, the normals stage drops by about 4.9 ms, and the overlapped preparation grows from 2.6 to 4.8–5.7 ms.
- **The machine slowed down from round 3 on**, adding about 3 ms per dab in both modes. That drift between rounds is larger than the gain, which is why the comparison uses rounds that alternate the two modes.
- **Visible latency improved by one 60 Hz frame only in rounds 1–2.** There the faster dab finished before the next frame: Draw p95 went from 33.6 to 25.5 ms, Smooth from 41.7 to 33.8 ms. In rounds 3–5 the dab still missed that frame, so p95 is unchanged. Over all 15 strokes, the median p95 is 33.9 → 33.5 ms (Draw) and 42.2 → 42.0 ms (Smooth). The stroke already finishes immediately after release in both modes.
- **21M, radius 0.125** (34K vertices per dab, 4 rounds): visible p95 is 17.1–17.2 ms in both modes. Per-dab time has an order effect: whichever mode runs second in a round is about 0.4 ms slower, and the candidate wins one round and loses the other. No regression and no measurable gain.

(Corrected in "Rates repeatability and what "visible" measures": the visible steps above are 8.33 ms phase steps, not frames, and a rerun found the per-dab gain within run-to-run variation.)

**Verdict: default unchanged (off).** The CPU saving at 84M is real and bit-identical. It is smaller than the drift between runs, and it changes visible latency only when it moves a dab across a frame boundary, which happened in 2 of 5 rounds. To make large strokes visibly more responsive, a dab needs to fall clearly under a frame: about 5 ms off Draw (21 → 16 ms), more for Smooth. The largest remaining serial pieces are the average normal (2.4 ms) and undo record (1.9 ms) loops. Both are serial to keep their summation and record order, and the overlap now hides most of the region walk behind them. The next candidates are the gather's query and selection (5.4 ms, already parallel) and the normals compute (2.9 ms, about 50 ns per vertex).

### Smooth bands: kernel-cell creases (fix, default)

Measured source: `cb4bf67` plus the uncommitted work. Both blends ran from the same build, chosen with `--smooth-kernel` / `strilinear`.

**Symptom.** On dense meshes (dense-mode dabs, 12 or more vertices per kernel cell), Smooth strokes on the sphere left broad diagonal bands with angular edges, easiest to see near the silhouette.

**Reproduction.** `SculptEngine --subdivisions <9|11> --selftest-scenario smooth --selftest <dir>` uses three fixed cameras from the plain sphere: full, close (zoom 6, panned to the upper-left silhouette) and closer. It captures each view before the strokes, after three Smooth strokes at the editor defaults (radius 0.25, strength 0.5; forward, back, forward), after a full reference recomputation of the normals, after undo and after redo. It also dumps the positions, both sets of normals and the triangles around the stroke. With the close camera and a levels stretch, the bands are plainly visible after the strokes and absent before them, so the stroke creates them; it does not amplify existing faceting.

**What it is not.** Incremental normals and the full reference recomputation agree within 0.04° and render identically (0 differing pixels in all three views at levels 6, 9 and 11), so shading and normals are not the cause. Undo restores every vertex exactly and redo restores the stroked state exactly.

**Cause.** The dense-mode filter averages the vertices of each kernel cell (cell size 0.3 × radius), then blends the averages at each vertex. The old blend was trilinear over the 2 × 2 × 2 nearest cell centres. On a curved surface a cell's average lies below the surface, by an amount that depends on how the surface cuts the cell, and a trilinear blend has a slope jump on every plane through cell centres. A single dab therefore leaves a lattice of creases: one shallow pillow per cell, with ridges exactly on the cell-centre planes (`se_bench smoothdump` single dab, high-pass of the radial displacement with the cell planes overlaid). The grid is anchored at each dab centre and dabs are 0.25 × radius apart, so a stroke stacks a shifted lattice per dab into the angular bands. The creases follow the kernel grid, not the triangle layout.

**Fix** (`SmoothBrush::setQuadraticKernel`, `--smooth-kernel quadratic`, default). The blend now uses quadratic B-spline weights over the 3 × 3 × 3 nearest cells. The weights stay continuous in value and slope where a vertex changes cell, so the blended surface has no crease. Everything else is unchanged: the count and facing weights, motion along the averaged normal, and the Laplacian pass. The grid already had the one cell of margin the wider neighbourhood needs.

**Results.**
- At level 9, the 99th percentile of the high-pass radial displacement drops 4× for one dab (7.7e-5 → 1.9e-5) and from 7.2e-5 to 4.9e-5 for three strokes. The cell lattice is gone from both maps.
- What remains are faint straight lines along the icosphere's base-triangle edges. They appear with both blends, come from the uneven triangle sizes of the subdivided icosahedron, and are not visible in the renders.
- `SmoothKernelTests`: one off-axis dense dab at level 8 gives a 99th-percentile discrete curvature of the displacement of 4.1e-5 (trilinear) and 7.3e-6 (quadratic).
- The quadratic blend reaches one cell further, so a dab smooths a little more: mean depth under the brush core 7.7e-4 → 1.0e-3 for one dab, and the three-stroke trough 0.0141 → 0.0171.
- In the matched renders at levels 9 and 11 (close view), the trilinear bands are gone.
- The self-test passes at levels 9 and 11 with both blends: reference normals render identically, undo is exact, and redo is exact.

**Cost.** `se_bench adjacency <L> 11` with the editor's flags, 2 interleaved rounds, ms per dab:

| | kernel pass, trilinear → quadratic | Smooth edit, trilinear → quadratic |
|---|---|---|
| 21M (L10) | 0.72 / 0.69 → 1.23 / 1.22 | 6.52 / 5.82 → 7.72 / 7.35 |
| 84M (L11) | 2.56 / 2.77 → 4.44 / 4.69 | 23.3 / 26.2 → 25.8 / 28.2 |

The kernel pass reads 27 cells instead of 8 and is about 1.7 times slower: +0.5 ms per dab at 21M and +1.9 ms at 84M, about 8% of a Smooth edit. Binning and the Laplacian pass are unchanged. Each mode's mesh hash is the same in both rounds.

**Adopted** as the default for correctness. `--smooth-kernel trilinear` (`strilinear` in `se_bench`) restores the old blend for comparison. A cheaper evaluation, such as per-cell normalised normals computed once per dab, is left for the performance work.

### Smooth strength across mesh resolutions (fix, default)

Measured source: `b1de3fb` plus the uncommitted work.

**Check.** `SculptEngine --subdivisions <n> --selftest-scenario smoothfeel --selftest <dir>`:
1. A broad ridge is drawn along X on the front of the sphere (Draw, radius 0.3, strength 0.5), with 7 small bumps on its crest (radius 0.08, 3 dabs each).
2. Smooth then runs along the +X half (x > 0.1; radius 0.3, strength 0.5): one pass, then four more back and forth.
3. Every stroke is a fixed world-space ray, so the path, radius and strength are the same at every level.
4. The scenario captures one close-up per stage under two grazing lights (an opt-in single-light mode of the mesh shader, `RenderSettings::inspectionLight`) and the editor's lighting. It notes the crest height every 0.05 along X, and how many Smooth dabs ran in dense mode.

**What it showed.**
- **No visible defect from Smooth at either resolution.** The bumps soften after one pass and are gone after five. The smoothed half blends into the untouched half without a step. Even with a contrast stretch, no diagonal bands appear at 81,920 or 21M triangles.
- **One pass smoothed about twice as much at 21M as at the default mesh.** Crest height at x = 0.45 (0.13 before):

| | default mesh (L6) | L7 | L8 | L9 | 21M (L10) |
|---|---|---|---|---|---|
| dense dabs, first pass | 4 of 9 | 9 of 9 | 9 of 9 | 9 of 9 | 9 of 9 |
| after 1 pass | 0.105 | 0.085 | 0.085 | 0.085 | 0.085 |

**Cause.** A Smooth dab uses the dense-mode filter only when its vertices average at least `kDenseVerticesPerCell` per kernel cell (cell size 0.3 × radius). Otherwise it runs 4 × strength Laplacian passes.
- The dense filter's reach is a fraction of the radius, so it behaves the same at L7 to L10, within ±0.001 of crest height.
- A Laplacian pass reaches only the one-ring, a few edges, so it smooths far less at the brush's scale.
- At 12 per cell the default mesh sits on the switch at the editor's radii: radius 0.3 is about 12 per cell and radius 0.25 about 8. Dabs along the stroke therefore flipped between the two modes, and the sparse ones hardly smoothed.

**Fix.** `kDenseVerticesPerCell` is now 4 (was 12). `--smooth-dense <n>` sets it for comparisons, and `SmoothBrush::setDenseThreshold` does the same in code.
- **Same stroke, default mesh:** all 9 dabs dense. The crest matches 21M within 0.002 after one pass (0.0814 vs 0.0833 at x = 0.45) and within 0.006 after five. The images match the 21M row.
- **Thresholds 8, 6, 4 and 3** give identical results on this stroke (every dab already dense).
- **Low density stays clean.** Forcing dense mode at L5, about 3 vertices per cell on this stroke, shows no lumps or cell pattern, so 4 leaves margin.
- **Bands.** The original band scenario (`--selftest-scenario smooth`) at the default mesh, now in dense mode, passes. Reference normals render identically, undo is exact and redo is exact. The high-pass change in close-view shading stays within ±2 grey levels out of 255, which is 8-bit contouring, with either kernel blend.
- **Unchanged:** 21M and denser meshes were already all dense. Dabs too sparse even for 4 per cell (a small radius on a coarse mesh) still use Laplacian passes, whose few-edge reach is then about the radius anyway.
- **Tests:** all 211 pass. `BrushTest.SmoothResultDoesNotDependOnVertexOrder` checks the Laplacian pass, so it now forces sparse mode, whose result it predicts.

**Also seen (not changed).** At 21M, with an extreme contrast stretch only, the Draw-only ridge shows faint overlapping arcs on its slopes. They are probably Draw's dab spacing, and they are not visible in the normal renders.

### Latest-wins Move updates (default)

Measured source: `f71cb46` plus the uncommitted work (source tree `ed4f5f01`; the benchmarks and self-tests ran on the same code with the default still off and the mode chosen by flag). Every other editor default, 11 helper threads.

**The change** (`AppConfig::coalesceMoveUpdates`, `--move-updates latest|all`). When the sculpt thread takes a Move update and more updates are already queued right behind it, it takes that whole run and applies only the newest update that changes anything; the others are dropped and recorded as coalesced. Rules:

- Only consecutive `UpdateStroke` commands, and only while a Move (Grab) stroke is active. Any other queued command ends the run: a stroke's end, undo, redo and the next stroke's begin are separate commands queued after it, so a run never crosses them. Settings are not commands: a stroke keeps the radius and strength it began with, so changing them mid-stroke changes nothing in either mode.
- The begin input is never dropped, and the stroke's end always comes after the last applied update.
- "Changes anything": an update the Grab rejects (ray parallel to the drag plane, or meeting it behind its origin) is a no-op. The newest *accepted* update is applied (`StrokeProcessor::grabUpdateApplies`, the same test `applyGrab` uses), so rejected updates at the end of a run cannot hide an earlier accepted one. If none is accepted, none changes anything, as before.
- Draw and Smooth are never coalesced: their dabs follow the polyline of every input and are cast against the surface already deformed by earlier dabs.

**Why the result is exact, and what else was checked.** A Grab update writes `original + drag × weight` for the vertices captured at begin, so the positions depend only on the newest accepted pointer ray. The other state each update touches, and whether it depends on the path:

| State | Depends on the path? | Checked by |
|---|---|---|
| Positions, normals (recomputed for the same edit region from the staged positions) | no | hash after every step, 4 helper counts |
| Undo record (`before` captured at begin, `after` read at the end) | no | `before` and `after` of every entry, bit for bit |
| Dirty pages (the same affected vertices are marked by every update) | no: same union | pending dirty set page by page |
| Chunk BVH boxes (refit from current positions) | no | every node box (`sameBounds`) |
| Local BVH re-partitioning (touched marks are the same triangles each update; runs only after the stroke) | no | node boxes, `needsOptimize` once idle |
| Top level: `topStale` is set when any refit changed a chunk's root box, so it can differ when the drag returns exactly to its start (intermediate refits changed the box, the last did not). If `topStale` was clear before the stroke, the top was built from boxes equal to the final ones, and rebuilding it gives the same tree | flag yes, tree no | every top node (box, child, chunk) after idle, including a stroke that ends exactly at its start |
| Incremental normal cones: a widened cone is widened by every intermediate normal set, so **while a stroke runs and before the background rebuild, cones differ** (latest-wins widens less often, so they are as tight or tighter). Widened chunks are marked stale and rebuilt exactly once the sculpt thread is idle | yes, until idle | every cone, the stale flags and picking after idle; `checkNormalCones` 6 times right after each overloaded stroke (no cone may miss a normal: 0 violations) |
| Statistics: dab count, per-input samples, stroke centre during the stroke | yes, by design | `inputSamples` marks dropped inputs; `strokeCoalescedInputs`, `coalescedInputs` |

**Tests** (`MoveCoalescingTests`, 2 tests). One script runs at 0, 1, 3 and 11 helpers with latest-wins and compares every step with every update applied. The script covers:

- an overloaded 80-update drag;
- direction reversals (out, back past the start, out again);
- a drag that ends exactly at its start;
- runs whose newest updates the Grab rejects;
- immediate release with 1 update, 0 updates, and all updates queued with the end;
- Draw and Smooth strokes between Move strokes;
- radius and strength changes mid-stroke and between strokes;
- undo ×3, redo, a new overloaded Move stroke discarding the redo, then undo all and redo all.

Overloaded strokes are queued while the test holds the document lock, so the sculpt thread is stopped at its first commit and the rest pile up. At every one of 28 steps, positions, normals, picks, boxes, cones, stale flags, top level, dirty pages, the undo and redo records and the history counts are equal. At least 100 updates are dropped per run, at least 60 in one stroke. A second test checks that queued Draw and Smooth strokes drop nothing, that a Move stroke keeps its begin input and newest update, and that the sample times stay in order. Two deliberate breaks fail the tests: applying the newest update even when the Grab rejects it, and coalescing every brush. The input-sample test now pins `coalesceMoveUpdates = false`.

**Rates benchmark, Move only** (`se_bench rates <level> 11 only=move [mlatest]`, 3 rounds per mode, interleaved all/latest/latest/all/all/latest; ranges are min–max over the rounds). Applied inputs include the begin input. "Visible (applied)" counts only inputs that were applied. "Visible (every input)" charges a dropped input until the frame showing the newer input that replaced it. "Release to visible" runs from the pointer release to the frame showing the stroke's last applied state. Queue wait is for applied inputs.

84M triangles (level 11):

| Radius | Rate | Mode | Applied / dropped | Queued max / at release | Drain | Queue wait p50 / p95 | Visible p50 / p95 / max (applied) | Visible p50 / p95 / max (every input) | Release to visible | Reader wait max |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.0625 | 60 Hz | all | 121 / 0 | 1 / 0–1 | 2 ms | 0.0 / 0.0 | 17 / 17 / 33 | 17 / 17 / 33 | 17 ms | 0.1–0.4 |
| 0.0625 | 60 Hz | latest | 121 / 0 | 1 / 0–1 | 2 ms | 0.0 / 0.0 | 17 / 17 / 33–34 | 17 / 17 / 33–34 | 16–17 ms | 0.0–0.4 |
| 0.0625 | 120 Hz | all | 241 / 0 | 1 / 0–1 | 2 ms | 0.0 / 0.0 | 16 / 17 / 17–18 | 16 / 17 / 17–18 | 17 ms | 0.3 |
| 0.0625 | 120 Hz | latest | 241 / 0 | 1 / 0 | 2 ms | 0.0 / 0.0 | 16 / 17 / 17–18 | 16 / 17 / 17–18 | 17–18 ms | 0.3–0.8 |
| 0.0625 | 240 Hz | all | 481 / 0 | 1 / 0 | 2 ms | 0.0 / 0.0 | 12 / 17 / 18 | 12 / 17 / 18 | 18 ms | 0.6–0.7 |
| 0.0625 | 240 Hz | latest | 481 / 0 | 1 / 0 | 2 ms | 0.0 / 0.0 | 12 / 17 / 18 | 12 / 17 / 18 | 18 ms | 0.5–0.6 |
| 0.125 | 60 Hz | all | 121 / 0 | 1 / 0 | 4 ms | 0.0 / 0.0 | 17 / 17 / 17–18 | 17 / 17 / 17–18 | 17 ms | 0.4–0.6 |
| 0.125 | 60 Hz | latest | 121 / 0 | 1 / 0–1 | 4 ms | 0.0 / 0.0 | 17 / 17 / 17 | 17 / 17 / 17 | 16–17 ms | 0.6–0.7 |
| 0.125 | 120 Hz | all | 241 / 0 | 1 / 0–1 | 4 ms | 0.0 / 0.0 | 16 / 17 / 17–18 | 16 / 17 / 17–18 | 17–18 ms | 0.6–2.8 |
| 0.125 | 120 Hz | latest | 241 / 0 | 1 / 0–1 | 4 ms | 0.0 / 0.0 | 16 / 17 / 17–18 | 16 / 17 / 17–18 | 17–18 ms | 0.6–2.0 |
| 0.125 | 240 Hz | all | 481 / 0 | 1 / 0 | 3–4 ms | 0.0 / 0.0–0.1 | 12 / 17 / 17–18 | 12 / 17 / 17–18 | 17–18 ms | 1.3–2.0 |
| 0.125 | 240 Hz | latest | 481 / 0 | 1 / 0–1 | 3–4 ms | 0.0 / 0.0–0.1 | 12 / 17 / 17–18 | 12 / 17 / 17–18 | 17–18 ms | 2.3–4.9 |
| 0.25 | 60 Hz | all | 121 / 0 | 1 / 0 | 14–15 ms | 0.0 / 0.0–0.1 | 17 / 17 / 19–36 | 17 / 17 / 19–36 | 16–17 ms | 2.0–3.1 |
| 0.25 | 60 Hz | latest | 121 / 0 | 1 / 0 | 14–15 ms | 0.0 / 0.0 | 17 / 17 / 34–35 | 17 / 17 / 34–35 | 16–17 ms | 3.3–4.7 |
| 0.25 | 120 Hz | all | 241 / 0 | 96–101 / 96–101 | 1333–1467 ms | 661.5–722.9 / 1254.2–1382.3 | 686–750 / 1276–1400 / 1336–1484 | 686–750 / 1276–1400 / 1336–1484 | 1333–1484 ms | 3.5–8.1 |
| 0.25 | 120 Hz | latest | 140–148 / 93–101 | 2 / 1–2 | 16–26 ms | 4.1–4.4 / 7.7–7.9 | 25 / 35–36 / 36–37 | 25 / 36–37 / 42–45 | 17–34 ms | 3.2–6.7 |
| 0.25 | 240 Hz | all | 481 / 0 | 336–341 / 336–341 | 4661–4985 ms | 2325.1–2453.8 / 4413.5–4719.2 | 2352–2479 / 4433–4734 / 4667–4989 | 2352–2479 / 4433–4734 / 4667–4989 | 4667–4985 ms | 3.6–3.7 |
| 0.25 | 240 Hz | latest | 141–146 / 335–340 | 4 / 2–4 | 14–23 ms | 1.9–2.1 / 3.9–4.0 | 21 / 32 / 34 | 26 / 38–39 / 42–46 | 17–34 ms | 3.5–4.1 |

At 21M nothing queues at any rate or radius (a grab takes at most 3.9 ms), so latest-wins drops nothing and every number matches the old mode within noise. The same holds at 84M for radii 0.0625 and 0.125 and at 60 Hz for radius 0.25. Only where the old mode fell behind does anything change:

- **120 Hz:** from 96–101 queued inputs at release and 1.33–1.47 s of drain to at most 2 queued and 16–26 ms, a 50–90× shorter drain.
- **240 Hz:** from 336–341 queued and 4.66–4.99 s to at most 4 queued and 14–23 ms.
- **Input to visible (applied):** p95 falls from 1.28–4.73 s to 32–36 ms. Counting every input, dropped ones included, it is 36–39 ms.
- **Release to visible:** 17–34 ms, at most two frames.

About 140–148 updates are applied per 2 s stroke, the rate one 13.9 ms grab per update allows. Renderer wait is unchanged: max 3.2–6.7 ms against 3.5–8.1. The final mesh hash is identical across modes, rounds and input rates.

**Draw and Smooth** (`se_bench rates 11 11` with each mode, one run each): every Draw and Smooth stroke has the same final hash and dab count in both modes, and nothing is dropped. Their code path is unchanged. The latest-wins run, made first, had dab p50 up to 15% shorter in most cases and 3% longer in one; that is run-to-run variation, not an effect of the setting. **Live bench at 500 Hz** (`se_bench live 11 11 1 … [mlatest]`): Move went from 400 grabs with the stroke done 5,030 ms after the first input (337 queued at release) to 63 grabs done after 823 ms (at most 7 queued). Draw and Smooth were unchanged: 82 and 71 dabs, done after 1.79–1.84 s, dab p50 21.1–21.5 and 25.0–26.0 ms.

**Vulkan Move stress test at 84M** (`SculptEngine --subdivisions 11 --selftest <dir> --selftest-scenario move --move-updates all|latest`). It makes three live Move strokes with one input every 2 ms (radius 0.125, 300 inputs; 0.25, 300; 0.5, 500 with 2.6M vertices per grab). Each goes out and reverses back to the midpoint while frames render. Then it undoes all and redoes all. Each stroke checks: one undo step, every input accounted for, no drops in the `all` mode, renderer wait under 50 ms, and every cone holds its normals. At the end it checks that undo restores every vertex exactly, that the undone mesh renders pixel-identical to the initial one, and that redo restores the hash and image.

| Stroke (84M) | all: queued at release, end applied after release, queue wait p50 | latest: applied / dropped, queued at release, end applied after release, queue wait p50 |
|---|---|---|
| radius 0.125, 300 inputs | 0–3, 15–18 ms, 0.0–12.0 ms | 168–301 / 0–133, 0, 10–15 ms, 0.0–0.8 ms |
| radius 0.25, 300 inputs | 21–217, 330–3,265 ms, 293–1,733 ms | 79–298 / 3–222, 0–4, 15–30 ms, 1.9–7.9 ms |
| radius 0.5, 500 inputs | 392–460, 23.9–27.5 s, 12.8–13.9 s | 40–128 / 373–554, 1–8, 80–122 ms, 8.4–18.4 ms |

Runs: `all` 3 times and `latest` 4 times. The Qt timer delivers inputs at different effective rates from run to run, which is why the ranges are wide. Renderer wait max was 0.1–4.2 ms for the two smaller strokes and 14.2–17.3 ms for the 2.6M-vertex one in both modes, which is the size of that grab's publish.

All 7 runs end with the same three mesh hashes (`43b8472d…`, `3a8c809f…`, `cb2a3eff…`). Six pass all 29 checks. In one latest run the third stroke recorded 620 inputs for 500 sent (66 applied + 554 dropped), so its "every input accounted for" check failed. The viewport turns real pointer events into stroke updates as well, and that run's hashes and all its other checks were correct. The three reruns (latest, all, latest) counted exactly, so stray pointer input during that run is the likely cause, but it was not proven. The standard self-test at 84M with `--move-updates latest` passes all 237 checks.

**Adopted** as the default (`--move-updates all` applies every update again). All 207 tests pass.

### Responsiveness checkpoint at 21M and 84M (measurement only)

Measured source: `f71cb46` plus the uncommitted work up to parallel Smooth binning and the input-sample instrumentation below (source tree `4019a5f0`); every editor default, 11 helper threads. Nothing in the sculpt path changed for this checkpoint: no inputs are dropped or coalesced, stroke spacing and brush semantics are unchanged.

**Method.** `se_bench rates <level> <helpers>` builds an `AppController` with the editor's `AppConfig` defaults and makes one 2 s stroke for each brush (Draw, Smooth, Move), radius (0.0625, 0.125, 0.25) and input rate (60, 120, 240 Hz), undoing each stroke afterwards. Draw and Smooth move along the equator at 1.5 units per second (a brisk, realistic stroke), so the input rate changes how many inputs describe the same path, not the path; Move drags 0.1 units per second. A reader thread takes the document lock every 16.67 ms (a 60 Hz renderer) and records when. The controller now stamps every stroke input with three times (queued, started, finished on the sculpt thread) and the dabs it applied (`InteractionStats::inputSamples`), so:

- **queue wait** = started − queued: time spent behind earlier inputs;
- **processing** = finished − started: the raycast, interpolation and every dab the input produced (shown for inputs that applied at least one dab; inputs that applied none take 3–10 µs);
- **input to visible** = the first reader frame at or after the input finished − queued. Its floor is the 16.7 ms frame interval (p50 below 17 means some inputs landed just before a frame);
- **drain** = the last input finished − pointer release; **queued** = commands waiting when inputs were sent and at release;
- **reader wait** = the renderer's wait for the document lock.

Two rounds each; where they differ, both values are shown separated by `;`.

**How inputs become dabs.** Every queued `UpdateStroke` is processed in order on the sculpt thread. Its ray is cast against the surface, then:

- **Draw and Smooth** place one dab per `spacing = radius × 0.25` of surface distance since the last dab (at most 256 per input), each on a ray interpolated between the previous and the new input and cast again against the already deformed surface. The dab rate therefore follows pointer speed ÷ spacing (96 dabs/s at radius 0.0625, 48 at 0.125, 24 at 0.25 at 1.5 units/s), whatever the input rate: at 240 Hz most inputs apply no dab, at 60 Hz small brushes apply 1–2 per input.
- **Move** applies the grab once per input (none at the begin input): every input re-applies the whole displacement since the start of the stroke to the positions captured at begin. Its cost scales with the input rate, and only the newest queued input determines the result.

There is no pressure in the input path today: commands carry only a ray.

**84M triangles (level 11), 11 helpers:**

| Brush, radius | Input rate | Dabs/s (per input) | Queued max / at release | Drain after release | Queue wait p50 / p95 | Processing p50 / p95 (inputs with dabs) | Input to visible p50 / p95 / max | Dab p50 | Reader wait max |
|---|---|---|---|---|---|---|---|---|---|
| Draw 0.0625 | 60 Hz | 96 (1.61) | 1 / 1; 1 / 0 | 3 ms; 2 ms | 0.0 / 0.0 | 4.2 / 5.0; 4.2 / 4.9 | 17 / 17 / 33; 17 / 17 / 34 | 2.2 | 0.7; 0.8 |
| Draw 0.0625 | 120 Hz | 98 (0.82) | 1 / 0 | 2 ms; 3 ms | 0.0 / 0.0 | 2.4 / 2.8; 2.4 / 2.7 | 17 / 17 / 18; 17 / 17 / 17 | 2.4 | 1.9; 3.8 |
| Draw 0.0625 | 240 Hz | 104 (0.43) | 1 / 0 | 2 ms; 3 ms | 0.0 / 0.0 | 2.4 / 2.8; 2.4 / 2.7 | 13 / 17 / 21; 13 / 17 / 18 | 2.4 | 0.7 |
| Draw 0.125 | 60 Hz | 49 (0.82) | 1 / 0 | 6 ms; 5 ms | 0.0 / 0.0 | 5.4 / 6.8; 5.5 / 6.6 | 17 / 17 / 17 | 5.4; 5.5 | 0.9; 0.8 |
| Draw 0.125 | 120 Hz | 52 (0.44) | 1 / 0 | 6 ms | 0.0 / 0.0 | 5.4 / 6.1; 5.4 / 6.2 | 9 / 17 / 17 | 5.4 | 0.8; 0.9 |
| Draw 0.125 | 240 Hz | 58 (0.24) | 2 / 0 | 0 ms | 0.0 / 1.6; 0.0 / 1.4 | 5.5 / 6.0; 5.4 / 6.0 | 13 / 21 / 26; 13 / 21 / 25 | 5.4 | 1.0; 0.8 |
| Draw 0.25 | 60 Hz | 26 (0.44) | 1 / 0 | 0 ms | 0.0 / 7.0; 0.0 / 6.9 | 21.5 / 25.1; 21.6 / 24.6 | 34 / 34 / 34; 33 / 34 / 34 | 21.5; 21.6 | 3.3 |
| Draw 0.25 | 120 Hz | 30 (0.25) | 3 / 0 | 0 ms | 0.0 / 13.7; 0.1 / 13.7 | 21.2 / 23.1; 21.1 / 22.9 | 25 / 34 / 34 | 21.2; 21.1 | 3.2; 7.2 |
| Draw 0.25 | 240 Hz | 34 (0.14) | 6 / 0; 7 / 1 | 0 ms | 4.1 / 17.2; 3.8 / 17.0 | 20.9 / 22.6; 20.6 / 22.2 | 26 / 34 / 38 | 20.9; 20.6 | 3.2 |
| Smooth 0.0625 | 60 Hz | 96 (1.60) | 1 / 1; 1 / 0 | 3 ms | 0.0 / 0.0 | 6.5 / 7.7; 5.9 / 6.8 | 17 / 17 / 17 | 3.5; 3.2 | 0.8; 2.5 |
| Smooth 0.0625 | 120 Hz | 96 (0.80) | 1 / 1; 1 / 0 | 0 ms | 0.0 / 0.0 | 3.3 / 3.8; 3.2 / 3.7 | 16 / 17 / 17; 17 / 17 / 17 | 3.3; 3.2 | 4.5; 0.8 |
| Smooth 0.0625 | 240 Hz | 96 (0.40) | 1 / 0; 1 / 1 | 0 ms | 0.0 / 0.0 | 3.4 / 3.9; 3.2 / 3.7 | 12 / 17 / 17 | 3.4; 3.2 | 0.8; 0.7 |
| Smooth 0.125 | 60 Hz | 48 (0.80) | 1 / 0 | 0 ms | 0.0 / 0.0 | 7.8 / 8.5; 7.6 / 8.6 | 17 / 17 / 17 | 7.8; 7.6 | 0.7; 8.6 |
| Smooth 0.125 | 120 Hz | 48 (0.40) | 1 / 1 | 0 ms | 0.0 / 0.1; 0.0 / 0.0 | 7.8 / 8.6; 7.6 / 8.6 | 16 / 17 / 25; 17 / 17 / 25 | 7.8; 7.6 | 0.8; 0.9 |
| Smooth 0.125 | 240 Hz | 48 (0.20) | 2 / 0 | 0 ms | 0.0 / 3.9; 0.0 / 3.7 | 7.8 / 8.5; 7.6 / 8.4 | 17 / 21 / 25 | 7.8; 7.6 | 0.9 |
| Smooth 0.25 | 60 Hz | 24 (0.40) | 2 / 0 | 0 ms | 0.0 / 15.4; 0.0 / 15.2 | 29.6 / 32.9; 29.6 / 33.6 | 33 / 34 / 36; 34 / 34 / 50 | 29.5; 29.6 | 3.0 |
| Smooth 0.25 | 120 Hz | 24 (0.20) | 4 / 0 | 0 ms | 4.0 / 21.9; 4.6 / 22.0 | 29.2 / 31.9; 29.8 / 32.6 | 34 / 42 / 42; 34 / 42 / 50 | 29.2; 29.7 | 3.4 |
| Smooth 0.25 | 240 Hz | 24 (0.10) | 9 / 1; 8 / 0 | 32 ms; 30 ms | 6.7 / 25.8; 5.9 / 25.2 | 30.2 / 33.5; 29.7 / 32.5 | 38 / 46 / 54; 37 / 42 / 46 | 30.2; 29.7 | 5.7; 3.3 |
| Move 0.0625 | 60 Hz | 60 (1.00) | 1 / 1; 1 / 0 | 2 ms | 0.0 / 0.0 | 2.0 / 2.3; 1.9 / 2.1 | 17 / 17 / 17; 17 / 17 / 18 | 2.0; 1.9 | 0.3; 1.2 |
| Move 0.0625 | 120 Hz | 120 (1.00) | 1 / 0 | 2 ms | 0.0 / 0.0 | 1.8 / 2.1; 1.8 / 2.0 | 16 / 17 / 17 | 1.8 | 0.3; 0.2 |
| Move 0.0625 | 240 Hz | 240 (1.00) | 1 / 1; 1 / 0 | 2 ms | 0.0 / 0.0 | 1.8 / 2.0; 1.7 / 2.0 | 12 / 17 / 17; 12 / 17 / 18 | 1.8; 1.7 | 0.2; 0.6 |
| Move 0.125 | 60 Hz | 60 (1.00) | 1 / 1; 1 / 0 | 4 ms | 0.0 / 0.0 | 3.6 / 4.0 | 17 / 17 / 17 | 3.6 | 4.1; 0.6 |
| Move 0.125 | 120 Hz | 120 (1.00) | 1 / 0 | 4 ms; 3 ms | 0.0 / 0.0 | 3.6 / 4.0; 3.6 / 3.9 | 16 / 17 / 17 | 3.6 | 3.9; 0.4 |
| Move 0.125 | 240 Hz | 240 (1.00) | 1 / 0 | 3 ms | 0.0 / 0.0 | 3.4 / 3.7; 3.3 / 3.6 | 12 / 17 / 18 | 3.4; 3.3 | 2.2; 1.8 |
| Move 0.25 | 60 Hz | 60 (1.00) | 1 / 0 | 14 ms | 0.0 / 0.0 | 13.9 / 14.4; 14.0 / 14.3 | 17 / 17 / 17 | 13.9; 14.0 | 1.7; 4.2 |
| Move 0.25 | 120 Hz | 72 (1.00) | 97 / 97 | 1352 ms | 671.6 / 1271.5; 672.1 / 1269.8 | 13.9 / 14.3 | 694 / 1292 / 1367; 694 / 1287 / 1361 | 13.9 | 3.3; 7.4 |
| Move 0.25 | 240 Hz | 72 (1.00) | 337 / 337 | 4677 ms; 4678 ms | 2336.1 / 4429.8; 2335.2 / 4429.9 | 13.8 / 14.2; 13.8 / 14.3 | 2362 / 4450 / 4684 | 13.8 | 3.5; 3.6 |

**21M triangles (level 10), 11 helpers:**

| Brush, radius | Input rate | Dabs/s (per input) | Queued max / at release | Drain after release | Queue wait p50 / p95 | Processing p50 / p95 (inputs with dabs) | Input to visible p50 / p95 / max | Dab p50 | Reader wait max |
|---|---|---|---|---|---|---|---|---|---|
| Draw 0.0625 | 60 Hz | 96 (1.61) | 1 / 0 | 1 ms | 0.0 / 0.0 | 2.4 / 2.9; 2.2 / 2.9 | 17 / 17 / 17 | 1.3; 1.2 | 0.7; 0.8 |
| Draw 0.0625 | 120 Hz | 98 (0.82) | 1 / 0 | 2 ms; 1 ms | 0.0 / 0.0 | 1.4 / 1.6; 1.3 / 1.5 | 17 / 17 / 17 | 1.4; 1.3 | 0.7; 0.6 |
| Draw 0.0625 | 240 Hz | 104 (0.43) | 1 / 0 | 2 ms; 1 ms | 0.0 / 0.0 | 1.4 / 1.6; 1.3 / 1.5 | 13 / 17 / 17 | 1.4; 1.3 | 0.6; 0.7 |
| Draw 0.125 | 60 Hz | 49 (0.82) | 1 / 1; 1 / 0 | 3 ms; 2 ms | 0.0 / 0.0 | 3.0 / 3.4; 2.5 / 2.9 | 17 / 17 / 18; 17 / 17 / 17 | 2.9; 2.5 | 0.7; 0.6 |
| Draw 0.125 | 120 Hz | 52 (0.44) | 1 / 0 | 3 ms | 0.0 / 0.0 | 2.9 / 3.2; 2.5 / 3.0 | 9 / 17 / 17 | 2.9; 2.5 | 0.7; 2.0 |
| Draw 0.125 | 240 Hz | 58 (0.24) | 1 / 1; 1 / 0 | 0 ms | 0.0 / 0.0 | 2.9 / 3.2; 2.5 / 2.8 | 12 / 17 / 17; 13 / 17 / 17 | 2.9; 2.5 | 0.8; 0.7 |
| Draw 0.25 | 60 Hz | 26 (0.44) | 1 / 0 | 0 ms | 0.0 / 0.0 | 6.1 / 7.2; 5.6 / 6.9 | 17 / 17 / 17 | 6.0; 5.6 | 0.7 |
| Draw 0.25 | 120 Hz | 30 (0.25) | 1 / 1; 1 / 0 | 0 ms | 0.0 / 0.0 | 6.0 / 6.4; 5.4 / 6.2 | 9 / 17 / 17 | 6.0; 5.4 | 0.8; 0.7 |
| Draw 0.25 | 240 Hz | 34 (0.14) | 1 / 1 | 0 ms | 0.0 / 1.9; 0.0 / 1.4 | 6.0 / 6.9; 5.4 / 6.0 | 17 / 21 / 21; 13 / 21 / 22 | 6.0; 5.4 | 0.7; 1.4 |
| Smooth 0.0625 | 60 Hz | 96 (1.60) | 1 / 0 | 2 ms | 0.0 / 0.0 | 3.2 / 3.9; 2.9 / 3.7 | 17 / 17 / 18 | 1.8; 1.6 | 0.7 |
| Smooth 0.0625 | 120 Hz | 96 (0.80) | 1 / 0 | 0 ms | 0.0 / 0.0 | 1.8 / 2.1; 1.6 / 2.0 | 16 / 17 / 17 | 1.8; 1.6 | 0.8; 0.7 |
| Smooth 0.0625 | 240 Hz | 96 (0.40) | 1 / 1 | 0 ms | 0.0 / 0.0 | 1.9 / 2.1; 1.7 / 2.0 | 12 / 17 / 17 | 1.9; 1.7 | 0.7; 0.6 |
| Smooth 0.125 | 60 Hz | 48 (0.80) | 1 / 1 | 0 ms | 0.0 / 0.0 | 3.7 / 4.0; 3.4 / 3.8 | 17 / 17 / 17 | 3.7; 3.3 | 0.8; 0.7 |
| Smooth 0.125 | 120 Hz | 48 (0.40) | 1 / 1; 1 / 0 | 0 ms | 0.0 / 0.0 | 3.7 / 4.1; 3.3 / 3.8 | 16 / 17 / 17 | 3.7; 3.3 | 0.8 |
| Smooth 0.125 | 240 Hz | 48 (0.20) | 1 / 1 | 0 ms | 0.0 / 0.0 | 3.8 / 4.2; 3.4 / 3.8 | 12 / 17 / 17 | 3.8; 3.4 | 0.8; 0.6 |
| Smooth 0.25 | 60 Hz | 24 (0.40) | 1 / 0; 1 / 1 | 0 ms | 0.0 / 0.0 | 8.7 / 9.3; 7.8 / 9.3 | 17 / 17 / 17 | 8.7; 7.8 | 0.7 |
| Smooth 0.25 | 120 Hz | 24 (0.20) | 1 / 0 | 0 ms | 0.0 / 0.6; 0.0 / 0.1 | 8.5 / 9.8; 7.8 / 8.8 | 17 / 17 / 25; 17 / 17 / 17 | 8.5; 7.8 | 1.2; 0.8 |
| Smooth 0.25 | 240 Hz | 24 (0.10) | 2 / 0; 2 / 1 | 8 ms | 0.0 / 4.3; 0.0 / 3.5 | 8.4 / 9.5; 7.7 / 8.7 | 13 / 21 / 21; 13 / 21 / 22 | 8.4; 7.7 | 0.7; 0.8 |
| Move 0.0625 | 60 Hz | 60 (1.00) | 1 / 1; 1 / 0 | 1 ms | 0.0 / 0.0 | 0.9 / 1.1; 0.9 / 1.2 | 17 / 17 / 17 | 0.9 | 0.0; 0.2 |
| Move 0.0625 | 120 Hz | 120 (1.00) | 1 / 0; 1 / 1 | 1 ms | 0.0 / 0.0 | 0.9 / 1.1; 0.8 / 1.1 | 9 / 17 / 17 | 0.9; 0.8 | 0.2 |
| Move 0.0625 | 240 Hz | 240 (1.00) | 1 / 0 | 1 ms | 0.0 / 0.0 | 0.9 / 1.1; 0.8 / 1.0 | 9 / 17 / 18; 9 / 17 / 17 | 0.9; 0.8 | 0.2 |
| Move 0.125 | 60 Hz | 60 (1.00) | 1 / 1 | 2 ms | 0.0 / 0.0 | 2.0 / 2.2; 1.8 / 2.1 | 17 / 17 / 17 | 2.0; 1.8 | 0.5; 0.6 |
| Move 0.125 | 120 Hz | 120 (1.00) | 1 / 1; 1 / 0 | 2 ms | 0.0 / 0.0 | 2.0 / 2.2; 1.8 / 2.0 | 16 / 17 / 18; 16 / 17 / 17 | 2.0; 1.8 | 0.6; 0.5 |
| Move 0.125 | 240 Hz | 240 (1.00) | 1 / 1; 1 / 0 | 2 ms | 0.0 / 0.0 | 2.0 / 2.2; 1.8 / 2.0 | 12 / 17 / 17 | 2.0; 1.8 | 0.3 |
| Move 0.25 | 60 Hz | 60 (1.00) | 1 / 0 | 4 ms | 0.0 / 0.0 | 3.9 / 4.1; 3.6 / 3.9 | 17 / 17 / 17 | 3.9; 3.6 | 0.8; 0.7 |
| Move 0.25 | 120 Hz | 120 (1.00) | 1 / 0 | 4 ms | 0.0 / 0.0 | 3.9 / 4.1; 3.6 / 3.8 | 16 / 17 / 17 | 3.9; 3.6 | 0.7; 0.6 |
| Move 0.25 | 240 Hz | 240 (1.00) | 1 / 1; 1 / 0 | 4 ms; 3 ms | 0.0 / 0.1; 0.0 / 0.0 | 3.7 / 3.9; 3.3 / 3.5 | 12 / 17 / 17; 12 / 17 / 18 | 3.7; 3.3 | 0.7; 0.5 |

**Full dab at 84M with every default** (`se_bench adjacency 11 11 pgather pmark dense icones creuse pregion pprep`, radius 0.25, per dab):

| Brush | Dab | Gather | Normals | BVH refit + cones | Publish | Brush |
|---|---|---|---|---|---|---|
| Draw | 21.5 ms | 8.55 (chunk query 2.41, vertex selection 3.60, overlapped record + normal + staging 2.54 wall) | 7.90 (edit region 4.55, compute 2.99, dirty 0.36) | 3.62 | 1.01 | 0.40 |
| Smooth | 25.4 | 7.63 | 6.60 | 3.36 | 0.90 | 6.87 (binning 2.06, kernel pass 3.01, Laplacian 1.30) |
| Move | 12.9 | — (vertices captured at begin) | 6.85 (edit region 3.78) | 3.44 | ~2.0 | — |

At radius 0.125 an 84M Draw dab is 4.94 ms, at 0.0625 1.93 ms; a 21M Draw dab at 0.25 is 4.71 ms. The cost follows the dab's vertex count. Inside the controller (the tables' "Dab p50") a Smooth dab is a little slower than in the bench (29.6 ms) because the renderer's frames and the background cone rebuild run alongside.

**Findings.**

- **Draw and Smooth keep up at every rate and footprint.** No backlog builds: at most 9 queued commands, drain ≤ 32 ms. Small and medium brushes and every 21M case are visible within the next frame (p95 17–21 ms). The one visible lag is the large 84M brush: Draw p50/p95 25–34 / 34 ms, Smooth 33–38 / 34–46 ms, max 54 ms, of which up to 26 ms (p95) is queue wait behind the previous dab and ~30 ms the dab itself. This is per-dab geometry cost × dab rate (24–34 dabs/s × 21–30 ms ≈ 60–90% of the sculpt thread): the input rate barely matters, and inputs that apply no dab cost microseconds.
- **Move at 84M, radius 0.25, falls behind above ~70 Hz.** Each input costs 13.9 ms, so the sculpt thread completes at most ~72 per second. At 120 Hz 97 inputs are queued at release, the stroke finishes 1.35 s after release and the median input is visible after 0.69 s; at 240 Hz 337 inputs, 4.7 s and 2.4 s. All of that is queue wait (processing p95 stays 14.3 ms), and all of it recomputes results that the next input overwrites.
- **Renderer wait** stays ≤ 3.5 ms in most strokes (outliers 4–8.6 ms); the lock is held only to publish.
- **Where the 218 queued Smooth inputs came from.** `se_bench live` sends an input every 2 ms (500 Hz) while turning 0.01 rad per input, about 5 units per second, 3.3 times the realistic speed above. At radius 0.25 that is about 5.6 inputs per dab: the 229 inputs queued at release correspond to about 41 remaining dabs × 25.9 ms ≈ 1.06 s, the measured drain (1052 ms). The backlog is dab work owed for distance already travelled, not redundant input work. Draw in the same bench: 223 queued, 1020 ms; Move: 338 queued, 4.35 s (one full application per input).

**What limits responsiveness.**

| Candidate limit | Evidence | Verdict |
|---|---|---|
| Geometry processing per dab | Draw/Smooth visible latency at 84M r=0.25 = queue wait behind one ~21–30 ms dab + that dab; small dabs are frame-bound | the limit for Draw and Smooth |
| Number of interpolated dabs | dabs/s = speed ÷ spacing, independent of input rate; fixed by the spacing contract | not adjustable here; it multiplies the geometry cost |
| Redundant queued input work | inputs without a dab cost 3–10 µs; Move re-applies the whole grab per input and only the newest counts | the limit for Move (backlog of 97–337 inputs, 1.4–4.7 s drain) |

**Recommendation.**

1. **Next: latest-wins Move updates, as a separate experiment** (`AppConfig` option, off until measured). When the sculpt thread pops a Move `UpdateStroke` and the following queued commands are Move updates of the same stroke, apply only the newest of them. Expected effect: Move at 84M r=0.25 visible within one grab (~14 ms) plus a frame at any input rate, queue ≤ 1, drain ≤ 14 ms instead of 1.35–4.7 s; nothing changes when the sculpt thread keeps up. How exactness is kept:
   - *Stroke shape:* `applyGrab` is path-independent: it applies the total displacement from the begin ray to the latest ray to the positions captured at begin. Skipping intermediate updates skips only intermediate frames; the final positions and normals are bit-identical.
   - *Ordering and boundaries:* never look past a command that is not a Move update of the same stroke (begin, end, undo, redo, brush or radius change, another stroke); the begin input and the end are always processed.
   - *Undo/redo:* the history entry stores the captured originals and the final positions, which do not depend on how many updates were applied; check the undo delta, hash, BVH boxes and cones bit for bit against the uncoalesced run, then undo/redo.
   - *Pressure:* inputs carry none today. If pressure is added to Move it must be stored with each input and the newest input's pressure applied to the originals, which keeps latest-wins exact; a pressure that accumulates along the path would rule it out.
   - *Draw and Smooth stay uncoalesced:* their dab positions depend on the polyline of every input and on rays recast against the deformed surface, so merging inputs would change the stroke.
   - *Measure* with `se_bench rates` (Move rows), the `live` bench and the Vulkan self-test; count skipped inputs in `InteractionStats`.
2. **Then the large-footprint geometry cost for Draw and Smooth at 84M:** the normals stage (edit region 4.55 ms + compute 2.99 ms per Draw dab, 6.6 ms for Smooth) and the gather's vertex selection (3.6 ms) are the largest repeatable costs left; each millisecond saved there removes about 1–1.3 ms of p95 visible latency at 24–34 dabs/s, and the Smooth brush (6.9 ms) follows.

### Smooth profile at 84M and parallel kernel-grid binning (default)

**Profile** (`se_bench adjacency … pgather pmark dense icones creuse pregion pprep`, editor defaults, radius 0.25, strength 0.5, per dab, two runs). Every Smooth dab in these strokes is dense (12 or more vertices per occupied kernel cell), so each runs the kernel pass and one Laplacian pass:

| Smooth brush stage | 84M, 11 helpers | 84M, 0 helpers | 21M, 11 helpers | 84M radius 0.125, 11 helpers |
|---|---|---|---|---|
| brush total | 8.34 / 8.52 ms | 37.8 / 38.5 | 2.39 / 1.80 | 2.49 / 2.42 |
| kernel grid: binning (serial) | 3.81 / 3.91 | 4.13 / 4.14 | 0.88 / 0.71 | 0.91 / 0.86 |
| kernel pass + commit (parallel) | 3.05 + 0.12 | 23.0 + 0.85 | 0.87 + 0.12 | 0.95 + 0.13 |
| Laplacian pass + commit (parallel) | 1.19 + 0.11 | 9.1 + 0.6 | 0.39 + 0.11 | 0.39 + 0.11 |
| grid clear, occupied-cell count | < 0.01 | < 0.01 | < 0.01 | < 0.01 |

The binning (each vertex's position, normal and count added to its kernel cell) is the largest stage at 84M with helpers. It is also the only one that helpers do not shorten: it takes 4.1 ms with or without them. As with the gather, the 21M and equal-footprint rows match each other: the cost follows the dab's vertex count, not the mesh size.

**The change** (`SmoothBrush::setParallelBinning`, `--smooth-binning parallel`, default). With helper threads and at least 32K dab vertices, binning takes two parallel passes:
1. Per block of 16K vertices, each vertex's cell and position are stored, and the block's cell histogram is counted.
2. The cells are cut into contiguous ranges of about equal vertex count, one task per range. A task walks the blocks in list order, skips blocks with no vertex in its range, and adds its own cells' vertices in list order.

Every cell therefore sums the same positions, normals and counts in the same order as the serial loop, so the brush result is bit-identical. Neighbour order, the kernel and Laplacian passes, strength, falloff and stroke spacing are untouched. Smaller dabs and runs without helpers keep the serial loop.

**Tests.** `SmoothBinningTests`:
- **Direct brush test.** One Smooth dab over a dense cap of 59K vertices, its list in ascending, shuffled and reversed order, at 1, 3 and 11 helpers, against serial binning without helpers and with the same helpers. A cap below the threshold stays serial.
- **Strokes.** Draw for relief, then Smooth strokes of 4 radii and strengths, at 0, 1, 3 and 11 helpers: every dab's hash and the undo deltas bit for bit.
- **Mutation.** Walking the blocks in reverse order in pass 2 fails both tests. The existing threaded-Smooth test (`ParallelReference.SmoothDenseModeMatchesSingleThreaded`) does not catch it, because its dabs are below the threshold.

204 / 204 tests pass.

**Interleaved runs**, serial → parallel binning, same binary (`sbinser` selects the serial loop), fresh processes, 2 rounds (round 1 / round 2, the order swapped in round 2), all with overlapped dab preparation.

Dabs (`adjacency`, ms per dab, 11 helpers):

| | Smooth: dab / brush / binning | Draw dab | Move dab |
|---|---|---|---|
| 84M | 26.03 / 26.52 → 25.45 / 24.07; 8.34 / 8.52 → 6.98 / 6.12; 3.81 / 3.91 → 2.13 / 1.66 | 20.74 / 20.92 → 21.35 / 20.91 | 12.25 / 12.42 → 12.73 / 12.41 |
| 84M, radius 0.125 | 7.55 / 7.58 → 6.06 / 7.41; 2.49 / 2.42 → 1.78 / 2.19; 0.91 / 0.86 → 0.62 / 0.67 | 5.59 / 5.72 → 4.69 / 5.75 | 3.39 / 3.50 → 2.79 / 3.51 |
| 21M | 7.50 / 5.70 → 7.14 / 5.54; 2.39 / 1.80 → 2.04 / 1.55; 0.88 / 0.71 → 0.61 / 0.49 | 5.73 / 4.46 → 5.96 / 4.49 | 3.50 / 2.60 → 3.50 / 2.55 |
| 84M, radius 0.03 (below the threshold) | 1.04 / 1.24 → 1.01 / 1.23; brush 0.28 / 0.40 → 0.27 / 0.41 | 0.81 / 0.89 → 0.82 / 0.88 | 0.44 / 0.53 → 0.46 / 0.54 |
| 21M, radius 0.03 | 0.33 / 0.39 → 0.33 / 0.46 | 0.23 / 0.27 → 0.23 / 0.36 | 0.13 / 0.18 → 0.13 / 0.21 |

- **Without helpers** both variants run the serial loop: 84M Smooth 121.7 / 125.9 → 119.2 / 118.7 ms, 21M 23.4 / 23.2 → 23.2 / 24.3 ms.
- **Binning stays at 1.7–2.1 ms** at 84M, half its serial time rather than a twelfth. How it splits between the two passes was not measured.

Live strokes (`live`, 400 inputs every 2 ms, 11 helpers; dab latency p50 / p95 / max ms, inputs queued max / at release, time to drain after the last input):

| | Smooth | Draw | Move |
|---|---|---|---|
| 84M | 27.6 / 31.4 / 37.2, 236 / 235, 1174 ms → 24.8 / 28.1 / 33.4, 218 / 218, 984 ms (r2: 28.8 / 33.5 / 37.9, 244 / 240, 1251 → 24.8 / 27.9 / 33.2, 218 / 218, 961) | 21.4 / 23.6 / 51.0, 223, 1006 → 20.9 / 23.7 / 48.9, 219, 979 (r2: 21.9 / 24.4 / 50.4, 228, 1064 → 21.1 / 23.6 / 49.4, 220, 984) | 12.7 / 13.1 / 13.8, 338, 4326 → 12.4 / 12.8 / 13.6, 336, 4198 (r2: 13.0 / 13.4 / 14.1, 339, 4452 → 12.5 / 12.9 / 14.0, 337, 4239) |
| 84M, radius 0.125 | 7.17 / 7.88 / 12.0, 62 / 62, 162 → 6.21 / 6.83 / 10.7, 15 / 14, 38 (r2: 6.94 / 7.75 / 11.2, 53 / 53, 137 → 6.81 / 7.40 / 12.3, 46 / 44, 114) | 5.19 / 5.77 / 25.5 → 5.33 / 5.91 / 29.3 (r2: 5.08 / 5.58 / 25.7 → 5.17 / 5.67 / 26.2) | 3.11 / 3.42 / 3.79 → 3.27 / 4.09 / 4.76 (r2: 3.12 / 3.43 / 3.93 → 3.14 / 3.43 / 3.81) |
| 21M | 7.79 / 8.72 / 10.6, 5 / 4, 24 → 7.62 / 8.61 / 9.81, 5 / 4, 8 (r2: 6.01 / 7.07 / 9.35, 4 / 1, 19 → 5.82 / 6.44 / 8.54, 4 / 1, 26) | 5.86 / 6.58 / 14.9 → 5.74 / 6.49 / 14.6 (r2: 4.48 / 5.03 / 12.6 → 4.63 / 5.45 / 12.4) | 3.45 / 3.71 / 4.08 → 3.37 / 3.62 / 4.03 (r2: 2.54 / 2.73 / 3.00 → 2.53 / 2.77 / 3.29) |
| 84M, radius 0.03 | 1.07 / 1.22 / 2.33 → 1.07 / 1.21 / 2.42 (r2: 1.31 / 1.77 / 2.67 → 1.61 / 1.87 / 2.95, 29 queued) | 0.86 / 0.99 / 3.39 → 0.85 / 0.97 / 3.37 (r2: 0.96 / 1.27 / 3.93 → 1.18 / 1.39 / 3.95) | 0.47 / 0.68 / 0.91 → 0.46 / 0.62 / 0.88 (r2: 0.55 / 0.77 / 1.06 → 0.77 / 1.02 / 1.14) |

- **Lock hold and renderer wait are unchanged:** the brush runs before the lock. 84M Smooth lock hold p50 3.16 / 3.22 → 3.20 / 3.23 ms; reader wait p95 / max 1.96 / 3.49 and 1.69 / 3.43 → 1.94 / 3.10 and 2.29 / 3.71 ms.
- **Below the threshold** nothing runs differently. In the second 84M radius-0.03 round, all three brushes of the parallel-binning process were slower, Draw and Move included. That points to that process, not the change.
- **Without helpers** live latency and backlog are unchanged (84M Smooth p50 125.1 / 122.5 → 125.2 / 121.4 ms, 365 queued).

**Same results.** Every stroke hash (Draw, Smooth and Move; 21M and 84M; radii 0.25, 0.125 and 0.03; 0 and 11 helpers) is identical between the variants.

**Memory.**
- **Brush scratch (`DabScratch`).** The per-vertex cell index (4 bytes per dab vertex), the block histograms and the earlier sizing of the point array add 4.5 MiB at 84M (13.13 → 17.64 MiB) and 1.1 MiB at 21M (3.30 → 4.43). It is not counted in the edit scratch.
- **Edit scratch and peak commit** are unchanged (234.2 MiB and 6.94 GiB at 84M, 59.1 MiB and 1.73 GiB at 21M).

**84M Vulkan standard self-test**, serial, parallel, parallel, serial, back to back: all PASS, 237 checks each, none failed.
- **Smooth stroke:** worst input latency 34.9 / 33.4 → 32.5 / 31.3 ms.
- **All brushes together:** dab latency median 16.7 / 17.0 → 16.9 / 17.2 ms, p95 22.2 / 22.4 → 22.1 / 22.8 ms, max 30.0 / 25.0 → 27.1 / 27.4 ms. The self-test sculpts mostly with Draw, so this changes within noise.
- **Unchanged:** lock hold median 2.73–2.84 ms in all four; renderer lock wait max ≤ 2.9 ms; edit scratch 437.5 MiB; peak commit 11.94 GiB.

**Adopted** as the default (`--smooth-binning serial` restores the serial loop).
- **84M, 11 helpers:** the Smooth brush is 1.4–2.4 ms shorter in both rounds. Live p50 drops by 2.8–4.0 ms, 18–26 fewer inputs are queued, and the drain after release is 190–290 ms shorter.
- **Smaller footprints** (21M, 84M at radius 0.125): the brush is 0.2–0.7 ms shorter.
- **Unchanged:** results bit for bit; Draw, Move, runs without helpers and dabs below the threshold stay within baseline variation.
- **Cost:** 4.5 MiB more brush scratch at 84M.

**Next.** The kernel pass (3.05 ms, already parallel) is now the largest Smooth stage, ahead of binning (1.7–2.1 ms) and the Laplacian pass (1.2 ms).

### Gather profile at 84M and overlapped dab preparation (default)

**Profile** (`se_bench adjacency … pgather pmark dense icones creuse pregion`, editor defaults, radius 0.25, 84M Draw, 11 helpers, per edit, two runs). The gather is 8.3–8.4 ms:

| Part | ms | Shrinks with helpers? |
|---|---|---|
| chunk query (`querySphereChunks` + `querySphere` per chunk) | 2.0–2.1 | yes (parallel above 8 chunks) |
| vertex selection | 3.3 = first corners 1.0 + distance test and falloff 1.75 + join 0.6 | yes |
| undo record | 1.35 | no: serial, also 1.38 ms with 0 helpers |
| average normal | 1.7 | no: serial, also 1.78 ms with 0 helpers |

Page staging (1.0–1.1 ms, counted in `stage`, not in the gather) runs right after the undo record. Record, normal and staging are the three serial steps between the selection and the brush: 4.2 ms of wall time per dab, the largest cost that more helpers do not reduce. At 84M with radius 0.125 (the same vertex footprint as 21M at 0.25) and at 21M the parts are a quarter of that (gather 2.0–2.2 ms: query 0.5, selection 0.9, record 0.3, normal 0.3).

**The change** (`StrokeProcessor::setOverlapPrepare`, `--dab-prepare overlap`, default). The three steps read the gathered lists and the mesh, which nothing changes before the commit, and each writes only its own state (the history entry, the staging copies, the brush normal). With helper threads and at least 16K gathered vertices they run as three tasks at the same time; each keeps its own serial loop. The vertex order, the falloff weights, the normal's summation order and the undo records are therefore the same as before by construction. Smaller dabs, Move (which has no per-dab gather) and runs without helpers take the serial path unchanged.

**Tests.** `OverlappedPrepareTests`: Draw, Smooth and Move strokes, crossing strokes (vertices already in the undo record) and a stroke below the threshold, overlapped at 0, 1, 3 and 11 helpers against serial preparation: every dab's hash, the undo deltas (vertices, before and after, bit for bit), BVH boxes and cones, and picks; undo/redo of four strokes through `AppController` with every step's hash, picks and boxes. 202 / 202 tests pass.

**Interleaved runs**, serial → overlapped preparation, same binary, fresh processes, 2 rounds (round 1 / round 2, the order of the two variants swapped in round 2).

Dabs (`adjacency`, ms per edit, 11 helpers; record + normal + staging wall):

| | Draw | Smooth | Move | Wall (Draw) |
|---|---|---|---|---|
| 84M | 20.57 / 20.43 → 18.59 / 18.41 | 25.34 / 25.16 → 23.43 / 22.91 | 10.83 / 10.88 → 10.87 / 10.75 | 4.18 / 4.16 → 2.19 / 2.19 |
| 84M, radius 0.125 | 5.07 / 6.09 → 4.63 / 5.58 | 6.43 / 7.81 → 5.86 / 7.59 | 2.67 / 3.49 → 2.65 / 3.51 | 0.86 / 0.95 → 0.40 / 0.53 |
| 21M | 5.11 / 4.95 → 4.73 / 4.57 | 6.48 / 6.06 → 6.08 / 5.54 | 2.78 / 2.58 → 2.75 / 2.56 | 0.88 / 0.88 → 0.49 / 0.40 |
| 84M, radius 0.03 (7.9K vertices, below the threshold) | 0.88 / 0.91 → 0.89 / 0.89 | 1.23 / 1.29 → 1.28 / 1.27 | 0.55 / 0.56 → 0.57 / 0.55 | 0.04 → 0.04 |

- **84M Draw gather by stage**, serial → overlapped: query 2.05 / 2.02 → 2.09 / 1.99, selection 3.30 / 3.27 → 3.32 / 3.27, record 1.35 / 1.35 → 1.55 / 1.52, normal 1.69 / 1.70 → 1.96 / 1.99 ms. Each step takes longer when they overlap, so the wall drops by 2.0 ms, not by the full 3 ms of the two shorter steps.
- **Without helpers** both variants take the serial path: 84M Draw 93.2 / 90.8 → 96.3 / 89.9, 21M 17.6 / 16.5 → 16.9 / 16.9 ms, the noise between rounds.
- **Below the threshold** (radius 0.03) nothing overlaps and the numbers stay within the spread of the serial runs.

Live strokes (`live`, 400 inputs every 2 ms, 11 helpers; dab latency p50 / p95 / max ms, inputs queued max / at release):

| | Draw | Smooth | Move |
|---|---|---|---|
| 84M | 20.5 / 22.5 / 51.0, 214 / 214 → 18.1 / 20.4 / 45.5, 192 / 189 (r2: 20.2 / 22.5 / 49.1, 212 / 209 → 18.2 / 20.1 / 44.8, 188 / 184) | 25.8 / 29.2 / 33.6, 223 / 223 → 23.7 / 27.7 / 32.9, 206 / 206 (r2: 25.1 / 29.1 / 34.3, 220 / 218 → 23.6 / 27.6 / 33.2, 207 / 206) | 10.6 / 11.2 / 11.7, 327 / 327 → 10.7 / 11.3 / 11.7, 328 / 328 (r2: 10.6 / 11.3 / 13.1, 326 / 326 → 10.6 / 11.3 / 11.9, 326 / 326) |
| 84M, radius 0.125 | 5.01 / 5.70 / 25.4, 12 / 0 → 4.50 / 5.10 / 24.9, 12 / 0 (r2: 6.25 / 7.13 / 27.0, 51 / 51 → 5.64 / 6.08 / 25.7, 15 / 12) | 6.72 / 7.24 / 11.1, 42 / 41 → 6.16 / 6.76 / 11.0, 15 / 14 (r2: 8.20 / 8.79 / 12.8, 103 / 103 → 7.93 / 8.69 / 13.6, 93 / 91) | 2.62 / 2.80 / 3.16, 98 → 2.65 / 2.83 / 3.32, 100 (r2: 3.47 / 3.62 / 3.91, 171 → 3.46 / 3.62 / 3.83, 171) |
| 21M | 6.43 / 7.22 / 15.3, 7 / 0 → 5.90 / 6.43 / 15.1, 7 / 1 (r2: 5.16 / 5.82 / 13.6, 6 / 1 → 4.51 / 5.18 / 12.5, 6 / 1) | 8.28 / 9.23 / 11.2, 5 / 4 → 7.90 / 8.84 / 10.4, 5 / 4 (r2: 6.70 / 7.46 / 9.84, 4 / 1 → 6.05 / 7.14 / 8.78, 4 / 1) | 3.44 / 3.63 / 3.88, 169 → 3.46 / 3.64 / 4.04, 170 (r2: 2.57 / 2.77 / 3.14, 92 → 2.61 / 2.80 / 3.14, 94) |

- **Lock hold and renderer wait are unchanged:** the preparation runs before the lock. Lock hold p50 at 84M is 2.87 / 2.89 → 2.84 / 2.84 ms per Draw dab; reader wait p95 / max 2.19 / 2.98 and 1.29 / 2.89 → 1.95 / 2.94 and 1.82 / 2.91 ms.
- **Without helpers** live latency and backlog do not change (84M Draw 98.3 / 104.1 / 120.3 and 93.4 / 101.1 / 115.3 → 96.6 / 104.4 / 119.0 and 93.1 / 100.5 / 114.2 ms, 360–365 queued).
- **Undo and redo** do not use the dab preparation: `history` at 84M, 11 helpers, undo hold 24.8 / 28.5 → 24.7 / 29.9 ms, undo total 44.9 / 52.1 → 45.2 / 51.5 ms, the same hash after the cycles.

**Same results.** Every stroke hash (Draw, Smooth and Move; 21M and 84M; radii 0.25, 0.125 and 0.03; 0 and 11 helpers) and the hash after the undo/redo cycles are identical between the variants.

**Memory.** No new buffer: the retained edit scratch is the same (233.8 / 234.2 MiB at 84M, 179.8 at 84M radius 0.125, 59.1 at 21M), and so are peak commit (6.94 GiB at 84M, 1.73 at 21M, 7.23 in the 84M history run) and peak working set (6.45 / 1.61 GiB).

**84M Vulkan standard self-test**, serial, overlapped, overlapped, serial, back to back: all PASS, 237 checks each, none failed. Dab latency median 18.3 / 17.6 → 17.0 / 16.5 ms, p95 23.9 / 23.8 → 21.7 / 21.6 ms, max 29.1 / 29.4 → 25.1 / 24.2 ms; lock hold median 2.76 / 2.71 → 2.72 / 2.71 ms; renderer lock wait max 2.78 / 3.30 → 2.81 / 3.17 ms; edit scratch 437.5 MiB, peak commit 11.94 GiB and peak working set 9.57 GiB in all four.

**Adopted** as the default (`--dab-prepare serial` restores the old order): with 11 helpers it shortens every large Draw and Smooth dab in both rounds (84M Draw −2.0 ms, live p50 −2.3 ms and 20–25 fewer queued inputs; the smaller footprint −0.4 to −0.5 ms), results are bit-identical, memory is unchanged, and Move, runs without helpers and dabs below the threshold stay within baseline variation. The remaining gather is the query (2.0 ms) and the selection (3.3 ms), both already parallel; the Smooth brush (7.7 ms) is the largest single stage of Smooth.

### Scaling checkpoint: 84M triangles

This checkpoint runs the next icosphere level, 11 (83,886,080 triangles, 41,943,042 vertices), with the current build and defaults. Level 10 (21M) is the control. Machine: Ryzen 9 7900X (12 cores), 32 GB RAM (17 GB free at the start), RTX 4060 Ti with 8 GB.

**Limit found first.** `kMaxIcosphereSubdivisions` was 10, and `makeIcosphere` and `--subdivisions` clamp to it, so asking for level 11 silently built 21M. It is now 11. Nothing else stores ids in fewer than 32 bits: 84M triangles and 42M vertices fit `u32`, and the 24-bit field in the compact adjacency is an offset inside one 1,024-vertex page.

**Memory.** Estimated ×4 from 21M before the runs, then measured:

| | 21M | 84M | Estimate for 100M |
|---|---|---|---|
| Mesh (positions, normals, triangles, adjacency) | 807 MiB | 3,230 MiB (adjacency 1,307) | 3.85 GiB |
| BVH (nodes, boxes, cones) | 583 MiB | 2,332 MiB (42.0M nodes) | 2.8 GiB |
| Construction peak heap, `makeIcosphere` | 1,527 MiB | 6,117 MiB | not an icosphere level |
| Construction peak heap, PagedMesh from flat arrays (an importer's path) | 1,285 MiB | 5,140 MiB | 6.1 GiB |
| Editor, drop in free RAM at the peak | about 1.7 GB | about 8 GB | about 9.5 GB |
| GPU pools (device-local, mapped) | 510 MiB | 2,040 MiB of 8 GiB | 2.4 GiB |
| Edit scratch after the self-test | 66 MiB | 261 MiB | |

Memory does not block 84M, and 100M fits the same way. The next level (335M) would need about 22 GiB resident plus construction, more than this machine has.

**Headless** (`se_bench … pgather pmark dense icones creuse`, 11 helpers unless stated). The brush radius is the same in world units, so a dab at 84M moves 4 times as many vertices (566K instead of 141K for Draw):

| | 21M | 84M |
|---|---|---|
| Startup through `AppController` | 8.3 s (icosphere 7.7, BVH 0.8) | 47.8 s (icosphere 33.1, BVH 14.5) |
| BVH build alone, one thread / 11 helpers | 4.5 s / 3.2 s | 30.4 s / 65.5 s |
| Picking (raycast) | 2.06 µs | 2.49 µs |
| Draw / Smooth / Move per dab (`adjacency`) | 7.9 / 9.1 / 4.7 ms | 34.1 / 36.2 / 22.5 ms |
| Live Draw: dab latency p50 · max, lock hold p50 | 7.8 · 19.0, 0.68 ms | 32.7 · 104, 2.90 ms |
| Live Move: dab latency p50, inputs queued at release | 4.6 ms, 226 | 22.4 ms, 365 (stroke ends 9.0 s after 0.8 s of input) |
| Live Draw inputs queued at release | 1 to 4 | 280 to 301 |
| Reader wait during strokes, max | 0.7 ms | 4.4 ms |
| Longest background BVH step hold | 2.2 ms | 7.9 ms |
| Undo all typical strokes: hold p50, total p50 | 6.2, 23.4 ms | 25.7, 90.6 ms |
| Undo strong strokes: hold p95, reader wait max | 11.8, 10.9 ms | 45.6, 44.5 ms |
| Live Draw without helpers: dab latency p50, lock hold p50 | 17.4, 3.9 ms | 95.1, 19.2 ms |

The cost per edited vertex does not change: 56 ns at 21M and 60 ns at 84M for Draw. Normals are about half of it (17.6 of 34.1 ms at 84M), then the gather (10.0 ms), BVH marking and refit (4.0 ms) and the staging. Hashes are deterministic, and the undo/redo cycles end where they began.

**Vulkan, 1920×1121** (standard self-test and history scenario, 11 helpers). Every run passes: 237 checks for the standard self-test at both sizes, exact undo/redo, and every visible chunk drawn (7,867 of 7,867 at 84M). The standard self-test's on-screen check noted the window as covered at 84M; the frame captures are correct.

| | 21M | 84M |
|---|---|---|
| Whole-mesh upload at startup | 107 ms | 567 ms |
| Live stroke (500 inputs at 2 ms): dab latency p50 · max | 6.4 · 8.8 ms | 28.9 · 46.7 ms |
| Inputs still queued at release | 3 | 155 |
| Lock hold per dab p50; renderer wait max | 0.60 ms; 0.39 ms | 2.66 ms; 2.24 ms |
| Frame CPU p50 during the stroke; upload per frame p50 | 0.48 ms; 2.0 MiB | 2.66 ms; 11.9 MiB |
| Strong strokes: undo p50 · max (sculpt thread) | 10.1 · 51.4 ms | 20.7 · 230 ms |
| History scenario, typical: undo hold p50, renderer wait p95 · max | 9.2 ms, 0.7 · 2.9 ms | 34.7 ms, 24.5 · 34.4 ms |
| History scenario, strong: undo hold p95, renderer wait max | 14.2 ms, 13.9 ms | 53.8 ms, 51.4 ms |
| Full view, culling on: triangles, GPU ms (light load) | 8.4M, 3.4 | 32.2M, 5.2 |

**First bottleneck hit: the BVH build at startup.** It grows 18 times for 4 times the triangles, and with helpers it is slower than on one thread (65.5 s against 30.4 s in the bench's level mode). Every chunk has the same 4,096 triangles at both sizes, so the work per chunk is the same, and the cause is the allocator:
- `Bvh::build` reserves `2n` nodes for three arrays (about 256 KiB each, 4 times the 2,047 nodes a 4,096-triangle chunk needs), then calls `shrink_to_fit` on each.
- That is about 6 large allocations and 3 copying reallocations per chunk on the shared process heap, 20,480 chunks at 84M.
- It runs right after `makeIcosphere` has freed a midpoint cache of about 31M entries.

A diagnostic build reserved the exact count instead. The builder's split depends only on the count, so the node count is a function of it. The sources were then restored. Interleaved, 2 runs each:
- **84M:** the controller's BVH build goes from 15.3 / 15.8 s to 2.17 / 2.17 s, and startup from 48.8 / 49.5 s to 35.7 / 35.8 s.
- **21M:** 0.89 / 0.85 s to 0.54 / 0.54 s. In the level mode, the build with 11 helpers goes from 3.43 to 0.53 s.
- **Identical results:** every hash and the BVH memory are identical.

**Next optimization** (adopted since, see "Exact-size chunk BVH build"). Build each chunk's tree with the exact node count, and allocate the per-chunk arrays once (or from per-thread scratch) rather than reserve-and-shrink. That is bit-identical and would remove about 13 s of startup at 84M.

After it, the remaining 33.5 s of startup is the test mesh generator. `makeIcosphere` uses a single-threaded `unordered_map` midpoint cache, then sorts and pages the mesh, so it matters only for the built-in sphere. An importer would hand over flat arrays: 6.8 s at 84M, peak 5.1 GiB.

**Interactive scaling.** At the same world radius, a dab is 4 times the work, and the inputs fall behind:
- **Backlog:** 155 to 300 queued inputs at release, against a few at 21M.
- **Undo:** holds and renderer waits grow 4 times, up to 51 ms.

Normals are the largest share of both the dab and the undo hold (10.1 of 25.7 ms), so they are the next target for interaction. Faster per-vertex work, or coalescing queued inputs while the sculpt thread is behind, would shorten the backlog.

### Face normals reused in exact cones (default)

An exact cone (`computeNormalCone`) makes two passes over a chunk's triangles:
- **Pass 1** computes each face normal (a cross product and its length) and sums the unit normals into the axis.
- **Pass 2** computes every face normal again to find the widest angle from that axis. It stops early once the cone is too wide to use.

Exact cones still do most of the cone work in undo and redo. About 87% of the chunks an undo touches change more than half of their leaves, so they fall back to an exact cone. The background rebuild and the initial build also use exact cones.

**The change** (`ChunkedBvh::setReuseConeNormals`, `AppConfig::reuseConeNormals`, `--cone-normals reuse|recompute`):
- Pass 1 stores each face normal and its length (`vec4`, 16 B per triangle) in a per-thread scratch. It is `thread_local` because `JobSystem` has no worker index.
- Pass 2 reads them back instead of recomputing them.
- The same expressions run in the same order, so the axis sum and the cone are bit-identical (`computeNormalConeTimed`; `computeNormalCone` itself is unchanged).

**Where the time goes.** Measured with `se_bench history … pgather pmark dense`, every touched chunk exact. Times are CPU summed over threads, p50 ms per undo of the strong strokes; redo and the typical strokes move the same way.

| | pass 1 | pass 2 | ns per triangle, pass 1 / pass 2 | second pass reaches |
|---|---|---|---|---|
| 21M, 0 helpers | 4.67 → 5.05 | 4.68 → 0.88 | 3.8 / 3.9 → 4.1 / 0.74 | 1.20M of 1.22M triangles |
| 21M, 11 helpers | 8.19 → 8.86 | 8.21 → 1.58 | 6.7 / 6.9 → 7.3 / 1.3 | |
| 5.2M, 0 helpers | 1.36 → 1.49 | 1.38 → 0.26 | | 337K of 354K |
| 5.2M, 11 helpers | 2.42 → 2.58 | 2.38 → 0.48 | | |

- **The two passes cost the same** before the change. The early stop almost never fires: 98% of triangles reach the second pass.
- **With reuse,** storing the normal makes pass 1 about 8% slower, and pass 2 becomes about 5 times faster.
- **With 11 helpers,** each triangle costs about 1.75 times as much CPU as with none, in both modes. That points to shared caches, memory bandwidth and lower clocks with every core busy, not to the scratch.

**Memory traffic.** Estimated from the data layout; this machine has no hardware counters.
- **Recompute.** Each pass loads 12 B of indices and 3 × 12 B of positions per triangle, through the page table. That is 192 KiB of loads per 4,096-triangle chunk and pass, from about 74 KiB of distinct data (48 KiB of triangles, about 26 KiB of positions). Pass 2 reads it from L2.
- **Reuse.** Pass 1 adds a 64 KiB sequential write, and pass 2 becomes a 64 KiB sequential read. Loads and stores fall from 384 to 320 KiB per chunk. Pass 2 also no longer does 4,096 cross products, square roots and page-table lookups.
- **DRAM.** The scratch stays in L2, so it adds no DRAM traffic. At 21M a strong undo writes and reads back about 19.5 MB of scratch, through a 64 KiB buffer per thread.
- **Scratch size.** 64 KiB per thread that computes cones, allocated once and kept: 64 KiB with no helpers, 768 KiB with 11 (`ChunkedBvh::coneScratchBytes`).

**Undo and redo with the editor's defaults** (dense box refit, incremental cones). Values are current build → reuse, p50 ms, median of 3 interleaved processes. The same build with `recompute` matches the current build within noise.

| | undo hold | undo total | refit + cones (wall, in the hold) | cones (CPU) | reader wait p95 · max |
|---|---|---|---|---|---|
| 21M, 0 helpers, typical | 42.4 → 39.6 | 69.3 → 65.0 | 17.7 → 15.2 | 8.8 → 6.1 | 0.73 · 46.4 → 0.74 · 41.8 |
| 21M, 0 helpers, strong | 38.4 → 36.4 | 61.6 → 59.5 | 16.3 → 14.0 | 7.9 → 5.4 | 0.73 · 87.7 → 0.72 · 76.9 |
| 21M, 11 helpers, typical | 7.53 → 7.25 | 23.7 → 24.0 | 2.59 → 2.23 | 15.1 → 10.2 | 0.73 · 8.0 → 0.75 · 7.0 |
| 21M, 11 helpers, strong | 6.86 → 6.62 | 22.0 → 22.0 | 2.35 → 2.05 | 13.7 → 9.3 | 0.74 · 13.8 → 0.75 · 13.3 |
| 5.2M, 0 helpers, typical | 8.14 → 7.33 | 13.7 → 12.4 | 4.11 → 3.38 | 2.18 → 1.51 | 0.73 · 8.4 → 0.74 · 7.3 |
| 5.2M, 0 helpers, strong | 6.98 → 6.62 | 11.8 → 11.6 | 3.49 → 3.01 | 1.94 → 1.36 | 0.74 · 17.0 → 0.73 · 15.7 |
| 5.2M, 11 helpers, typical | 1.80 → 1.68 | 5.88 → 5.85 | 0.50 → 0.48 | 2.64 → 1.96 | 0.71 · 1.8 → 0.72 · 1.7 |
| 5.2M, 11 helpers, strong | 1.47 → 1.45 | 4.62 → 5.05 | 0.63 → 0.52 | 3.43 → 2.32 | 0.73 · 2.6 → 0.72 · 3.2 |

- **Per run,** the undo hold's refit + cones p50 is lower with reuse in all 24 runs (both stroke sets, 3 rounds, 2 sizes, 0 and 11 helpers), with one tie at 5.2M with 11 helpers. Redo moves the same way.
- **With 11 helpers,** the totals do not change, because the helpers already shared the cone work.
- **Hashes.** The hash after the cycles is identical in every run.

**Small dabs** (`se_bench adjacency … dense icones r=0.05`). Values are current build → reuse, ms per dab, median of 3 interleaved processes:

| | Draw | Smooth | Move |
|---|---|---|---|
| 5.2M, 0 helpers | 0.177 → 0.181 | 0.247 → 0.243 | 0.104 → 0.105 |
| 5.2M, 11 helpers | within ±2% | within ±2% | within ±2% |
| 21M, 0 helpers | 0.723 → 0.714 | 1.006 → 0.984 | 0.490 → 0.469 |
| 21M, 11 helpers | 0.921 → 0.907 | 1.202 → 1.167 | 0.533 → 0.540 |

Small dabs widen their cones, so they run few exact cones. Only their fallbacks get faster: at 21M the Draw refit + cones in the hold goes 0.213 → 0.188 ms without helpers and 0.134 → 0.126 ms with 11. The hashes are identical.

**Vulkan history scenario at 21M** (`--selftest-scenario history`, interleaved, 2 runs per mode). Every run passes: exact end state, cones hold every normal, the undone mesh is pixel-identical to the initial one, and 1,913 of 1,913 visible chunks are drawn. Values are p50 ms, run 1 / run 2:

| | undo hold | redo hold | undo total | renderer wait p95 · max |
|---|---|---|---|---|
| typical, 0 helpers | 50.3 / 50.3 → 47.3 / 47.1 | 50.4 / 50.0 → 46.5 / 46.9 | 77.9 / 78.3 → 74.9 / 74.6 | 45.8 · 52.3 / 49.6 · 51.2 → 39.7 · 48.8 / 42.0 · 48.6 |
| strong, 0 helpers | 12.6 / 12.9 → 11.7 / 11.9 (p95 86.1 / 83.6 → 77.9 / 77.3) | 14.8 / 14.9 → 13.8 / 13.7 | 20.4 / 20.7 → 19.0 / 19.7 | 77.3 · 84.4 / 64.0 · 82.1 → 69.7 · 75.9 / 59.5 · 77.5 |
| typical, 11 helpers | 9.63 / 9.67 → 9.24 / 9.10 | 9.54 / 9.64 → 9.29 / 9.34 | 29.8 / 29.6 → 29.7 / 28.9 | 5.5 · 7.1 / 3.5 · 5.6 → 0.7 · 2.9 / 3.0 · 4.1 |
| strong, 11 helpers | 2.83 / 2.88 → 2.97 / 2.78 (p95 14.9 / 15.0 → 14.2 / 13.8) | 3.02 / 3.12 → 2.92 / 2.86 | 8.36 / 8.19 → 9.37 / 8.84 | 5.3 · 14.8 / 4.3 · 14.7 → 4.8 · 13.9 / 12.5 · 14.7 |

- **Triangles drawn.** The triangles submitted per frame match within 0.03%, as identical cones should give.
- **GPU time.** Frame GPU time at this load follows the GPU's clock state in both builds.
- **Standard 21M self-test** with `--cone-normals reuse`: PASS, 237 checks, none failed. This includes the culling comparisons, which require identical images with culling off, frustum only and on, in 16 views after strong deformation, undo and redo.

**Correctness tests** (`NormalConeTests.cpp`):
- **`ReusedFaceNormalsGiveTheSameConeBitForBit`** compares the bytes of three cones: from `computeNormalCone`, and from `computeNormalConeTimed` without and with the scratch. It covers smooth, crumpled, folded and degenerate meshes and 1 to 8,192 triangles, and requires some second passes to stop early and some cones to be unusable.
- **`ReusedFaceNormalsKeepChunkConesExactThroughFolds`** folds the mesh repeatedly with chunks of 64, 512 and 4,096 triangles, serial and with 3 helpers. Every box and cone must equal both the run without reuse and `computeNormalCone`. A brute force over eyes must find no rejected chunk with a triangle facing the eye.
- **The controller test** of `IncrementalConeTests.cpp` also runs with reuse, with exact and incremental cones and 3 helpers, and requires the same hash, boxes and picks.

A mutation confirms that the tests catch a change in the arithmetic: normalizing in the second pass as `dot(axis, n / length)` instead of `dot(axis, n) / length` fails 3 of them.

**Decision.** Reusing the face normals is the default (`AppConfig::reuseConeNormals`):
- **Gains.** Exact cones take a third less CPU. The undo/redo hold is 5 to 10% shorter without helpers and 1 to 7% shorter with 11. This repeats in every bench run and in both Vulkan runs per configuration. Without helpers, the renderer's worst wait drops with the hold.
- **Culling.** The cones are bit-identical, so culling does not change.
- **Cost.** 64 KiB of scratch per thread.
- **What does not change.** With 11 helpers, the totals and small dabs stay the same.
- **What remains.** Pass 1 is now about 85% of the cones' CPU time.

### Parallel gather (default with helper threads)

The gather (sphere query, then each candidate vertex tested once at its first occurrence, with its falloff weight) now runs on the helper threads and gives the serial gather's vertex list, weights and order exactly, so positions, normals and undo records are bit-identical at any thread count (docs/Architecture.md, "Parallel gather"). The query runs one task per BVH chunk, joined in chunk order. An atomic minimum gives each vertex its first corner, and fixed blocks of 2,048 triangles test those corners and are joined by prefix sums. The undo record and the average normal stay serial. Dabs with fewer than 32K candidate triangles (or fewer than 8 chunks for the query), and any run without helpers, gather on the sculpt thread. `--gather serial` turns it off.

**Exactness.** Stroke hashes are unchanged at 1.3M, 5.2M and 21M triangles with 0 and 11 helpers (all 36 benchmark runs below give the same hash per stroke). `ParallelGatherTests.cpp` (2 tests) compares the serial gather with the parallel one at 0, 1, 3 and 11 helpers: every dab's hash, the undo deltas (vertices, before, after) and exact undo/redo, plus switching modes between strokes. Swapping two blocks in the join makes it fail.

**Gather and dab** (`se_bench adjacency`, per dab; 3 interleaved rounds of the baseline (prepare/commit build), the new build with the serial gather and the new build with the parallel gather; median, range in brackets):

| | gather, baseline → parallel | query / select / undo record, parallel | dab, baseline | dab, serial (new build) | dab, parallel |
|---|---|---|---|---|---|
| 5.2M Draw, 11 helpers | 1.11 → 0.73 ms | 0.23 / 0.36 / 0.07 | 3.06 (3.05–3.21) | 3.10 (3.07–3.24) | 2.64 (2.57–2.70) |
| 5.2M Smooth, 11 helpers | 1.09 → 0.66 ms | 0.20 / 0.35 / 0.07 | 3.41 (3.26–3.58) | 3.35 (3.28–3.52) | 2.89 (2.83–3.05) |
| 5.2M Move, 11 helpers (control) | – | – | 1.76 (1.64–1.92) | 1.79 (1.65–1.89) | 1.71 (1.65–1.85) |
| 5.2M Draw, 0 helpers | 0.90 → 0.92 ms | serial fallback | 4.57 (4.54–4.65) | 4.63 (4.52–4.69) | 4.58 (4.54–5.46) |
| 21M Draw, 11 helpers | 5.27 → 2.30 ms | 0.70 / 0.97 / 0.34 | 13.8 (12.8–14.2) | 13.2 (12.4–13.9) | 10.1 (9.7–12.5) |
| 21M Smooth, 11 helpers | 4.78 → 2.06 ms | 0.49 / 0.92 / 0.36 | 14.3 (12.7–14.6) | 13.3 (13.1–14.3) | 10.7 (10.7–12.0) |
| 21M Move, 11 helpers (control) | – | – | 6.96 (6.24–7.32) | 6.29 (6.02–6.93) | 5.98 (5.97–7.61) |
| 21M Draw, 0 helpers | 4.65 → 4.86 ms | serial fallback | 21.6 (20.8–22.2) | 22.8 (20.7–23.5) | 22.9 (20.2–23.7) |

Move gathers once per stroke, so its dab does not change; its spread (up to 10% at 21M) is the run-to-run variation of this machine. With helpers the serial query and selection are slower than without (5.2M: 1.11 against 0.90 ms), because the helpers share the cores and caches; the parallel version recovers that and more.

**Live, headless** (`se_bench live <level> <helpers> 2 [pgather]`, 400 inputs every 2 ms, reader locking every 4 ms; 3 interleaved processes per build × 2 rounds; medians of the 6 strokes):

| | dab latency p50 / p95 / max | inputs queued max / at release | lock hold per dab p50 / p95 / max | reader wait p95 / max |
|---|---|---|---|---|
| 5.2M Draw, 11, baseline | 3.49 / 3.99 / 5.24 | 2 / 1 | 0.51 / 0.60 / 0.71 | 0.00 / 0.61 |
| 5.2M Draw, 11, parallel | 2.87 / 3.31 / 4.52 | 2 / 1 | 0.47 / 0.55 / 0.59 | 0.14 / 0.72 |
| 5.2M Smooth, 11, baseline | 4.13 / 4.79 / 5.08 | 2 / 2 | 0.48 / 0.59 / 0.63 | 0.04 / 0.61 |
| 5.2M Smooth, 11, parallel | 3.53 / 4.00 / 4.45 | 2 / 1 | 0.45 / 0.55 / 0.58 | 0.00 / 0.62 |
| 5.2M Move, 11, baseline / parallel (p50) | 1.83 / 2.05 | 6 / 12 (worst 20 both) | 0.48 / 0.50 | 0.23 / 0.39 |
| 21M Draw, 11, baseline | 11.8 / 14.0 / 21.9 | 86 / 82 | 1.23 / 1.58 / 2.36 | 0.72 / 1.35 |
| 21M Draw, 11, serial (new build) | 12.1 / 14.3 / 20.6 | 86 / 83 | 1.24 / 1.45 / 2.23 | 0.64 / 1.38 |
| 21M Draw, 11, parallel | 9.85 / 11.7 / 16.7 | 22 / 19 | 1.29 / 1.62 / 1.87 | 0.78 / 1.39 |
| 21M Smooth, 11, baseline | 12.2 / 15.0 / 16.8 | 46 / 46 | 1.12 / 1.29 / 1.49 | 0.32 / 1.19 |
| 21M Smooth, 11, serial (new build) | 12.7 / 15.3 / 18.9 | 61 / 60 | 1.09 / 1.30 / 1.43 | 0.56 / 1.18 |
| 21M Smooth, 11, parallel | 10.2 / 11.7 / 13.8 | 10 / 3 | 1.13 / 1.40 / 1.49 | 0.66 / 1.19 |
| 21M Move, 11, baseline / parallel (p50) | 5.67 / 6.03 | 258 / 267 | 1.04 / 1.12 | 0.75 / 0.76 |
| 21M Draw, 0, baseline / parallel (p50) | 19.4 / 19.4 | 208 / 207 | 6.96 / 7.12 | 5.98 / 6.32 |

Per-process dab p50 with 11 helpers: 21M Draw 10.6–13.4 ms baseline, 10.5–13.9 serial, 8.4–11.2 parallel; 5.2M Draw 3.28–3.66 baseline, 3.11–3.58 serial, 2.59–3.03 parallel. The parallel gather shortens dabs by 14 to 18% at 5.2M and 16 to 19% at 21M, and at 21M it cuts the backlog of this 2 ms stress test by about four times for Draw (86 → 22) and Smooth (46 → 10). The lock hold and reader wait do not change: the gather runs before the commit. Undo/redo holds are the same (21M, 11 helpers: undo 7.8 / 8.0 ms, redo 6.3 / 6.4 ms, baseline / parallel). Without helpers the parallel build gathers serially and matches the baseline within its spread.

**Vulkan self-test** (standard script, 11 helpers, 500 inputs at 2 ms; 21M interleaved with the baseline; all PASS, 237 checks, exact undo/redo):

| | dab latency median / p95 / max | sculpt ms per input p95 / max | lock hold per dab median / p95 / max | renderer wait p95 / max | frame CPU p95 | queued at release |
|---|---|---|---|---|---|---|
| 21M, baseline | 11.3 / 15.7 / 18.9 ms | 13.6 / 18.9 | 1.39 / 1.61 / 1.71 | 0.030 / 1.27 | 1.82 | 0 |
| 21M, parallel | 8.1 / 10.5 / 11.9 ms | 9.3 / 12.0 | 1.03 / 1.29 / 1.34 | 0.001 / 0.53 | 0.77 | 0 |
| 5.2M, parallel | 2.76 / 3.56 / 4.26 ms | 3.1 / 4.3 | 0.50 / 0.60 / 0.65 | 0.000 / 0.25 | 0.95 | 0 |

Strong strokes at 21M: sculpt ms per input p95 33.6 (baseline 51.4), max 98 (152).

**Scratch.** The per-chunk triangle lists and the per-block vertex lists add 1.2 MiB at 5.2M (4.7 → 5.8 MiB) and 4.3 MiB at 21M (18.2 → 22.5 MiB) in `se_bench`, and 16 MiB after the self-test's larger brushes at 21M (41.1 → 57.3 MiB). Like the rest of the scratch, they grow to the largest dab seen and are reused; the serial gather does not allocate them.

**Decision.** Parallel gather is the default: with helpers the speed-up is repeatable at 5.2M and 21M (the parallel ranges do not overlap the baseline's at 5.2M, and barely at 21M), and the serial path of the new build stays within the baseline's run-to-run spread. Without helpers, and for dabs below 32K candidate triangles, it is the serial gather. What remains serial in a 21M Draw dab with 11 helpers: undo record 0.34 ms, average normal 0.3 ms, the BVH leaf marking (2.5 ms; parallel since, see "Parallel BVH leaf marking") and the de-duplication inside the normal update.

### Prepare and commit: the document lock is held only to publish

Every dab is computed without the document lock and published under it as one update (docs/Architecture.md, "One lock for the document"). The sculpt thread is the only writer of the mesh, the BVH and the pending dirty set (strokes, undo/redo and the background BVH steps all run on it), so it can read them without the lock.
- **Prepare (no lock):** gather, undo recording, then the brush on a staging view (vertex pages holding the dab's vertices are copied; the brushes are unchanged), the new normals of the moved vertices and their one-ring computed from that view, dirty marking, and the BVH leaf queueing (`ChunkedBvh::markTriangles`).
- **Commit (lock):** write positions and normals, refit the queued boxes and the normal cones of their chunks plus the top level (`ChunkedBvh::finishMarkedRefit`), hand the dirty pages over.
- **Unchanged:** undo and redo held the lock for their whole update (since then they mark their BVH leaves before locking; see "Parallel BVH leaf marking"); a background BVH step holds it for one step; `--lod` is untouched.

**Exactness.** Stroke hashes (all positions and normals) are identical to the previous build at 1.3M, 5.2M and 21M triangles, with 0 and 11 helpers, for stored and derived neighbours. New tests (`PrepareCommitTests.cpp`, 3) check that preparing a dab changes nothing a reader sees, that after every commit normals, BVH boxes and cones match the positions bit for bit, and that a thread locking the document during strokes only ever sees states that a serial run passes through between two dabs, with a mirror built from the dirty pages equal to the mesh at every read. Both of two deliberate breaks (releasing the lock before the refit; writing positions before taking it) make that test fail 5 runs out of 5. Undo and redo of every stroke restore the initial hash exactly.

**Commit profile** (`se_bench adjacency`, 11 helpers, per dab). Before deciding what to parallelise, the commit was measured. It first was the publish plus the whole refit; the largest part of the refit was the serial leaf queueing, which only touches refit scratch, so it moved to prepare. Nothing else was parallelised.

| | dab | commit | publish (positions, normals) | local refits + cones (parallel) | top level | leaf queueing, now in prepare |
|---|---|---|---|---|---|---|
| 5.2M Draw | 3.1 ms | 0.42 ms | 0.13 | 0.29 | 0.003 | 0.57 |
| 21M Draw | 13.3 ms | 1.35 ms | 0.23 | 1.10 | 0.012 | 2.53 |
| 21M Move | 6.3 ms | 1.04 ms | 0.20 | 0.83 | 0.008 | 1.87 |

What remains in the commit is mostly the normal cones, recomputed from all 4,096 triangles of every touched chunk (399K triangles per 21M Draw dab).

**Live, headless** (`se_bench live <level> 11 2`): Draw, Smooth and Move strokes through `AppController`, 400 inputs every 2 ms, a reader thread locking the document every 4 ms like the renderer. 3 fresh processes per build, interleaved, 2 rounds each; medians of the 6 strokes' p50/p95/max, worst max in brackets. "before" is the same instrumentation with whole-dab locking.

| | lock hold per dab p50 / p95 / max | reader wait p50 / p95 / max | dab latency p50 / p95 / max | inputs queued, max |
|---|---|---|---|---|
| 5.2M Draw, before | 3.17 / 3.81 / 5.34 (5.97) | 0.00 / 2.76 / 4.77 (5.78) | 3.18 / 3.81 / 5.35 | 2 |
| 5.2M Draw, after | 0.48 / 0.59 / 0.68 (0.73) | 0.00 / 0.19 / 0.68 (0.77) | 3.23 / 4.09 / 5.12 | 2 |
| 5.2M Smooth, before | 3.66 / 4.49 / 5.09 (5.30) | 0.00 / 3.08 / 4.53 (5.09) | 3.67 / 4.50 / 5.10 | 2 |
| 5.2M Smooth, after | 0.41 / 0.55 / 0.60 (0.68) | 0.00 / 0.04 / 0.72 (0.81) | 3.66 / 4.56 / 4.95 | 2 |
| 5.2M Move, before | 1.63 / 1.96 / 2.37 (2.61) | 0.00 / 1.49 / 1.93 (2.29) | 1.63 / 1.96 / 2.38 | 1 |
| 5.2M Move, after | 0.42 / 0.53 / 0.61 (0.67) | 0.00 / 0.21 / 0.54 (0.68) | 1.72 / 2.16 / 2.50 | 1 (one stroke 19) |
| 21M Draw, before | 12.8 / 15.2 / 23.2 (27.9) | 0.00 / 12.6 / 22.3 (27.8) | 12.8 / 15.2 / 23.2 | 114 |
| 21M Draw, after | 1.36 / 1.63 / 1.95 (3.26) | 0.00 / 0.67 / 1.39 (2.24) | 12.5 / 15.0 / 20.8 | 96 |
| 21M Smooth, before | 13.4 / 16.7 / 18.1 (20.1) | 0.00 / 12.9 / 17.8 (20.1) | 13.4 / 16.7 / 18.2 | 75 |
| 21M Smooth, after | 1.23 / 1.41 / 1.48 (1.67) | 0.00 / 0.54 / 1.25 (1.33) | 13.1 / 15.5 / 17.0 | 72 |
| 21M Move, before | 6.15 / 6.98 / 8.61 (11.0) | 2.79 / 5.65 / 7.87 (9.35) | 6.17 / 7.00 / 8.63 | 271 |
| 21M Move, after | 1.15 / 1.31 / 1.42 (1.55) | 0.00 / 0.78 / 1.28 (1.45) | 6.20 / 7.10 / 8.84 | 272 |

Before, the lock hold equalled the whole dab; after, it is 9 to 24% of it. Total dab latency does not change at 21M and is 0 to 6% higher at 5.2M (the staging copy and the publish writes: 0.06 + 0.13 ms per 5.2M Draw dab). The input backlog is the same: at 21M the dabs (6 to 13 ms) are slower than inputs arriving every 2 ms in this stress setting, with or without the split; shortening the hold does not make the sculpt thread faster. Undo/redo hold the lock as before: 2.0 / 1.7 ms at 5.2M and 8.0 / 6.5 ms at 21M (medians; worst 9.9 ms), a background BVH step at most 1.2 ms at 5.2M and 2.5 ms at 21M (the same in both builds).

**Live, Vulkan self-test** (standard script, 500 inputs at 2 ms intervals while the window renders; both builds PASS, 237 checks, exact undo/redo):

| | renderer wait for the lock, p95 / max | frame CPU, p95 / max | sculpt lock hold per dab, median / p95 / max | dab latency, median / max | inputs queued at release |
|---|---|---|---|---|---|
| 21M, before | 14.6 / 16.7 ms | 15.4 / 18.6 ms | 12.1 / 15.4 / 17.2 ms | 12.7 / 19.0 ms | 0 |
| 21M, after | 0.000 / 0.001 ms | 0.76 / 2.19 ms | 1.33 / 1.63 / 1.75 ms | 12.3 / 18.7 ms | 0 |
| 5.2M, after | 0.000 / 0.50 ms | 0.55 / 0.81 ms | 0.56 / 0.72 / 0.80 ms | 3.6 / 5.5 ms | 0 |

At 21M the frame no longer waits for the sculpt thread: frame CPU p95 drops from 15.4 to 0.76 ms. Strong strokes at 21M: ms per input p95 47 (before 50), undo median 15 ms.

**Scratch memory.** The stroke processor's per-dab scratch is the edit region plus the staging pages (the dab's footprint in whole 12 KiB vertex pages), a pointer and a byte per vertex page, and the new normals: 4.7 MiB at 5.2M (before 3.0) and 18.2 MiB at 21M (before 12.0) in `se_bench live`; after the larger brushes of the self-test, 41.1 MiB at 21M (before 25.0) and 10.5 MiB at 5.2M. It grows to the largest dab seen and is reused.

**Next experiment, kept separate:** parallel gather. Done since; see "Parallel gather" above: it shortens dab latency and the input backlog, not the hold.

## Known limitations

- **The CPU copy is always complete.** A GPU budget limits only GPU memory; nothing is streamed from disk, evicted from RAM or compressed.
- **No level of detail by default.** When the visible set does not fit the budget, far chunks are simply not drawn. A coarse fallback exists as an opt-in (`--lod`): border-locked per-chunk stand-ins, about 5.7% of a full copy taken from the budget, all drawn with one multi-draw-indirect call per pass. It is not part of the default renderer, and its coarse topology is fixed at load time, so it follows strong deformation only approximately.
- **No occlusion culling.** Chunks are culled against the frustum and when they face entirely away from the viewer. Front-facing chunks hidden behind other surfaces are still drawn.
- **Widened cones cull a little less until they are rebuilt.** With incremental cones (default), a chunk changed by a dab or an undo keeps a cone widened around its old axis, or none, until the sculpt thread rebuilds it in idle time. In the measurements this let through at most 0.13% more triangles after an undo, and up to 0.7% at the end of a long stroke with no idle time. Chunks with more than half of their leaves changed still recompute their cone exactly under the lock.
- **84M triangles start slowly and lag behind large brushes.** Startup takes about 36 s at 84M, almost all of it the built-in sphere generator (a loaded mesh would take about 7 s). At the same brush radius, a dab moves 4 times the vertices of 21M, so long strokes queue hundreds of inputs (fewer since the parallel edit region: a Draw dab takes 25 ms) and undo can hold the document for up to 55 ms.
- **1 to 5 pixels differ after heavy eviction under a budget.** In the pressure self-test at 90%, after the strong deformations are redone, 1 to 5 pixels of a 1920x1121 frame differ from the whole-mesh reference while every visible chunk is drawn, with or without normal cones and with or without `--lod`. They are not missing geometry, and the cause has not been found.
- **Mapped vertex memory.** The pools prefer coherent, mappable device-local memory and fall back to coherent host memory, which is several times slower to draw from. The host fallback was exercised by forcing it in a test build, not on a GPU without resizable BAR.
- **GPU timings at light load** reflect clock state more than rendering cost; the self-test's loaded rows (64 passes per frame) are the ones to compare.
- **Renderer CPU per frame** is about 0.05 ms higher at 5.2M than with the removed fully resident path (pages requested and page table written every frame). Not visible in frame pacing; not profiled further.
- **Draw calls under a budget.** Pool slots are reused out of order, so merging adjacent slots finds fewer runs than with whole-mesh pools.
- **Copy-on-write reserve.** Spare vertex slots are kept for edited pages so that edits always reach the screen: an eighth of the page count in whole-mesh pools (7.5 MiB at 5.2M), an eighth of the pool under a budget, which leaves that much less for the view. A stroke that changes more pages within one frame-slot cycle than there are spares draws the extra ones a frame late.
- **Chunk membership is fixed.** A chunk is a fixed set of triangles. Its tree and the top-level tree are rebuilt after edits, but triangles never move to another chunk, so after extreme stretching chunk boxes overlap more than a fresh partition would.
- **Adjacency is still 40% of the mesh memory** with the compact default (82 of 202 MiB at 5.2M triangles; wide: 140 of 260). `--neighbors derived` halves it (41 MiB, mesh 162 MiB) but makes Smooth 3 to 6% slower per edit, so it is opt-in (see "Derived neighbours").
- **Parallel speed-up is modest.** Helper threads give about 2.3x on large dabs (21M Draw 21 → 9 ms with 11 helpers) and little on small ones. The gather, the BVH leaf marking and the edit-region walk are parallel (the walk only 2.2 to 2.5x: random accesses to a per-vertex stamp array); the undo record, the average normal and the de-duplication in undo/redo's normal update are still serial.
- **Upload granularity.** Dirty tracking works on vertex pages. A brush dirties 2 to 10% of the mesh per frame (table above); on small meshes the page size is the limit.
- **A slow sculpt thread shows as lag, not as a frozen window.** If edits cost more than the time between two mouse events, inputs queue up and the stroke trails the cursor. Since prepare/commit the renderer waits only for a dab's commit (about 1.3 ms at 21M triangles), but the dab itself still takes 5 to 9 ms there with 11 helpers; the normals step (3.6 ms, including the edit-region walk) is now the largest part.
- **Undo and redo hold the document lock to restore positions, update normals and refit.** For 460K-vertex steps at 21M with 11 helpers the hold is 7.7 ms p50 and 14.9 ms worst with the dense box refit (default); it was 9.4 / 17.4 ms with the incremental refit. With `--history-normals before` the normals are also computed before the lock, which shortened the hold by about 2.3 ms more when measured with the incremental refit, but the total is 3 to 13% longer. Refit + cones is 2.6 ms of what remains (21M, 11 helpers), and the cones are about half of it: incremental cones (default) widen only about 13% of the chunks an undo touches, because the others changed almost entirely and still recompute their cone exactly. While frames render back to back (`--selftest-scenario history`), the renderer's worst wait at 21M with 11 helpers is 14 to 15 ms for strong strokes and 4 to 7 ms for typical ones.
- **Compact adjacency needs spatial order to pay off.** Rows whose ids are not all within 16 bits of the page anchor fall back to u32 ids (2.7% of rows on the 5.2M icosphere). A mesh in arbitrary order is then about as large as with the wide layout (never more than 10% larger), never wrong.
- **Startup is 2 s at 5.2M triangles**, almost all of it single-threaded mesh generation and spatial ordering.
- **Smooth flattens at brush scale.** On dense meshes it filters at a fraction of the brush radius, so it behaves the same at any triangle count, but it slowly flattens curvature under the brush, can leave faint facets at the scale of its filter cells, and two nearby surfaces facing the same way can influence each other.
- **Fixed topology.** Stretching the surface far with Move or Draw produces long thin triangles. There is no remeshing, subdivision or dynamic topology.
- **Rendering is still on the UI thread** (`QVulkanWindow`). It costs about 0.1 ms of CPU per frame, so this is not a bottleneck today.
- **Brush dabs use a sphere in space**, so on thin or folded shapes a brush can affect a surface behind the one under the cursor.
- **Undo history** is capped at 128 strokes and is not persisted. There is no file loading or saving.
- **Renderer**: one mesh, one pipeline per storage mode, swapchain owned by `QVulkanWindow`.
- **Platform**: built and run on Windows only so far.

## Documentation

- [docs/Architecture.md](docs/Architecture.md): modules, data flow, design decisions, extension points
- [docs/Development.md](docs/Development.md): layout, conventions, how to add a brush, debugging

## Repository layout

```
CMakeLists.txt, CMakePresets.json
cmake/            SPIR-V embedding script
scripts/          optional project-local Windows toolchain
src/
  Core/           logging, errors, assertions, timing, job system, fair mutex
  Geometry/       paged mesh, spatial chunks, BVH, icosphere (no Qt, no Vulkan)
  Sculpt/         brushes, stroke processing, undo        (no Qt, no Vulkan)
  Application/    AppController: the engine's public face (no Qt, no Vulkan)
  Renderer/       orbit camera, Vulkan renderer, shaders
  Editor/         Qt windows, input handling, self-test
  main.cpp
tests/            GoogleTest suites (GeometryTests, SculptTests)
tools/Bench/      headless benchmark
docs/
```

## License

Copyright (C) 2026 NicolasDCX

SculptEngine is free software, licensed under the [GNU General Public License v3.0](LICENSE). You may use, study, modify and redistribute it, provided that any distributed work based on it is also released under the GPL-3.0 with its source code.

### Commercial licensing

If you want to use SculptEngine in a closed-source or proprietary product without the GPL's obligations, a separate commercial license is available. Contact the author through [GitHub](https://github.com/NicolasDCX).

### Contributions

To keep dual licensing possible, contributions are accepted only if the contributor agrees that the author may distribute them under both the GPL-3.0 and commercial licenses.
