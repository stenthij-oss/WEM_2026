# Builds the calibration structure: the letter F in relief on a 3 x 3 m slab.
#
# An F reads differently under every turn and mirror, so laying it on a floor, a ceiling and walls
# facing each way shows at a glance whether the room manager lays structures the right way round.
# It is one-sided, of all three types, 16 x 16 cells beam to beam.
#
# Run from a terminal, in a Blender of its own:
#   blender --background --factory-startup --python make_calibration.py
# It saves Calibration.blend beside this script and exports Calibration_F through the WEM add-on,
# into ../Export, ready for the structure importer.

import os
import sys

import bmesh
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import wem_structures  # noqa: E402  (the add-on, run as a plain module)

# Blender metres. The slab fills the opening of a 16 x 16 segment (15 cells of 20 cm) and the
# half-cell from the segment's mid-plane to its face; the F stands proud of the face, as relief may.
SLAB_HALF = 1.5
SLAB_TOP = 0.10
RELIEF_TOP = 0.25

# The F's three rectangles, (x0, x1, y0, y1): the stem, the top bar and the middle bar. The
# importer's calibration test (WEM.Structures.Import.CalibrationAxes) knows them too.
F_RECTANGLES = [
    (-0.6, -0.3, -1.0, 1.0),
    (-0.3, 0.7, 0.7, 1.0),
    (-0.3, 0.4, 0.0, 0.3),
]


def make_material(name, colour):
    material = bpy.data.materials.new(name)
    if material.node_tree is None:
        material.use_nodes = True
    shader = material.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*colour, 1.0)
    shader.inputs["Roughness"].default_value = 0.8
    return material


def add_box(mesh, x0, x1, y0, y1, z0, z1, material_index):
    corners = [mesh.verts.new((x, y, z)) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]

    for face in faces:
        mesh.faces.new([corners[index] for index in face]).material_index = material_index


def build():
    # Start from nothing: the factory startup's cube, camera and light are not part of it.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)

    mesh = bmesh.new()
    add_box(mesh, -SLAB_HALF, SLAB_HALF, -SLAB_HALF, SLAB_HALF, 0.0, SLAB_TOP, 0)

    for x0, x1, y0, y1 in F_RECTANGLES:
        add_box(mesh, x0, x1, y0, y1, SLAB_TOP, RELIEF_TOP, 1)

    bmesh.ops.recalc_face_normals(mesh, faces=mesh.faces)

    # Plain box-projected UVs, so the exporter can write tangents.
    uv_layer = mesh.loops.layers.uv.new("UVMap")
    for face in mesh.faces:
        axis = max(range(3), key=lambda a: abs(face.normal[a]))
        for loop in face.loops:
            co = loop.vert.co
            u, v = [co[a] for a in range(3) if a != axis]
            loop[uv_layer].uv = (u, v)

    data = bpy.data.meshes.new("Calibration_F")
    mesh.to_mesh(data)
    mesh.free()

    data.materials.append(make_material("Calibration_Slab", (0.55, 0.55, 0.55)))
    data.materials.append(make_material("Calibration_F", (0.8, 0.05, 0.03)))

    obj = bpy.data.objects.new("Calibration_F", data)
    bpy.context.scene.collection.objects.link(obj)

    settings = wem_structures.get_settings(obj)
    settings.types = {"GROUND", "CEILING", "WALL"}
    settings.two_sided = False
    settings.load_bearing = True
    settings.holds_items = True
    settings.weight = 1.0

    return obj


def main():
    wem_structures.register()

    obj = build()
    bpy.context.scene.wem_structures_folder = os.path.dirname(HERE)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "Calibration.blend"))

    problems = wem_structures.export_structure(obj, bpy.context)
    if problems:
        print("Calibration_F was not exported:\n  " + "\n  ".join(problems))
        sys.exit(1)

    print("Calibration_F exported.")


if __name__ == "__main__":
    main()
