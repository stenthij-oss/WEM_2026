# Relative roles

Planned and approved 2026-10-09, built 2026-10-09/10 on branch `segment-structures` after 981bf29. Not pushed.

## The idea

Ground, wall and ceiling stop being world directions. Every segment carries its own **Up** (`FGridPlatform::Up`), one of the six lattice directions, and its role follows from that:

- **Floor**: Up lies along the segment's normal. The face looking along Up is its **Ground** (or, since roofs, its **Roof**) and the other one its **Ceiling** (or the ground under a ground) - see [Roofs and sides](#roofs-and-sides).
- **Wall:** Up lies in the segment's plane. Both faces are Wall, and Up is the way its structures stand.

So Ground always means "the face pointing up" - up in the segment's own frame, which need not be the world's.

A new segment takes Up from a segment it grows off (its **parent**, drawn at random among the segments it touches), and the geometry sets the role: a fold off a floor is a wall standing on it, a fold off a wall's top or foot is a floor, a fold off a wall's side is a corner wall, and carrying on in the same plane keeps the role. The three turns below are the only things that change Up.

The first segment is a floor with Up = +Z, and so is every segment saved before this change. With all three turns off every Up stays +Z, and the structures fill as before.

## Settings (room manager)

| Setting (Details panel) | Property | Default | What it does |
|---|---|---|---|
| Grid \| Up: **Flip On Perpendicular Growth** + Flip Chance | `bFlipOnFold`, `FlipChance` | off, 0.15 | A segment folding off its parent takes the opposite Up. |
| Grid \| Up: **Hang From Ceilings** | `bHangFromCeilings` | off | A wall folding off a floor's Ceiling face, where that face holds items, hangs from it: its Up points away from the floor, so it stands upside down to it. |
| Grid \| Up: **Quarter-Turn** + Quarter Turn Chance | `bQuarterTurn`, `QuarterTurnChance` | off, 0.15 | A segment carrying a wall on in the wall's own plane becomes a floor whose Ground looks out of one of the wall's holding faces. Floors open out of walls. |
| Grid \| Up: **Walls Stand On Grounds** + Walls Stand Chance | `bWallsStandOnGrounds`, `WallsStandChance` | on, 0.8 | Rule 1. Off a ground that holds items, a wall about to hang below it is drawn again with the chance. |
| Grid \| Up: **Floor Or Ceiling By Opposite Edge** + Floors Close Rooms Chance | `bFloorsCloseRooms`, `FloorsCloseRoomsChance` | on, 0.8 | Rule 2. Off a wall's holding face, a floor goes to the end the room on that side still needs: the foot when a ceiling closes the top, the top when a ground runs along the foot, the foot when it has neither. A floor about to go to the other end is drawn again with the chance. |
| Grid \| Structures: **Interior Backs Exterior** + chance | `bInteriorBacksExterior`, `InteriorBacksExteriorChance` | on, 0.8 | Rule 3. With the chance, the back of a one-sided interior face is drawn only from the exteriors that fit. Behind an exterior, anything. Each structure's **Side** says which it is. |
| Grid \| Structures: **Walls Face Interior Grounds** + chance | `bWallsFaceInteriorGrounds`, `WallsFaceInteriorGroundsChance` | on, 0.95 | Rule 4. A wall's face looking over an interior ground the wall stands on is drawn from the interiors. |

"Holds items" is the structure's *Holds items* on that face. A face with no structure holds items. Both faces of a two-sided structure count.

Readout: **Debug Probe Role** (Grid \| Coordinates) names what the probed face's fill makes it, which side its structure stands for, and its segment's Up, as in `roof, exterior, up -Z`.

## What reads the segment's Up instead of world Z

- **Structure types:** `WEMStructures::GetFaceType`, and with it every fit check.
- **Shape fit:** a wall's structure stands with its length along Up, on whichever in-plane axis that is; only one that *may lie sideways* lies across it. Floors still turn freely.
- **Mesh transform:** a wall's mesh top (Blender +Y) points along Up, straight down included. A two-sided structure on a floor faces its Ground face (it was modelled lying on the ground); on a wall it still faces the positive normal.
- **Fill order:** a floor draws its Ground face first.
- **Load-bearing:** a wall that isn't load-bearing blocks its top beam - the edge across Up at the end Up points to.
- **Fences:** a wall fence runs across its Up. Its foot and top for "stands on something" run across Up too. A corner turn keeps Up and stands as tall along it.
- **Saving and replay:** Up is saved on each platform, since the turns are random, and the rebuild reads it from there.

