# SculptEngine architecture

SculptEngine is a modular monolith: one executable, built from static libraries with one-way dependencies.

## Modules

```
            +--------+
            |  Core  |   types, log, errors, assert, stopwatch, JobSystem, FairMutex
            +---^----+
                |
          +-----+------+
          |  Geometry  |   PagedArray, PagedMesh, ChunkedBvh, Bvh, SpatialOrder, Icosphere, DirtyVertexSet
          +--^------^--+
             |      |
     +-------+--+   |
     |  Sculpt  |   |      Brush, BrushLibrary, StrokeProcessor, UndoStack, NormalUpdater
     +-----^----+   |
           |        |
  +--------+------+ |
  |  Application  | |      AppController
  +--------^------+ |
           |      +-+----------+
           |      |  Renderer  |   OrbitCamera, RenderScene, Frustum, ChunkPageMap, PagePool, Vulkan/*
           |      +-----^------+
        +--+------------+--+
        |      Editor      |       EditorApp, MainWindow, ViewportWindow, SelfTest
        +------------------+
```

| Module | CMake target | May depend on | Must not depend on |
|---|---|---|---|
| Core | `se_core` | standard library | everything else |
| Geometry | `se_geometry` | Core, GLM | Vulkan, Qt, Sculpt |
| Sculpt | `se_sculpt` | Geometry | Vulkan, Qt, Renderer |
| Application | `se_application` | Sculpt | Vulkan, Qt, Renderer |
| Renderer | `se_renderer` | Geometry, Qt Gui, Vulkan headers | Sculpt, Application |
| Editor | `se_editor` | Application, Renderer, Qt Widgets | |

The rules are enforced by the link graph: `se_geometry`, `se_sculpt` and `se_application` do not link Qt or the Vulkan headers, so an offending `#include` does not compile. The `headless` preset builds exactly these three libraries with their tests.

There is no ECS, no plugin system and no scene graph. The document is one mesh, stored in pages.

## Data flow of one brush sample

```
mouse move (Qt)
  ViewportWindow::pointerMove
    OrbitCamera::screenRay            pixel -> world-space ray
    AppController::updateStroke(ray)  queue the ray, return immediately
--- sculpt thread ---
      StrokeProcessor::update
        Bvh::raycast                  where is the pointer on the surface
        (spacing)                     how many dabs since the last sample
        for each dab:                 prepare (no lock; the mesh is not changed):
          ChunkedBvh::querySphere     1. candidate triangles (per chunk on the helpers, joined in order)
          gather                         vertices inside the radius + falloff weights
                                         (first corners in fixed blocks on the helpers, joined in order)
          recordForUndo               2. first touch per stroke: remember old position
          stagePages                     copy the vertex pages holding them to staging pages
          Brush::apply                3. deform the staged positions (one virtual call, tight loop inside)
          EditRegion::collect         4. one walk: triangles around the moved vertices,
                                         moved vertices + their one-ring (each once)
                                         (fixed blocks on the helpers above 16K moved vertices, joined in order)
          PagedMesh::computeNormals      their normals, from the staged positions, into scratch
          DirtyVertexSet::mark           remember which vertex pages change
          ChunkedBvh::markTriangles      queue the BVH leaves to refit (one chunk per task on the helpers)
                                      commit (holding AppController::documentMutex):
          write positions, normals    5. publish the dab
          ChunkedBvh::finishMarkedRefit  boxes and normal cones of the queued chunks, top level
          drainDirty                  6. hand the changed vertex pages to the renderer
      change listener                 post "engine changed" to the GUI thread
--- GUI thread ---
  ViewportWindow::onEngineChanged     requestUpdate()
frame (Qt, GUI thread)
  SceneRenderer::startNextFrame
    lock AppController::documentMutex (waits for at most one dab's commit)
    markDirty                         the GPU storage learns which vertex pages changed
    selectChunks                      frustum test of every chunk's current bounds
    ResidentMeshBuffers::prepare      copy edited pages to spare slots; with a budget,
                                      make visible chunks resident, nearest first
    unlock; record render pass (one draw per run of adjacent index slots), frameReady()
```

When the stroke ends, the sculpt thread pushes the stroke's sparse delta on the undo stack. `Bvh::optimize`, which re-partitions only the blocks of the tree the stroke touched, then runs in small steps whenever the sculpt thread has no input to process. Undo and redo are queued commands handled the same way (incremental refit, optimize later). The model bounds are the BVH root box, so nothing in the edit path walks the whole mesh.

## Key decisions

## Geometry storage: three separate concepts

| Concept | What it is | Unit | Owns | Code |
|---|---|---|---|---|
| Geometry page | Storage. A fixed-capacity block of vertices or of triangles. | 1,024 vertices; 4,096 triangles (configurable) | The data: positions, normals, triangle corners, adjacency | `geo::PagedArray`, `geo::PagedMesh` |
| Spatial chunk | Organisation. A patch of surface with its own BVH. | 4,096 triangles (configurable) | A local BVH and its bounds; no geometry | `geo::ChunkedBvh`, `geo::Bvh` |
| GPU page | Presentation. A run of vertices on the GPU: the content of one vertex pool slot. | 1,024 vertices (renderer setting) | Which slot holds it, whether that copy is current | `render::PagePool`, `render::ResidentMeshBuffers` |

