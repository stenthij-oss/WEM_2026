# Zones and deletion

Asked for on 2026-10-15 and built the same day on branch `nature`, after the paths. Growth is constant; now things also go away.

## What was asked, and how it reads in the code

| Ask | Built as |
|---|---|
| Divide the cube into 9 sub-cubes, zones of influence, with a debug view in 9 colours, off by default | Nine **zones**, three by three across X and Y, each the full height of the cube, their borders on lattice lines a third of the way across (`GetZoneLine`). A platform is in the zone its middle lies in. *Show Zones* draws each as a box of lines in its own colour, with a cross over its floor and roof (`ZoneColors`, nine defaults; the fifth is the middle zone). |
| Multiple first tiles, one per zone, random within it; the middle zone's as now; everything lines up | **Seeds** (`bSeedEveryZone`). The first beat lays the middle one at the heart of the cube as before. The next eight beats lay one in each other zone, a zone a beat. Each lies flat at a random place in its zone, on whole tile-widths (X), tile-lengths (Y) and shorter sides (Z) away from the middle one, so where their growth meets it lines up on the same tile grid. It must be clear of everything standing and of all held-back air, and touch nothing, so it starts growth of its own. 32 places are tried before the zone goes without. |
| Each segment has health 80-120 | Every platform is laid with health drawn evenly between `MinHealth` and `MaxHealth` (the damage stream). Beat state, not saved. |
| Every 1-3 s, 10-20 damage to about 10% of all segments; at 0 it is removed, no effects | A **strike** every `MinStrikeInterval`-`MaxStrikeInterval` s. Each platform is hit with `StrikeFraction` (0.1), for `MinDamage`-`MaxDamage`. A platform with no health left is queued and taken away, nothing more. |
| Every 10 s, in a random zone, a variable number of segments holding no objects are deleted while new ones are laid elsewhere in the zone; this respects paths, pockets and molds | A **reshuffle** every `ReshuffleInterval`: a zone is drawn, then 4-12 of its platforms with no object standing on either face (`MinReshuffleCount`/`MaxReshuffleCount`). Every `ReshuffleStepInterval` (0.25 s) one is taken away, and a platform is laid in the same zone as a beat would lay one, so by every placement rule, paths, pockets and molds included. It is laid away from every spot the reshuffle has emptied so far. |
| Molds and pockets switch positions every couple of minutes; each has a half-life and breaks down; not the air over ground | Each pocket and mold draws a **life** so that half of them have broken down by `PocketHalfLife` (120 s), and none sooner than `MinPocketLife` (30 s). Then it **breaks down** from the top, one slab of the lattice (a module deep) per clock tick. Once gone, it **forms again** somewhere else from the bottom, a slab a tick, the same shape and size. One moves at a time. Molds stay near the middle (`MoldSpread`) and clear of each other; pockets stay clear of molds. The air over ground and water never moves. |
| Segments inside a pocket/mold that has moved there: every hit does 60% more damage | **Vulnerable**: a platform crossing any held-back air (a pocket, a mold's held-back box, a path, or the air over a landscape) takes `VulnerableDamageMultiplier` (1.6x) from a hit. |
| No new segments inside a pocket, but inside the mold's shape, yes | As before: the mold's open shape and floor stay buildable, the rest of its box and every pocket hold new placements back. |
| Accelerate placement inside molds by raising their weight | A beat draws a placement whose middle lies in a mold's box `MoldGrowthWeight` (4) times as likely as one anywhere else. Weighted only while a mold stands, so without one the draw is as it was. |
| Ground air pockets grow like a mushroom the more ground and water connects, more free space in X and Y, higher and wider | **Mushroom air** (`bMushroomAir`). A **landscape** is ground and water joined across their seams in one plane with one Up. For each piece beyond the first, its air rises by `MushroomStoreysPerPiece` (0.25, up to `MaxMushroomStoreys` 4). A **cap** `MushroomCapStoreys` (1) thick at the top of the air reaches `MushroomSpreadPerPiece` (2) cells further past the landscape's edges, in whole modules, up to `MaxMushroomSpread` (48). |
| A segment crossing an airpocket/path/mold air becomes more vulnerable | The same vulnerability rule, for all held-back air, mushrooms included. A mushroom growing over what stands leaves it standing, and lets the strikes wear it away. |

## How deletion works

**Taking platforms away** is a port of the laplacian branch's step B (b0849f5), adapted to nature. `RemovePlatforms` → `RemovePlacedPlatforms` undoes just what the platforms brought:
- Their module faces go.
- Every module edge and rim cell they took in is worked out afresh from what stands around it (`RecomputeModuleEdge`, `RecomputeRimCell`).
- Every promotion that hung on them is let go and worked out again.
- Every index that names a platform is renumbered: module faces, blocked edges, face fills, nature by form, and the platforms standing in each path.
- Their pieces and overlay squares are swapped out of the chunks (`RemovePieceFromLayer`, which needs `PieceSlots`/`ChunkKeys` on every piece layer now).
- Anything waiting on them goes: fence ends, a spilling field's pieces, health, the queue to die.

The candidate lists are refreshed in place (`RefreshCandidatesAfterRemoval`): every placement taking in a module edge of theirs, listed or not, and every placement folding off a run whose promotion changed. Laplacian marked the lists stale instead, which would mean a 0.2-1.3 s regather on every removal here.

The wholesale rebuild stays the reference. `wem.RoomManager.VerifyEvery` checks removals too. `wem.RoomManager.RemoveByRebuild 1` takes platforms away by rebuilding, for comparison. Two rules came with removal, as on laplacian: a placement demotes promoted surfaces it builds against, and a floor's block on a beam holds only while it is the one platform there (so the rebuild keeps a platform standing on a beam blocked before it in the list).

**The clock.** The 0.05 s path clock is now the clock for everything that moves during play (`AdvanceClock`). Each tick it:
- redraws the path or two longest due;
- carries the pocket or mold that is moving on by one slab, or starts the one longest due (`StepPocketMove`);
- strikes when due;
- starts a reshuffle when due, and steps the one under way;
- takes away at most 4 dying platforms together (`MaxRemovalsPerTick`), so a strike costs no single frame much. That is 80 a second at most.

**Held-back air** used to live in the obstacle keys, which the rebuild also asks, so a platform in a pocket would have been dropped on any rebuild. That is why pockets were never moved. Now:
- Pockets and molds count their pieces into refcounted `PocketFaces`/`PocketEdges`/`PocketNodes`, like the paths. Each pocket keeps its pieces slab by slab (`SlabEnds`). Every lattice piece lies in exactly one slab, so a pocket can be let go or cut a slab at a time.
- The air over the fields is a set of boxes, bucketed in 32-cell cubes (`AirBoxBuckets`). A platform is held back by a box exactly where one of its cells lies in it, so asking boxes asks the same thing as the lattice pieces did.

All of it holds back only new placements (`IsClearOfHeldAir` in `IsPlacementOpen`). What stands in it stays, through a rebuild too, and is vulnerable. The obstacle keys are the objects' alone again.

When air moves or changes, two region refreshes keep the lists right without a regather:
- `DropCandidatesIn` judges again the listed placements reaching into what is now held back.
- `FreeCandidatesIn` judges every placement reaching into what was let go that shares an edge with a platform near it. It finds them from the platforms near the region, not by walking the region.

The field air is worked out afresh on every field change (`GatherFieldAir`), but only the boxes that differ are swapped in. A stem that only rises or sinks frees just the slab between, and a cap that only widens frees nothing.

**Readouts:** *Zone Summary* (platforms per zone, at each strike), *Deletion Summary* (strikes, hits and vulnerable hits, taken away, waiting, reshuffles, pocket moves), *Air Summary* (landscapes, boxes, and the largest landscape's mushroom).

**Buttons:** *Strike Segments* (a strike, and the dead taken away at once), *Reshuffle Zone* (a whole reshuffle at once), *Move Air Pockets* (every pocket and mold at once).

## Settings

| Details panel | Property | Default |
|---|---|---|
| Grid \| Zones: Show Zones, Zone Colors | `bShowZones`, `ZoneColors` | off, nine colours |
| Grid \| Zones: Seed Every Zone | `bSeedEveryZone` | on |
| Grid \| Deletion: Enable Damage | `bEnableDamage` | on |
| Grid \| Deletion: Min / Max Health | `MinHealth`, `MaxHealth` | 80, 120 |
| Grid \| Deletion: Min / Max Strike Interval | `MinStrikeInterval`, `MaxStrikeInterval` | 1, 3 s |
| Grid \| Deletion: Strike Fraction | `StrikeFraction` | 0.1 |
| Grid \| Deletion: Min / Max Damage | `MinDamage`, `MaxDamage` | 10, 20 |
| Grid \| Deletion: Vulnerable Damage Multiplier | `VulnerableDamageMultiplier` | 1.6 |
| Grid \| Deletion: Enable Reshuffle | `bEnableReshuffle` | on |
| Grid \| Deletion: Reshuffle Interval | `ReshuffleInterval` | 10 s |
| Grid \| Deletion: Min / Max Reshuffle Count | `MinReshuffleCount`, `MaxReshuffleCount` | 4, 12 |
| Grid \| Deletion: Reshuffle Step Interval | `ReshuffleStepInterval` | 0.25 s |
| Grid \| Air Pockets: Move Pockets | `bMovePockets` | on |
| Grid \| Air Pockets: Pocket Half Life, Min Pocket Life | `PocketHalfLife`, `MinPocketLife` | 120 s, 30 s |
| Grid \| Molds: Mold Growth Weight | `MoldGrowthWeight` | 4 |
| Grid \| Air: Mushroom Air | `bMushroomAir` | on |
| Grid \| Air: Mushroom Storeys Per Piece, Max Mushroom Storeys | | 0.25, 4 |
| Grid \| Air: Mushroom Spread Per Piece, Max Mushroom Spread | | 2 cells, 48 cells |
| Grid \| Air: Mushroom Cap Storeys | `MushroomCapStoreys` | 1 |

## My calls (flagged for the user)

- **"9 sub-cubes" are a 3x3 split across X and Y, the full height of the cube.** A cube splits into 8 or 27 cubes, never 9, and "the middle zone" needs an odd split, so I read it as nine columns. They are columns, not cubes.
- **Seeds line up by keeping to the middle one's tile grid**, which guarantees growth fronts meet flush in NewMap, which only has 16x16 tiles. With mixed tile sizes it still keeps them in step, but growth can meet with steps the flush rule then refuses.
- **Seeds go one a beat, at the start**, keeping one piece per tick. In NewMap that is the first ~0.1 s.
- **Seeds are placed at any height.** If every platform is taken away, the cube is seeded again the same way.
- **"About 10%" is a 10% chance per platform per strike**, so the share varies a little from strike to strike.
- **Dead platforms are taken away on the clock, at most 4 per 0.05 s tick**, rather than in the strike's own frame. At NewMap's defaults that is plenty (see Measurements). If the deaths outrun it, the queue shows in *Deletion Summary* as "waiting".
- **"Hold objects"** means an object placed by the object placer stands on either face, read from the obstacles the placer adds. Damage strikes every platform, those holding objects included, as asked. An object on a platform that dies is left standing: the object placer cannot take objects away yet. NewMap's object placer is inert, so this does not show there yet.
- **"Different sections in the zone"**: a reshuffle never lays a platform across an opening it has emptied. It may share the beams round one.
- **"Half-life of sorts"**: each pocket's life is drawn from an exponential, so half are gone by the half-life. It then breaks down slab by slab from the top and forms again slab by slab elsewhere, keeping its shape and size. It does not shrink. At first a pocket moved in one go, but that cost 50-100 ms frames with NewMap-sized molds, so it moves a slab per tick instead: a 64-cell pocket takes about 1.6 s, and a 128-cell mold about 3.2 s.
- **Where a pocket forms again**: anywhere a pocket could be drawn, molds near the middle. If no clear place is drawn in 16 tries it stays where it was.
- **Mold weighting** counts placements whose middle lies in a mold's whole box, since only the open shape and floor are buildable there anyway.
- **Mushroom shape**: stems over every piece, raised together, and one flat cap over the landscape's bounding rectangle, grown out by the spread. An L-shaped landscape gets a rectangular cap. Cliffs do not join landscapes, so terraces at different heights are separate mushrooms.
- **The mushroom's defaults** keep a landscape of 1-2 pieces exactly as before (no extra storeys, a spread under one module). It grows from 3 pieces on.

## Measurements

`WEM.Deletion.Perf` uses a NewMap-sized cube (509 cells), nine first tiles, the test kit's slabs, six pockets and two molds, grown to 3000 platforms. It then simulates 40 s of play at NewMap's pace:
- 60 growth beats a second (NewMap's 0.0107 s beat at 60 fps);
- a clock tick every third beat;
- a strike every 2 s;
- a reshuffle every 10 s;
- a pocket move every 15 s.

The cube grew to 5,380 platforms. Every step is timed on its own:

| Work | Count | Mean | Longest |
|---|---|---|---|
| Growth beat | 2400 | 1.61 ms | 4.9 ms |
| Strike (all ~5000 platforms) | 20 | 1.26 ms | 1.7 ms |
| Removal tick (up to 4 platforms) | 9 | 5.1 ms | 9.5 ms |
| Reshuffle step (one taken, one laid) | 38 | 3.6 ms | 6.9 ms |
| Pocket move step (one slab) | 105 | 2.35 ms | 16 ms |
| Pocket move all at once (the first version) | 3 | 52 ms | 100 ms |

The older stress tests, after these changes, against their numbers from `NATURE.md`:

| Test | Before | Now |
|---|---|---|
| `WEM.Nature.Perf` city beat alone | 1.45 / 4.1 ms | 1.57 / 6.7 ms |
| `WEM.Nature.Perf` city beat with nature and pockets | 1.74 / 4.8 ms | 1.78 / 5.3 ms |
| `WEM.Nature.Perf` nature beat | 4.5 / 10.2 ms | 4.9 / 19.8 ms |
| `WEM.Nature.Perf` clearing (drawing the pockets) | 13 ms | 15 ms |
| `WEM.Paths.Perf` one path redraw | 1.2 / 4.2 ms | 1.2 / 4.2 ms |

The nature beat's longest doubled. A piece that grows its landscape's mushroom swaps in taller stems and a wider cap, and judges the placements there again.

40 s is too short for the strikes to kill much: health 80-120 at 10% x 15 damage every 2 s gives about 130 s on average to wear a platform down. Only 20 died. Over a long run, deaths settle where they match growth. At about 60 placements a second and a 130 s life, that is up to roughly 8,000 platforms and 60 deaths a second. That fits within the clock's 80 removals a second, but would cost about 75 ms a second of removal work. Lower *Strike Fraction*, or raise health, to calm it.

## Tests

`WEM.Deletion.*`: Remove, Visuals, Strike, Seeds, Reshuffle, Pockets, MoldPull, and the stress test Perf. `WEM.Nature.Mushroom`. **103/103 product tests pass.** The test fixture keeps the old single first tile and no strikes, reshuffles or moving pockets, so the existing tests grow as they did. Two existing tests changed with the rules:
- `WEM.Nature.AirPockets` reads the pocket keys instead of the obstacle keys.
- `WEM.Structures.LoadBearing.Floor` has laplacian's updated assertion: a platform standing on a beam blocked before it in the list is kept.

What the new tests show:
- Removal agrees with a full rebuild, candidate lists, pieces and overlay included, after single and batched removals and growth afterwards.
- 86 of 300 platforms crossing paths die on exactly the second strike, and only they.
- Nine first tiles, one per zone, on the middle one's grid.
- Reshuffles lay nothing outside their zone and never move a platform holding an object.
- Pockets move, keep their shape, and leave what stands vulnerable. A slab-by-slab move agrees with a rebuild at every step and takes 17 ticks.
- A mold weight of 8 puts 39 platforms in the molds against 23 at weight 1.
- A nine-piece landscape's air rises 2 storeys, with a cap 16 cells past its edges. It shrinks when a piece goes and splits in two when a column goes.

## Known limits

- **Objects on a platform that dies stay standing** (see above).
- **Removal costs about what a placement does**, so a cube with a high death rate spends that much more of each frame on the room manager.
- **A strike walks every platform**, but only asks the ones it hits whether they are vulnerable.
- **The field air is worked out afresh on every field change**: about 1 ms per thousand pieces of ground and water, before the swap.
- **The category streaks and patches are history**: a removal leaves them as they were, so *Category Patch Size* counts faces that have gone.
