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

On top of the meetings, every cliff has to stand wholly on the outline of ground or water (since 2026-10-17, see Cliffs as runs). So a cliff still meets a cliff above it only where both stand on a field, never as a stack of cliffs alone.

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
2. **Then a cliff still running.** Since 2026-10-17 a cliff carries on like a fence (see Cliffs as runs below), one piece a beat, the oldest waiting end first.
3. **Otherwise something new starts**, drawn by `GroundWeight`, `CliffWeight` and `WaterWeight` among the forms that have somewhere to go:
   - a field of **ground**, against an exterior or at a cliff's top or foot (a terrace);
   - a **cliff**, off the outline of ground or water;
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
| Grid \| Nature: Cliff Continue Chance | `CliffContinueChance` | 0.8 | The chance each end of a cliff, with room beyond it, carries the cliff on by another, as a fence's run does. Each end goes on for 4 cliffs on average at 0.8. Zero: every cliff stands alone. |
| Grid \| Nature: Cliff Turn Chance | `CliffTurnChance` | 0.25 | Where a run could both go straight on and turn the corner, on an outline either way, how often it turns. Where only one is on an outline, it takes that one. |
| Grid \| Ground Spill: Interior Ground Spill Chance | `InteriorGroundSpillChance` | 0.1 | The chance a floor growth lays with an interior ground on top spills out into a patch of interior grounds (see The city's grounds spill below). Zero never. |
| Grid \| Ground Spill: Interior Ground Spill Min / Max | `InteriorGroundSpillMin`, `InteriorGroundSpillMax` | 2, 4 | Pieces such a patch spills out to, its first included, drawn evenly. |
| Grid \| Ground Spill: Exterior Ground Spill Chance, Min / Max | `ExteriorGroundSpillChance`, `ExteriorGroundSpillMin`, `ExteriorGroundSpillMax` | 0.1, 2, 4 | The same for an exterior ground. |
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

On a structure (data asset): **Section** (Urban/Nature) and, for nature, **Nature Form**, **Flat Color**, **Flat Material** and **Cover Categories** (see Covers below). A nature structure that no cover covers and that has no mesh is drawn as a slab of its flat colour across the opening, from the mid-plane out to its face, lit like the beams. The slab is drawn in its **Flat Material** where it names one (water does, see Water as one sheet below), or in the beams' material where it doesn't, given the flat colour either way through the material's `Color` parameter. Types, Two-sided, Fence and Load-bearing are not used for nature. *Holds items* is: ground holds items, while the water and cliff assets hold none. *Weight* chooses among several structures of one form, and categories take part in the pull as usual.

The assets: `/Game/Structures/Nature/DA_Nature_Ground`, `DA_Nature_Cliff`, `DA_Nature_Water`. Nature only grows where these are in a room manager's **Structures** list.

Readout: **Debug Probe Role** reads `ground, nature, up +Z` on a nature face, followed by `, covered with <structure>` when a cover covers it.

## Covers

Asked for on 2026-10-17: "The nature ground tiles are currently green. The top part needs to have a structure", from the natural flora, sand and stone categories, the ones set as ground; cliffs the same, the ones set as walls.

A nature structure now names **Cover Categories** (`UPlatformStructureData::CoverCategories`). When the nature beat lays a piece, it draws the nature, then the city structure behind it as before, and then a **cover** for the nature face. The cover is one of the room manager's structures that:
- belongs to any of those categories,
- is one-sided,
- has the type the face takes: **Ground** on ground and water, **Wall** on a cliff,
- fits the opening.

It is drawn from the nature stream with the category pull, like any face, and kept on the platform as `FGridPlatform::NatureCover`. The pieces draw the cover across the nature face in place of the flat slab. The face offers the cover's categories to the faces beside it, so a field tends to stay one category, and fields form patches of sand, flora or stone.

To growth the face is still nature. The meeting rules, the air, load-bearing and *Holds items* all read the nature asset as before. The cover is only how the face looks and which categories it offers.

Set up: `DA_Nature_Ground` and `DA_Nature_Cliff` name NaturalFlora, NaturalSand and NaturalStone. `DA_Nature_Water` names none, so water keeps its own look (since 2026-10-17 one sheet of water, see below). In NewMap's Structures list that gives 48 grounds for ground (18 flora, 21 sand, 9 stone) and 26 walls for cliffs (2 sand, 24 stone, no flora).

Seen in Simulate (nature interval at 0.03 s for the run, then set back): 1688 platforms, 328 ground, 19 cliff and 150 water. Every ground piece and every cliff was covered, and no water was. A six-piece field sampled was all sand (sand, dirt and gravel).

![Covered fields: sand, dirt and stone ground, natural walls on the cliffs, water still flat](Screenshots/nature_covers.jpg)

Test: `WEM.Nature.Cover` checks:
- ground takes only one-sided grounds of its categories, and both categories turn up;
- a cliff takes the one wall of its categories;
- water stays bare;
- a covered piece is drawn as its cover and not as the slab, and the cover goes when the piece is taken away;
- its face offers the cover's category;
- with the pull certain, a spilled field is all one category, and agrees with a full rebuild.

105/105 product tests pass.

My calls:
- **The face's type decides, not the name.** Ground needs a structure set as Ground (not Roof), and a cliff one set as Wall. `NatFlora_Wall_Moss_01` is set as Ground, so it covers ground, not cliffs. That is why flora has no walls for cliffs. Set it to Wall in Blender if it is meant for cliffs.
- **Side is not asked.** All 74 natural structures are exteriors anyway.
- **Two-sided structures never cover**, since a cover is one face with the city's structure behind it.
- **No cover that fits leaves the old look.** That means the nature's own mesh, or else its flat colour, so nature still grows on tiles the natural structures don't come in (they are all 16x16). The same holds if a cover's size later changes so that it no longer fits.
- **The pull works both ways.** The city's own faces beside a field are pulled toward the field's category like any neighbour. So the city's exterior grounds next to a sand field tend to carry on as sand.
- **The city still draws the natural structures** for its own exterior grounds and walls, as before. Nothing was taken from it.

## Water as one sheet

Asked for on 2026-10-17: one animated water texture that connects without seams across the water pieces, with only the beams of the outline showing. The user chose a texture I generate and a plain glossy surface over Single Layer Water.

**Seams.** Where two pieces of the same flat water lie side by side in one plane, looking the same way, and nothing stands on the beam between them on the water's side, that beam is a **seam**. A seam beam keeps only the half of the platforms' layer behind the water, and the water fills the other half, level with the slabs on either side. A post is cut down the same way where every beam built at it is a seam of the same sheet, or hangs behind it. Beams and posts round the outline stay whole, the posts where a seam meets the outline included. A wall hanging under the water from a seam beam is still carried by the half that is left. A cliff standing on it on the water's side parts the pools, and the beam is whole again.

How it is drawn (`RoomManager.h`, by `GatherEdgePieces`):
- `FindWaterSeam` / `FindWaterSeamAtNode` decide, from `ModuleFaceOwners` alone, whether a beam or post is a seam.
- A cut-down beam or post, and the water over it, are keyed with `WaterSeamPieceBit` (bit 62), apart from the whole piece. A full sync that wants the other kind removes one and draws the other, so nothing else in the sync had to change. The water pieces go in the water asset's own layer, beside its slabs.
- `RefreshLatticePieces` brings named beams and posts in line in place. Removal now uses it in place of its own loop, and placement calls it too whenever there is water, for every beam and post the new platform takes in, inside it as well as round it. So a pool joins up as each piece is laid, and comes apart as one is struck. Removal now names every node of a removed platform, not just its rim.
- `ArePiecesInStep` is the check `AppendPlatformPieces` and removal each made for themselves.

**The look.** The user then gave a reference: murky, silty water in a flooded town at sunset, with the bright web of caustics over it. They asked for that look, animated. So the water is murky olive-grey, takes its sheen from the sky, and carries a moving caustic web.

`DA_Nature_Water` names `/Game/Structures/Nature/Water/M_Nature_Water` as its Flat Material. The material is opaque and default lit, and has two custom nodes:
- **WaterSurfaceNormal** gives a world-space normal. It maps the ripples by world position in the plane the face lies in, so every piece samples the same surface, whatever its size or where it starts. Two layers of the one ripple tile cross at different sizes, the second turned a quarter, and drift different ways over time, so neither the tile nor its single wind direction shows.
- **WaterColor** gives the base colour: Color brightened along the caustic web. The web is two copies of the caustics tile in the same plane, drifting against each other and averaged, so lines cross, part and meet again rather than slide past as one pattern. The ripples' lean shifts where the web lands, so it wobbles with the surface. Because the web is in the base colour, it is lit like the rest of the colour: bright in sunlight, faint in shadow, as in the reference.

Its parameters, as material defaults:

| Parameter | Default | What |
|---|---|---|
| Color | set by the room manager | `DA_Nature_Water`'s Flat Color, now (0.05, 0.048, 0.033), murky olive-grey |
| Roughness | 0.15 | Soft reflections of the sky |
| Specular | 0.5 | |
| RippleSize | 220 | cm across one repeat of the first ripple layer; the second is 0.61 of it |
| RippleSpeed | 12 | cm/s the ripple layers drift |
| RippleStrength | 0.25 | How far the ripples tilt the surface; 0 is a mirror |
| CausticSize | 260 | cm across one repeat of the web; the second copy is 1/1.13 of it |
| CausticSpeed | 10 | cm/s the two webs drift |
| CausticStrength | 2.2 | How much brighter the colour is on a line; 0 is no web |
| CausticDistort | 25 | cm the ripples' lean moves the web |

Only Color is set per run, so the rest are tuned on `M_Nature_Water` itself.

**The textures**, each a 1024² seamless tile with its generator script and source PNG in `Structures/Water/`:
- `T_Water_Ripples_N` (normal map) comes from `make_water_ripples.py`. It shapes white noise in frequency space with a wind-sea spectrum spread about one direction.
- `T_Water_Caustics` (greyscale, sRGB off) comes from `make_water_caustics.py`. It traces light straight down through a smooth periodic surface, bends each ray by the slope, and counts where it lands, wrapping round the tile, so the web is the real focusing of light rather than a drawn pattern. A surface with fine ripples in it broke the web into grain, so the surface the light passes through keeps only waves near 9 per tile. Bend 12 gave clean cells; 20 and 30 folded the light into tangles.

Every wave fits the tile a whole number of times, so both tiles are seamless by construction. On each PNG, the difference across the wrap matches the difference between neighbouring pixels. Run either script with other settings and re-import over the asset for a different water.

Test: `WEM.Nature.WaterSheet` lays two 16x16 pools side by side and checks:
- 4 seam beams and 3 posts are cut down, with water over each;
- the cut beam is the lower half and the water the upper, level with the outline;
- outline beams and the posts where the seam meets the outline are whole;
- the slab is drawn in the water's Flat Material;
- a wall hanging under the seam keeps it a seam, and a cliff standing on it ends it;
- taking the cliff away joins the pools again, and taking a pool away leaves the beam whole for the wall;
- after every step, a full sync has nothing to add or take away.

106/106 product tests pass.

Seen in Simulate (NewMap as saved, nature interval 0.1 s): 1806 platforms, 109 water pieces, the largest pool 23 pieces. Only its outline shows. The ripples run straight across the joins, and two captures a few seconds apart differ, so it moves. These first two shots are the first pass, in clear blue:

![A 23-piece pool as one sheet of water, only its outline beams showing](Screenshots/water_sheet.jpg)

![Closer: the ripples carry on across the joins between pieces](Screenshots/water_sheet_near.jpg)

The murky look, from just above a pool. In the second shot strikes have since taken pieces out of the pool, so the beams crossing it are the outlines of the holes and of the ground beside them.

![Murky water with the caustic web in the sunlit part and the sky's sheen in the shade](Screenshots/water_murky_sun.jpg)

![The same pool after strikes, the web stronger and the colour darker, as now set](Screenshots/water_murky.jpg)

My calls:
- **Seams only between the same water asset.** Two different water assets side by side keep a beam between them, as two pools.
- **Only flat water joins.** Water drawn with a mesh or a cover is drawn as that, so it has no slab to carry across a beam.
- **Water on walls works the same**, with *Nature Facing* set to Any Way: the material maps in the plane the face lies in.
- **The custom node samples with `Texture2DSample`**, not `Tex.Sample`. The ray-tracing hit shaders reject a sample with implicit derivatives, and the material failed to compile until this changed.

## Cliffs as runs

Asked for on 2026-10-17: "Cliffs also need to behave like fences but make sure they are on a natural ground or water outline." The user then chose that every cliff touches a field, so cliffs no longer stack.

**On an outline.** Every cliff, the first of a run or not, stands wholly on the outline of ground or water (`StandsOnOutline`). Its nature face meets ground or water round every module edge along its foot, or round every one along its top. Its back meets none there, since a field on both sides would make the edge a seam, not an outline. A cliff half off the end of an edge doesn't count, and nor does one looking away from the field. So a cliff rises off a field's edge or drops from it, and it is one tile high. A taller drop only comes from a terrace: a field laid on a cliff's top, with a new cliff off that field's outline.

New cliffs now start off ground **or water** (before, only off ground, or carried on from another cliff). The cliff-off-cliff start is gone.

**As a run.** When the nature beat lays a cliff, each of its ends with room beyond it carries the cliff on with `CliffContinueChance` and waits in a queue of its own, as a fence's ends do. While an end waits, the nature beat lays its next cliff before it starts anything new. The next cliff is the same cliff asset on the same tile, with the same Up and at the same height, either straight on or round the beam at the end onto a wall facing the other way. It is judged with its nature face either way round, and only spots that stand on an outline and meet the run legally are kept. So the run follows the outline, turns outward at a field's corners and inward at its inner corners, and stops where the outline ends. Where both straight on and a turn are on an outline, it turns with `CliffTurnChance`. A run that reaches a cliff of its own closes, as a fence round a roof does. Its back and its cover are drawn fresh for each cliff, the cover with the category pull, so a run tends to keep one look.

One edge of an outline takes one cliff, rising or dropping, never both: the dropping one would look into the back of the rising one.

Test: `WEM.Nature.CliffRun` checks:
- On a single piece of ground, with every end carrying on, the first cliff runs round all four edges at one height, then nothing more is laid.
- An L of three pieces takes a run of eight, turning in at its inner corner.
- A cliff wholly on the outline stands on it. One half off the edge, one looking away from the field, and one on the seam between two pieces do not.
- With no chance of carrying on, one cliff stands on each edge and no end ever waits.
- Water's outline takes cliffs too.

Seen in Simulate (NewMap as saved, nature beat 0.1 s), read off the platforms rather than by eye, since the city overhead hides most pools from the camera:
- At 3264 platforms there were 76 cliffs and 269 pieces of ground and water.
- Every cliff had a field at its foot or along its top.
- The cliffs formed runs of 16, 15, 7, 7, 7, 7, 6, 4 and 2, with five cliffs alone. The 16-run rises round a pool of water, with stone and dirt walls as covers.
- When stopped: 730 ground, 252 cliffs and 263 water, with the city still at 59 platforms a second.

`WEM.Nature.Perf` afterwards: the nature beat took 3.7 ms on average and 10.8 ms at worst (4.5 and 10.2 before). It laid 75 cliffs in 3000 platforms (26 before).

## The city's grounds spill

Asked for on 2026-10-17: "I want ground:interior and ground:exterior to spill out sometimes as the natural tiles (albeit on much lower settings), obviously they will still need to be placed where they can be placed." The user chose:
- each piece of a patch is a ground of the same side, drawn as usual (by weight, with the category pull);
- the face behind it is drawn as any floor's is.

**When.** A floor that growth lays, the first platform and the zones' first tiles included, may start a patch. Its face looking along its Up must be a Ground of side Interior or Exterior, and not a fence. It starts one with that side's chance, `InteriorGroundSpillChance` or `ExteriorGroundSpillChance` (0.1 each). The patch's size is drawn between that side's min and max (2 to 4, its first piece included). Nature's fields always spill, to 4 to 12.

**How.** The patch waits in a queue of its own. A growth beat lays its next piece before it draws anything new, after any fence panel that waits. The piece is a floor beside the pieces the patch has laid, in their plane and with their Up, likelier where it would meet more of them. It is held to everything any floor there is held to: it must be free and flush, clear of held air, and meet nature only as the city may. Then its top is drawn from the grounds of the patch's side, those of both sides among them. What goes behind it follows the usual rules, so behind an interior ground, an exterior 80% of the time. A patch with no room left stops where it is.

My calls:
- **Both binds nothing**, so a ground of both sides starts no patch, though it can be a piece of one of either side.
- **Only the face looking along Up.** A ground hanging face down under a floor (ground over ground) starts nothing.
- **A reshuffle starts no patch**, since it keeps to its zone and a patch could wander out of it.
- **Both chances are 0.1, and both sizes 2 to 4**, for "much lower settings".

Tests: `WEM.Structures.GroundSpill.*`:
- **Patch:** an interior patch of four lays a piece a beat, each in the plane, topped with an interior ground, an exterior behind it, beside a piece already laid. An exterior patch is all exterior grounds.
- **Starts:** interior and exterior grounds start a patch. A ground of both sides, a roof, a wall, a ground looking down, and a chance of zero don't.
- **Hemmed:** a floor ringed by floors has nowhere to go, so its patch stops at once. A patch whose piece is struck goes.
- **Growth:** 400 beats at 0.3 each. Patches take beats, the placements agree with a rebuild, and the same seed grows the same city.

The test fixture sets both chances to zero, so every older test grows as it did. 111/111 product tests pass.

Not measured on its own: how much a spill beat costs. Each one walks the rims of at most four pieces, and NewMap's city held 59 platforms a second through the Simulate run above with spills on.

## Walls run round the outline of grounds

Asked for on 2026-10-17: "Interior and exterior walls also have fence-like qualities sometimes, meaning that they are built around the outline of ground tiles where they can be built." The user chose:
- a run follows the outline of the city's grounds of its own side;
- each next wall keeps that side on the face over the ground, drawn as usual.

**When.** A wall that growth lays may start a run if it stands wholly on the outline of the city's grounds (`StandsOnGroundOutline`, the check cliffs use, with city grounds in place of nature). That means:
- one face meets a Ground of a side round every module edge along its foot (or along its top);
- the face over the ground stands for that same side (interiors over interior grounds, exteriors over exterior grounds), with grounds of both sides counting for either;
- its other face meets no such ground there, since that would be a seam inside a patch, a partition rather than an outline.

It starts a run with that side's chance, `InteriorWallRunChance` or `ExteriorWallRunChance` (0.25 each). Nature ground doesn't count, a wall of both sides over the ground starts nothing, and nor does a reshuffle.

**How.** A run goes as a fence's does, with its own queue on the city beat, after fences and ground spills:
- Each end with room beyond it carries on with `WallRunContinueChance` (0.8).
- The next wall goes straight on or round the corner, on the same tile, with the same Up and at the same height, wherever it would stand wholly on the outline too. So the run follows the outline, turns at the patch's corners, inner ones included, and stops where the outline ends.
- Where both straight on and a turn stand on it, the run turns with `WallRunTurnChance` (0.25).
- Its face over the ground is drawn from the walls of the run's side, by weight with the category pull. Its back is drawn as any wall's is, so behind an interior there is an exterior 80% of the time.
- A run closes when the next spot already holds a wall on the same outline.
- A wall only goes where any wall could be placed. A floor that isn't load-bearing blocks its free rim, so no wall stands on it.

Settings (Grid | Wall Runs on the room manager):

| Property | Default | What |
|---|---|---|
| `InteriorWallRunChance` | 0.25 | The chance a wall laid on the outline of interior grounds, its interior over them, starts a run. |
| `ExteriorWallRunChance` | 0.25 | The same for exteriors over exterior grounds. |
| `WallRunContinueChance` | 0.8 | The chance each end carries on. |
| `WallRunTurnChance` | 0.25 | Where it could go straight or turn, how often it turns. |

How it is built:
- `DrawFill` takes an `FFaceWish` (face, type, side): the face is drawn first from the matching structures. The ground spill uses the same thing for its Ground tops.
- `FFenceEnd` carries the run's side.
- `IsRunEndClosed` closes a fence or cliff run on more of itself, and a run of walls on a wall on the same outline.

Tests: `WEM.Structures.WallRun.*`:
- **Ring:** a wall on an interior floor's edge runs round all four edges, each wall interior over the floor. Round an L of three floors it takes eight, turning in at the inner corner.
- **Starts:**
  - Start a run: an interior over interior ground, and an exterior over exterior ground.
  - Start none: an interior over exterior ground, a wall of both sides, a wall on the seam between two floors, a wall hanging below the floor's edge, a wall half off the edge, and a chance of zero.
- **Growth:** at 0.5 with spills on, runs take beats, the placements agree with a rebuild, and the same seed grows the same city.

The fixture sets both chances to zero. 114/114 product tests pass.

Seen in Simulate (NewMap, 20 s each, values set on the instance and not saved). The measure is walls standing on a city ground's edge with ground on one side only, and how many of them sit in chains of 4 or more joined at their ends:

| Wall run chances | Platforms | Walls on a ground's edge | In chains of 4+ | Longest chain |
|---|---|---|---|---|
| 0 (off) | 1862 | 322 (17%) | 69 (21%) | 10 |
| 0.25 (default) | 2653 | 458 (17%) | 93 (20%) | 7 |
| 1, continue 1 | 3167 | 654 (21%) | 222 (34%) | 11 |

So at the default of 0.25 the runs don't show in one 20 s run; at 1 they clearly do. Why so weak:
- Most walls on an edge get there before their floor does, and only a wall laid where the ground already is starts a run.
- Many outlines are blocked or already built against.

Raise the two chances for more, or ask for runs to start when a floor is laid beside walls already standing.

In NewMap the walls standing on a ground's edge were mostly exteriors over exterior grounds (341 of 654). Interiors over interior grounds were 65, and walls of both sides over the ground 76.

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
- **What new nature starts against.** New ground starts against an exterior or at a cliff's top or foot, never against ground alone, so fields stay patches. New water starts beside ground or at a cliff's top or foot, and a cliff off ground or another cliff (since 2026-10-17: off the outline of ground or water only, see Cliffs as runs). The first terrace test caught the opposite behaviour, where fields kept growing through the city's own placements next to them.
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
- **Water has no covers.** Ground and cliffs are covered by the natural structures (see Covers). Water is one animated sheet instead (see Water as one sheet), and names no cover categories. A nature structure with a mesh is drawn with the mesh, fitted like any structure, where no cover covers it.
- **The object placer treats nature like any surface.** Ground holds items, so the city's objects will stand on meadows. Water and cliffs hold nothing.
- **The nature beat samples**, so with very few places left it can miss them for a few beats.
- **Debug lines are redrawn whole** whenever a piece of a field is laid. That is fine at one beat a second. With thousands of fields and a fast nature beat, turn `bShowAir` off.
