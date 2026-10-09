# Builds the structure test kit: plain coloured structures for every tile size, category and kind of
# fill, made to watch growth with - not to keep. The real structures are authored by hand.
#
#   - mostly one-sided: per size four grounds, three ceilings and four walls across the categories,
#     ribbed so their face reads, with four lying walls for each of 8 x 16 and 16 x 32;
#   - fewer two-sided: two slabs per size and three striped multi-category slabs, every one with a
#     front and back that cannot be mistaken - its colour on the front with a white corner tab, a dark
#     shade on the back with a black bar across;
#   - two fences, both two-sided walls with an arrow along the run: a load-bearing railing, 8 x 8,
#     and a parapet that is not load-bearing, 8 x 16 lying;
#   - a facade that is not load-bearing, upright 8 x 16.
#
# Every name starts with TK_. Run from a terminal, in a Blender of its own:
#   blender --background --factory-startup --python make_test_kit.py
# It saves TestKit.blend beside this script and exports every structure through the WEM add-on into
# Export/ beside it, for DA_StructureImporter_TestKit to import into /Game/Structures/TestKit.

import os
import sys

import bmesh
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import wem_structures  # noqa: E402  (the add-on, run as a plain module)

CELL = wem_structures.CELL
SIZES = [(8, 8), (8, 16), (16, 16), (16, 32), (32, 32)]

# Category asset name suffix, and its colour.
CATEGORIES = [
    ("ExteriorUrban", (0.32, 0.38, 0.52)),
    ("ExteriorRural", (0.78, 0.62, 0.30)),
    ("ExteriorIndustrial", (0.55, 0.22, 0.10)),
    ("InteriorDomestic", (0.88, 0.52, 0.55)),
    ("InteriorOffice", (0.40, 0.72, 0.74)),
    ("NaturalFlora", (0.18, 0.52, 0.16)),
    ("NaturalSand", (0.93, 0.83, 0.55)),
    ("NaturalStone", (0.30, 0.30, 0.28)),
]

COLOURS = dict(CATEGORIES)
MATERIALS = {}


def asset(key):
    return "DA_Category_" + key


def material(name, colour):
    if name not in MATERIALS:
        made = bpy.data.materials.new(name)
        if made.node_tree is None:
            made.use_nodes = True
        shader = made.node_tree.nodes.get("Principled BSDF")
        shader.inputs["Base Color"].default_value = (*colour, 1.0)
        shader.inputs["Roughness"].default_value = 0.85
        MATERIALS[name] = made
    return MATERIALS[name]


def category_material(key):
    return material("TK_" + key, COLOURS[key])