What stays the world's: tiles' *Horizontal only / Vertical only*, `VerticalPlacementChance`, and the horizontal/vertical counts. Upside down, a "wall" can be laid from a horizontal-only tile.

## Built

| Phase | Commit | What |
|---|---|---|
| 1 | e626304 | `FGridPlatform::Up` and `IsWall()`, inheritance from a random parent (`GatherTouchingPlatforms`, `ChooseUp`, its own `RoleStream`), every reader above, `DebugProbeRole`. |
| 2 | 17b3497 | The three turns (`TurnUp`). |
| 3 | 886ff5e | Rules 1 and 2 (`GetRuleRedrawChance`, `HasFloorAlong`, `FaceHoldsItems`), the redraw in the beat. |
| 4 | aa3f251 | Rule 3, in `DrawFill`. |
| 5 | - | Nothing to change: see below. |
| 6 | this commit | These notes, a turned case in the beat benchmark, the structure-type docs. |

**Phase 5 needed no code.** The plan said objects stand on world-up faces. That was wrong: the object placer already stands an object on every open all-object face of every direction, its up along that face's own normal (`WEMPlacement::MakePoseFrame`). So on an upside-down floor objects already stand on its Ground face (pointing down in the world) and against its Ceiling face, and nothing about Up changes what they do.

## Calls the plan left open

- **Order of turns on a fold:** hang first, then flip, so with both on, a flip can undo a hang. A quarter-turn only happens on an in-plane continuation of a wall, never on a fold.
- **When a turned Up can't be filled** by any structure, the segment takes the Up it would have inherited, then the Up of another segment it touches (in index order), then +Z. The candidate lists are still kept for the world's Up (+Z), so a placement that *only* a turned Up could fill is never drawn.
- **The rules redraw** from the same tile and kind, at most four times a beat (`MaxRuleRedraws`), and only roll when a placement goes against a rule, so a beat where no rule applies draws exactly as before. The chance is the chance a placement going against the rule is sent back. Where standing and hanging are equally on offer, 0.8 gives about five standing walls to every hanging one; 1.0 makes the rule nearly strict.
- **Rule 2's "ceiling" and "ground"** are any floor (a segment whose Up lies along its normal) running along that edge on the same side of the wall. A wall lying there doesn't count. A room closed at both ends wants nothing, and a face that holds no items faces no room.
- **Rule 3** first read "Interior" and "Exterior" from each category's name, both ways round. Since roofs and sides it reads each structure's **Side** and binds only interiors (below). The back is still pulled by its neighbours among the structures left.
- **Fence corners** use the same tile laid the same way round when that keeps the panel as tall along Up (always, for a wall standing upright in the world), else turned when the tile may turn, else there is no corner to turn.
- **Hang from ceilings** reads a floor's Ceiling face, whichever way it points: a wall standing on an upside-down floor in the world hangs from it, and so comes out the right way up.

## Measurements

Tests: 76 WEM tests pass (the 60 from before and 16 new: fifteen `WEM.Structures.Roles.*` and `WEM.Structures.Transform.Relative`). One naming trap: a test called `WEM.Structures.Fit.Relative` turned `WEM.Structures.Fit` into a group and silently stopped the old test running, so the new one is `WEM.Structures.Roles.Fit`.

**All three turns on at 0.3** (`Roles.TurnedGrowth`, 400 placements, seed 1234, every placement checked against a full rebuild): 0 drift; 221 of 400 segments not up the world's way, 156 upside down, 26 walls lying flat, 27 floors standing up. Every fill fits its segment with its Up.

**Rules at placement time** (`Roles.RuleGrowth`, 300 beats, seed 1234, each placement asked against the ones before it):

| | Walls hanging below a ground | Floors at the end of a wall its room did not need |
|---|---|---|
| Rules off | 62 | 39 |
| Rules on (0.8) | 29 | 18 |

Measured on the finished structure instead, the share of hanging walls hardly moves (15% to 16%), because later floors grow under a hanging wall's foot either way. The rule acts on the draw, not on what the structure later becomes.