The three sizes are independent parameters. A chunk's triangles span several triangle pages, its vertices are in whatever vertex pages they are in, and a dirty geometry page maps onto GPU pages by vertex id range. Tests cover mismatched sizes for each pair.

There is one authoritative copy of the geometry: the pages of `PagedMesh`. The BVH stores triangle ids, undo stores vertex ids and positions, the GPU buffers are filled directly from the pages.

### Stable ids

A vertex id or triangle id is assigned once and never changes. The id encodes the location: `id >> pageShift` is the page, `id & (capacity - 1)` the slot. Pages are separate allocations reached through a page table, and they are never moved, compacted or renumbered, so:

- growing a `PagedArray` adds pages without moving existing elements (tested: addresses stay the same);
- an id stays valid whatever happens to other pages, which is what loading and evicting pages later requires;
- everything that refers to geometry (adjacency, BVH leaves, undo records, dirty sets, the GPU index buffer) stores ids, never pointers.

Ids are plain `u32` aliases (`VertexId`, `TriangleId`), not wrapper types. A wrapper would have touched every loop in the engine for no behavioural gain at this stage.

### PagedMesh

- **Vertex pages**: positions and normals as two paged arrays with the same layout.
- **Triangle pages**: three vertex ids per triangle.
- **Adjacency pages**: one block per vertex page holding, for its vertices, the neighbour list and the incident triangle list. Entries are global ids, so a vertex's neighbours can be in any page. Nothing special happens at page borders, and no vertex is duplicated. Two storage layouts (`geo::AdjacencyLayout`): *compact* (the default: a u32 header per vertex and a signed 16-bit offset per entry from a per-page anchor; rows out of 16-bit reach keep u32 ids; 58% of the wide size at 5.2M triangles) and *wide* (`--adjacency wide`: a u32 offset per vertex and a u32 id per entry). With `NeighborStorage::Derived` (opt-in, `--neighbors derived`) no neighbour table is stored: `PagedMesh::forEachNeighbor` rebuilds a ring from the incident-triangle row (regular fans, flagged by one bit per vertex at construction, by the corner after the vertex in each triangle and a sorting network; any other fan by sorting and deduplicating all corners), giving the same ids in the same order. Both layouts decode to the same ids in the same order; readers get an `AdjacencyRow` (range-for, `size()`, `forEach`).
- **Per page**: bounds (kept lazily: recomputed on request after the page was written) and dirty tracking (`makeDirtySet()` gives a set with one bit per vertex page).
- **Accounting**: `memory()` reports bytes by kind.

Element access goes through small view objects (`mesh.positions()[id]`), which cost one extra load compared with a flat array: the page pointer. Code that used to index flat arrays compiles unchanged; hot loops take the views into locals so the page tables stay in registers.

Topology is still immutable. Every algorithm relies on that.

Trade-off, measured: the page-table lookup made the first paged version 20 to 55% slower per edit. Two things fixed most of it: storing each adjacency page as one block (one pointer to reach both the offsets and the lists), and inlining the per-triangle BVH marking. What remains is within about 10% of the flat version (README). The alternative, one virtually contiguous array with pages committed on demand, has no lookup cost but ties page management to the operating system and makes non-resident pages harder; it was not chosen.

### Spatial order

Mesh creation orders vertices and triangles by recursive median splits (a k-d tree written out in leaf order), giving the left side of every split a power-of-two count. That makes every aligned power-of-two run of ids the content of one box, for all sizes at once, so vertex pages and chunks of any configured size are compact patches.

Two space-filling curves were tried first and rejected on measurement. With Morton order some chunks of 1/80 of a sphere had bounding boxes 1.87 across (the sphere is 2 across), with Hilbert order 1.5: on a thin surface the curve leaves the surface and returns somewhere else. With median order the largest is below 1.2 and the mean below 0.6 (tested). The cost is construction time: ordering is the largest part of startup at 5.2M triangles.

### Spatial chunks and the two-level BVH

A chunk is a run of 2^n consecutive triangle ids; `chunkOf(triangle)` is a shift. Membership never changes. Chunks partition triangles, not vertices: a vertex on a chunk border is one vertex referenced by triangles of several chunks.

`ChunkedBvh` holds one `geo::Bvh` per chunk (the same class that can also span a whole mesh) and a top-level tree with one leaf per chunk.

