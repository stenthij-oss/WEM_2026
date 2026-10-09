# WEM structures: a Blender add-on for authoring the structures that fill WEM_2026's segments.
#
# A structure is a mesh that fills the opening of a segment between its beams. This add-on keeps the
# settings Unreal needs on the object itself - categories, types, two-sided, load-bearing, holds
# items, fence, may lie sideways and weight - and exports the object as a .glb with a settings .json
# beside it, which the structure importer in Unreal reads. Install it as an add-on, or open it in the
# Text Editor and press Run Script. See README.md beside it.

bl_info = {
    "name": "WEM Structures",
    "author": "WEM_2026",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D Viewport > Sidebar (N) > WEM",
    "description": "Settings and export for WEM_2026 segment structures",
    "category": "Import-Export",
}

import datetime
import json
import os
import re

import bpy
from mathutils import Vector

# The settings file format the importer reads. Bump it only together with the importer.
SETTINGS_FORMAT = 1

# One cell of the room manager's lattice, in metres. A structure of W x L cells beam to beam fills an
# opening of (W - 1) x (L - 1) cells, since each beam takes half a cell from the segments either side.
CELL = 0.20

# The tile sizes growth lays segments from, beam to beam, as the room manager's tiles have them. A
# structure has to fit one of them, either way round.
TILE_SIZES = [(8, 8), (8, 16), (16, 16), (16, 32), (32, 32)]

# How far a measured size or position may stray, in metres.
TOLERANCE = 0.01

# A one-sided structure fills half the segment's 20 cm, a two-sided one all of it.
ONE_SIDED_DEPTH = 0.10
TWO_SIDED_DEPTH = 0.20

TYPE_ITEMS = [
    ("GROUND", "Ground", "The top of a floor"),
    ("CEILING", "Ceiling", "The underside of a floor"),
    ("WALL", "Wall", "Either face of a wall"),
]

TYPE_NAMES = {"GROUND": "Ground", "CEILING": "Ceiling", "WALL": "Wall"}

# Category asset name -> display name, as last read from categories.json.
_categories = {}
_categories_source = ""


# ---------------------------------------------------------------------------------------------------
# Where things are


def find_structures_folder(context):
    """The project's SourceArt/Structures folder: the scene's setting, or found from this file or the .blend."""
    configured = bpy.path.abspath(context.scene.wem_structures_folder) if context.scene.wem_structures_folder else ""
    if configured and os.path.isdir(configured):
        return os.path.normpath(configured)

    candidates = []
    if __file__:
        candidates.append(os.path.dirname(os.path.abspath(__file__)))
    if bpy.data.filepath:
        candidates.append(os.path.dirname(bpy.data.filepath))

    for start in candidates:
        folder = start
        for _ in range(6):
            if os.path.isfile(os.path.join(folder, "categories.json")):
                return folder
            nested = os.path.join(folder, "SourceArt", "Structures")
            if os.path.isfile(os.path.join(nested, "categories.json")):
                return nested
            parent = os.path.dirname(folder)
            if parent == folder:
                break
            folder = parent

    return ""


def load_categories(context):
    """Reads categories.json, which the Unreal importer writes. Returns an error message, or None."""
    global _categories, _categories_source

    folder = find_structures_folder(context)
    path = os.path.join(folder, "categories.json") if folder else ""

    if not path or not os.path.isfile(path):
        _categories, _categories_source = {}, ""
        return "categories.json was not found. Set the Structures folder, or press Write Category List in Unreal."

    try:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    except (OSError, ValueError) as error:
        return f"categories.json could not be read: {error}"

    if data.get("format") != 1:
        return f"categories.json is format {data.get('format')}, and this add-on reads format 1."

    _categories = {entry["asset"]: entry.get("name", entry["asset"]) for entry in data.get("categories", [])}
    _categories_source = path
    return None


# ---------------------------------------------------------------------------------------------------
# The settings on an object


