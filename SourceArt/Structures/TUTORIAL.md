# Tutorial: many structures from Blender to Unreal

For working at scale - hundreds to a thousand panels. The conventions and what each setting means
are in `README.md`; this is the workflow.

## 0. Once: categories

Make your categories in Unreal before tagging anything in Blender.

1. Content Browser, `/Game/Structures/Categories`, right-click > Miscellaneous > Data Asset >
   **Structure Category**. Name it `DA_Category_<Something>` (no spaces). Set **Display Name**.
2. Open `/Game/Structures/DA_StructureImporter`, press **WriteCategoryList**. This writes
   `SourceArt/Structures/categories.json`.
3. In Blender's WEM tab, press the reload button next to Categories.

Do this again whenever you add a category. Renaming or deleting a category asset breaks every
structure that names it: the importer reports "there is no category asset named ...".

## 1. Organise the .blend files

A thousand objects in one .blend is slow to work in. Split by family instead, for example
`facades_urban.blend`, `floors_natural.blend`, `ceilings_office.blend`, roughly 50-150 structures each.
All of them export into the same folder, `SourceArt/Structures/Export`.

- **Names are global.** The object name becomes `SM_<Name>` and `DA_Structure_<Name>`, and the export
  folder is shared. The add-on refuses to export a name already exported from another .blend. Use a
  prefix per file: `FacU_014`, `FlrN_203`. Letters, digits and underscores only.
- **One collection per kind** in each file - Ground, Ceiling, Wall, TwoSided, Fences - makes selecting
  and batch-tagging easy.
- **Material names are shared within a size folder** in Unreal: two different materials both called
  `Brick` in 16 x 16 structures become one. Name materials per family (`FacU_Brick_Red`). Use image
  textures on a Principled BSDF; procedural node trees do not survive export.
- Save the .blend before exporting: the settings file records which .blend each structure came from.

## 2. Build each structure to the convention

Origin at the world origin, transforms applied (Ctrl A > All Transforms), back on Z = 0, centred in X
and Y, opening size exactly one of the tile openings (1.40, 3.00 or 6.20 m per side). The panel in the
WEM tab shows the measured size in cells, and lists every problem in red before you export.

A handy start: duplicate a correct blank of each size (for example a 1.40 x 3.00 x 0.10 box) and model
on top of it, so every piece starts in the right place.

## 3. Tag in bulk

You never need to set a thousand panels one by one:

1. Set up one object fully in the WEM tab: types, categories, two-sided, load-bearing, holds items,
   fence, may lie sideways, weight.
2. Select all the objects that should share those settings, then **shift-click the set-up one last**
   so it is the active object.
3. Press **Copy Settings to Selected**.

Then adjust the odd ones individually. The settings live on the objects, so they are saved in the .blend.

Mostly one-sided is a good default: each face then gets its own structure and its own category pull,
so the two sides of a wall differ. Keep two-sided for pieces that really are one thing through
(fences, railings, grilles, thin slabs), and give their front and back different looks.

## 4. Export in bulk

**From Blender's UI:** select everything (or one collection), press **Export Selected** in the WEM tab.
Refused objects are listed in the Info editor with the reason; the rest are written. An object whose
mesh and settings have not changed keeps its files as they were, so re-exporting everything is cheap
and the importer will skip it.

**From the command line** (no UI, good for many files):

```
blender --background facades_urban.blend --python "SourceArt/Structures/wem_structures.py" -- --export-all
blender --background facades_urban.blend --python "SourceArt/Structures/wem_structures.py" -- --export-all --collection Walls
```

It prints `checked, changed and written, refused` and exits with an error when anything was refused.
To export every .blend in a folder, in PowerShell:

```
Get-ChildItem "D:\path\to\blends\*.blend" | ForEach-Object {
  & "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background $_.FullName --python "D:\Unreal projects\Weltraum_Erde_Mensch\WEM_2026\SourceArt\Structures\wem_structures.py" -- --export-all --folder "D:\Unreal projects\Weltraum_Erde_Mensch\WEM_2026\SourceArt\Structures"
}
```

`--folder` points at `SourceArt/Structures` when the .blend files live elsewhere.

## 5. Import

Open `/Game/Structures/DA_StructureImporter`, press **Import**.

- It skips every structure whose .glb and .json have not changed, so after the first big import only
  what you changed comes in.
- The first import of a thousand structures takes a while - roughly a second each, so plan on 15-20
  minutes. Let it run; a progress bar shows.
- New structures are added to every room manager in the open level (NewMap). Changed ones are
  re-imported and the room managers rebuild.
- Read the **Structure Import** message log, or `LastReport` on the importer: every refused file,
  unknown category, missing type and every setting an import overwrote is listed there.
- **File > Save All** afterwards. The importer does not save for you.

Settings made in Unreal are overwritten by the next import of that structure, except the weight. Make
lasting changes in Blender. `bReimportAll` on the importer forces everything in again once.

## 6. Tune

With hundreds of structures in the room manager's `Structures` list:

- **Weights** are tuned in Unreal on each `DA_Structure_*` while watching Simulate; they are never
  overwritten by an import.
- **Every tile shape needs a complete fill.** If a size has no structure for a face, that tile is never
  laid that way, and the log says "no structures can fill tile ...". Check the log after a big import.
- The pull settings on the room manager (ContinueChance, MaxContinueChance, StreakForMaxChance) decide
  how strongly categories cluster; `CategoryPatchSize` shows the result. More categories per structure
  means more overlap between districts.

## 7. Removing a structure

Delete its `.glb` and `.json` from `Export`, then in Unreal remove it from the room manager's
`Structures` list and delete its `SM_` and `DA_Structure_` assets. The importer never deletes anything.

## 8. Version control

Every `.glb` goes to Git LFS. A thousand panels with textures can be several GB; commit in batches, and
keep source textures out of `Export` (they are embedded in the .glb anyway).