**Stress tests with the rules on by default** (all pass): `Fill.VerifyEvery1000` - 1000 placements, 0 drift;
`Fence.RunLength` - mean run 9.08 panels, as before; `LoadBearing.Stall` - growth to the end of a 129-cell cube
stops at a median 3466 platforms all load-bearing (3488 before) and 3348 with 5% light (3415 before). The
rules leave a little more room unused at the end, about 1-2%.

**Beat time** (`WEM.Structures.Perf.Beat`, NewMap's setup, beats 2000 to 3000, best of three, interleaved rounds against the same benchmark built from 981bf29 in its own worktree):

| ms a beat | 981bf29: rounds 1 / 2 / 3 | This branch: rounds 1 / 2 / 3 | Best, before -> after |
|---|---|---|---|
| Plain slabs | 1.545 / 1.595 / 1.455 | 1.353 / 1.509 / 1.430 | 1.455 -> 1.353 |
| Test kit | 1.742 / 1.741 / 1.592 | 1.558 / 1.799 / 1.641 | 1.592 -> 1.558 |
| Test kit, all three turns on | - | 1.722 / 1.583 / 1.549 | 1.549 |

The rounds spread by about 8% on either build, more than any difference between them, so the cost is
not measurable: a beat now gathers the platforms it touches (for its parent and the rules) and may
draw up to four more times, all small beside the rest of the beat. The two builds also grow different
structures past the first beat a rule applies to, so they are not timed on the same 3000 platforms.
These runs are slower than the 1.363 / 1.406 ms in `NOTES.md` from the same benchmark on 2026-10-09;
the machine was busier (Blender was open), which is why the rounds were interleaved.

## Seen in the editor

NewMap, all three turns on at 0.3, seed 7, about 1400 segments after 20 s of Simulate. The settings were
put back afterwards (all off, 0.15, seed 0) and the level was not saved.

![Turned, from outside](Screenshots/turned_overview.jpg)

Walls lie flat in the world, wall textures running across segments that face up, beside floors facing
every way.

![Turned, inside](Screenshots/turned_room.jpg)

Inside: a roof-tile ground standing upright on the left - a floor turned a quarter off a wall - and
objects hanging from the ceilings overhead, standing on those faces as on any other.

## Known limits

- With the turns on, structures that may not lie sideways can't fill some sideways walls. Those segments fall back to their parent's Up or +Z, so a turned region can snap back to the world's up where the kit doesn't fit it.
- Tile orientation stays the world's (decision 2): a horizontal-only tile can be a wall upside down.
- `SourceArt/Structures/README.md` and the Blender add-on now describe the four types and the side (roofs and sides, below), edited on top of your uncommitted changes there.
- On a wall's faces objects stand out sideways, as they always did on world walls.
- The rules make growth depend a little on fills (through *holds items*), as load-bearing already did. A seed grows differently from 981bf29 wherever a rule applies.

## Roofs and sides

Asked 2026-10-10: a **Roof** type, and every structure saying whether it is **interior or exterior**.
Behind an interior an exterior; a ceiling has a roof on its other side, and an interior ground an
exterior ground.

**The user's decisions:**

- A floor is **a roof over a ceiling, a ground over a ceiling, or a ground over a ground**. Never a roof
  over a ground. (First asked as only roof over ceiling and ground over ground; corrected the same day:
  "ceiling has either ground or roof on top of it, not necessarily roof".)
- **Only interiors are bound.** Behind an exterior, anything that pairs may follow: exteriors back to back
  (a garden wall, earth on both sides of a slab, a porch roof over a porch ceiling) or an interior.
- **Rule 3 keeps its chance** (0.8), now reading the new property.
- New: **interior grounds almost always raise interior walls** on the side facing the ground (rule 4).
- Some structures are **both interior and exterior** (asked the same day): Side has a third value, Both.

**What changed:**

- `EStructureType::Roof`, and `UPlatformStructureData::Side` (`EStructureSide`: Exterior, the default,
  Interior, or Both). The old category-name reading is gone; a category named `Interior…` no longer makes
  a structure an interior.
- What a face takes (`WEMStructures::GetFaceTypes`): the face looking along Up takes Ground or Roof, the
  other one Ceiling or Ground, a wall's faces Wall. What pairs back to back (`GetBackTypes`,
  `PairsBackToBack`): Roof with Ceiling, Ceiling with Roof or Ground, Ground with Ceiling or Ground,
  Wall with Wall. A one-sided structure is
  offered only where one that pairs with it fits behind; the face behind takes only those. A two-sided
  structure has to pair with itself: Ground for a floor (ground both faces), or Roof and Ceiling.
