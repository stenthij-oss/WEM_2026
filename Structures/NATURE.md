# Nature

Brainstormed and approved 2026-10-14, built the same day on branch `nature` (off `Eden-growth` at e07d747), one commit per phase. Not pushed.

## The idea

Every structure now belongs to a **section** (`UPlatformStructureData::Section`): **Urban**, the default, so every structure that existed is urban without being touched, or **Nature**. Nature comes in three **forms** (`NatureForm`): **Ground**, **Cliff** and **Water**. Growth never draws nature. Only the **nature beat** lays it, on a timer of its own.

A nature segment is **one-sided**. Its nature face looks into the open air, and behind it stands any one-sided urban structure of that face's type: a **ceiling** under ground and water, a **wall** behind a cliff. Ground and water lie along their Up, their nature face looking along it. A cliff is a wall. By default ground and water only **lie flat and look up** at the sky, so their cliffs stand upright (`NatureFacing`, see below). A nature segment is always load-bearing, whatever stands behind it.

Each form only **meets** certain faces across a shared beam. "Meets" is the same walk round an edge that the category pull uses: the two faces that bound one region of air there.

| Form | Its face may meet | How |
|---|---|---|
| Ground | ground or water | in its own plane |
| | an exterior (a structure of side Exterior or Both) | in its plane, or folded |
| | a cliff | along the cliff's top or foot (the edges across the cliff's Up) |
| Cliff | ground or water | along its own top or foot |
| | another cliff with the same Up | beside it, above it, or round a corner at its side |
| Water | ground or water | in its own plane |
| | a cliff | along the cliff's top or foot |