class Builder:
    """A mesh made of boxes and extruded outlines, each in one of its materials."""

    def __init__(self):
        self.mesh = bmesh.new()
        self.materials = []

    def slot(self, mat):
        if mat not in self.materials:
            self.materials.append(mat)
        return self.materials.index(mat)

    def box(self, x0, x1, y0, y1, z0, z1, mat):
        index = self.slot(mat)
        corners = [self.mesh.verts.new((x, y, z)) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
        for face in [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]:
            self.mesh.faces.new([corners[i] for i in face]).material_index = index

    def prism(self, outline, z0, z1, mat):
        index = self.slot(mat)
        bottom = [self.mesh.verts.new((x, y, z0)) for x, y in outline]
        top = [self.mesh.verts.new((x, y, z1)) for x, y in outline]
        self.mesh.faces.new(list(reversed(bottom))).material_index = index
        self.mesh.faces.new(top).material_index = index
        count = len(outline)
        for i in range(count):
            j = (i + 1) % count
            self.mesh.faces.new([bottom[i], bottom[j], top[j], top[i]]).material_index = index

    def finish(self, name):
        bmesh.ops.recalc_face_normals(self.mesh, faces=self.mesh.faces)

        # Plain box-projected UVs, so the exporter can write tangents.
        uv_layer = self.mesh.loops.layers.uv.new("UVMap")
        for face in self.mesh.faces:
            axis = max(range(3), key=lambda a: abs(face.normal[a]))
            for loop in face.loops:
                u, v = [loop.vert.co[a] for a in range(3) if a != axis]
                loop[uv_layer].uv = (u, v)

        data = bpy.data.meshes.new(name)
        self.mesh.to_mesh(data)
        self.mesh.free()
        for mat in self.materials:
            data.materials.append(mat)

        obj = bpy.data.objects.new(name, data)
        bpy.context.scene.collection.objects.link(obj)
        return obj


def half_extents(width, length):
    return (width - 1) * CELL / 2.0, (length - 1) * CELL / 2.0


def configure(obj, types, categories, two_sided, load_bearing=True, fence=False, lie=False):
    settings = wem_structures.get_settings(obj)
    settings.types = set(types)
    settings.two_sided = two_sided
    settings.load_bearing = load_bearing
    settings.holds_items = True
    settings.fence = fence
    settings.may_lie_sideways = lie
    settings.weight = 1.0
    settings.set_categories([asset(key) for key in categories])


def back_material(key):
    """The same category, much darker: the back of a two-sided structure never passes for its front."""
    return material("TK_" + key + "_Back", tuple(0.3 * c for c in COLOURS[key]))


def mark_front_and_back(builder, hx, hy):
    """
    A white tab standing on the front face at its +X, +Y corner, and a black bar across the back
    from corner to corner, so either face, and which way round it lies, reads at a glance.
    """
    tab = min(hx, hy) * 0.35
    builder.box(hx - tab - 0.05, hx - 0.05, hy - tab - 0.05, hy - 0.05, 0.20, 0.23, material("TK_FrontMark", (0.95, 0.95, 0.95)))
    w = 0.08
    builder.prism([(-hx + 0.05, -hy + 0.05 + w), (-hx + 0.05 + w, -hy + 0.05), (hx - 0.05, hy - 0.05 - w), (hx - 0.05 - w, hy - 0.05)],
                  -0.03, 0.0, material("TK_BackMark", (0.03, 0.03, 0.03)))


def make_slab(width, length, key):
    """Two-sided: its category colour on the front half, a dark shade of it on the back half."""
    hx, hy = half_extents(width, length)
    builder = Builder()
    builder.box(-hx, hx, -hy, hy, 0.0, 0.10, back_material(key))
    builder.box(-hx, hx, -hy, hy, 0.10, 0.20, category_material(key))
    mark_front_and_back(builder, hx, hy)
    obj = builder.finish(f"TK_Slab_{width}x{length}_{key}")
    configure(obj, {"GROUND", "CEILING", "WALL"}, [key], two_sided=True, lie=True)


def make_mixed(width, length, keys, number):
    """Two-sided, striped in its categories on the front, dark on the back."""
    hx, hy = half_extents(width, length)
    builder = Builder()
    step = 2.0 * hx / len(keys)
    builder.box(-hx, hx, -hy, hy, 0.0, 0.10, material("TK_MixBack", (0.06, 0.06, 0.07)))
    for index, key in enumerate(keys):
        builder.box(-hx + index * step, -hx + (index + 1) * step, -hy, hy, 0.10, 0.20, category_material(key))
    mark_front_and_back(builder, hx, hy)
    obj = builder.finish(f"TK_Mix_{width}x{length}_{number}")
    configure(obj, {"GROUND", "CEILING", "WALL"}, keys, two_sided=True, lie=True)


def make_one_sided(kind, width, length, key):
    """A 10 cm slab with ribs on its face: across Y on a ground, across X on a wall or ceiling."""
    hx, hy = half_extents(width, length)
    builder = Builder()
    mat = category_material(key)
    builder.box(-hx, hx, -hy, hy, 0.0, 0.10, mat)

    ribs = 5
    if kind == "GROUND":
        for i in range(ribs):
            x = -hx + (i + 0.5) * 2.0 * hx / ribs
            builder.box(x - 0.03, x + 0.03, -hy, hy, 0.10, 0.13, mat)
    else:
        for i in range(ribs):
            y = -hy + (i + 0.5) * 2.0 * hy / ribs
            builder.box(-hx, hx, y - 0.03, y + 0.03, 0.10, 0.13, mat)

    label = {"GROUND": "Ground", "CEILING": "Ceiling", "WALL": "Wall"}[kind]
    obj = builder.finish(f"TK_{label}_{width}x{length}_{key}")
    configure(obj, {kind}, [key], two_sided=False)


def arrow(hx, hy, z0, z1, builder, mat):
    """An arrow along +X across the middle of the slab, the way the run goes."""
    shaft = 0.12 * hy
    head = 0.45 * hy
    tip = 0.75 * hx
    neck = tip - 0.6 * head
    outline = [(-0.75 * hx, -shaft), (neck, -shaft), (neck, -head), (tip, 0.0), (neck, head), (neck, shaft), (-0.75 * hx, shaft)]
    builder.prism(outline, z0, z1, mat)


def make_fence(name, width, length, key, load_bearing):
    hx, hy = half_extents(width, length)
    builder = Builder()
    front = material("TK_FenceBody", (0.85, 0.85, 0.82))
    back = material("TK_FenceBack", (0.20, 0.20, 0.22))
    builder.box(-hx, hx, -hy, hy, 0.0, 0.10, back)
    builder.box(-hx, hx, -hy, hy, 0.10, 0.20, front)

    # An arrow on both faces, so the run reads from either side: black on the light front, white on
    # the dark back.
    arrow(hx, hy, 0.20, 0.24, builder, material("TK_FenceArrow", (0.05, 0.05, 0.05)))
    arrow(hx, hy, -0.04, 0.0, builder, material("TK_FenceArrowBack", (0.95, 0.95, 0.95)))

    obj = builder.finish(name)
    configure(obj, {"WALL"}, [key], two_sided=True, load_bearing=load_bearing, fence=True)


def make_facade():
    width, length = 8, 16
    hx, hy = half_extents(width, length)
    builder = Builder()
    wall = category_material("ExteriorUrban")
    frame = material("TK_FacadeFrame", (0.08, 0.08, 0.09))
    builder.box(-hx, hx, -hy, hy, 0.0, 0.10, wall)

    # Two storeys of window frames standing proud of the face.
    for row in (-0.5, 0.5):
        cy = row * hy
        for x0, x1, y0, y1 in [(-0.45, 0.45, -0.42, -0.36), (-0.45, 0.45, 0.36, 0.42), (-0.45, -0.39, -0.42, 0.42), (0.39, 0.45, -0.42, 0.42)]:
            builder.box(x0, x1, cy + y0, cy + y1, 0.10, 0.16, frame)

    obj = builder.finish("TK_Facade_8x16")
    configure(obj, {"WALL"}, ["ExteriorUrban"], two_sided=False, load_bearing=False)


def build():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)

    keys = [key for key, _ in CATEGORIES]

    def pick(start, count):
        return [keys[(start + i) % len(keys)] for i in range(count)]

    # Two-sided: two slabs per size, a striped slab for three sizes, and the two fences.
    for index, (width, length) in enumerate(SIZES):
        for key in pick(index * 2, 2):
            make_slab(width, length, key)

    mixes = [["ExteriorUrban", "ExteriorIndustrial"], ["ExteriorRural", "NaturalFlora"], ["NaturalSand", "NaturalStone", "NaturalFlora"]]
    for number, (width, length) in enumerate([(8, 16), (16, 16), (32, 32)], start=1):
        make_mixed(width, length, mixes[number - 1], number)

    make_fence("TK_Fence_Railing_8x8", 8, 8, "ExteriorUrban", load_bearing=True)
    make_fence("TK_Fence_Parapet_16x8", 16, 8, "ExteriorIndustrial", load_bearing=False)

    # One-sided, the majority: per size four grounds, three ceilings and four walls across the
    # categories, and four lying walls for each long size.
    for index, (width, length) in enumerate(SIZES):
        for key in pick(index, 4):
            make_one_sided("GROUND", width, length, key)
        for key in pick(index + 3, 3):
            make_one_sided("CEILING", width, length, key)
        for key in pick(index + 5, 4):
            make_one_sided("WALL", width, length, key)
        if width != length:
            for key in pick(index + 1, 4):
                make_one_sided("WALL", length, width, key)

    make_facade()


def main():
    wem_structures.register()
    build()

    bpy.context.scene.wem_structures_folder = HERE
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "TestKit.blend"))

    # Structures no longer in the kit leave no files behind for the importer to pick up.
    export = os.path.join(HERE, "Export")
    names = {obj.name for obj in bpy.context.scene.objects}
    for file in os.listdir(export) if os.path.isdir(export) else []:
        if os.path.splitext(file)[0] not in names:
            os.remove(os.path.join(export, file))

    changed, refused = wem_structures.export_many(bpy.context.scene.objects, bpy.context)
    for name, problems in refused.items():
        print(f"{name} was not exported:\n  " + "\n  ".join(problems))

    two_sided = sum(1 for obj in bpy.context.scene.objects if wem_structures.get_settings(obj).two_sided)
    print(f"Test kit: {len(names) - len(refused)} exported ({len(names) - two_sided} one-sided, {two_sided} two-sided), "
          f"{len(changed)} changed, {len(refused)} refused.")
    sys.exit(1 if refused else 0)


if __name__ == "__main__":
    main()