- **Queries** traverse the top level, then the local trees of the chunks they reach.
- **Refit after an edit**: each triangle around a moved vertex (the `EditRegion` list, shared with the normal update, so the adjacency is walked once per dab) is routed to its chunk's tree. The local refits are independent, so they run in parallel when an edit touches more than eight chunks. Then the top level is updated for the chunks whose overall box changed. An edit across chunk borders needs no special case; it simply marks triangles in several chunks.
- **Normal cones** (`AppConfig::normalCones`, on by default): each chunk also keeps a cone containing the face normal of every one of its triangles (`geo::NormalCone`). The refit recomputes the cone of every chunk it touches, in the same parallel task as that chunk's local tree, so on the sculpt thread a chunk's cone and box always change together and describe the same mesh. A chunk's cone is reset to "rejects nothing" the moment the refit routes a triangle to it and only becomes usable again once recomputed. A cone costs one pass over the chunk's triangles (4,096 cross products, twice), which is why chunks with cones are spread over helper threads one at a time instead of eight at a time. Full refits and builds compute every cone; `optimize` does not change a chunk's triangles and leaves cones alone. With incremental cones (`AppConfig::incrementalCones`, default), a chunk whose refit queued at most half of its leaves keeps its cone's axis and only widens the cone to the new normals of the triangles in the queued leaves (`widenNormalCone`, the same arithmetic and margin as `computeNormalCone`), or gets an unusable cone when it would become too wide. The cone still holds every normal of the chunk and is published with the box. The chunk is then marked stale: when its queue is empty, the sculpt thread computes exact cones for a few stale chunks without the lock (`ChunkedBvh::computeStaleCones`, it is the only writer) and publishes them under it (`publishStaleCones`), before any BVH optimize step. Once no chunk is stale, the cones equal exact mode bit for bit. An exact cone keeps the face normals of its first pass (normal and length, 16 B per triangle) in a per-thread scratch and reads them back in its second pass (`AppConfig::reuseConeNormals`, default; `ChunkedBvh::exactCone`), computed by the same expressions in the same order, so the cone is bit-identical to recomputing them.
- **Local refit** (unchanged from before): the tree keeps a parent and a depth per node and the leaf of every triangle. It works level by level from the deepest; a parent is queued only if a child changed, so every affected node is recomputed once.
- **Tidy-up**: `optimize(mesh, maxTriangles)` re-partitions the local trees of touched chunks in budgeted steps, then rebuilds the top level (one leaf per chunk, so it is cheap). The structure is valid for queries and refits between steps.
- **Build**: local trees are built in parallel; ten times faster than the single tree at 5.2M triangles.

Compared with the single tree used before: no part of the hierarchy is frozen in its initial shape any more (the top level is rebuilt after edits), the build is parallel, and the memory is lower because chunk sizes are powers of two and leaves are full. Raycasts are 10 to 20% slower.

Correctness is tested against brute force and against the single tree: raycasts for five chunk sizes, incremental refit equal to full refit, stepped optimize equal to one-shot optimize, parallel equal to serial, and a structural check of every invariant.

Because every split is an exact median, the shape of a local tree depends only on its triangle count, so a subtree can be rebuilt in place in the node slots it already occupies. That property is what makes the tidy-up allocation free.
### Brushes

`sculpt::Brush` has one virtual `apply(const DabContext&)`. The stroke processor makes one virtual call per dab; the per-vertex loop is inside the brush, so it contains no indirect calls. `DabContext` hands the brush everything it may need: the affected vertices, their falloff weights, the average normal, a drag vector and a reusable scratch buffer.

Two stroke modes cover the three brushes:

- `Dabs` (Draw, Smooth): the brush is stamped every `0.25 x radius` of distance travelled over the surface. The stroke processor interpolates the *pointer ray* between two events and re-casts it for each dab, so dabs land on the surface as it currently is and their count does not depend on the mouse event rate.
- `Grab` (Move): the vertices are gathered once at the start; every update re-applies the total pointer displacement to the positions saved at that moment. The result depends only on where the pointer is, not on the path.

New brushes are registered with `BrushLibrary::add`. A test (`CustomBrushCanBeRegisteredWithoutTouchingGeometry`) defines a Flatten brush entirely outside the engine to prove nothing else needs to change.

The Smooth brush computes all targets from the positions at the start of each pass and commits afterwards, so the result is independent of vertex order and of thread timing (tested).

Smooth adapts to mesh density. Averaging edge neighbours reaches a few edges, which on a dense mesh is a very small distance, so broad shapes would barely change. When a dab holds many vertices per filter cell (12 or more), the brush first bins the vertices into a grid whose cell size is 0.3 x the brush radius and moves each vertex along the locally averaged normal onto the locally averaged surface (trilinear blend of the eight nearest cell averages, weighted by vertex count and by agreement of normals). The reach of that filter is in world units, so the effect is the same at any triangle count; a test compares 82k and 328k triangles. One edge-neighbour pass then removes fine noise. Sparse dabs use edge-neighbour passes only, as before.

### Allocation behaviour

