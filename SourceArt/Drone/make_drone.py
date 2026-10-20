"""
Builds the hover drone's body and one rotor and writes them as FBX files beside this script.

The drone is modelled in Blender metres with its camera looking along +X and up along +Z, centred
on the pawn's origin (where the player's camera sits), so it lands on BP_OpalHoverDrone's root
without an offset. The rotor is a separate mesh with its hub at the origin: the Blueprint places
four copies on the motors and their material spins them.

Material slots (named here, filled in Unreal by the MI_Drone_* instances):
    Shell  - the cream body              Accent - the orange battery and antenna tips
    Frame  - arms, guards, belly, legs   Metal  - motor bells, shafts, lens barrel
    Lens   - the camera glass            LedFront / LedBack - the glowing lights under the motors
    Rotor  - the blades (rotor file only)

Run it with Blender:  blender --background --factory-startup --python make_drone.py
"""

import math
import os

import bpy
import bmesh

HERE = os.path.dirname(os.path.abspath(__file__))

ARM_ANGLES = (45.0, 135.0, 225.0, 315.0)  # front-right... in degrees from +X
MOTOR_RADIUS = 0.19  # body centre to motor axis
ROTOR_HEIGHT = 0.045  # hub height over the body centre
GUARD_RADIUS = 0.078


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.unit_settings.system = 'METRIC'
    bpy.context.scene.unit_settings.scale_length = 1.0


MATERIALS = {}


def material(name):
    if name not in MATERIALS:
        MATERIALS[name] = bpy.data.materials.new(name)
    return MATERIALS[name]


def finish(obj, mat, bevel=0.0, segments=2):
    obj.data.materials.clear()
    obj.data.materials.append(material(mat))
    if bevel > 0.0:
        mod = obj.modifiers.new('Bevel', 'BEVEL')
        mod.width = bevel
        mod.segments = segments
        mod.limit_method = 'ANGLE'
    return obj


def box(name, size, location, mat, rotation=(0.0, 0.0, 0.0), bevel=0.0, segments=2):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=location, rotation=rotation)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(obj, mat, bevel, segments)


def cylinder(name, radius, depth, location, mat, rotation=(0.0, 0.0, 0.0), vertices=20, bevel=0.0):
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=vertices, radius=radius, depth=depth, location=location, rotation=rotation)
    obj = bpy.context.active_object
    obj.name = name
    return finish(obj, mat, bevel, 1)


def sphere(name, radius, location, mat, segments=16, rings=8):
    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=segments, ring_count=rings, radius=radius, location=location)
    obj = bpy.context.active_object
    obj.name = name
    return finish(obj, mat)