class WEMStructureSettings(bpy.types.PropertyGroup):
    """A structure's settings, kept on its object so they are saved in the .blend."""

    # Category asset names, separated by semicolons. A string rather than a fixed list, since the
    # categories come from Unreal and change without the add-on changing.
    categories: bpy.props.StringProperty(name="Categories", default="")

    types: bpy.props.EnumProperty(
        name="Types",
        description="The faces the structure may fill. A two-sided one needs the types of both faces: Ground and Ceiling for a floor",
        items=TYPE_ITEMS,
        options={"ENUM_FLAG"},
        default={"GROUND", "CEILING", "WALL"},
    )

    two_sided: bpy.props.BoolProperty(
        name="Two-sided",
        description="Fills the whole 20 cm of the segment, both faces. Otherwise it fills the 10 cm from the mid-plane to one face",
        default=False,
    )

    load_bearing: bpy.props.BoolProperty(
        name="Load-bearing",
        description="Off: nothing may be built onto it. A wall blocks its top beam, a floor or ceiling its free beams",
        default=True,
    )

    holds_items: bpy.props.BoolProperty(
        name="Holds items",
        description="Off: its face carries no objects and opens into no floorspace",
        default=True,
    )

    fence: bpy.props.BoolProperty(
        name="Fence",
        description="Tends to continue as a straight run of copies of itself. Only for two-sided structures",
        default=False,
    )

    may_lie_sideways: bpy.props.BoolProperty(
        name="May lie sideways",
        description="A wall structure may be turned on its side to fill a wall of the other shape",
        default=False,
    )

    weight: bpy.props.FloatProperty(
        name="Weight",
        description="How often it is drawn, relative to the others that fit. Only used when the structure is first imported; tune it in Unreal after that",
        default=1.0,
        min=0.0,
    )

    def get_categories(self):
        return [name for name in self.categories.split(";") if name]

    def set_categories(self, names):
        self.categories = ";".join(dict.fromkeys(names))


def get_settings(obj):
    return obj.wem_structure


# ---------------------------------------------------------------------------------------------------
# Measuring and checking


def measure(obj, context):
    """The evaluated mesh's bounds in the object's own space, modifiers applied: (min, max) as Vectors."""
    depsgraph = context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()

    try:
        if not mesh.vertices:
            return None
        xs = [v.co.x for v in mesh.vertices]
        ys = [v.co.y for v in mesh.vertices]
        zs = [v.co.z for v in mesh.vertices]
        return Vector((min(xs), min(ys), min(zs))), Vector((max(xs), max(ys), max(zs)))
    finally:
        evaluated.to_mesh_clear()


def cells_for(extent):
    """Cells beam to beam for an opening this many metres across, or None when it is not a whole number of cells."""
    opening = round(extent / CELL)
    if opening < 1 or abs(extent - opening * CELL) > TOLERANCE:
        return None
    return opening + 1