`StrokeProcessor`, its `EditRegion` and `NormalUpdater` own all scratch storage (triangle list, vertex list, weights, the edit region's bitsets and lists) and are kept alive for the lifetime of the mesh. After the buffers have grown to working size, a dab performs no heap allocation except for amortised growth of the stroke's undo record. De-duplication uses generation stamps or bitsets cleared through the lists they fill, not hash sets.

### Undo / redo

A finished stroke becomes one `StrokeDelta`: the list of vertices it touched with their positions before and after. Positions are recorded on first touch within the stroke. Undo and redo write the stored positions back, which makes them bit-exact; normals are derived data and are recomputed. Memory per stroke is proportional to the touched area (about 0.1 MiB for a typical stroke on the default mesh), never to the mesh size.

### Renderer

`SceneRenderer` implements `QVulkanWindowRenderer`. `QVulkanWindow` provides the device, swapchain, depth and MSAA attachments, default render pass, per-frame command buffers and resize handling. The engine owns the pipeline and the buffers.

- This trades control for a large reduction in code and risk. Only `SceneRenderer` and `VulkanContext` know about `QVulkanWindow`; `GpuBuffer`, `ResidentMeshBuffers` and `MeshPipeline` take plain Vulkan handles. An engine-owned swapchain can replace it later without touching the other classes.
- All Vulkan objects are owned by RAII types (`GpuBuffer`, `MeshPipeline`) and released in `releaseResources()`, which Qt calls while the device is idle and still alive.
- There is one storage path: page pools (`ResidentMeshBuffers`, see "GPU storage" below). By default they are sized for the whole mesh; a budget (`RenderSettings::gpuBudgetBytes` or `gpuBudgetFraction`) makes them smaller. Until the unification there was also a fully resident path (`MeshGpuBuffers`: one vertex buffer per frame slot plus a static index buffer); it was removed once the pools matched it (README).
- Nothing in the frame loop waits for the GPU. Pools are written only into slots no queued frame reads, and each frame slot has its own page table.
- Per-frame constants (view-projection, eye position, brush cursor, GPU page size) are push constants; the page table and vertex pool are one descriptor set per frame slot.
- GPU time is measured with two timestamp queries per frame slot (`GpuTimer`), read back when the slot comes round again, so measuring never waits.
- The brush cursor is drawn by the fragment shader as the intersection of the brush sphere with the surface. It needs no extra geometry and always matches the real influence region.
- Rendering is on demand: a frame is requested only when the camera, the cursor or the mesh changed.

### The editor boundary

The editor reaches the engine only through `app::AppController` (input as world-space rays, output as a `DirtyVertexSet`) and reaches the renderer only through `render::RenderScene` (mesh pointer, camera pointer, cursor, pending dirty vertices).

### Dirty pages and GPU pages

`geo::DirtyVertexSet` is a bitset with one bit per page of vertices; the page size is a parameter. Sets created by the mesh use the vertex page size, so "dirty page" and "geometry page" are the same thing. Edits mark pages; the set is drained from the stroke processor into the controller's pending set, which the renderer consumes under the document lock.

On the renderer side `ResidentMeshBuffers::markDirty` accepts a set with any page size, maps its vertex runs onto GPU pages and marks those pages dirty in the vertex `PagePool`. The next frame that needs a dirty page copies it from the geometry pages into a fresh slot (copy on write, below).

Index data is copied once per chunk into the index pool, in triangle id order within the chunk.

### Chunk culling

Every frame, under the document lock, the renderer reads each chunk's box from `ChunkedBvh` (the root of the chunk's local tree, refitted by every edit, so it follows the deformation) and tests it against the six planes of the view frustum (`render::Frustum`, planes taken from the view-projection matrix in Vulkan clip space). The test is conservative: a box is rejected only when it lies entirely outside one plane, so near frustum corners some invisible chunks are drawn, never the reverse.

**Back-facing chunks.** With `CullMode::On` (the default) a chunk that passes the frustum test is also skipped when its normal cone proves that every one of its triangles faces away from the eye, so that the rasterizer's back-face culling (counter-clockwise front faces) would discard all of them anyway. The test (`geo::coneFacesAway`) is conservative: it takes the worst normal in the cone against the worst point of the chunk's bounding sphere and requires the result to clear a margin of 1e-3 of the distance, so any chunk with a triangle near edge-on is drawn. Cones wider than about 84 degrees are not kept and reject nothing. The renderer reads each cone under the same lock and at the same moment as the chunk's box, both published by the sculpt thread's refit, so the two always describe the same mesh. One case remains where the mesh the cone describes is not the one the GPU would draw: a resident vertex page that was edited but not re-uploaded yet (copy on write waiting for a free slot). A chunk with such a page is never rejected as back-facing (`ResidentMeshBuffers::hasStaleCopy`); the self-test reports how often that happens (`backFacingKeptStale`). `CullMode::Frustum` keeps the frustum test only; `--normal-cones off` also stops the sculpt thread from computing cones.

Under a budget, back-facing chunks also stop competing for pool slots and upload time: pages are requested only for chunks that are drawn.

**Draw calls.** Each drawable chunk is an index range in its index pool slot. `ResidentMeshBuffers::buildRuns` sorts those ranges by slot and merges each one that starts where the previous ends (`mergeIndexRuns`, unit tested). Slots hold a full chunk's capacity, so consecutive slots of full chunks merge. Pools sized for the whole mesh are filled in chunk order at startup, so chunk *c* sits in slot *c* and stays there (index data never changes): a full view is one draw call, and a zoomed view at 5.2M triangles a few dozen, as with the old static index buffer. Under a budget, slots are reused out of order and merging finds fewer runs. `CullMode::OnUnmerged` draws one call per chunk, to measure what merging saves.

Nothing is duplicated: shared vertices stay shared, culling only selects index ranges. The self-test renders four views with each mode (off, frustum only, frustum + back-facing, one draw per chunk) and requires identical images, then repeats the comparison in four more views after strong deformations (large Draw and Move strokes from both sides, which fold the surface and leave some chunks without a usable cone), after undoing them and after redoing them, and checks after each stage that every published cone still holds every triangle normal. Unit tests check the cones against a fresh computation after every incremental refit, that parallel and serial refits give bit-identical cones, and, by brute force over many eye positions, that a rejected chunk has no triangle facing the eye. Measurements are in the README.

### GPU storage: page pools