def torus(name, major, minor, location, mat, height=1.0):
    """A ring lying flat; height > 1 stretches its cross-section upwards into a band."""
    bpy.ops.mesh.primitive_torus_add(
        major_segments=40, minor_segments=8, major_radius=major, minor_radius=minor, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = (1.0, 1.0, height)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return finish(obj, mat)


def polar(angle_deg, radius, z):
    a = math.radians(angle_deg)
    return (radius * math.cos(a), radius * math.sin(a), z)


def build_body():
    parts = []
    # Core, battery on top, belly plate under it.
    parts.append(box('Core', (0.18, 0.11, 0.05), (0.0, 0.0, 0.0), 'Shell', bevel=0.014, segments=3))
    parts.append(box('Battery', (0.11, 0.065, 0.03), (-0.012, 0.0, 0.038), 'Accent', bevel=0.006))
    parts.append(box('Belly', (0.14, 0.08, 0.012), (0.0, 0.0, -0.03), 'Frame', bevel=0.003))

    for angle in ARM_ANGLES:
        front = math.cos(math.radians(angle)) > 0.0
        parts.append(box('Arm', (0.16, 0.024, 0.016), polar(angle, 0.11, 0.0), 'Frame',
                         rotation=(0.0, 0.0, math.radians(angle)), bevel=0.004))
        x, y, _ = polar(angle, MOTOR_RADIUS, 0.0)
        parts.append(cylinder('MotorBase', 0.021, 0.012, (x, y, 0.004), 'Frame'))
        parts.append(cylinder('MotorBell', 0.019, 0.022, (x, y, 0.021), 'Metal', bevel=0.002))
        parts.append(cylinder('Shaft', 0.003, 0.014, (x, y, 0.037), 'Metal', vertices=8))
        # A duct round the blade plane, held by three struts from the motor.
        parts.append(torus('Guard', GUARD_RADIUS, 0.0045, (x, y, ROTOR_HEIGHT - 0.002), 'Frame', height=2.2))
        for strut in (angle - 90.0, angle, angle + 90.0):
            sx, sy, _ = polar(strut, GUARD_RADIUS * 0.5 + 0.01, 0.0)
            parts.append(box('Strut', (GUARD_RADIUS - 0.02, 0.006, 0.006), (x + sx, y + sy, 0.034), 'Frame',
                             rotation=(0.0, 0.0, math.radians(strut))))
        parts.append(sphere('Led', 0.009, (x, y, -0.006), 'LedFront' if front else 'LedBack'))
        lx, ly, _ = polar(angle, 0.13, 0.0)
        parts.append(cylinder('Leg', 0.004, 0.035, (lx, ly, -0.025), 'Frame', vertices=8))

    # Camera on a gimbal ball under the nose.
    parts.append(sphere('Gimbal', 0.024, (0.095, 0.0, -0.022), 'Frame'))
    parts.append(cylinder('LensBarrel', 0.013, 0.016, (0.118, 0.0, -0.022), 'Metal',
                          rotation=(0.0, math.radians(90.0), 0.0)))
    parts.append(cylinder('LensGlass', 0.0105, 0.004, (0.127, 0.0, -0.022), 'Lens',
                          rotation=(0.0, math.radians(90.0), 0.0)))

    # Two antennas leaning back.
    for side in (-1.0, 1.0):
        lean = math.radians(-35.0)
        base = (-0.085, side * 0.03, 0.03)
        tip_offset = (-math.sin(-lean) * 0.035, 0.0, math.cos(lean) * 0.035)
        centre = (base[0] + tip_offset[0], base[1], base[2] + tip_offset[2])
        parts.append(cylinder('Antenna', 0.0025, 0.07, centre, 'Frame', rotation=(0.0, lean, 0.0), vertices=6))
        tip = (base[0] + 2.0 * tip_offset[0], base[1], base[2] + 2.0 * tip_offset[2])
        parts.append(sphere('AntennaTip', 0.005, tip, 'Accent', segments=8, rings=4))

    return join(parts, 'SM_Drone_Body')


def build_rotor():
    parts = [cylinder('Hub', 0.009, 0.008, (0.0, 0.0, 0.0), 'Rotor', vertices=12)]
    for side in (0.0, 180.0):
        blade = box('Blade', (0.062, 0.016, 0.0022), (0.0, 0.0, 0.0), 'Rotor')
        # Move the blade out from the hub along +X, narrow its tip, and pitch it round its own length.
        bm = bmesh.new()
        bm.from_mesh(blade.data)
        pitch = math.radians(14.0)
        for v in bm.verts:
            x = v.co.x + 0.036
            y = v.co.y * (0.55 if x > 0.05 else 1.0)
            z = v.co.z
            v.co = (x, y * math.cos(pitch) - z * math.sin(pitch), y * math.sin(pitch) + z * math.cos(pitch))
        bm.to_mesh(blade.data)
        bm.free()
        # Turn the second blade round the hub.
        blade.rotation_euler = (0.0, 0.0, math.radians(side))
        parts.append(blade)
    return join(parts, 'SM_Drone_Rotor')


def join(parts, name):
    bpy.ops.object.select_all(action='DESELECT')
    for obj in parts:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.convert(target='MESH')  # applies the bevels
    bpy.ops.object.join()
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = name
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bpy.ops.object.shade_smooth_by_angle(angle=math.radians(35.0))
    return obj


def export(obj, filename):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    path = os.path.join(HERE, filename)
    bpy.ops.export_scene.fbx(
        filepath=path, use_selection=True, object_types={'MESH'},
        mesh_smooth_type='FACE', use_mesh_modifiers=True, add_leaf_bones=False,
        bake_anim=False, axis_forward='-Z', axis_up='Y')
    print('wrote', path, len(obj.data.polygons), 'faces')


if __name__ == '__main__':
    clear_scene()
    body = build_body()
    export(body, 'SM_Drone_Body.fbx')
    clear_scene()
    MATERIALS.clear()
    rotor = build_rotor()
    export(rotor, 'SM_Drone_Rotor.fbx')
