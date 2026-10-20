# Orbs

Asked for and built on 2026-10-19/20, on branch `nature`. Not committed.

Each of the 27 zones has an orb hovering over one of its horizontal segments. When you fly close, a
halo opens around the orb and glistens wider and harder. **A** catches it: the orb fades, a
snippet of text is written out on screen letter by letter, and the count in the top right goes up.
When every orb (140) has been caught, the game stops.

## Controls

- **A** on the gamepad, or **Space**, catches the nearest orb that is near enough (5 m).
- The zoom that used to be held on A is now held on **B**.

## How it behaves

- **Where an orb stands:** on a horizontal segment of its zone, picked at random, as near the
  middle as there is room. The surface under it must be one that carries objects (not a beam or a
  closed-off face), and the air above must be open up to the top of its bob.
- **Segment lost:** if the segment is struck down, cut loose or reshuffled, the orb goes to another
  segment of the same zone in the same frame. If growth builds into the orb's air, it moves too.
  Orbs don't hold growth back.
- **After a catch:** the segment the orb hovered over gets **+300 health** (segments start with
  80-120), and the zone **rests for 60 s** before it gets a new orb.
- **Snippets:** each one is shown once before any is shown again. When all have been shown, the
  order is shuffled again, and the first of the new round is never the one just shown.
- **Text and counter:** drawn over the finished picture, so the sky glitch, the phone look and the
  AI overlay never touch them. In portrait mode (P) they sit inside the portrait frame.
- **AI overlay:** never paints the orb. The overlay only paints pixels marked with the segments'
  custom stencil value (1). The orb writes its own value (2) over that, so an orb in front of a
  segment stays as it is. Only the dithered fringe of the halo can let a little overlay through.

## Writing snippets

Snippets are plain text files in `Content/Orbs/Snippets/`. Edit them in any text editor. There's
nothing to import and nothing to rebuild: the files are read again every time Play starts.

- One snippet is one paragraph. Leave a blank line between snippets.
- A line break inside a paragraph is kept on screen. Long lines wrap by themselves.
- A snippet of any length works. One too long to fit below where the text starts begins higher, up
  to **Text Top**, so it ends above **Text Bottom**. If it still doesn't fit, its letters get
  smaller, down to **Smallest Text Scale**. Short snippets look exactly the same as before.
- Long snippets are also written faster, so none takes more than **Longest Write Seconds** (15) to
  appear.
- Lines starting with `#` are notes and are never shown.
- Every `.txt` file in the folder is read, in name order, so the text can be split over several
  files (one per theme, one per writer, and so on).

The snippets in `Snippets.txt` now are placeholders.

## Settings

Select **ObjectPlacer** in the Outliner. Its Details panel has an **Orbs** section and an
**Orbs › Text** section.

| Setting | Default | What it does |
|---|---|---|
| Place Orbs | on | The whole feature. |
| Orb Class | Orb | What stands in each zone. A Blueprint made from Orb can change its size, bob and halo distances. |
| Orb Hover Height | 130 cm | How high the middle of an orb hovers over its segment. |
| Orb Max Shift | 4 cells | How far from a segment's middle an orb may stand when the middle has no room. |
| Zone Cooldown Seconds | 60 | How long a zone rests after its orb is caught. |
| Catch Health Boost | 300 | Health given to the segment an orb was caught over. |
| Orbs To Catch | 140 | The count's total. When it's reached, the game stops. |
| Catch Action | IA_Drone_Interact | The input that catches. |
| Snippet Folder | Orbs/Snippets | Where the snippets are, under Content. |
| Text Font | (empty) | A Font asset. Left empty, the Garamond that comes with Windows is used. |
| Text Color | yellow | Colour of the snippets and the count. |
| Text Height | 0.03 | A letter's height, as a share of the frame's height. |
| Letter Spacing | 0.12 | Extra room after each letter, as a share of a letter's height. |
| Letters Per Second | 10 | How fast the snippet is written out. |
| Letter Fade Seconds | 0.3 | How long each letter takes to fade in. |
| Longest Write Seconds | 15 | The longest a snippet takes to be written out. Longer snippets are written faster. |
| Hold Seconds, Hold Seconds Per Letter | 2.5, 0.05 | How long a snippet stays once written out. |
| Text Fade Out Seconds | 1.5 | How long it takes to fade away. |
| Text Tilt | 3° | How far each line leans, rising to the right. |
| Text Position, Text Width | (0.28, 0.54), 0.36 | Where the first line starts and how wide lines run, as shares of the frame. |
| Portrait Text Position, Portrait Text Width | (0.1, 0.56), 0.8 | The same in the portrait frame. |
| Text Top, Text Bottom | 0.12, 0.94 | How high a long snippet may start and how low it may end, as shares of the frame's height. |
| Smallest Text Scale | 0.6 | The smallest a long snippet's letters get, as a share of Text Height. |
| Counter Height | 0.032 | The count's height. |
| Counter Icon | T_Orb | The orb beside the count. |

On the orb itself (a Blueprint made from **Orb**): Diameter 90 cm, Hover Amplitude 12 cm, Hover
Period 3.2 s, Halo Distance 12 m (the halo starts to show), Full Halo Distance 1.5 m (at its
widest), Catch Distance 5 m, Fade Seconds 1.4.

The look is in `/Game/Orbs/M_Orb`: an unlit material with scalar parameters `OrbBrightness`,
`HaloBrightness`, `OrbEdge` and the vector `HaloColor`. A material instance of it can be set on an
Orb Blueprint.

## Notes

- The orb is drawn opaque with a dithered edge, not translucent. The sky glitch decides what is sky
  by depth, so a translucent orb seen against the sky would be painted over by the trails.
- The Garamond that comes with Windows is Monotype's and arrives with Microsoft Office. It's fine on
  this machine, but its licence is unlikely to allow shipping it in a build. EB Garamond is free to
  embed. Import it as a Font asset and set it as Text Font.
- Furniture: the placer still places furniture when a catalog is set in **Catalogs**. NewMap has
  none, so it places only orbs.
- Code: `Orb.h/.cpp`, `OrbSpot.h/.cpp`, `OrbSnippets.h/.cpp`, `Private/OrbHud.h/.cpp`, the Orbs
  part of `ObjectPlacer.cpp`, tests in `Tests/OrbTests.cpp` (`WEM.Orbs.*`).
