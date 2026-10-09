# Segment structures: notes

Branch `segment-structures`, from `platform-cube-lattice` at 1263ce0. Not pushed.

A segment (an `FGridPlatform`) is now built as beams and posts around an opening, and the opening is
filled by **structures**: meshes modelled in Blender, exported through the WEM add-on and imported as
`UPlatformStructureData` assets. The placeholder interiors are gone. How to author and import
structures is in `SourceArt/Structures/README.md`.

![Overview](Screenshots/overview.png)

## What was built

| Phase | Commit | What |
|---|---|---|
| 1 | 429c544 | `UStructureCategory` and eight categories, `UPlatformStructureData`, the pure placement transform (`WEMStructures`), the Blender add-on, a first importer, the calibration F. |
| 2 | 404e0de | Fills drawn in the same beat as their segment, from a stream of their own; the no-fill rule; holds items; one chunked piece layer per structure; placeholders and the tile's `Mesh` and `BlockedFace` removed. |
| 3 | 719c971 | The category pull: faces meeting round shared edges in turning order, per-category streaks, the draw with offers, `CategoryPatchSize`. |
| 4 | 0d2d932 | Load-bearing: `BlockedEdges`, beams and posts built only where something uses them unblocked, beams that are not built are not there. |
| 5 | 3a1e269 | Fences: run ends, the queue, fence beats, normal beats landing on a waiting spot, a light floor fence crossing its own blocked edge. |
| 6 | b857940 | The full importer, `categories.json`, the README, the test kit (69 structures) through the pipeline. |
| 7 | this commit | Materials that draw instanced, fresh re-imports, a beat benchmark, these notes. |

The calibration F, laid by the C++ transform on a floor, a ceiling and walls facing each way, read from
the side each faces:

![Calibration](Screenshots/calibration.png)

## Settings and what they do

On the room manager, under **Grid | Structures**:

| Setting | Default | What it does |
|---|---|---|
| `Structures` | - | The structures growth fills segments from, each drawn by weight among those that fit. A tile no structures fill completely, laid a given way, is not laid that way; with none for `FirstTile` laid flat and load-bearing, nothing grows (logged). |
| `ContinueChance` | 0.35 | How likely a face continues a neighbour's category whose streak is 1. |
| `MaxContinueChance` | 0.70 | The chance once a streak is `StreakForMaxChance` long. |
| `StreakForMaxChance` | 4 | Where the chance stops rising. |
| `FenceContinueChance` | 0.8 | How likely each end of a fence panel carries its run on. A run is about 1 + 2p / (1 - p) panels. |
| `FenceTurnChance` | 0.25 | How likely a wall fence that could go straight or turn the corner turns. |
| `CategoryPatchSize` | readout | Size-weighted mean size of same-category face patches. |

The probe (`DebugProbe*`) now also shows the structure on the face, its categories with their streaks
on that face, and whether it is a fence panel.

On each structure: types, two-sided, categories, load-bearing, holds items, fence, may lie sideways,
weight (see the README). On the importer: source and destination folders, the category list file,
the material folder, the Nanite threshold, `bReimportAll`, and the `LastReport` readout.

## Measurements

**Drift.** `wem.RoomManager.VerifyEvery 1` over 1000 placements from empty, with one- and two-sided
fills, categories, light structures and fences together: 0 drift
(`WEM.Structures.Fill.VerifyEvery1000`, a stress-filter test).

**Growth with light structures** (phase 4, `WEM.Structures.LoadBearing.Stall`, a 129-cell cube grown
to its end, six seeds): all load-bearing, growth stops at 3464-3590 platforms (median 3488); with 5% of
the fill weight on structures that are not load-bearing, at 3346-3461 (median 3415), about 2% fewer,
with about 170 light segments placed. Growth still ends because the cube fills, not because blocking
walls it in.

**Fence runs** (phase 5, `WEM.Structures.Fence.RunLength`, a floor fence started alone 3000 times):
mean run 9.08 panels against 1 + 2p / (1 - p) = 9 at p = 0.8. In ordinary growth runs end sooner,
on blocked spots: the editor run below had 396 railing panels in 49 runs (8.1 a run).

**Category patches** (`WEM.Structures.Perf.CategoryPatches`, the test kit, NewMap's own tiles, 3000
platforms, four seeds):

| ContinueChance / MaxContinueChance | CategoryPatchSize (median) |
|---|---|
| 0 / 0 (no pull) | 1.8 |
| 0.15 / 0.35 | 4.6 |
| 0.35 / 0.70 (defaults) | 22.0 |
| 0.50 / 0.90 | 125.6 |