The CPU copy is complete and is the only source: everything uploaded is copied from the geometry pages at the moment of upload. By default the pools hold the whole mesh; with `gpuBudgetBytes` (or `gpuBudgetFraction`) set, they hold part of it.

**Default sizing.** One vertex slot per GPU page plus spare slots (an eighth of the page count, at least 64), and one index slot per chunk. That is 31.9 MiB at 1.3M triangles and 127.5 MiB at 5.2M (the old fully resident path used 45 and 180 MiB: it kept one vertex buffer per frame in flight). `uploadAll` fills both pools in chunk order when resources are created (25 ms at 5.2M), so the first frame is complete. Afterwards every page stays resident; an edited page moves to a spare slot and its old slot is freed once no queued frame reads it. If more pages change within one frame-slot cycle than there are spares, the extra pages are drawn one frame stale and the renderer requests another frame; if no free slot is retired, `PagePool` evicts the least recently used page not in view, which is uploaded again when it is next visible. Strokes in the self-tests caused no eviction.

**What a chunk needs.** `ChunkPageMap`, built once (topology is fixed), lists for every chunk the GPU vertex pages its triangles reference: sorted, unique, 7 to 8 per chunk of 4,096 triangles, but only 2.1 to 2.3 distinct pages per chunk over a hemisphere because neighbours share them. Shared vertex pages exist once on the GPU and are referenced by every chunk that needs them.