So all of a landscape's nature faces look into the same air, and their backs face into the city. A placement is held to this for every face it meets, from both sides. That gives the **city's rule** (the user's decision): the city grows against ground only with exteriors facing it, and never against water or a cliff. An urban placement that would meet water or a cliff is never listed. A face of one that meets ground is drawn from the exteriors, and if no exterior fits, the beat draws a different placement.

**Air.** Every piece of ground and water keeps the air above it open. This is a box of cells out along its Up, as high as it drew between `MinAirStoreys` and `MaxAirStoreys`. It covers the piece's opening and every seam it shares with a field laid before it in its plane, but never its outline, so cliffs can still rise and drop there. The air holds growth back the way a placed object's obstacle does, without changing any surface, so objects still stand on the ground. A piece is only laid where its lowest storeys of air are already clear of every platform.

**Air pockets.** Whenever the cube is cleared (the start of play, or the Clear Platforms button), `AirPocketCount` pockets are drawn at random across it. The shapes are spheres, cubes, rectangular boxes, ziggurats and cylinders. Each pocket holds back every lattice piece it reaches into, so nothing is ever built there.

**Molds** (asked for on 2026-10-15). A mold is a box of air with a shape left open inside it, for the city to grow into. The shape is a ziggurat, a cylinder, a dome, a square tower, or a combination of two or three of them stacked, each no wider than the one below, with a dome only ever on top. The box holds back everything except the shape and an **open floor**: a layer across the bottom of the box (`MoldOpenFloor`, one module by default). The city spreads in under the shape from any side through that floor, and walls standing on it rise into the shape. Inside the shape the city grows as it does anywhere else. (The first version, without the open floor, almost never got filled. See Known limits.)

Molds are drawn before the pockets, from a stream of their own. Molds stay clear of each other, and pockets stay clear of molds, but pockets may overlap each other. Molds are drawn near the cube's centre, where growth starts (`MoldSpread`).

**Paths** (asked for on 2026-10-15). Paths are smaller air pockets in the shape of routes. They keep doorways open, so the cube is never fully closed. Each path is a thin square tube, `PathWidth` cells thick, running down the middle of the modules. It goes in a few straight legs of whole modules and turns at every corner. Paths start anywhere in the cube, so they are dispersed over all of it.
- While a path is thinner than a module (the default is 2 cells), it reaches only the lattice planes it crosses. So it stops walls, floors and ceilings from being laid across it, and leaves the ones along its sides alone: a line of doorways. At a module thick or more, it also holds back the planes along it: a corridor of open air.
- A path stops **new** placements only (growth, fence panels and nature), never what already stands. The rebuild doesn't ask about paths either, so a path may be drawn through platforms at any time and they stay. That is what the coming deletion mechanic will start from: the summary counts the platforms standing in paths.
- During play, each path stays put for `PathRerollInterval` (30 s by default), give or take `PathRerollJitter`, and is then drawn afresh somewhere new. The paths fall due **one after another** across the interval, not all at once. A clock ticks every 0.05 s and moves at most two paths a tick, the longest-due first.
- Where a path runs comes from the run's seed, its place in the list and its own count of redraws, so moving one path leaves every other path where it is. Any edit to the room manager draws them all again on the same routes, so changing the width or leg lengths shows straight away on the same layout, for calibrating by eye. The **Reroll Paths** button moves them all at once.
- **Why one at a time** (2026-10-15, after the user saw the frame hang): moving every path in one frame took 20 to 70 ms with 12 paths, and 717 ms with the 162 paths NewMap was calibrated to. Moving one path judges again only the placements on the pieces that path left and now reaches. Where the path left, a placement that meets no platform along its rim is skipped, since it can never be listed. `WEM.Paths.Perf` (162 paths, a 509 cube, 3000 platforms, one path every 17th beat) measured 1.2 ms on average and 4.2 ms at worst per move, and 218 ms for the Reroll Paths button. Gathering every list afresh instead took 0.2 to 1.3 s. Each path keeps its pieces, and the shared path keys count how many paths reach each piece, so a piece stays held back while another path still reaches it.
- At most 40 paths can move per second (two every 0.05 s). With a very short interval and many paths, they fall behind and move as fast as that allows.

## The beat

One piece per nature beat (`NaturePlacementInterval`), drawn from a stream of its own:

1. **A field still spilling takes the beat.** Ground and water are laid as fields. Each spills out, one piece a beat, to the size it drew when it started (`GroundSpillMin`/`Max`, `WaterSpillMin`/`Max`). It spreads in its own plane, beside the pieces it has laid, and is likelier to go where it would meet more of them, so it spreads as a patch rather than a line. The oldest spilling field goes first, like the fence queue.
2. **Otherwise something new starts**, drawn by `GroundWeight`, `CliffWeight` and `WaterWeight` among the forms that have somewhere to go:
   - a field of **ground**, against an exterior or at a cliff's top or foot (a terrace);
   - a **cliff**, off a field's outline or carrying a cliff on;
   - a pool of **water**, beside ground or at a cliff's top or foot.

   Only a spill grows off its own field. Starting next to ground alone is not enough for new ground, so fields stay distinct patches.

Where things could start is sampled, not walked in full: 48 of the city's open placements for ground, and 24 edges of nature's rim for what grows off nature. So a beat costs about the same however far the city and nature have spread.

## Settings (room manager)

| Details panel | Property | Default | What it does |
|---|---|---|---|
| Grid \| Nature: Auto Place Nature | `bAutoPlaceNature` | on | The nature beat runs on its own once play begins. It lays nothing while no nature is in Structures. |
| Grid \| Nature: Nature Placement Interval | `NaturePlacementInterval` | 1.0 s | Seconds between nature beats. |
| Grid \| Nature: Nature Facing | `NatureFacing` | Flat, Looking Up | Which ways ground and water may face in the world. **Flat, Looking Up**: horizontal, face to the sky, cliffs upright. **Flat, Up Or Down**: horizontal, or hanging face down under an overhang. **Any Way, Following Up**: as the city's segments face, standing on facades and hanging included. Takes effect on the next nature beat. |
| Grid \| Nature: Ground / Cliff / Water Weight | `GroundWeight`, `CliffWeight`, `WaterWeight` | 1, 1, 0.5 | How often a beat with no field spilling starts each form. Zero never. |
| Grid \| Nature: Ground Spill Min / Max | `GroundSpillMin`, `GroundSpillMax` | 4, 12 | Pieces a field of ground spills out to, its first included, drawn evenly. |
| Grid \| Nature: Water Spill Min / Max | `WaterSpillMin`, `WaterSpillMax` | 2, 6 | The same for a pool of water. |
| Grid \| Nature: Ground / Cliff / Water Count | readouts | | Platforms of each form standing. |
| Grid \| Air: Storey Cells | `StoreyCells` | 16 | Cells in a storey (3.2 m). |
| Grid \| Air: Min / Max Air Storeys | `MinAirStoreys`, `MaxAirStoreys` | 3, 5 | Storeys of air above each piece of ground and water. Each piece draws its own height between the two. Zero keeps no air. |
| Grid \| Air: Show Air, Air Color | `bShowAir`, `AirColor` | on, amber | The air drawn as boxes of lines. |
| Grid \| Air Pockets: Air Pocket Count | `AirPocketCount` | 6 | Pockets drawn each time the cube is cleared. |
| Grid \| Air Pockets: Min / Max Air Pocket Size | `MinAirPocketSize`, `MaxAirPocketSize` | 24, 64 | Cells across: a sphere's or cylinder's diameter, a box's side, a ziggurat's base. Every measure is drawn evenly between the two. A rectangular box draws its three sides separately; a ziggurat and a cylinder draw their length separately. |
| Grid \| Air Pockets: Sphere / Box / Rectangular / Ziggurat / Cylinder Weight | | 1 each | How often each shape is drawn. Zero never. |
| Grid \| Air Pockets: Ziggurat Tiers | `ZigguratTiers` | 4 | Steps, each narrower than the one below by an even step. |
| Grid \| Air Pockets: Air Pockets Along Any Axis | `bAirPocketsAlongAnyAxis` | on | Ziggurats and cylinders stand along any axis, either way. Off, they stand upright in the world. |
| Grid \| Air Pockets: Show Air Pockets | `bShowAirPockets` | on | Each pocket's outline, in its shape's colour. |
| Grid \| Air Pockets: Show Air Pocket Pieces | `bShowAirPocketPieces` | off | A point at every lattice piece a pocket holds back. This is what growth can no longer use, which is coarser than the outline. Many points in a large cube. |
| Grid \| Air Pockets: Sphere / Box / Rectangular / Ziggurat / Cylinder Color | | cyan, magenta, orange, lime, violet | Debug colours. |
| Grid \| Air Pockets: Air Pocket Summary | readout | | What was drawn, by shape, and how many were left out. |
| Grid \| Molds: Mold Count | `MoldCount` | 2 | Molds drawn each time the cube is cleared. |
| Grid \| Molds: Min / Max Mold Size | `MinMoldSize`, `MaxMoldSize` | 64, 128 | Cells along each side of the box. Each side is drawn separately. |
| Grid \| Molds: Min / Max Mold Fill | `MinMoldFill`, `MaxMoldFill` | 0.5, 0.9 | How much of its box (above the open floor) the open shape fills. Width and height are each drawn between the two. A shape narrower than a tile (17 cells for a 16x16) can never hold a platform. |
| Grid \| Molds: Mold Open Floor | `MoldOpenFloor` | 4 | Cells at the bottom of the box left open all the way across, for growth to come in under the shape. Zero means only the shape's own foot is open. |
| Grid \| Molds: Mold Ziggurat / Cylinder / Dome / Tower / Combination Weight | | 1 each | How often each shape is drawn. A ziggurat steps in `ZigguratTiers` tiers. |
| Grid \| Molds: Mold Spread | `MoldSpread` | 0.35 | How far from the cube's centre a mold may be drawn, as a part of the cube. 1 means anywhere. |
| Grid \| Molds: Molds Along Any Axis | `bMoldsAlongAnyAxis` | off | Off, molds stand upright with their floor at the bottom. |
| Grid \| Molds: Show Molds, Mold Color, Mold Shape Color | | on, light grey, red | Each box in one colour and its open shape in another. *Show Air Pocket Pieces* also marks what each mold holds back. |
| Grid \| Molds: Mold Summary | readout | | What was drawn, by shape, and how many were left out. |
| Grid \| Paths: Enable Paths | `bEnablePaths` | on | Turns paths on or off. Off, none are drawn and the timed redraws do nothing, so the redraw's long frame goes away. It takes effect at once, in play too. Switched back on, the paths return at the next redraw, or straight away on an edit or the Reroll Paths button. |
| Grid \| Paths: Path Count | `PathCount` | 12 | Paths through the cube. Zero draws none. |
| Grid \| Paths: Min / Max Path Legs | `MinPathLegs`, `MaxPathLegs` | 2, 5 | Straight legs per path, drawn evenly. |
| Grid \| Paths: Min / Max Path Leg Length | `MinPathLegLength`, `MaxPathLegLength` | 16, 64 | Cells per leg, rounded to whole modules, cut short at the cube's side. |
| Grid \| Paths: Path Width | `PathWidth` | 2 | Cells across. Under a module (4), it makes doorways only. At a module or more, it makes corridors of open air. |
| Grid \| Paths: Path Vertical Chance | `PathVerticalChance` | 0.2 | How often a leg runs up or down, keeping holes open in floors and ceilings. |
| Grid \| Paths: Path Reroll Interval / Jitter | `PathRerollInterval`, `PathRerollJitter` | 30 s, 0.2 | Seconds each path stays put during play before it moves, and how far each path's wait may stray from it. The paths move one after another across the interval. An interval of zero never moves them. |
| Grid \| Paths: Show Paths, Path Color | `bShowPaths`, `PathColor` | on, pale cyan | Each leg drawn as a thin box of lines. |
| Grid \| Paths: Path Summary | readout | | Paths, legs, how many single moves there have been since the cube was cleared, the pieces held back, and the platforms standing in paths. Each path counts the platforms standing in it when it is drawn. |

On a structure (data asset): **Section** (Urban/Nature) and, for nature, **Nature Form** and **Flat Color**. A nature structure without a mesh is drawn as a slab of its flat colour across the opening, from the mid-plane out to its face, lit like the beams. Types, Two-sided, Fence and Load-bearing are not used for nature. *Holds items* is: ground holds items, while the water and cliff assets hold none. *Weight* chooses among several structures of one form, and categories take part in the pull as usual.

The assets: `/Game/Structures/Nature/DA_Nature_Ground`, `DA_Nature_Cliff`, `DA_Nature_Water`. Nature only grows where these are in a room manager's **Structures** list.

Readout: **Debug Probe Role** reads `ground, nature, up +Z` on a nature face.

## Built

| Phase | Commit | What |
|---|---|---|
| 1 | 55e5147 | Section, nature form and flat colour. The meeting rules (`ForEachMeeting`, `AcceptsMeeting`, `AreNatureMeetingsLegal`, `MeetsClosedNature`). `DrawNatureFill`, nature always load-bearing, flat slabs. The city's exteriors-only rule in `DrawFill` and `IsPlacementOpen`. The beat draws a placement again when its fill can't be drawn. |
| 2 | dd53782 | The nature beat: its own timer and stream, the spill queue, sampling, judging candidates (`GatherNatureAround`, `JudgeNatureCandidate`, cached stand-in fills in `FindNatureFill`), and per-form readouts. |
| 3 | 630587c | Air above fields (`GatherAirBoxes`, `IsAirClear`, `DropCandidatesCutBy`), its own line batcher. |
| 4 | 362b80c | Air pockets (`SpawnAirPockets`, `IsCellInPocket`, `CutPocketPieces`) with debug outlines and points. The long nature docs moved out of the class comment, which the header tool embeds as a string and which had grown past MSVC's limit. They now sit by the nature code in RoomManager.h. |
| - | 54387f1 | A placement is only asked what it meets when it touches nature. `WEM.Nature.Perf`. |

Tests: `WEM.Nature.*`, 13 product tests and 1 stress test (`Perf`). 91/91 product tests pass. The existing tests' growth is unchanged, because their fixture asks for no pockets.

## My calls (the brainstorm left these open, or the user went with my defaults)

- **"Vertical" means in the field's own frame.** A cliff is a wall in the frame of the field it rises from or drops off, which follows the earlier decision that there is no global floor or wall.
- **Ground and water lie flat and look up** (the user's decision, after seeing it in Simulate). The first answer was that ground follows Up any way, which put most fields on facades and under overhangs. `NatureFacing` keeps that and a flat-either-way option as settings. The default looks up, because the original spec put the exterior on top of a horizontal ground.
- **I read "When Vertical water is placed against the side with an exterior structure" as being about ground.** I flagged this to the user, and it did not come up again.
- **"Spills out" is a queue**, one piece a beat, like fences ("one piece per tick").
- **A field spills in its own plane**, and turning a corner is the cliffs' job. A field never meets ground or water across a fold.
- **Water spills too**, as smaller pools.
- **What new nature starts against.** New ground starts against an exterior or at a cliff's top or foot, never against ground alone, so fields stay patches. New water starts beside ground or at a cliff's top or foot, and a cliff off ground or another cliff. The first terrace test caught the opposite behaviour, where fields kept growing through the city's own placements next to them.
- **Both counts as exterior** for the city's rule and for ground, in line with "Both satisfies either side's filter". A face with no structure (laid before structures existed) counts as anything.
- **The city never meets water or a cliff.** That is the user's rule as written: those forms meet no urban face. The city still builds against their backs, so rooms can sit behind a cliff and under a pool.
- **Nature is always load-bearing**, whatever stands behind it. Otherwise a non-load-bearing ceiling behind ground would block the field's free beams and stop it spilling.
- **Air**: a storey is 16 cells (3.2 m). Each piece draws its own height, so the clearance over a field is stepped. A seam takes the lower of its two pieces' heights. The height comes from the run's seed and the piece's key, so a rebuild keeps the same air, but a level loaded later, without the seed, draws other heights. Air may overlap air, pockets and the city above its fewest storeys. It holds back only what comes later.
- **Air pockets** are drawn only when the cube is cleared, never while platforms stand, because a pocket drawn later could sit through platforms already grown, and the rebuild would then leave those platforms out. A change to their settings mid-run takes effect at the next clear. A pocket that would take the first platform's place is drawn elsewhere, up to 16 times, then left out. (Changed 2026-10-15: pockets and molds now move during play, and no longer count as obstacles to the rebuild. See `DELETION.md`.)
- **"Rectangular" vs "box"**: box is a cube, rectangular is any cuboid (the user's decision).
- **Pockets reach outside the cube.** A pocket is drawn with its centre inside the cube, and only the pieces inside the cube are held back.

## Measurements

`WEM.Nature.Perf`: a NewMap-sized cube (509 cells), the test kit plus the three forms, pieces built, the overlay off, grown to 3000 platforms with a nature beat after every tenth beat of the city. Timed after the first 1000 platforms.

| | City beat (mean / longest) | Nature beat (mean / longest) | Clearing |
|---|---|---|---|
| City alone | 1.45 ms / 4.1 ms | - | 0 ms |
| With nature and 6 pockets | 1.74 ms / 4.8 ms | 4.5 ms / 10.2 ms | 13 ms (drawing the pockets) |

At 3000 platforms that run had 228 ground, 26 cliff and 18 water. The city's extra ~0.3 ms is the obstacle lookups every placement makes once pockets and air exist. Placed objects add the same cost. The nature beat costs most while a field spills, because it judges every placement round the field. Its air check is the costliest part of judging one.

## Seen in the editor

I made the three assets and added them to the end of NewMap's room manager **Structures** list (822 → 825), then ran Simulate. NewMap's city beat is 0.0107 s and nature's default beat is 1 s, so in a short run nature is only a sprinkle. For the look below I set nature's interval to 0.03 s on the editor instance, then set it back to 1 s, so that change is not saved. After about 20 s: 2154 platforms, 490 ground, 49 cliff, 128 water, and six pockets (two spheres, three ziggurats, a cylinder).

- **Ground** mostly grew up facades and hung under overhangs, since it then followed Up any way. That is what led to `NatureFacing` and its looking-up default. Fields are patches of 4-12 pieces, often mixed with pools. Their air reaches out into the open sky as amber boxes.
- **The air lines are dense** once hundreds of fields stand. Turn off *Show Air* to see the structure.
- **Colours are lit**, so the level's auto-exposure washes them out: the green reads as a pale lime. At 0.45 grey the cliffs vanished among the pale HL2 textures, so I set `DA_Nature_Cliff` to 0.1 grey, darker than the 0.21 beams. They are still easier to find with the probe than by eye, and the colour is on the asset.
- **Pocket outlines** (magenta box, lime ziggurats, cyan sphere, violet cylinder) mostly sit outside the structure early on, since the cube is 101 m across and the city starts at its centre.

To see more nature, lower *Nature Placement Interval* (each beat costs about 4.5 ms at 3000 platforms) or raise the spill sizes.

## Known limits

- **Molds fill slowly.** `WEM.Nature.MoldFill` (stress) grows a NewMap-sized cube to 3000 platforms with four molds at a spread of 0.35:
  - With no open floor, 0-7 platforms reached each shape.
  - With the open floor, at default sizes, the platforms that rose into each shape and lay in its floor were:

    | Open floor | In the shape (per mold) | In the floor (per mold) |
    |---|---|---|
    | 4 cells (default) | 29, 0, 11, 5 | 0, 0, 142, 133 |
    | 8 cells | 13, 0, 7, 22 | 218, 7, 130, 157 |
    | 20 cells | 19, 0, 3, 16 | 398, 1, 376, 657 |

  Deeper floors mostly collect sideways growth. A mold fills as the city reaches it, so it fills over time rather than at once.

- ~~Nature never goes away.~~ Since 2026-10-15 platforms are struck down and reshuffled, nature among them, and pockets, molds and the air over the fields hold back only new placements. See `DELETION.md`.
- **Flat colours only.** A nature structure with a mesh is drawn with the mesh, fitted like any structure, but no meshes exist yet.
- **The object placer treats nature like any surface.** Ground holds items, so the city's objects will stand on meadows. Water and cliffs hold nothing.
- **The nature beat samples**, so with very few places left it can miss them for a few beats.
- **Debug lines are redrawn whole** whenever a piece of a field is laid. That is fine at one beat a second. With thousands of fields and a fast nature beat, turn `bShowAir` off.