These match the 2D stand-in simulation (1.7 with no pull, 22 at the defaults). With fences growing
(the screenshot run, where 8 x 8 walls and turned 8 x 16s were allowed), the defaults gave 350 at
2982 platforms: a fence run carries its category panel after panel, and the faces around it are
pulled toward it.

**Beat time** (`WEM.Structures.Perf.Beat`: NewMap's setup - a 509-cell cube, its three tiles, pieces
built - timing beats 2000 to 3000, best of three, three interleaved rounds against the same benchmark
built from 1263ce0 in a separate worktree):

| | ms a beat |
|---|---|
| Before structures (1263ce0) | 1.295 |
| Plain two-sided slabs | 1.363 (+5.3%) |
| The test kit | 1.406 (+8.6%) |

Within the +10% asked for. The performance notes' 0.7 ms a beat was measured on 2026-09-23 with a
single tile size, before tile variation, the flush rule and obstacles, so it is not a like-for-like
baseline; in the editor during Simulate the beat at about 2800 platforms measured 1.9-2.1 ms with the
object placer running beside it.

## Screenshots

| | |
|---|---|
| Inside a room | ![Room](Screenshots/room.png) |
| A facade (upright, not load-bearing) | ![Facade](Screenshots/facade.png) |
| A railing run | ![Fence run](Screenshots/fence_run.png) |
| A parapet going round a corner | ![Parapet](Screenshots/parapet_corner.png) |

All from one Simulate of NewMap with the test kit, seed 7, paused at 2982 platforms. NewMap's tiles keep
8 x 8 flat only and 8 x 16 from turning, so as saved neither test fence can grow there; for these runs
8 x 8 was allowed on walls and 8 x 16 to turn. Those changes were not saved.

## Decisions the brief did not settle

- **A two-sided fill faces the positive normal** (up on a floor, +X or +Y on a wall). The platform stores
  only the asset on each face, so the facing is not saved; the 50/50 first face on a wall only decides
  which face's neighbours pull first.
- **A structure that fits as authored is not turned**: a square structure on a square opening keeps its
  orientation. "Turned" means the structure's length runs along the segment's first axis.
- **An end that reaches a panel of its own fence is closed.** Following the brief's rules alone, a
  parapet that closes round a roof leaves one end facing its own first panel, and that end would carry
  on into the air. With this rule the loop closes (`WEM.Structures.Fence.Parapet`).
- **The end of a panel that carries a run on is not an end of the run**, so runs do not branch at joints.
- **Category patches are joined when a face is filled and never split**, so a wall that later stands on a
  seam does not divide the floor patch it stands on (the streaks are fixed at fill time anyway).
- **Where only one way round of a tile can be filled**, the placement is drawn by place among the
  fillable ones; where every way can, the draw is the one it was before.
- **Imported materials are the project's own.** The engine's glTF pipeline makes instances of parent
  materials in the engine's content, which may not be drawn instanced, so every structure drew the
  default checker. The importer now leaves that pipeline out, so Interchange builds plain materials,
  and turns on instanced use for every project material a structure uses.
- **A changed structure is imported afresh**: its old mesh is deleted first, since a re-import only
  looks for the materials the mesh had. Only its structure asset referenced it.
- Materials of the same name in one size folder are shared (the test kit's category colours are).
- The add-on was written in phase 1, so the calibration went through the real export from the start.
  Its list of tile sizes is fixed in the add-on (8x8, 8x16, 16x16, 16x32, 32x32).
- A structure asset's types default to all three; one with no mesh is left out with a warning, one with
  no type or no weight quietly.
- An existing `DA_Structure_<Name>` of another size is an import error rather than being moved.

## Known limits

- Two-sided structures always show their front on the positive side.
- Wall fences may run off into empty space where nothing stands - gravity is optional, and standing is
  only preferred. The overview shows a long railing leaving the structure.
- A re-import deletes and recreates the mesh; anything but the structure asset that referenced it loses
  the reference.
- A structure material from outside the project draws the default checker; the import report says so.
- The fence queue is beat state and is not saved, as asked.
- Each blocked edge records the first platform to block it; only the fence-run exception ever lets a
  second platform take a blocked edge.
- `Content/Placeholders` was deleted - nothing referenced it. The three `MI_WEMDebug_*` instances in
  `/Game/Debug` that only it used are left in place.
- NewMap's room manager lists the 69 test-kit structures (the importer added them). Remove them when
  the real structures arrive.