**Storage.** Two pools of fixed-size slots: a vertex pool (one GPU page per slot: 1,024 positions, then 1,024 normals) and an index pool (one chunk's indices per slot). Under a budget they are sized in the proportion one full copy would use. The index data holds global vertex ids. A vertex-pulling shader (`mesh_paged.vert`) finds a vertex through a per-frame-slot page table: `slot = pageTable[id >> pageShift]`, then reads the pool. Because indices are global ids, moving a vertex page to another slot changes one table entry and no index data.

**Which pages.** With a budget, visible chunks are processed nearest first, so that when not everything fits the closest chunks get memory and upload time; with whole-mesh pools they stay in chunk order. For each, every page it needs is requested from `PagePool`; the chunk is drawn this frame only if all of them and its index slot are usable. `PagePool` (no Vulkan code, unit tested) decides:

1. **Frames in flight.** Every use stamps the slot with the frame number; a slot may be reused only when its stamp is at least *frame slot count* frames old, i.e. when the frame that last read it has certainly finished (Qt has waited for that frame slot). No GPU wait is ever needed.
2. **Edits.** A resident page that was edited is never rewritten in place: it is uploaded to another slot and its table entry switches (copy on write); the old slot retires normally. If no slot is free yet, the old content is drawn one or more frames stale and the renderer keeps requesting frames until it catches up.
3. **Reserve.** Spare vertex slots can only be taken for copy on write: under a budget an eighth of the pool, in whole-mesh pools the slots beyond the page count. Without it, a view that fills the pool stamps every slot every frame, no slot ever retires, and edits never reach the screen. The pressure self-test found exactly that.
4. **Full pool.** Once a pool has no slot left for this frame, a farther chunk is drawn only if all its pages are already in use this frame; requesting more would claim slots for a chunk that cannot complete.
5. **Eviction** takes the least recently used retired slot. An evicted page needs no bookkeeping: whenever it is requested again, it is copied from the current CPU data, edits made in the meantime included.
6. **Upload limit.** At most `maxUploadBytesPerFrame` (32 MiB) is copied per frame; the rest is deferred to the next frames, which the renderer requests itself.

**Memory placement.** Pools and page tables are persistently mapped and written by the CPU at the start of the frame that uses them. They are allocated in device-local, host-visible memory (resizable BAR) when the driver offers it, otherwise in host memory. Measured at 5.2M triangles, full view: 10.6 ms of GPU time with the pools forced into host memory (light load); in GPU memory about 0.6 ms per pass with the GPU at full clock (README).

**What is verified.** Unit tests cover the frustum (including a brute-force check that no chunk with a visible triangle is culled), the chunk page lists and runs, and the pool: LRU order, the frames-in-flight rule, copy on write, the reserve, stale fallback, the upload limit, edits to evicted pages, and a randomized 3,000-frame simulation with a model of frames in flight that checks no upload overwrites a slot an unfinished frame reads and that every drawn page is the right one. The pressure self-test drives the real viewport through views, camera motion, a live stroke, undo and redo with whole-mesh pools and under budgets of 90% and 25%; captures where the view fits are compared pixel for pixel with a reference run. The reference images used for the unification came from the old fully resident path; the standard self-test's ten scripted captures (sculpting, undo, redo, camera moves) and the pressure captures matched them exactly, and with whole-mesh pools every capture is also checked for full coverage (every visible chunk drawn, no stale page).

**Vertex order, measured.** Numbering vertices by first use in the chunk-ordered triangles was tried, to make chunks need fewer pages. It changed pages per chunk little (9% fewer at 1.3M triangles, 4% more at 5.2M), did not reduce the distinct pages a view needs (2.28 vs 2.26 per chunk over a hemisphere at 1.3M), and a brush dab then dirtied 15 to 20% more pages. The k-d vertex order was kept.

### Threading

Three kinds of threads:

| Thread | Does | Owns |
|---|---|---|
| GUI thread | Qt events, camera, toolbar, frame recording (`QVulkanWindow` renders here) | camera, cursor, brush settings, GPU buffers |
| Sculpt thread (one, inside `AppController`) | every mesh edit: strokes, undo, redo, BVH tidy-up | the right to write the document |
| Helper threads (`core::JobSystem`) | chunks of per-vertex loops, on request of the sculpt thread | nothing |

**Commands in, notifications out.** `AppController`'s public functions push a command (begin / update / end stroke, undo, redo) on a queue and return; measured cost is about 0.0001 ms per input. The only work done on the caller is the hit test of `beginStroke`, because the editor needs to know immediately whether the click was on the model. After each command the sculpt thread calls a listener, which the viewport uses to post one coalesced "redraw" to the GUI thread.

**One lock for the document, held only to publish.** Vertex positions, normals, the BVH and the pending dirty set are guarded by `AppController::documentMutex()`. Readers (the renderer's upload, hover picking, `bounds()`) take that lock. Topology never changes, so indices and adjacency are read without it.

The sculpt thread is the only writer of the mesh, the BVH and the controller's dirty set: strokes, undo and redo, and the background BVH tidy-up all run on it. So it may read them without the lock, and every dab is split in two (`StrokeProcessor`):
- **Prepare**, without the lock: gather, undo recording, the brush, the normals and the BVH leaf queueing. The brush works on a staging view (`DabContext::positions`): a private page table in which every vertex page holding a vertex of the dab points to a copy made for the dab, and every other page to the mesh's own page. A brush therefore keeps its simple code (read and write by vertex id, several passes that see each other's results), and the normals read the same view, so they see the proposed positions of moved vertices and the current positions of all others, at the cost of an ordinary paged read. `ChunkedBvh::markTriangles` queues leaves and chunks but changes nothing a query, `bounds()` or `chunkCone()` reads.
- **Commit**, between the stroke processor's commit hooks, which the controller uses to take and release the document lock: write the moved positions and the recomputed normals into the mesh, refit the queued BVH boxes and the normal cones of the same chunks (`ChunkedBvh::finishMarkedRefit`), then, in the leave hook, hand the dirty vertex pages to the renderer's pending set. A reader therefore sees the document either before or after a dab, never in between, and waits for at most one commit instead of one dab.

Stroke commands take the lock for nothing else. Undo and redo queue their BVH leaves before taking it (`markTriangles` over the edit region of the history entry's vertices, read with `UndoStack::undoVertices` / `redoVertices` without moving the cursor), then hold it to restore the positions, update the normals and finish the refit. With `AppConfig::historyNormalsBeforeLock` (`--history-normals before`, opt-in) they also compute the normals before the lock. They stage the history entry's positions in the dab staging pages (`PositionStaging`, which `StrokeProcessor` lends between strokes), so moved vertices read the entry and all others the mesh. The affected vertices' normals come from that view with `computeNormals`, the same arithmetic as `recomputeNormals`, and under the lock they are only copied in. Positions, normals, boxes, cones and dirty pages are still published under one lock acquisition. `HistoryStagingTests` checks the result bit for bit against computing under the lock, and checks a concurrent reader. Refit + cones is now most of the remaining hold (`ChunkedBvh::RefitTimings` times each chunk's box refit and cone inside the same parallel loop, and `Bvh::RefitStats` splits the box refit into leaf boxes, inner nodes and parent queueing). A chunk whose refit queued at least half of its leaves sweeps its whole tree in node order (dense box refit, default; `Bvh::finishRefit` with `allowDense`): every leaf, then every inner node from the last index down. Boxes are minima and maxima, so unqueued nodes get the box they already have and the result equals the level-by-level refit bit for bit. It reads nodes, leaf contents and vertices in memory order and skips the level lists, which makes the box refit 2.3 to 2.7 times faster for undo at 21M (`DenseRefitTests`). A background BVH step holds it for one step. Results are bit-identical with the previous whole-dab locking, and with and without helper threads: the staging view gives the brushes the same values, and `computeNormals` performs the same arithmetic as `recomputeNormals`.

The staging pages cost about the dab's footprint rounded up to whole 12 KiB vertex pages (copied once per dab), plus one pointer and one byte per vertex page.

**Fairness.** A plain mutex lets the thread that just unlocked re-lock at once, which would let a busy sculpt thread starve the renderer. `core::FairMutex` queues every locker behind a second mutex, so a waiting reader always goes next. A reader therefore waits for at most one commit (see README "Prepare and commit" for the measured holds and waits).

**Consistency of what is drawn.** The renderer takes the pending dirty pages and copies the vertex data under one lock acquisition, so a vertex buffer always holds one complete state of the mesh, never half of a dab. A test runs strokes, undo and redo while a second thread mirrors the mesh from the dirty pages alone and checks that the mirror ends up identical. Another (`PrepareCommitTests`) locks the document as often as it can during strokes and checks every state it sees against the states a serial run passes through between dabs (a hash of all positions and normals), that its mirror built from the dirty pages equals the mesh at every read, and that normals, BVH boxes and cones match the positions. Moving any write of a dab out of the commit, or splitting the commit in two, makes it fail.

**Background BVH work.** After a stroke the sculpt thread calls `Bvh::optimize` with a budget of 4,096 triangles per step, re-checking the command queue between steps. A new stroke can start at any time; nothing waits for the tidy-up.

**Parallel loops and determinism.** A dab's per-vertex loops (brush, normals, BVH level updates) go through `core::parallelFor`. Each chunk writes only its own elements and reads data that no chunk writes, so there are no locks inside a dab and the result does not depend on the number of threads. Reference tests run the same strokes with and without helpers and require bit-identical positions, normals, undo records and BVH boxes. Loops below 4,096 elements stay on the sculpt thread; waking helpers for less made edits slower in measurements.

**Parallel gather (default with helpers).** The gather is the dab's first step: the sphere query, then each vertex of the candidate triangles tested once, at its first occurrence, in triangle order. Its order fixes the order of the vertex list, the weights and the undo record, and through them the floating-point sums of every later step, so a parallel version must give exactly that order (`StrokeProcessor::gatherParallel`):
- **Query.** `ChunkedBvh::querySphereChunks` runs the top-level traversal of `querySphere` and records the chunks it would search, in order. Each chunk's local query runs as its own task into its own list, and the lists are concatenated in chunk order: the serial query's triangle list.
- **First corners.** Corner k of the t-th candidate triangle has the index 3t + k. For every vertex, an atomic minimum (`std::atomic_ref` compare-exchange on the per-vertex stamp array) leaves the index of its first corner. The minimum does not depend on which thread wins a race, so the result is the serial gather's "first time seen".
- **Selection.** The triangle list is cut into fixed blocks of 2,048 triangles, independent of the thread count. Each block keeps the corners that are their vertex's first corner, tests the distance, computes the falloff, and writes its own inside and outside lists. Prefix sums over the blocks place them, and the concatenation is the serial vertex and weight list. Each vertex is in exactly one block, which then resets its stamp to "no corner".
- **Undo record and average normal** stay serial: the undo record is about 0.3 ms at 21M and a blocked version was slower; the average normal is an ordered float sum.

Small dabs keep everything on the sculpt thread: the query below 8 chunks hit, the selection below 32K candidate triangles, and the whole gather when there are no helpers. The serial fallback runs the serial loop with the same stamp meaning. The two modes give bit-identical results (`ParallelGatherTests`: serial against 0, 1, 3 and 11 helpers, deltas, every dab's hash and exact undo/redo; a reordering of the blocks fails it). `setParallelGather` may be switched between strokes; it resets the stamps. Measurements: README "Parallel gather".

**Parallel leaf marking (default with helpers).** `ChunkedBvh::markTriangles` queues, for every affected triangle, its leaf in its chunk's local tree (once per leaf, with the ancestors flagged up to the root) and its chunk in the refit queue (once per chunk). A chunk's local tree is written only by marks of that chunk's triangles, so chunks can be marked in parallel if each sees its own triangles in the serial order, and the refit queue, which also fixes the order of the background optimize queue, keeps the order in which the serial loop first reaches each chunk (`ChunkedBvh::markTrianglesParallel`):
1. The triangle list is cut into fixed blocks of 8,192 triangles. Per block, a task lists the chunks it reaches in first-occurrence order with a triangle count each (a spatially ordered list jumps chunks only every few dozen triangles, so these lists are short).
2. Serially, over those short lists in block order: each chunk joins the refit queue the first time it appears, which is the serial order; then each chunk gets a range of a bucket array, and each block's share within it, in block order.
3. Per block, a task copies its triangles into their chunks' ranges: a stable counting sort, so every range holds that chunk's triangles in list order.
4. One task per queued chunk runs `beginRefit` and `markTriangle` over its range.

Lists under 32K triangles, and a job system without helpers, use the serial loop. Undo and redo use the same path, before they take the document lock. `ParallelMarkingTests` compares the two at 0, 1, 3 and 11 helpers: boxes, cones, picking before and after the background optimize steps (compared after every 512-triangle step, so a different queue order would show), with a shuffled list that reaches chunks out of index order and has duplicates, and through Draw, Smooth and Move strokes with exact undo/redo. A third test runs undo and redo through the controller with parallel and serial marking and compares hash, picks, boxes and cones after every step. Reversing the chunk order or dropping a chunk's marks fails them. The order of marks inside a chunk does not change any box (each node is recomputed from its children), so the tests cannot observe it; it is preserved by construction.

**Parallel edit region (default with helpers).** `EditRegion::collect` must give the triangle list in the order the serial walk first meets each triangle (moved vertices in list order, each one's incident row in order), and the vertex list as the moved vertices followed by the other corners in the order the triangle list first shows them; the BVH refit and the normals read these lists, so a different order would change results. With helpers and at least 16K moved vertices (`EditRegion::collectParallel`), one `u32` stamp per vertex (thread-local, kept) records first occurrences by atomic minimum:
1. Each moved vertex's stamp becomes its first index in the moved list (flagged as moved).
2. Per block of 2,048 moved vertices, a task keeps the vertices whose stamp is their own index and the incident triangles whose owner, the smallest moved stamp among their corners, is that vertex, in row order.
3. Block sizes are joined by a prefix sum into the triangle list.
4. Each unmoved corner's stamp becomes its smallest position `3·i + k` in the triangle list.
5. Per block of 4,096 triangles, a task keeps the corners whose stamp is their own position; a second prefix sum appends them after the moved vertices.
6. The stamps are reset through the lists.

Smaller edits and a job system without helpers use the serial walk with its bitsets. Undo and redo use the same path before the lock. `ParallelRegionTests` compares both lists with the serial walk element for element and strokes and undo/redo bit for bit.

**Why a small job system instead of oneTBB.** The need is a blocking parallel-for from one thread. `core::JobSystem` is about 150 lines, has no dependency and builds everywhere the project does. `parallelFor` is the only entry point used by the engine, so oneTBB can be substituted behind it if work stealing or task graphs become necessary.

**What is not threaded.** Rendering stays on the GUI thread because `QVulkanWindow` renders there; a render thread requires an engine-owned swapchain. The gather's vertex selection, the BVH leaf marking and the edit-region walk are parallel (above); the de-duplication inside `NormalUpdater` (undo/redo) and the undo record are still serial (README).

**Verification limits.** No race detector was run (ThreadSanitizer is unavailable with the MinGW toolchain used). The evidence is the design above plus the tests: equality with the single-threaded result, the mirror test, repeated runs, and destruction with queued work.

## Extension points for later phases

None of the following is implemented. The list records where each is meant to attach, so the core does not need rewriting.

| Future system | Where it attaches | What already supports it |
|---|---|---|
| Spatial order for imported meshes | `geo::sortSpatially` | Already applied to generated meshes; an importer calls the same function. |
| oneTBB | `core::parallelFor` | It is the only parallel entry point the engine uses. |
| Parallel de-duplication elsewhere | `NormalUpdater` (undo/redo) | It still de-duplicates serially. The gather's first-corner scheme applies to it as well. |
| Render thread | `SceneRenderer` | The document lock and dirty pages already define what a renderer on another thread may read and when. |
| GPU compute sculpting | a `Brush` or `StrokeProcessor` backend | Brushes are expressed as data in, data out (`DabContext`); a compute backend consumes the same inputs. The renderer would then own the authoritative vertex buffer for the edited region, so the dirty-range direction reverses. |
| Geometry streaming (SSD) | the page tables of `PagedArray` / `PagedAdjacency`; `PagePool` | GPU residency exists: a chunk is drawn only when its pages are resident. CPU pages are all in memory; streaming them needs a resident/absent state on the CPU side and readers (brushes, BVH) that tolerate absent pages. |
| Level of detail, occlusion culling | `SceneRenderer::selectChunks`, `ResidentMeshBuffers::prepare` | Chunks are already selected and ordered per frame. An opt-in coarse fallback exists behind `--lod` (`geo::buildCoarseChunks`, `CoarseMeshBuffers`): border-locked simplifications of chunks (or groups of chunks, `--lod-group-chunks`) drawn for visible chunks whose detailed pages are not usable, allocated only under a budget that cannot hold the mesh, exact-size storage with a few copy-on-write spares, one multi-draw-indirect call per pass, drawn two-sided. Coarse topology is fixed at load time (re-simplifying edited groups is the open item); see its notes before building on it. |
| Growing or shrinking the mesh | `PagedArray::resize` | Pages are added and released without moving existing elements. Adjacency and the BVH assume fixed topology and would need matching updates. |
| Multiresolution | above `geo::Mesh` | Requires a level structure and displacement storage; brushes would run on the active level. |
| Sparse SDF / voxel remeshing, mesh to voxel, voxel to mesh | a sibling of `geo::Mesh` in Geometry, converted through free functions | Sculpt and Renderer depend on `Mesh`, `Bvh` and `DirtyVertexSet` only. A remesh produces a new `Mesh`, which is why changing topology must go through `AppController` (it has to rebuild the BVH, reset the stroke processor and tell the renderer to recreate buffers). That path does not exist yet. |
| Tracy profiling | `core::Stopwatch` call sites | The interactive path is already timed at the controller and renderer boundaries. |

## Main technical risks

1. **Serial parts of a dab** are the scaling limit today: an edit costs about 0.1 microseconds per vertex under the brush and helpers remove at most a third of that, so very large brushes on dense meshes still take several milliseconds. The interface no longer waits for them, but the stroke lags.
2. **Fixed topology and fixed chunk membership** limit how far a shape can be pushed before triangles and chunk boxes degrade. Solving it (dynamic topology or voxel remesh) invalidates the "topology never changes" assumption listed above.
3. **Memory per triangle is about 78 bytes** (mesh 50, acceleration 28), more than half of the mesh part being adjacency. One hundred million triangles would need about 8 GiB before any GPU memory. Compact adjacency (now the default: mesh about 40 bytes per triangle) is a first step; on-demand neighbour lists and BVHs are further prerequisites for the long-term target.
4. **Mapped memory placement depends on the device.** The pools prefer coherent, mappable device-local memory and fall back to coherent host memory, which is several times slower to draw from. On a GPU without resizable BAR (a 256 MiB mappable heap) large meshes would land in host memory; a budget that fits the heap may then be faster than whole-mesh pools. Not tested on such a GPU.
5. **No validation-layer run yet.** Vulkan usage was exercised on one NVIDIA driver only.
6. **Threading correctness is tested, not proven.** No race detector has been run.
7. **A budget smaller than the view leaves holes.** The nearest chunks are drawn and the rest are missing; there is no level of detail to fill in.
