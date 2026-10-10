# Relative roles: plan

Approved 2026-10-09, on branch `segment-structures` after 981bf29, with the decisions at the end.

## The idea

Ground, wall and ceiling stop being world directions. Every segment carries its own **Up**, one of the six lattice directions, and its role follows from that:

- **Ground / ceiling:** Up lies along the segment's normal. The face looking along Up is Ground and the other one is Ceiling.
- **Wall:** Up lies in the segment's plane. Both faces are Wall, and Up is the way its structures stand.

A new segment takes Up from the segment it grows off (its **parent**), and the geometry sets the role. A perpendicular off a ground is a wall standing on it. Off a wall's top or bottom edge it's a ground or ceiling. Off a wall's side edge it's a corner wall. Carrying on in the same plane keeps the role. The three turns below are the only things that change Up. Each is a setting you can switch on and off, with its own chance.

The first segment is a ground with Up = +Z, and so is every segment placed before this change. So with all three turns off, growth looks exactly like it does today.

## Settings (room manager)

| Setting | Default | What it does |
|---|---|---|
| **Flip on perpendicular growth** (on/off) + chance | off, 0.15 | A segment grown perpendicular off its parent sometimes takes the opposite Up. |
| **Hang from ceilings** (on/off) | off | A wall grown off a ceiling's holding face, the underside, hangs from it: its Up points away from the ceiling, so it stands upside down. |
| **Quarter-turn** (on/off) + chance | off, 0.15 | A segment carrying on a wall in the wall's own plane sometimes becomes a ground, taking the direction of the wall's holding face as its Up. Floors appear on walls. |
| **Walls stand on grounds** (on/off) + chance | on, 0.8 | Rule 1: at the edge of a ground whose top holds items, a wall standing up from it is favoured over one hanging below it. |
| **Floor or ceiling by the opposite edge** (on/off) + chance | on, 0.8 | Rule 2: off a wall's holding face, a ceiling on the wall's top edge favours a ground at its bottom edge. A ground on its bottom edge favours a ceiling at its top edge. With neither, a ground is favoured. |
| **Interior backs exterior** (on/off) + chance | on, 0.8 | Rule 3: the back of a one-sided face from an `Interior…` category is pulled toward `Exterior…` categories, and the reverse. This is read from the category asset's name. Other categories aren't affected. |

"Holding face" means the face whose structure has *Holds items* on. Both faces of a two-sided structure count.

## What changes from world up to the segment's Up

- **Structure types:** `GetFaceType` reads the segment's Up, not world Z.
- **Shape fit:** "a wall stands with its length up" uses the axis Up lies along. Grounds and ceilings still turn freely.
- **Mesh transform:** a wall's mesh top (Blender +Y) points along Up, including straight down. Ground and ceiling meshes face out along ±Up.
- **Fill order:** a ground or ceiling segment draws its Ground face first.
- **Load-bearing:** a wall that isn't load-bearing blocks its top beam, where "top" is the edge along Up.
- **Fences:** a wall fence runs along the ground in its own frame, across Up. "Stands on something" uses its own top and bottom. Corner turns keep Up.
- **Saving and replay:** Up is stored on `FGridPlatform`, because flips are random. The rebuild replays it from there.

## Phases (one commit each)

1. **Up on every segment.** Up is stored and inherited, and every reader above switches to it. No turns or biases yet. All 60 existing tests should pass unchanged. New tests cover the types, fit, transform, top beam and fence run on an upside-down and a sideways frame.
2. **The three turns.** Flip, hang from ceilings and quarter-turn, each with a toggle and a chance. Tests cover each one alone and all together, plus a 1000-placement VerifyEvery check.
3. **Growth biases (rules 1 and 2).** The candidate draw stays uniform. A drawn candidate that goes against a rule is redrawn with the rule's chance, up to a few tries, so the cost per beat stays small. Beat time is measured against 981bf29.
4. **Interior backs exterior (rule 3).** This adds a pull on the second face's draw in `DrawFill`, plus a test.
5. **Objects follow Up.** Objects stand on a segment's Ground face and hang from its Ceiling face, whichever way they point in the world.
6. **Notes and measurements** in `Structures/NOTES.md`.

## Decisions (2026-10-09)

1. **Parent:** when a candidate touches several segments, one of them is drawn at random as its parent.
2. **Tiles and the kind roll stay on the lattice.** *Horizontal only / Vertical only* tiles and `VerticalPlacementChance` keep meaning world planes. Upside down, a "wall" can therefore be laid from a horizontal-only tile.
3. **Objects follow Up** (phase 5).
4. **All phases are built in one go**, one commit each.