def check(obj, context):
    """Everything the export would refuse, as messages, and the measured (width, length, bounds)."""
    problems = []

    if obj is None or obj.type != "MESH":
        return ["Select a mesh object."], None

    if not re.fullmatch(r"[A-Za-z0-9_]+", obj.name):
        problems.append(f"The object's name '{obj.name}' becomes an asset name: use only letters, digits and underscores.")

    if obj.location.length > 0.0005:
        problems.append("The origin has to be at the world origin: the object's location is not zero.")

    if any(abs(angle) > 0.0001 for angle in obj.rotation_euler) or obj.rotation_mode not in {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"}:
        problems.append("Apply the rotation (Ctrl A > Rotation): it is not zero.")

    if any(abs(scale - 1.0) > 0.0001 for scale in obj.scale):
        problems.append("Apply the scale (Ctrl A > Scale): it is not 1.")

    if obj.parent is not None:
        problems.append("Clear the parent: the structure is exported on its own.")

    bounds = measure(obj, context)
    if bounds is None:
        return problems + ["The mesh has no vertices."], None

    low, high = bounds
    settings = get_settings(obj)
    depth = TWO_SIDED_DEPTH if settings.two_sided else ONE_SIDED_DEPTH

    # A one-sided structure's back is the segment's mid-plane, and its partner fills the other half
    # below it, so nothing may go below Z = 0. A two-sided one may carry relief beyond both faces.
    if not settings.two_sided and abs(low.z) > 0.0005:
        problems.append(f"The back has to lie on Z = 0 with nothing below it: the lowest point is at Z = {low.z:.4f} m.")
    elif settings.two_sided and low.z > 0.0005:
        problems.append(f"The back has to lie on Z = 0: the lowest point is at Z = {low.z:.4f} m.")

    if high.z < depth - TOLERANCE:
        side = "two-sided" if settings.two_sided else "one-sided"
        problems.append(f"A {side} structure reaches Z = {depth:.2f} m, its face: the highest point is at Z = {high.z:.4f} m.")

    if abs(low.x + high.x) > TOLERANCE or abs(low.y + high.y) > TOLERANCE:
        problems.append("Centre the structure on the origin in X and Y.")

    width = cells_for(high.x - low.x)
    length = cells_for(high.y - low.y)
    sizes = TILE_SIZES + [(l, w) for w, l in TILE_SIZES]

    if width is None or length is None or (width, length) not in sizes:
        wanted = ", ".join(f"{(w - 1) * CELL:.2f} x {(l - 1) * CELL:.2f}" for w, l in TILE_SIZES)
        problems.append(
            f"It measures {high.x - low.x:.3f} x {high.y - low.y:.3f} m, which is not the opening of a tile "
            f"(either way round, within 1 cm): {wanted} m.")

    if settings.fence and not settings.two_sided:
        problems.append("A fence has to be two-sided.")

    if not settings.types:
        problems.append("Choose at least one type.")
    elif settings.two_sided and "WALL" not in settings.types and not {"GROUND", "CEILING"} <= set(settings.types):
        problems.append("A two-sided structure covers both faces: as a floor it needs Ground and Ceiling, or else Wall.")

    if not bpy.data.filepath:
        problems.append("Save the .blend first, so the settings file can say where the structure came from.")

    return problems, (width, length, low, high)


# ---------------------------------------------------------------------------------------------------
# Export


def write_settings(path, obj, width, length, low, high):
    settings = get_settings(obj)

    # Keys in this order, so the files read and diff alike.
    data = {
        "format": SETTINGS_FORMAT,
        "name": obj.name,
        "blend": os.path.basename(bpy.data.filepath),
        "object": obj.name,
        "width": width,
        "length": length,
        "twoSided": settings.two_sided,
        "categories": settings.get_categories(),
        "types": [TYPE_NAMES[key] for key, _, _ in TYPE_ITEMS if key in settings.types],
        "loadBearing": settings.load_bearing,
        "holdsItems": settings.holds_items,
        "fence": settings.fence and settings.two_sided,
        "mayLieSideways": settings.may_lie_sideways,
        "weight": round(settings.weight, 4),
        "bounds": {
            "min": [round(value, 4) for value in low],
            "max": [round(value, 4) for value in high],
        },
        "exported": datetime.datetime.now().replace(microsecond=0).isoformat(),
    }

    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(format_settings(data))


def format_settings(data):
    """Two-space JSON, with short lists and the bounds kept on one line each, as the brief shows them."""

    def inline(value):
        return json.dumps(value, ensure_ascii=False)

    lines = ["{"]
    items = list(data.items())

    for index, (key, value) in enumerate(items):
        comma = "," if index < len(items) - 1 else ""
        if key == "bounds":
            text = "{ " + ", ".join(f'"{k}": {inline(v)}' for k, v in value.items()) + " }"
        else:
            text = inline(value)
        lines.append(f'  "{key}": {text}{comma}')

    lines.append("}")
    return "\n".join(lines) + "\n"


def export_structure(obj, context):
    """Checks the object and exports it. Returns the problems that stopped it, or an empty list."""
    problems, measured = check(obj, context)
    if problems:
        return problems

    width, length, low, high = measured
    folder = os.path.join(find_structures_folder(context) or os.path.dirname(bpy.data.filepath), "Export")
    os.makedirs(folder, exist_ok=True)

    # Only this object, with its modifiers applied. glTF fixes its axes in the format, where FBX
    # leaves them to per-file settings, so the importer always knows how to read them.
    for other in context.view_layer.objects:
        other.select_set(other == obj)
    context.view_layer.objects.active = obj

    bpy.ops.export_scene.gltf(
        filepath=os.path.join(folder, obj.name + ".glb"),
        export_format="GLB",
        use_selection=True,
        export_apply=True,
        export_yup=True,
        export_normals=True,
        export_tangents=True,
        export_materials="EXPORT",
        export_animations=False,
    )

    write_settings(os.path.join(folder, obj.name + ".json"), obj, width, length, low, high)
    return []


# ---------------------------------------------------------------------------------------------------
# Operators and panel


class WEM_OT_reload_categories(bpy.types.Operator):
    bl_idname = "wem.reload_categories"
    bl_label = "Reload Categories"
    bl_description = "Read categories.json again, which the Unreal importer writes"

    def execute(self, context):
        error = load_categories(context)
        if error:
            self.report({"WARNING"}, error)
        else:
            self.report({"INFO"}, f"{len(_categories)} categories read.")
        return {"FINISHED"}


class WEM_OT_toggle_category(bpy.types.Operator):
    bl_idname = "wem.toggle_category"
    bl_label = "Toggle Category"
    bl_description = "Add the category to the structure, or take it away"
    bl_options = {"REGISTER", "UNDO"}

    category: bpy.props.StringProperty()

    def execute(self, context):
        settings = get_settings(context.active_object)
        names = settings.get_categories()
        if self.category in names:
            names.remove(self.category)
        else:
            names.append(self.category)
        settings.set_categories(names)
        return {"FINISHED"}


class WEM_OT_export_structure(bpy.types.Operator):
    bl_idname = "wem.export_structure"
    bl_label = "Export"
    bl_description = "Check the structure against the convention and export it as .glb and .json for the Unreal importer"

    def execute(self, context):
        problems = export_structure(context.active_object, context)
        if problems:
            for problem in problems:
                self.report({"ERROR"}, problem)
            return {"CANCELLED"}

        self.report({"INFO"}, f"Exported {context.active_object.name}.")
        return {"FINISHED"}


class WEM_PT_structure(bpy.types.Panel):
    bl_label = "WEM Structure"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "WEM"

    def draw(self, context):
        layout = self.layout
        obj = context.active_object

        layout.prop(context.scene, "wem_structures_folder", text="Folder")

        if obj is None or obj.type != "MESH":
            layout.label(text="Select a mesh object.")
            return

        settings = get_settings(obj)
        problems, measured = check(obj, context)

        box = layout.box()
        if measured and measured[0] and measured[1]:
            box.label(text=f"{obj.name}: {measured[0]} x {measured[1]} cells", icon="MESH_GRID")
        else:
            box.label(text=obj.name, icon="MESH_GRID")

        layout.label(text="Types")
        layout.row().prop(settings, "types", expand=True)

        row = layout.row()
        row.label(text="Categories")
        row.operator(WEM_OT_reload_categories.bl_idname, text="", icon="FILE_REFRESH")

        if not _categories:
            load_categories(context)

        chosen = settings.get_categories()
        column = layout.column(align=True)
        for asset, name in _categories.items():
            column.operator(WEM_OT_toggle_category.bl_idname, text=name, depress=asset in chosen).category = asset
        for asset in chosen:
            if asset not in _categories:
                column.operator(WEM_OT_toggle_category.bl_idname, text=f"{asset} (unknown)", depress=True, icon="ERROR").category = asset
        if not _categories:
            column.label(text="No categories.json found.", icon="INFO")

        layout.prop(settings, "two_sided")
        if settings.two_sided:
            layout.prop(settings, "fence")
        layout.prop(settings, "load_bearing")
        layout.prop(settings, "holds_items")
        layout.prop(settings, "may_lie_sideways")
        layout.prop(settings, "weight")

        if problems:
            column = layout.column()
            column.alert = True
            for problem in problems:
                column.label(text=problem, icon="ERROR")

        row = layout.row()
        row.scale_y = 1.4
        row.operator(WEM_OT_export_structure.bl_idname, icon="EXPORT")


CLASSES = (
    WEMStructureSettings,
    WEM_OT_reload_categories,
    WEM_OT_toggle_category,
    WEM_OT_export_structure,
    WEM_PT_structure,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.wem_structure = bpy.props.PointerProperty(type=WEMStructureSettings)
    bpy.types.Scene.wem_structures_folder = bpy.props.StringProperty(
        name="Structures Folder",
        description="The project's SourceArt/Structures folder. Left empty, it is looked for beside the add-on and the .blend",
        subtype="DIR_PATH",
        default="",
    )


def unregister():
    del bpy.types.Scene.wem_structures_folder
    del bpy.types.Object.wem_structure
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
