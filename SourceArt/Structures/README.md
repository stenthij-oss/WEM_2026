# Structures: from Blender to the room manager

A **structure** is a mesh that fills a segment's opening between its beams. You model it in Blender,
give it its settings in the **WEM** tab, press **Export**, then press **Import** in Unreal. The room
manager draws from every structure in its `Structures` list, by weight, among those that fit.

For many structures at once - tagging in bulk (**Copy Settings to Selected**), **Export Selected**,
exporting from the command line, and how to organise a thousand panels - see `TUTORIAL.md`.

## The convention

| | |
|---|---|
| Units | Real size, Blender's default units: 1 unit = 1 m. |
| Lying | On the ground, face up (+Z). The back is on Z = 0. |
| Centre | Centred on the origin in X and Y. Transforms applied: location 0, rotation 0, scale 1. |
| Width, length | X is the width, Y the length. On a wall, X runs along the ground and +Y points up: the top of a facade is at +Y, the top of the screen in Blender's top view. |
| Thickness | One-sided: Z 0 to 0.10 (from the segment's mid-plane to its face). Two-sided: Z 0 to 0.20 (both faces). |
| Relief | May rise beyond a face (grass, stones). Nothing of a one-sided structure may go below Z = 0, where its partner is. A structure that holds items keeps its walkable face at the face plane. |
| Edges | Closed: they show wherever a beam is not built. |

Sizes are counted in 20 cm cells **beam to beam**, as the tiles are. Each beam takes 10 cm from the
segments on either side of it, so the opening, which is what you model, is one cell short:

| Tile (cells) | Opening (m) |
|---|---|
| 8 x 8 | 1.40 x 1.40 |
| 8 x 16 | 1.40 x 3.00 |
| 16 x 16 | 3.00 x 3.00 |
| 16 x 32 | 3.00 x 6.20 |
| 32 x 32 | 6.20 x 6.20 |

Either way round: an **upright** 8 x 16 wall structure is 1.40 m wide and 3.00 m tall; a **lying**
16 x 8 one is 3.00 m wide and 1.40 m tall. The size is measured from the bounds, to within 1 cm.

## The add-on

`wem_structures.py` in this folder. Install it (Edit > Preferences > Add-ons > Install from Disk),
or open it in Blender's Text Editor and press Run Script. It adds a **WEM** tab to the 3D viewport's
sidebar (N) for the active object:

- **Folder**: this folder (`SourceArt/Structures`). Left empty, it is looked for beside the add-on
  and the .blend.
- **Types**, **Categories** (click to toggle; the reload button reads `categories.json` again),
  **Two-sided**, **Fence** (only shown for two-sided), **Load-bearing**, **Holds items**,
  **May lie sideways**, **Weight**. They are stored on the object, so they live in the .blend.
- **Export** checks the convention - origin, applied transforms, back on Z = 0 and nothing below it
  for one-sided, a tile's opening either way round within 1 cm, fence only when two-sided, the .blend
  saved - and refuses with the reason when a check fails. Otherwise it writes
  `Export/<Name>.glb` (glTF, modifiers applied, normals and tangents) and the settings file
  `Export/<Name>.json` beside it. The object's name becomes the asset name: letters, digits and
  underscores only.

## Importing

Open `/Game/Structures/DA_StructureImporter` and press **Import**. It:

1. writes `categories.json` here, the list of categories the add-on offers;
2. imports every new or changed .glb to `/Game/Structures/<W>x<L>/SM_<Name>`, with its materials
   beside it, and checks its bounds again;
3. makes or updates `DA_Structure_<Name>` from the settings file;
4. turns Nanite on for a mesh with more than `NaniteTriangleThreshold` triangles;
5. gives a material slot named after a material in `/Game/Structures/Materials` that material;
6. adds new structures to every room manager in the open level, as one step you can undo;
7. reports to the **Structure Import** message log and to `LastReport` on the importer.

Save afterwards (File > Save All). A structure whose .glb and .json have not changed since its last
import is left as it is.

## Adding a category

Categories are data assets, so no build is needed. In the Content Browser, in
`/Game/Structures/Categories`, right-click > Miscellaneous > Data Asset > **Structure Category**,
name it `DA_Category_<Something>` and set its **Display Name**. Press **WriteCategoryList** (or
Import) on the importer, then the reload button in Blender's WEM tab.

## What each setting does

- **Types**: which faces it may fill. Ground is the top of a floor, Ceiling its underside, Wall either
  face of a wall. A two-sided structure fills both faces, so a floor needs Ground and Ceiling.
- **Two-sided**: fills the whole segment, both faces. Otherwise it fills one face, and another
  one-sided structure fills the other.
- **Categories**: any number. A face is pulled toward the categories of the faces beside it, more
  strongly the longer the run of that category is, so districts form. A structure with none takes no
  part in the pull.
- **Load-bearing** (on by default): off means nothing is built onto it. A wall stops at its top beam;
  more panels can still join beside it. A floor or ceiling blocks every beam no segment used before it.
- **Holds items** (on by default): off means its face carries no objects and opens into no
  floorspace. The beams beside it stay wall surface, so walls can still fold up there.
- **Fence**: two-sided only. The structure tends to continue as a run of copies of itself - along the
  ground on a wall, turning corners, or straight along its width across a floor. A fence that is not
  load-bearing makes a parapet.
- **May lie sideways**: a wall structure may be turned on its side to fill a wall of the other shape.
  Without it, upright and lying walls each need their own structure. Floors turn freely anyway.
- **Weight**: how often it is drawn among the structures that fit. Only set from Blender when the
  structure is first imported; tune it in Unreal after that.

## The settings file

`<Name>.json` beside `<Name>.glb` holds everything the room manager needs, as plain text you can read
and diff: the format, the name, where it came from (`blend`, `object`), its size in cells (measured,
never typed), the settings above and its bounds in metres in Blender's axes.

**The settings file is the source of truth.** A re-import overwrites the structure asset's settings
from it, and the report lists every value it changed, so an edit made in Unreal is never lost without
a word - make lasting changes in Blender. The one exception is the **weight**, which is only written
when the asset is created, because weights get tuned in Unreal while watching growth.

## Materials

glTF carries image textures and plain values on a **Principled BSDF** (base colour, roughness,
metallic, normal map). Procedural node trees do not survive any export format: bake them to images
first. Or name the material slot after an Unreal material and put that material in
`/Game/Structures/Materials`: the importer gives the slot that material instead of the imported one.
Materials of the same name in one size folder are shared, so give different materials different names.

The room manager draws structures instanced, which a material has to allow. The importer turns that
on for every material in the project a structure uses; a material from outside the project (an
engine or plugin material) cannot be changed, and the report says so - the default checker is drawn
in its place.

## In this folder

- `wem_structures.py`: the add-on.
- `categories.json`: written by the importer; do not edit.
- `Export/`: what the add-on exports, and what `DA_StructureImporter` imports.
- `Calibration/`: the letter F that checks structures are laid the right way round
  (`make_calibration.py`, run with `blender --background --factory-startup --python`).
- `TestKit/`: plain coloured test structures for watching growth, built by `make_test_kit.py` and
  imported by `/Game/Structures/TestKit/DA_StructureImporter_TestKit`. Test content only.