- Rule 4 (`GetFacesOverInteriorGround`): a floor meeting the wall in a corner around an edge they
  share, whose face looking into that corner is its Ground (the one along its Up) and an interior. The
  wall's face looking back into the corner is drawn from the interiors with the chance. Where only one
  face of a wall is over an interior ground, that face is drawn first, so rule 3 then puts an exterior
  behind it. Rule 4 outranks rule 3, so a wall between two rooms is inside on both faces.
- The importer reads `"side"` and `"Roof"`; the add-on has a Side switch and a Roof type and writes
  `"side"` into every file it exports. A file without `"side"` stands for an exterior.
- The probe readout names the face by its fill (`roof`, `ground`, ...) and its side.

**My calls:**

- "Perpendicular, facing where the ground tile is" is read in the floor's own frame: the wall stands on
  the ground's walked face. Off an upside-down interior ground, the wall below it counts; a wall hanging
  under a floor does not, and neither does an interior ground facing down that is the back of another.
- A structure that is several types fills any face one of them fits, and pairs with whatever pairs with
  any of them.
- The default for **Types** stays Ground, Ceiling and Wall, so assets saved with the default keep it.
  Roof has to be ticked.
- **Both** counts as either wherever a rule asks for a side (an exterior behind an interior, an
  interior over an interior ground), but binds nothing itself: behind it anything follows, and a ground
  of both sides does not make the walls on it interiors.
- Rule 4 only acts on the wall's draw. A floor growing at the foot of an interior wall is not made an
  interior by it; the category pull still leans it that way where the categories match.

**Consequences to know about:**

- Your `ExtRural_Ground_RoofTile_*` are typed Ground, so they grow as walkable grounds, under ceilings
  as well as over them. Ticking Roof instead of Ground on them in Blender, and exporting them again,
  keeps them to tops over ceilings.
- An interior ground may have a ceiling under it as well as a ground; rule 3 makes that back an
  exterior four times in five.
- The underside of a ground-over-ground floor faces down and holds items unless its structure says
  otherwise, so objects stand on it upside down, as on any face.
- Rules 1, 2 and Hang From Ceilings still read a floor's faces by position: the face along Up is its
  ground, the other its ceiling, even where that other face is filled with an upside-down ground.

**Measured** (headless: all 77 WEM product tests pass, `Roles.WallsFaceInteriorGrounds` new among them):

| Test | Result |
|---|---|
| `Fill.OneSidedPairs` (200 beats, one-sided ground, roof, ceiling and wall per size) | 55 floors roof over ceiling, 18 ground over ceiling, 23 ground over ground, no roof over a ground |
| `Roles.Fill` (64 fills each way up) | upside down: 32 roof over ceiling with the roof underneath, 15 ground over ceiling, 17 ground over ground |
| `Roles.InteriorBacksExterior` at 1.0 (an interior, two exteriors and a wall of both sides) | 101 interior faces, 0 with an interior behind; behind exteriors 101 interiors and 258 exteriors; the wall of both sides goes behind interiors and has interiors behind it |
| same, rule off | 120 interior faces, 32 with an interior behind |
| `Roles.WallsFaceInteriorGrounds` at 1.0 | 100 of 100 walls on a room's edge inside over the room and outside behind; 100 of 100 between two rooms inside both faces; rule off, 43 of 100 inside over the room |
| `Roles.TurnedGrowth` (all three turns at 0.3, roofs in the kit) | 400 placements, 0 drift, every fill pairs |

Stress tests, all pass (run with the editor open): `Fill.VerifyEvery1000` 1000 placements, 0 drift;
`Fence.RunLength` mean run 9.08 panels, as before; `LoadBearing.Stall` median 3466 platforms all
load-bearing, 3348 with 5% light, as before. `Perf.Beat` (one best-of-three run, no like-for-like
baseline): plain slabs 1.316 ms, test kit 1.470 ms, test kit turned 1.560 ms a beat.

`Perf.CategoryPatches` moved: default pull median 11.2 (6.8 to 17.3 over four seeds), strong 60.7.
`NOTES.md` has 22.0 and 125.6 from 2026-10-09, before relative roles as well, so it is not a
like-for-like comparison. The test kit has no roofs, so its floors are now ground over ceiling or
ground over ground, and some undersides draw from its grounds' categories instead of its ceilings'.
