"""Human-friendly Blender authoring and export tools for SKATE v8.

This addon and exporter are original project code. They do not import,
invoke, redistribute, or depend on ArenaBuilder.
"""

from __future__ import annotations

import bpy
import importlib
import math
from pathlib import Path
import re
import traceback
from bpy.app.handlers import persistent
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import Material, Object, Operator, Panel, PropertyGroup, Scene
from bpy_extras.io_utils import ExportHelper

from . import exporter as _exporter

# Blender may replace an installed extension while keeping its Python package
# alive. Reload the implementation module so a newly installed panel cannot
# call an older cached exporter with an incompatible function signature.
exporter = importlib.reload(_exporter)


bl_info = {
    "name": "Owned World Authoring",
    "author": "Skate 3 Custom Engine Layer contributors",
    "version": (1, 7, 4),
    "blender": (5, 0, 0),
    "location": "3D View > Sidebar > Skate 3 Map",
    "description": "Create, validate, and export Skate 3 Custom Engine maps",
    "category": "Import-Export",
}


_SYNCING = False


_AUDIO_NAMES = """
Undefined|Asphalt_Smooth|Asphalt_Rough|Concrete_Polished|Concrete_Rough|
Concrete_Aggregate|Wood_Ramp|Plywood|Dirt|Metal|Grass|
Metal_Solid_Round_1|Metal_Solid_Round_1_Up|Metal_Solid_Round_2|
Metal_Solid_Square_1|Metal_Solid_Square_2|Metal_Hollow_Round_1|
Metal_Hollow_Round_1_Dead|Metal_Hollow_Round_1_Dn|Metal_Hollow_Round_2|
Metal_Hollow_Round_2_Dead|Metal_Hollow_Round_2_Dn|Metal_Hollow_Round_3|
Metal_Hollow_Round_4|Metal_Hollow_Square_1|Metal_Hollow_Square_2|
Metal_Hollow_Square_3|Metal_Hollow_Square_3_Dead|Metal_Hollow_Square_4|
Metal_Hollow_1|Metal_Hollow_2|Metal_Sheet|Metal_Complex_1|Metal_Complex_2|
Metal_Complex_3|Metal_Complex_4|Metal_Complex_5|Metal_Complex_6|
Metal_Complex_7|Metal_Complex_8|Metal_Complex_Debris|Wood_1|Wood_1_Up|
Wood_2|Wood_3|Wood_3_Up|Wood_4|Plastic_1|Plastic_2|Plastic_3|Plastic_4|
Glass_Thick_Large|Glass_Thin_Small|Concrete_Curb|Concrete_Bench|Leaves|
Bush|Pottery|Paper|Cardboard|Garbage_Bag|Garbage_Spill|Bottle|
Tile_Ceramic|Marble_or_Slate|Brick_Smooth|Brick_Coarse|Manhole_Metal|
Metal_Grate_Sewer|Metal_Grate_Planter|DeepSnow|PackedSnow|Ice|Antennas|
Chandelier|Plexiglass_Small|Plexiglass_Large|Potted_Plant|Crumpled_Paper|
Cloth|Pop_Can|Paper_Cup|Wire_Cable|VolleyBall|OilDrum|DMORail|Fruit|
Plastic_Bottle|Drum_Pylon|Metal_Rail_4|Wood_5|Metal_Ramp|
Complex_Plastic_1|Max_Mappable_Surface
"""
_AUDIO_NAMES = [
    name.strip()
    for name in _AUDIO_NAMES.replace("\n", "").split("|")
]
AUDIO_ITEMS = [
    (str(index), name, f"Native Skate 3 audio surface {index}")
    for index, name in enumerate(_AUDIO_NAMES)
]

PHYSICS_ITEMS = [
    ("0", "Undefined", "Default behavior"),
    ("1", "Smooth", "Fast smooth riding"),
    ("2", "Rough", "Medium-friction riding"),
    ("3", "Slow", "Slows the rider"),
    ("4", "Slippery", "Low-friction riding"),
    ("5", "VerySlow", "Very slow / breadcrumb blocked"),
    ("6", "Unrideable", "Cannot be ridden"),
    ("7", "DoNotAlign", "Do not align board to this surface"),
    ("8", "Stair", "Native stair behavior"),
    ("9", "InstantBail", "Immediately forces a bail"),
    ("10", "SlipperyRagdoll", "Low-friction bail behavior"),
    ("11", "BouncyRagdoll", "Bouncy bail behavior"),
    ("12", "Water", "Native water/swimming behavior"),
]

PATTERN_ITEMS = [
    ("0", "None", "No native surface pattern"),
    ("1", "SpiderCrack", "Cracked surface"),
    ("2", "Square2x2", "2 by 2 tiles"),
    ("3", "Square4x4", "4 by 4 tiles"),
    ("4", "Square8x8", "8 by 8 tiles"),
    ("5", "Square12x12", "12 by 12 tiles"),
    ("6", "Square24x24", "24 by 24 tiles"),
    ("7", "IrregularSmall", "Small irregular pattern"),
    ("8", "IrregularMedium", "Medium irregular pattern"),
    ("9", "IrregularLarge", "Large irregular pattern"),
    ("10", "Slats", "Slatted surface"),
    ("11", "Sidewalk", "Sidewalk pattern"),
    ("12", "BrickTileRandomSize", "Random-size brick tile"),
    ("13", "MiniTile", "Small tile pattern"),
    ("14", "Special1", "Native special pattern 1"),
    ("15", "Special2", "Native special pattern 2"),
]

ALPHA_ITEMS = [
    ("0", "Opaque", "Fully opaque"),
    ("1", "Cutout", "Alpha-tested leaves, grates, and fences"),
    ("2", "Blend", "Alpha-blended glass and soft surfaces"),
]

PRESET_ITEMS = [
    ("CONCRETE", "Polished Concrete", ""),
    ("ROUGH_CONCRETE", "Rough Concrete", ""),
    ("ASPHALT", "Rough Asphalt", ""),
    ("WOOD", "Wood Ramp", ""),
    ("METAL_RAIL", "Metal Rail", ""),
    ("METAL_SHEET", "Metal Sheet", ""),
    ("GRASS", "Grass", ""),
    ("TILE", "Ceramic Tile", ""),
    ("GLASS", "Glass", ""),
    ("ICE", "Ice / Slippery", ""),
    ("STAIRS", "Concrete Stairs", ""),
    ("WATER", "Water", ""),
    ("INSTANT_BAIL", "Instant Bail", ""),
]

PRESETS = {
    "CONCRETE": (3, 1, 0, 0.72, 0.0, 0),
    "ROUGH_CONCRETE": (4, 2, 0, 0.90, 0.0, 0),
    "ASPHALT": (2, 2, 7, 0.94, 0.0, 0),
    "WOOD": (6, 1, 10, 0.72, 0.0, 0),
    "METAL_RAIL": (11, 1, 0, 0.22, 0.88, 0),
    "METAL_SHEET": (31, 1, 0, 0.34, 0.78, 0),
    "GRASS": (10, 3, 7, 0.96, 0.0, 0),
    "TILE": (63, 1, 4, 0.38, 0.0, 0),
    "GLASS": (51, 1, 0, 0.08, 0.0, 2),
    "ICE": (72, 4, 0, 0.08, 0.0, 0),
    "STAIRS": (53, 8, 11, 0.84, 0.0, 0),
    "WATER": (0, 12, 0, 0.04, 0.0, 2),
    "INSTANT_BAIL": (4, 9, 0, 0.85, 0.0, 0),
}

PHYSICS_TYPE_ITEMS = [
    ("STATIC", "Static", "Ordinary static world geometry"),
    (
        "HINGED_DOOR",
        "Hinged Door",
        "Contact-driven rigid door constrained to one hinge axis",
    ),
]


def _sync(settings: "OwnedWorldMaterialSettings") -> None:
    if _SYNCING:
        return
    material = settings.id_data
    if not isinstance(material, Material):
        return
    # Once an author changes any value in the material panel, preserve that
    # choice on future automatic scene preparation. Materials carrying the
    # True marker are still owned by the importer and may be refreshed from
    # their Blender shader graph to heal stale/partial imports.
    material["ow_auto_imported"] = False
    material["ow_audio_surface"] = int(settings.audio_surface)
    material["ow_physics_surface"] = int(settings.physics_surface)
    material["ow_surface_pattern"] = int(settings.surface_pattern)
    material["ow_alpha_mode"] = int(settings.alpha_mode)
    material["ow_alpha_cutoff"] = float(settings.alpha_cutoff)
    material["ow_roughness"] = float(settings.roughness)
    material["ow_metallic"] = float(settings.metallic)
    material["ow_friction"] = float(settings.friction)
    material["ow_restitution"] = float(settings.restitution)
    material["ow_emissive"] = float(settings.emissive_strength)
    material["ow_baked_strength"] = float(settings.baked_strength)
    material["ow_collision_enabled"] = bool(settings.collision_enabled)
    for prop, image in (
        ("ow_albedo_image", settings.albedo_image),
        ("ow_lightmap_image", settings.lightmap_image),
        ("ow_normal_image", settings.normal_image),
        ("ow_orm_image", settings.orm_image),
        ("ow_emissive_image", settings.emissive_image),
    ):
        material[prop] = image.name if image is not None else ""


def _updated(self, _context) -> None:
    _sync(self)


def _sync_physics(settings: "OwnedWorldPhysicsSettings") -> None:
    if _SYNCING:
        return
    obj = settings.id_data
    if not isinstance(obj, Object):
        return
    obj["ow_physics_type"] = settings.physics_type
    if settings.collision_material is not None:
        obj["ow_material"] = settings.collision_material.name
    obj["ow_upward_surface"] = bool(settings.upward_surface)
    obj["ow_hinge_axis"] = tuple(settings.hinge_axis)
    obj["ow_door_min_angle_degrees"] = math.degrees(
        float(settings.minimum_angle)
    )
    obj["ow_door_max_angle_degrees"] = math.degrees(
        float(settings.maximum_angle)
    )
    obj["ow_door_initial_angle_degrees"] = math.degrees(
        float(settings.initial_angle)
    )
    obj["ow_door_mass"] = float(settings.mass)
    obj["ow_door_angular_damping"] = float(settings.angular_damping)
    obj["ow_door_return_spring_strength"] = float(
        settings.return_spring_strength
    )
    obj["ow_door_maximum_angular_speed"] = float(
        settings.maximum_angular_speed
    )
    obj["ow_door_contact_impulse_scale"] = float(
        settings.contact_impulse_scale
    )
    obj["ow_door_friction"] = float(settings.friction)
    obj["ow_door_restitution"] = float(settings.restitution)


def _physics_updated(self, _context) -> None:
    _sync_physics(self)


class OwnedWorldMaterialSettings(PropertyGroup):
    audio_surface: EnumProperty(
        name="Wheel / Grind Sound",
        items=AUDIO_ITEMS,
        default="3",
        update=_updated,
    )
    physics_surface: EnumProperty(
        name="Physics Behavior",
        items=PHYSICS_ITEMS,
        default="1",
        update=_updated,
    )
    surface_pattern: EnumProperty(
        name="Contact Pattern",
        items=PATTERN_ITEMS,
        default="0",
        update=_updated,
    )
    collision_enabled: BoolProperty(
        name="Collision Enabled",
        default=True,
        update=_updated,
    )
    alpha_mode: EnumProperty(
        name="Transparency",
        items=ALPHA_ITEMS,
        default="0",
        update=_updated,
    )
    alpha_cutoff: FloatProperty(
        name="Cutout Threshold",
        min=0.0,
        max=1.0,
        default=0.5,
        update=_updated,
    )
    roughness: FloatProperty(
        name="Fallback Roughness",
        min=0.0,
        max=1.0,
        default=0.78,
        update=_updated,
    )
    metallic: FloatProperty(
        name="Fallback Metallic",
        min=0.0,
        max=1.0,
        default=0.0,
        update=_updated,
    )
    friction: FloatProperty(
        name="Contact Friction",
        description="Physical friction exported for this surface",
        min=0.0,
        max=2.0,
        default=0.82,
        update=_updated,
    )
    restitution: FloatProperty(
        name="Bounce",
        min=0.0,
        max=1.0,
        default=0.0,
        update=_updated,
    )
    emissive_strength: FloatProperty(
        name="Emissive Strength",
        min=0.0,
        max=100.0,
        default=0.0,
        update=_updated,
    )
    baked_strength: FloatProperty(
        name="Baked Lighting Strength",
        min=0.0,
        max=8.0,
        default=1.0,
        update=_updated,
    )
    albedo_image: PointerProperty(
        name="Base Color / Albedo",
        type=bpy.types.Image,
        update=_updated,
    )
    lightmap_image: PointerProperty(
        name="Baked Lighting",
        type=bpy.types.Image,
        update=_updated,
    )
    normal_image: PointerProperty(
        name="Normal Map",
        type=bpy.types.Image,
        update=_updated,
    )
    orm_image: PointerProperty(
        name="ORM (AO/Rough/Metal)",
        type=bpy.types.Image,
        update=_updated,
    )
    emissive_image: PointerProperty(
        name="Emissive Map",
        type=bpy.types.Image,
        update=_updated,
    )


class OwnedWorldPhysicsSettings(PropertyGroup):
    collision_material: PointerProperty(
        name="Collision Material",
        description=(
            "Visual material that supplies this collision object's sound "
            "and physics behavior"
        ),
        type=Material,
        update=_physics_updated,
    )
    upward_surface: BoolProperty(
        name="Rideable Top Surface Only",
        description=(
            "Require every collision triangle to face upward; useful for "
            "floors and ramps"
        ),
        default=False,
        update=_physics_updated,
    )
    physics_type: EnumProperty(
        name="Physics Type",
        items=PHYSICS_TYPE_ITEMS,
        default="STATIC",
        update=_physics_updated,
    )
    hinge_axis: FloatVectorProperty(
        name="Hinge Axis",
        description="World-space hinge axis; Z is vertical in Blender",
        size=3,
        default=(0.0, 0.0, 1.0),
        subtype="DIRECTION",
        update=_physics_updated,
    )
    minimum_angle: FloatProperty(
        name="Minimum Angle",
        subtype="ANGLE",
        default=-1.8325957,
        min=-3.1415927,
        max=0.0,
        update=_physics_updated,
    )
    maximum_angle: FloatProperty(
        name="Maximum Angle",
        subtype="ANGLE",
        default=1.8325957,
        min=0.0,
        max=3.1415927,
        update=_physics_updated,
    )
    initial_angle: FloatProperty(
        name="Initial Angle",
        subtype="ANGLE",
        default=0.0,
        min=-3.1415927,
        max=3.1415927,
        update=_physics_updated,
    )
    mass: FloatProperty(
        name="Mass",
        description="Door-leaf mass in kilograms",
        default=32.0,
        min=0.1,
        max=1000.0,
        unit="MASS",
        update=_physics_updated,
    )
    angular_damping: FloatProperty(
        name="Angular Damping",
        default=2.2,
        min=0.0,
        max=20.0,
        update=_physics_updated,
    )
    return_spring_strength: FloatProperty(
        name="Self-Close Strength",
        description=(
            "Angular spring strength that returns the leaf to its initial "
            "closed angle; zero disables self-closing"
        ),
        default=0.0,
        min=0.0,
        max=40.0,
        update=_physics_updated,
    )
    maximum_angular_speed: FloatProperty(
        name="Maximum Swing Speed",
        description="Maximum hinge speed in radians per second",
        default=8.0,
        min=0.05,
        max=30.0,
        update=_physics_updated,
    )
    contact_impulse_scale: FloatProperty(
        name="Push Response",
        description=(
            "Scales physical momentum transferred from the player to the door"
        ),
        default=1.0,
        min=0.0,
        max=4.0,
        update=_physics_updated,
    )
    friction: FloatProperty(
        name="Contact Friction",
        default=0.55,
        min=0.0,
        max=2.0,
        update=_physics_updated,
    )
    restitution: FloatProperty(
        name="Bounce",
        default=0.02,
        min=0.0,
        max=1.0,
        update=_physics_updated,
    )


def _sync_npc_path(settings: "OwnedWorldNpcPathSettings") -> None:
    if _SYNCING:
        return
    obj = settings.id_data
    if not isinstance(obj, Object):
        return
    obj["ow_npc_skater_count"] = int(settings.skater_count)
    obj["ow_npc_speed"] = float(settings.speed)
    obj["ow_npc_spawn_spacing"] = float(settings.spawn_spacing)


def _npc_path_updated(self, _context) -> None:
    _sync_npc_path(self)


class OwnedWorldNpcPathSettings(PropertyGroup):
    skater_count: IntProperty(
        name="Skaters",
        description="Number of native AI skaters assigned to this route",
        default=1,
        min=1,
        max=32,
        update=_npc_path_updated,
    )
    speed: FloatProperty(
        name="Route Speed",
        description="Target movement speed along this route in metres/second",
        default=5.5,
        min=0.1,
        max=30.0,
        unit="VELOCITY",
        update=_npc_path_updated,
    )
    spawn_spacing: FloatProperty(
        name="Spawn Spacing",
        description="Distance between skaters when a route has a population",
        default=3.0,
        min=0.0,
        max=100.0,
        unit="LENGTH",
        update=_npc_path_updated,
    )


def _sync_scene(settings: "OwnedWorldSceneSettings") -> None:
    if _SYNCING:
        return
    scene = settings.id_data
    if not isinstance(scene, Scene):
        return
    scene["ow_map_name"] = settings.map_name.strip() or "Untitled Map"
    scene["ow_cycle_seconds"] = float(settings.cycle_seconds)
    scene["ow_start_hour"] = float(settings.start_hour)
    scene["ow_end_hour"] = float(settings.end_hour)
    scene["ow_cycle_ping_pong"] = bool(settings.cycle_ping_pong)
    scene["ow_orbit_azimuth"] = float(settings.orbit_azimuth)
    scene["ow_sky_zenith"] = tuple(settings.sky_zenith)
    scene["ow_sky_horizon"] = tuple(settings.sky_horizon)
    scene["ow_sky_nadir"] = tuple(settings.sky_nadir)
    scene["ow_twilight_zenith"] = tuple(settings.twilight_zenith)
    scene["ow_twilight_horizon"] = tuple(settings.twilight_horizon)
    scene["ow_twilight_nadir"] = tuple(settings.twilight_nadir)
    scene["ow_night_zenith"] = tuple(settings.night_zenith)
    scene["ow_night_horizon"] = tuple(settings.night_horizon)
    scene["ow_night_nadir"] = tuple(settings.night_nadir)
    scene["ow_sun_color"] = tuple(settings.sun_color)
    scene["ow_moon_color"] = tuple(settings.moon_color)
    scene["ow_sun_intensity"] = float(settings.sun_intensity)
    scene["ow_moon_intensity"] = float(settings.moon_intensity)
    scene["ow_day_ambient"] = float(settings.day_ambient)
    scene["ow_night_ambient"] = float(settings.night_ambient)
    scene["ow_sky_tint"] = tuple(settings.sky_tint)
    spawn = bpy.data.objects.get(exporter.SPAWN_OBJECT)
    if spawn is not None:
        spawn["ow_heading_radians"] = float(settings.spawn_heading)


def _scene_updated(self, _context) -> None:
    _sync_scene(self)


class OwnedWorldSceneSettings(PropertyGroup):
    map_name: StringProperty(
        name="Map Name",
        description="Name shown in the in-game Maps menu",
        default="My Skate Map",
        update=_scene_updated,
    )
    output_path: StringProperty(
        name="SKATE File",
        description="Destination package loaded by the game",
        default="//my_skate_map.skate",
        subtype="FILE_PATH",
    )
    cycle_seconds: FloatProperty(
        name="Day/Night Duration",
        description="Seconds per cycle; zero freezes the lighting",
        default=96.0,
        min=0.0,
        max=1800.0,
        unit="TIME",
        update=_scene_updated,
    )
    start_hour: FloatProperty(
        name="Start Hour",
        default=9.0,
        min=0.0,
        max=24.0,
        update=_scene_updated,
    )
    end_hour: FloatProperty(
        name="End Hour",
        default=17.0,
        min=0.0,
        max=24.0,
        update=_scene_updated,
    )
    cycle_ping_pong: BoolProperty(
        name="Move Between Start and End",
        description="Animate back and forth without passing through night",
        default=True,
        update=_scene_updated,
    )
    orbit_azimuth: FloatProperty(
        name="Sun Orbit Direction",
        subtype="ANGLE",
        default=0.62,
        min=-math.pi,
        max=math.pi,
        update=_scene_updated,
    )
    spawn_heading: FloatProperty(
        name="Spawn Heading",
        subtype="ANGLE",
        default=0.0,
        min=-math.pi,
        max=math.pi,
        update=_scene_updated,
    )
    sky_zenith: FloatVectorProperty(
        name="Sky Top",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.09, 0.34, 0.72),
        update=_scene_updated,
    )
    sky_horizon: FloatVectorProperty(
        name="Sky Horizon",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.58, 0.78, 0.98),
        update=_scene_updated,
    )
    sky_nadir: FloatVectorProperty(
        name="Sky Bottom",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.18, 0.25, 0.34),
        update=_scene_updated,
    )
    twilight_zenith: FloatVectorProperty(
        name="Twilight Top",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.045, 0.10, 0.26),
        update=_scene_updated,
    )
    twilight_horizon: FloatVectorProperty(
        name="Twilight Horizon",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(1.0, 0.32, 0.10),
        update=_scene_updated,
    )
    twilight_nadir: FloatVectorProperty(
        name="Twilight Bottom",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.05, 0.035, 0.06),
        update=_scene_updated,
    )
    night_zenith: FloatVectorProperty(
        name="Night Top",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.007, 0.015, 0.045),
        update=_scene_updated,
    )
    night_horizon: FloatVectorProperty(
        name="Night Horizon",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.045, 0.085, 0.17),
        update=_scene_updated,
    )
    night_nadir: FloatVectorProperty(
        name="Night Bottom",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.008, 0.014, 0.032),
        update=_scene_updated,
    )
    sun_color: FloatVectorProperty(
        name="Sun Colour",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(1.0, 0.92, 0.78),
        update=_scene_updated,
    )
    moon_color: FloatVectorProperty(
        name="Moon Colour",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(0.42, 0.56, 0.92),
        update=_scene_updated,
    )
    sun_intensity: FloatProperty(
        name="Sun Strength",
        default=1.25,
        min=0.0,
        max=4.0,
        update=_scene_updated,
    )
    moon_intensity: FloatProperty(
        name="Moon Strength",
        default=0.18,
        min=0.0,
        max=2.0,
        update=_scene_updated,
    )
    day_ambient: FloatProperty(
        name="Day Ambient",
        default=0.32,
        min=0.0,
        max=1.0,
        update=_scene_updated,
    )
    night_ambient: FloatProperty(
        name="Night Ambient",
        default=0.11,
        min=0.0,
        max=1.0,
        update=_scene_updated,
    )
    sky_tint: FloatVectorProperty(
        name="Sky Colour",
        description="RGB tint multiplied over the day, twilight, and night sky palettes",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=4.0,
        default=(1.0, 1.0, 1.0),
        update=_scene_updated,
    )
    show_advanced_lighting: BoolProperty(
        name="Advanced Lighting",
        description="Show twilight, night, sun and ambient map defaults",
        default=False,
    )
    export_mode: EnumProperty(
        name="Export Mode",
        items=(
            (
                "AUTO",
                "Fast / Automatic",
                "Reuse unchanged map data and rebuild only when needed",
            ),
            (
                "FORCE",
                "Force Full Rebuild",
                "Ignore the incremental cache and rebuild everything",
            ),
            (
                "METADATA",
                "Lighting / Spawn Only",
                "Patch world and spawn settings into an existing package",
            ),
        ),
        default="AUTO",
    )
    last_status: StringProperty(
        name="Last Result",
        default="Run Validate Map before your first export.",
    )
    validation_details: StringProperty(
        name="Validation Details",
        default="",
        options={"HIDDEN"},
    )


class OWPHYSICS_OT_hinge_from_cursor(Operator):
    bl_idname = "owphysics.hinge_from_cursor"
    bl_label = "Set Hinge From 3D Cursor"
    bl_description = (
        "Store the current 3D cursor as this door's world-space hinge line"
    )
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        obj = context.object
        if obj is None or obj.type != "MESH":
            self.report({"ERROR"}, "Select a mesh object")
            return {"CANCELLED"}
        obj["ow_hinge_position"] = tuple(context.scene.cursor.location)
        obj.owned_world_physics.physics_type = "HINGED_DOOR"
        _sync_physics(obj.owned_world_physics)
        self.report({"INFO"}, f"Set hinge for {obj.name}")
        return {"FINISHED"}


def _ensure_collection(
    scene: bpy.types.Scene, name: str
) -> bpy.types.Collection:
    collection = bpy.data.collections.get(name)
    if collection is None:
        collection = bpy.data.collections.new(name)
    if collection.name not in scene.collection.children:
        scene.collection.children.link(collection)
    return collection


def _link_object(
    collection: bpy.types.Collection, obj: bpy.types.Object
) -> None:
    if obj.name not in collection.objects:
        collection.objects.link(obj)


def _selected_of_type(context, object_type: str) -> list[bpy.types.Object]:
    return [
        obj for obj in context.selected_objects if obj.type == object_type
    ]


_AUTO_COLLIDER_MARKERS = (
    "collider",
    "collision",
    "ucx_",
    "ubx_",
    "usp_",
    "ucp_",
    "col - no pres",
)
_AUTO_NON_COLLISION_MARKERS = (
    "billboard",
    "decal",
    "foliage",
    "leaf",
    "leaves",
    "fern",
    "bush",
    "grass blade",
    "meadow_grass",
    "ivy",
    "vine",
    "wall stain",
    "moss stain",
    "skyline",
    "backdrop",
    "palm",
    "tree",
    "bark",
    "plant",
    "roots_system",
    "roots system",
    "shadow blocker",
    "shadowblocked",
    "reflection probe",
)
_AUTO_CUTOUT_MARKERS = (
    "billboard",
    "foliage",
    "leaf",
    "leaves",
    "fern",
    "bush",
    "grass",
    "ivy",
    "vine",
    "chainlink",
    "chain link",
    "fence",
    "grate",
)
_AUTO_BLEND_MARKERS = ("glass", "window", "water")
_AUTO_HELPER_OBJECT_MARKERS = (
    "character_size",
    "character size",
    "player_size",
    "player size",
    "scale_reference",
    "scale reference",
)


def _object_semantics(obj: bpy.types.Object) -> str:
    names = [obj.name, getattr(obj.data, "name", "")]
    current = obj.parent
    while current is not None:
        names.append(current.name)
        current = current.parent
    names.extend(
        slot.material.name
        for slot in obj.material_slots
        if slot.material is not None
    )
    return " ".join(names).lower()


def _is_auto_helper_object(obj: bpy.types.Object) -> bool:
    object_identity = (
        f"{obj.name} {getattr(obj.data, 'name', '')}".lower()
    )
    return any(
        marker in object_identity
        for marker in _AUTO_HELPER_OBJECT_MARKERS
    )


def _is_explicit_collider(obj: bpy.types.Object) -> bool:
    semantics = _object_semantics(obj)
    return any(marker in semantics for marker in _AUTO_COLLIDER_MARKERS)


def _auto_visual_candidate(obj: bpy.types.Object) -> bool:
    name = obj.name.lower()
    lod_match = re.search(r"(^|[_. -])lod([1-9])($|[_. -])", name)
    has_lod0_sibling = bool(
        lod_match
        and obj.parent is not None
        and any(
            child.type == "MESH"
            and re.search(
                r"(^|[_. -])lod0($|[_. -])", child.name.lower()
            )
            for child in obj.parent.children
        )
    )
    return (
        obj.type == "MESH"
        and len(obj.data.polygons) > 0
        and not obj.hide_render
        and bool(obj.get("ow_export_visual", True))
        and not _is_explicit_collider(obj)
        and not _is_auto_helper_object(obj)
        and not has_lod0_sibling
        and not any(
            marker in _object_semantics(obj)
            for marker in ("shadow blocker", "shadowblocked")
        )
    )


def _auto_collision_candidate(obj: bpy.types.Object) -> bool:
    if obj.type != "MESH" or len(obj.data.polygons) == 0:
        return False
    if _is_explicit_collider(obj):
        return True
    semantics = _object_semantics(obj)
    return (
        _auto_visual_candidate(obj)
        and not any(
            marker in semantics for marker in _AUTO_NON_COLLISION_MARKERS
        )
    )


def _node_images_upstream(socket) -> list[bpy.types.Image]:
    images: list[bpy.types.Image] = []
    pending = [link.from_node for link in socket.links]
    visited: set[int] = set()
    while pending:
        node = pending.pop(0)
        identity = node.as_pointer()
        if identity in visited:
            continue
        visited.add(identity)
        if node.type == "TEX_IMAGE" and node.image is not None:
            images.append(node.image)
            continue
        for input_socket in node.inputs:
            pending.extend(link.from_node for link in input_socket.links)
    return images


def _principled_node(
    material: bpy.types.Material,
) -> bpy.types.Node | None:
    tree = material.node_tree
    if tree is None:
        return None
    return next(
        (node for node in tree.nodes if node.type == "BSDF_PRINCIPLED"),
        None,
    )


def _socket_image(
    node: bpy.types.Node | None, socket_name: str
) -> bpy.types.Image | None:
    if node is None:
        return None
    socket = node.inputs.get(socket_name)
    if socket is None:
        return None
    images = _node_images_upstream(socket)
    return images[0] if images else None


def _socket_float(
    node: bpy.types.Node | None, socket_name: str, fallback: float
) -> float:
    if node is None:
        return fallback
    socket = node.inputs.get(socket_name)
    return float(socket.default_value) if socket is not None else fallback


def _socket_color(
    node: bpy.types.Node | None,
    socket_name: str,
    fallback: tuple[float, float, float],
) -> tuple[float, float, float]:
    if node is None:
        return fallback
    socket = node.inputs.get(socket_name)
    if socket is None:
        return fallback
    return tuple(float(value) for value in socket.default_value[:3])


def _named_image(
    material: bpy.types.Material, markers: tuple[str, ...]
) -> bpy.types.Image | None:
    if material.node_tree is None:
        return None
    for node in material.node_tree.nodes:
        if node.type != "TEX_IMAGE" or node.image is None:
            continue
        identity = f"{node.name} {node.label} {node.image.name}".lower()
        if any(marker in identity for marker in markers):
            return node.image
    return None


def _infer_contact_preset(material: bpy.types.Material) -> str:
    name = material.name.lower()
    rules = (
        (("water",), "WATER"),
        (("glass", "window"), "GLASS"),
        (("ice",), "ICE"),
        (("grass", "dirt", "soil", "mud"), "GRASS"),
        (("stair", "step"), "STAIRS"),
        (("wood", "plywood", "timber"), "WOOD"),
        (("rail", "coping", "pipe", "metal"), "METAL_RAIL"),
        (("asphalt", "road", "tarmac"), "ASPHALT"),
        (("tile", "marble", "slate"), "TILE"),
        (("concrete", "cement", "brick", "stone"), "CONCRETE"),
    )
    for markers, preset in rules:
        if any(marker in name for marker in markers):
            return preset
    return "CONCRETE"


def _auto_configure_material(material: bpy.types.Material) -> bool:
    global _SYNCING
    authored_keys = (
        "ow_albedo_image",
        "ow_normal_image",
        "ow_orm_image",
        "ow_emissive_image",
        "ow_audio_surface",
        "ow_physics_surface",
    )
    if (
        any(key in material for key in authored_keys)
        and not bool(material.get("ow_auto_imported", False))
    ):
        return False

    shader = _principled_node(material)
    base_image = _socket_image(shader, "Base Color")
    normal_image = _socket_image(shader, "Normal")
    emissive_image = _socket_image(shader, "Emission Color")
    orm_image = _named_image(material, ("orm", "rma", "arm"))
    lightmap_image = _named_image(
        material, ("lightmap", "light map", "baked light")
    )
    display_color = _socket_color(
        shader, "Base Color", tuple(material.diffuse_color[:3])
    )
    roughness = max(
        0.0, min(1.0, _socket_float(shader, "Roughness", 0.78))
    )
    metallic = max(
        0.0, min(1.0, _socket_float(shader, "Metallic", 0.0))
    )
    emissive = max(
        0.0, _socket_float(shader, "Emission Strength", 0.0)
    )

    alpha_socket = shader.inputs.get("Alpha") if shader is not None else None
    alpha_linked = bool(alpha_socket and alpha_socket.links)
    alpha_value = (
        float(alpha_socket.default_value) if alpha_socket is not None else 1.0
    )
    name = material.name.lower()
    if any(marker in name for marker in _AUTO_BLEND_MARKERS):
        alpha_mode = 2
    elif alpha_linked or alpha_value < 0.999 or any(
        marker in name for marker in _AUTO_CUTOUT_MARKERS
    ):
        alpha_mode = 1
    else:
        alpha_mode = 0

    preset = _infer_contact_preset(material)
    audio, physics, pattern, _preset_roughness, _metallic, _alpha = PRESETS[
        preset
    ]
    material["ow_albedo_image"] = base_image.name if base_image else ""
    material["ow_lightmap_image"] = (
        lightmap_image.name if lightmap_image else ""
    )
    material["ow_normal_image"] = normal_image.name if normal_image else ""
    material["ow_orm_image"] = orm_image.name if orm_image else ""
    material["ow_emissive_image"] = (
        emissive_image.name if emissive_image else ""
    )
    material["ow_display_color"] = display_color
    material["ow_flags"] = 1
    material["ow_audio_surface"] = audio
    material["ow_physics_surface"] = physics
    material["ow_surface_pattern"] = pattern
    material["ow_alpha_mode"] = alpha_mode
    material["ow_alpha_cutoff"] = 0.5
    material["ow_roughness"] = roughness
    material["ow_metallic"] = metallic
    material["ow_friction"] = 0.82
    material["ow_restitution"] = 0.0
    material["ow_emissive"] = emissive
    material["ow_baked_strength"] = 1.0 if lightmap_image else 0.0
    material["ow_collision_enabled"] = not any(
        marker in name for marker in _AUTO_NON_COLLISION_MARKERS
    )
    material["ow_auto_imported"] = True
    was_syncing = _SYNCING
    _SYNCING = True
    try:
        _hydrate_material(material)
    finally:
        _SYNCING = was_syncing
    return True


def _new_uv_layer(
    mesh: bpy.types.Mesh, name: str
) -> bpy.types.MeshUVLoopLayer:
    # Blender 5.1 can successfully add a UV layer while returning None from
    # MeshUVLoopLayers.new() for some imported meshes. Resolve the created
    # layer from the collection instead of trusting that return value.
    mesh.uv_layers.new(name=name)
    layer = mesh.uv_layers.get(name)
    if layer is None:
        raise RuntimeError(
            f"{mesh.name}: Blender could not create the {name!r} UV layer"
        )
    return layer


def _ensure_auto_uv_layers(mesh: bpy.types.Mesh) -> tuple[int, bool]:
    created = 0
    source = mesh.uv_layers.get("UVMap") or mesh.uv_layers.active
    missing_source = source is None
    if source is None:
        source = _new_uv_layer(mesh, "UVMap")
        created += 1
    elif source.name != "UVMap" and mesh.uv_layers.get("UVMap") is None:
        uv_map = _new_uv_layer(mesh, "UVMap")
        for destination, original in zip(
            uv_map.data, source.data, strict=False
        ):
            destination.uv = original.uv
        source = uv_map
        created += 1
    else:
        source = mesh.uv_layers.get("UVMap") or source

    if mesh.uv_layers.get("Lightmap") is None:
        if len(mesh.uv_layers) >= 8:
            lightmap = mesh.uv_layers[-1]
            if lightmap != source:
                lightmap.name = "Lightmap"
        else:
            lightmap = _new_uv_layer(mesh, "Lightmap")
            for destination, original in zip(
                lightmap.data, source.data, strict=False
            ):
                destination.uv = original.uv
            created += 1
    return created, missing_source


def _default_material() -> bpy.types.Material:
    material = bpy.data.materials.get("SKATE_Auto_Concrete")
    if material is None:
        material = bpy.data.materials.new("SKATE_Auto_Concrete")
        material.diffuse_color = (0.45, 0.45, 0.47, 1.0)
        _auto_configure_material(material)
    return material


def _first_collision_material(
    obj: bpy.types.Object,
    used_materials: set[str],
    fallback: bpy.types.Material,
) -> bpy.types.Material:
    for slot in obj.material_slots:
        material = slot.material
        if (
            material is not None
            and material.name in used_materials
            and bool(material.get("ow_collision_enabled", True))
        ):
            return material
    return fallback


def _auto_prepare_scene(
    context, *, include_existing_roles: bool = False
) -> tuple[str, list[str]]:
    global _SYNCING
    scene = context.scene
    visual_collection = _ensure_collection(
        scene, exporter.VISUAL_COLLECTION
    )
    collision_collection = _ensure_collection(
        scene, exporter.COLLISION_COLLECTION
    )
    _ensure_collection(scene, exporter.GRIND_COLLECTION)
    _ensure_collection(scene, exporter.NPC_PATH_COLLECTION)

    scene_meshes = [
        obj for obj in scene.objects if obj.type == "MESH"
    ]
    visual_empty = not any(
        obj.type == "MESH" for obj in visual_collection.all_objects
    )
    collision_empty = not any(
        obj.type == "MESH" for obj in collision_collection.all_objects
    )
    configure_visuals = include_existing_roles or visual_empty
    configure_collision = include_existing_roles or collision_empty

    visual_candidates = [
        obj for obj in scene_meshes if _auto_visual_candidate(obj)
    ]
    if configure_visuals:
        for obj in visual_candidates:
            _link_object(visual_collection, obj)
            obj["ow_export_visual"] = True

    active_visuals = [
        obj
        for obj in visual_collection.all_objects
        if obj.type == "MESH" and bool(obj.get("ow_export_visual", True))
    ]
    default_material = _default_material()
    configured_materials = 0
    created_uv_layers = 0
    missing_source_uvs: list[str] = []
    visited_meshes: set[int] = set()
    used_materials: set[str] = set()
    for obj in active_visuals:
        if not obj.data.materials:
            obj.data.materials.append(default_material)
        mesh_identity = obj.data.as_pointer()
        if mesh_identity not in visited_meshes:
            created, missing_source = _ensure_auto_uv_layers(obj.data)
            created_uv_layers += created
            if missing_source:
                missing_source_uvs.append(obj.name)
            visited_meshes.add(mesh_identity)
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            used_materials.add(material.name)
            configured_materials += int(_auto_configure_material(material))

    fallback = next(
        (
            bpy.data.materials.get(name)
            for name in sorted(used_materials)
            if bpy.data.materials.get(name) is not None
            and bool(
                bpy.data.materials[name].get(
                    "ow_collision_enabled", True
                )
            )
        ),
        default_material,
    )
    if configure_collision:
        for obj in scene_meshes:
            if not _auto_collision_candidate(obj):
                continue
            _link_object(collision_collection, obj)
            material = _first_collision_material(
                obj, used_materials, fallback
            )
            obj["ow_material"] = material.name
            obj["ow_upward_surface"] = False
            was_syncing = _SYNCING
            _SYNCING = True
            try:
                _hydrate_physics(obj)
            finally:
                _SYNCING = was_syncing

    if scene.owned_world.map_name == "My Skate Map" and bpy.data.filepath:
        scene.owned_world.map_name = Path(bpy.data.filepath).stem
    if (
        scene.owned_world.output_path == "//my_skate_map.skate"
        and bpy.data.filepath
    ):
        scene.owned_world.output_path = str(
            Path(bpy.data.filepath).with_suffix(".skate")
        )

    warnings: list[str] = []
    if missing_source_uvs:
        warnings.append(
            f"{len(missing_source_uvs)} mesh(es) had no source UVs; "
            "their generated UVMap is empty until unwrapped."
        )
    stats = (
        f"Auto-prepared {len(active_visuals)} visual object(s), "
        f"{len(collision_collection.all_objects)} collision object(s), "
        f"{configured_materials} Blender material(s), "
        f"{created_uv_layers} UV layer(s), and "
        f"{len(exporter._visible_local_light_objects())} real Blender "
        "local light(s)."
    )
    return stats, warnings


def _validate_scene(
    context,
    *,
    inspect_geometry: bool = True,
) -> tuple[list[str], list[str], str]:
    issues: list[str] = []
    warnings: list[str] = []
    scene = context.scene
    settings = scene.owned_world

    visual_collection = bpy.data.collections.get(exporter.VISUAL_COLLECTION)
    collision_collection = bpy.data.collections.get(
        exporter.COLLISION_COLLECTION
    )
    grind_collection = bpy.data.collections.get(exporter.GRIND_COLLECTION)
    npc_path_collection = bpy.data.collections.get(
        exporter.NPC_PATH_COLLECTION
    )
    spawn = bpy.data.objects.get(exporter.SPAWN_OBJECT)

    visual_objects = (
        [
            obj
            for obj in visual_collection.all_objects
            if obj.type == "MESH"
            and bool(obj.get("ow_export_visual", True))
            and not _is_auto_helper_object(obj)
        ]
        if visual_collection is not None
        else []
    )
    collision_objects = (
        [
            obj
            for obj in collision_collection.all_objects
            if obj.type == "MESH"
            and not _is_auto_helper_object(obj)
        ]
        if collision_collection is not None
        else []
    )
    grind_objects = (
        [
            obj
            for obj in grind_collection.all_objects
            if obj.type == "CURVE"
        ]
        if grind_collection is not None
        else []
    )
    npc_path_objects = (
        [
            obj
            for obj in npc_path_collection.all_objects
            if obj.type == "CURVE"
        ]
        if npc_path_collection is not None
        else []
    )
    local_lights = exporter._visible_local_light_objects()
    sun_lights = [
        obj
        for obj in exporter._visible_light_objects()
        if obj.data.type == "SUN"
    ]
    if len(sun_lights) > 1:
        warnings.append(
            f"{len(sun_lights)} Sun lights are enabled; "
            f"{sun_lights[0].name!r} will control the world sun."
        )

    if visual_collection is None:
        issues.append("Missing OW_VISUAL collection; select Prepare Scene.")
    elif not visual_objects:
        issues.append("OW_VISUAL has no mesh objects.")

    used_materials: set[str] = set()
    for obj in visual_objects:
        mesh = obj.data
        if mesh.uv_layers.get("UVMap") is None:
            issues.append(f"{obj.name}: missing UVMap UV layer.")
        if mesh.uv_layers.get("Lightmap") is None:
            issues.append(f"{obj.name}: missing Lightmap UV layer.")
        materials = [
            slot.material for slot in obj.material_slots
            if slot.material is not None
        ]
        if not materials:
            issues.append(f"{obj.name}: no material assigned.")
        used_materials.update(material.name for material in materials)
        if (
            str(obj.get("ow_physics_type", "STATIC")) == "HINGED_DOOR"
            and "ow_hinge_position" not in obj
        ):
            warnings.append(
                f"{obj.name}: door hinge uses the object origin."
            )

    if collision_collection is None:
        issues.append(
            "Missing OW_COLLISION collection; select Prepare Scene."
        )
    elif not collision_objects:
        issues.append("OW_COLLISION has no mesh objects.")
    for obj in collision_objects:
        material_name = str(obj.get("ow_material", ""))
        if not material_name:
            issues.append(
                f"{obj.name}: collision material is not assigned."
            )
        elif material_name not in used_materials:
            issues.append(
                f"{obj.name}: collision material {material_name!r} is not "
                "used by OW_VISUAL."
            )
    collision_audit = None
    if collision_objects and inspect_geometry:
        _triangles, collision_audit = exporter.audit_collision_geometry(
            collision_objects
        )
        issues.extend(collision_audit.issues)
        warnings.extend(collision_audit.warnings)

    for obj in grind_objects:
        if not any(
            len(spline.points) >= 2 or len(spline.bezier_points) >= 2
            for spline in obj.data.splines
        ):
            warnings.append(f"{obj.name}: grind curve has fewer than 2 points.")
    for obj in npc_path_objects:
        if not any(
            len(spline.points) >= 2 or len(spline.bezier_points) >= 2
            for spline in obj.data.splines
        ):
            issues.append(f"{obj.name}: NPC path has fewer than 2 points.")

    if spawn is None:
        issues.append("Missing OW_SPAWN; select Set Spawn at 3D Cursor.")
    if not settings.map_name.strip():
        issues.append("Map Name cannot be empty.")

    raw_output = settings.output_path.strip()
    if not raw_output:
        issues.append("Choose an SKATE output file.")
    elif Path(bpy.path.abspath(raw_output)).suffix.lower() != ".skate":
        warnings.append("The .skate extension will be added automatically.")

    collision_stats = (
        f", {collision_audit.exported_triangles} usable collision triangle(s)"
        if collision_audit is not None
        else ""
    )
    stats = (
        f"{len(visual_objects)} visual object(s), "
        f"{len(collision_objects)} collision object(s), "
        f"{len(grind_objects)} grind curve(s), "
        f"{len(npc_path_objects)} NPC route(s), "
        f"{len(used_materials)} material(s), "
        f"{len(local_lights)} local light(s)"
        f"{collision_stats}"
    )
    return issues, warnings, stats


def _store_validation(
    settings: OwnedWorldSceneSettings,
    issues: list[str],
    warnings: list[str],
    stats: str,
) -> None:
    lines = [
        *(f"ERROR: {message}" for message in issues),
        *(f"WARNING: {message}" for message in warnings),
    ]
    settings.validation_details = "\n".join(lines)
    if issues:
        settings.last_status = (
            f"Map needs attention: {len(issues)} error(s), "
            f"{len(warnings)} warning(s)."
        )
    elif warnings:
        settings.last_status = (
            f"Ready to export with {len(warnings)} warning(s): {stats}."
        )
    else:
        settings.last_status = f"Ready to export: {stats}."


def _resolved_output(settings: OwnedWorldSceneSettings) -> Path:
    raw = settings.output_path.strip()
    if not raw:
        raw = "//my_skate_map.skate"
    output = Path(bpy.path.abspath(raw)).resolve()
    if output.suffix.lower() != ".skate":
        output = output.with_suffix(".skate")
    return output


def _run_export(context, output: Path) -> set[str]:
    settings = context.scene.owned_world
    _auto_stats, auto_warnings = _auto_prepare_scene(context)
    _sync_scene(settings)
    # The exporter performs the authoritative geometry audit once. Keeping
    # this preflight structural-only avoids evaluating huge collision meshes
    # twice during Quick Export.
    issues, warnings, stats = _validate_scene(
        context, inspect_geometry=False
    )
    warnings = [*auto_warnings, *warnings]
    _store_validation(settings, issues, warnings, stats)
    if issues:
        raise ValueError(
            "Map validation failed. Open the Skate 3 Map panel for details."
        )
    window_manager = context.window_manager
    last_percent = -1
    window_manager.progress_begin(0, 100)

    def update_progress(fraction: float, stage: str) -> None:
        nonlocal last_percent
        percent = max(0, min(100, int(round(fraction * 100.0))))
        settings.last_status = f"Exporting {percent}% — {stage}"
        window_manager.progress_update(percent)
        workspace = getattr(context, "workspace", None)
        if workspace is not None:
            try:
                workspace.status_text_set(
                    text=f"SKATE export: {percent}% — {stage}"
                )
            except (AttributeError, RuntimeError):
                pass
        if percent == last_percent:
            return
        last_percent = percent
        print(
            f"SKATE progress: {percent}% — {stage}",
            flush=True,
        )
        if bpy.app.background:
            return
        screen = getattr(context, "screen", None)
        if screen is not None:
            for area in screen.areas:
                area.tag_redraw()
        try:
            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        except RuntimeError:
            pass

    try:
        result = exporter.export_scene(
            output,
            force_rebuild=settings.export_mode == "FORCE",
            metadata_only=settings.export_mode == "METADATA",
            progress=update_progress,
        )
    finally:
        window_manager.progress_end()
        workspace = getattr(context, "workspace", None)
        if workspace is not None:
            try:
                workspace.status_text_set(text=None)
            except (AttributeError, RuntimeError):
                pass
    settings.output_path = str(result)
    settings.last_status = f"Exported successfully: {result.name}"
    collision_audit = exporter.LAST_COLLISION_AUDIT
    if collision_audit is not None:
        warnings.extend(collision_audit.warnings)
    settings.validation_details = "\n".join(
        f"WARNING: {message}" for message in warnings
    )
    return {"FINISHED"}


def _store_export_failure(
    settings: OwnedWorldSceneSettings,
    error: Exception,
) -> None:
    if isinstance(error, exporter.CollisionGeometryError):
        _store_validation(
            settings,
            error.issues,
            error.warnings,
            "collision geometry audit",
        )
        return
    settings.last_status = f"Export failed: {error}"
    settings.validation_details = f"ERROR: {error}"


class SKATE_OT_prepare_scene(Operator):
    bl_idname = "skate_map.prepare_scene"
    bl_label = "Prepare Scene"
    bl_description = (
        "Create required map structure plus optional grind and NPC collections"
    )
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        for name in (
            exporter.VISUAL_COLLECTION,
            exporter.COLLISION_COLLECTION,
            exporter.GRIND_COLLECTION,
            exporter.NPC_PATH_COLLECTION,
        ):
            _ensure_collection(context.scene, name)
        spawn = bpy.data.objects.get(exporter.SPAWN_OBJECT)
        if spawn is None:
            spawn = bpy.data.objects.new(exporter.SPAWN_OBJECT, None)
            spawn.empty_display_type = "ARROWS"
            spawn.empty_display_size = 1.0
            context.scene.collection.objects.link(spawn)
            spawn.location = context.scene.cursor.location
        _sync_scene(context.scene.owned_world)
        context.scene.owned_world.last_status = (
            "Scene prepared. Add visual and collision objects next."
        )
        self.report({"INFO"}, "Owned World scene structure is ready")
        return {"FINISHED"}


class SKATE_OT_auto_prepare_scene(Operator):
    bl_idname = "skate_map.auto_prepare_scene"
    bl_label = "Auto Prepare Blender Map"
    bl_description = (
        "Automatically adopt visible Blender meshes, shader textures, "
        "ordinary Blender lights, UV layers, and sensible static collision"
    )
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            stats, warnings = _auto_prepare_scene(
                context, include_existing_roles=True
            )
            settings = context.scene.owned_world
            settings.last_status = stats
            settings.validation_details = "\n".join(
                f"WARNING: {warning}" for warning in warnings
            )
            self.report({"INFO"}, stats)
            return {"FINISHED"}
        except Exception as error:
            traceback.print_exc()
            _store_export_failure(context.scene.owned_world, error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class SKATE_OT_assign_selected(Operator):
    bl_idname = "skate_map.assign_selected"
    bl_label = "Assign Selected"
    bl_description = "Assign selected objects to an SKATE role"
    bl_options = {"REGISTER", "UNDO"}

    role: EnumProperty(
        name="Role",
        items=(
            ("VISUAL", "Visual", "Rendered map geometry"),
            ("COLLISION", "Collision", "Authoritative collision geometry"),
            ("GRIND", "Grind", "Curve used as a grind path"),
            (
                "NPC_PATH",
                "NPC Path (Experimental)",
                "Experimental route data for native AI skaters",
            ),
        ),
    )

    def execute(self, context):
        if self.role in {"GRIND", "NPC_PATH"}:
            objects = _selected_of_type(context, "CURVE")
            collection_name = (
                exporter.GRIND_COLLECTION
                if self.role == "GRIND"
                else exporter.NPC_PATH_COLLECTION
            )
        else:
            objects = _selected_of_type(context, "MESH")
            collection_name = (
                exporter.VISUAL_COLLECTION
                if self.role == "VISUAL"
                else exporter.COLLISION_COLLECTION
            )
        if not objects:
            expected = (
                "curves" if self.role in {"GRIND", "NPC_PATH"} else "meshes"
            )
            self.report({"ERROR"}, f"Select one or more {expected}")
            return {"CANCELLED"}

        collection = _ensure_collection(context.scene, collection_name)
        missing_material: list[str] = []
        for obj in objects:
            _link_object(collection, obj)
            if self.role == "VISUAL":
                obj["ow_export_visual"] = True
            elif self.role == "COLLISION":
                material = obj.active_material
                if material is None:
                    material = next(
                        (
                            slot.material
                            for slot in obj.material_slots
                            if slot.material is not None
                        ),
                        None,
                    )
                if material is None:
                    missing_material.append(obj.name)
                else:
                    obj.owned_world_physics.collision_material = material
            elif self.role == "NPC_PATH":
                _sync_npc_path(obj.owned_world_npc_path)

        if missing_material:
            self.report(
                {"WARNING"},
                "Collision assigned, but these objects need a material: "
                + ", ".join(missing_material[:4]),
            )
        else:
            self.report(
                {"INFO"},
                f"Assigned {len(objects)} object(s) as {self.role.lower()}",
            )
        return {"FINISHED"}


class SKATE_OT_create_uv_layers(Operator):
    bl_idname = "skate_map.create_uv_layers"
    bl_label = "Create Missing UV Layers"
    bl_description = (
        "Create UVMap and Lightmap layers, copying the active UVs when "
        "available"
    )
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        objects = _selected_of_type(context, "MESH")
        if not objects:
            self.report({"ERROR"}, "Select one or more mesh objects")
            return {"CANCELLED"}
        created = 0
        empty_created = 0
        for obj in objects:
            layers = obj.data.uv_layers
            source = layers.active
            uv_map = layers.get("UVMap")
            if uv_map is None:
                uv_map = _new_uv_layer(obj.data, "UVMap")
                created += 1
                if source is not None and source != uv_map:
                    for destination, original in zip(
                        uv_map.data, source.data, strict=False
                    ):
                        destination.uv = original.uv
                else:
                    empty_created += 1
            lightmap = layers.get("Lightmap")
            if lightmap is None:
                lightmap = _new_uv_layer(obj.data, "Lightmap")
                created += 1
                for destination, original in zip(
                    lightmap.data, uv_map.data, strict=False
                ):
                    destination.uv = original.uv
        if empty_created:
            self.report(
                {"WARNING"},
                "Layers created; meshes without existing UVs still need "
                "to be unwrapped",
            )
        else:
            self.report({"INFO"}, f"Created {created} missing UV layer(s)")
        return {"FINISHED"}


class SKATE_OT_set_spawn(Operator):
    bl_idname = "skate_map.set_spawn"
    bl_label = "Set Spawn at 3D Cursor"
    bl_description = "Create or move the player spawn to the 3D cursor"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        spawn = bpy.data.objects.get(exporter.SPAWN_OBJECT)
        if spawn is None:
            spawn = bpy.data.objects.new(exporter.SPAWN_OBJECT, None)
            spawn.empty_display_type = "ARROWS"
            spawn.empty_display_size = 1.0
            context.scene.collection.objects.link(spawn)
        spawn.location = context.scene.cursor.location
        spawn["ow_heading_radians"] = float(
            context.scene.owned_world.spawn_heading
        )
        self.report({"INFO"}, "Spawn moved to the 3D cursor")
        return {"FINISHED"}


class SKATE_OT_validate(Operator):
    bl_idname = "skate_map.validate"
    bl_label = "Validate Map"
    bl_description = "Check the scene and explain anything blocking export"

    def execute(self, context):
        settings = context.scene.owned_world
        _auto_stats, auto_warnings = _auto_prepare_scene(context)
        _sync_scene(settings)
        issues, warnings, stats = _validate_scene(context)
        warnings = [*auto_warnings, *warnings]
        _store_validation(settings, issues, warnings, stats)
        if issues:
            self.report(
                {"ERROR"},
                f"Found {len(issues)} error(s); see the Skate 3 Map panel",
            )
            return {"CANCELLED"}
        self.report(
            {"INFO"},
            f"Map is ready ({len(warnings)} warning(s)): {stats}",
        )
        return {"FINISHED"}


class SKATE_OT_quick_export(Operator):
    bl_idname = "skate_map.quick_export"
    bl_label = "Quick Export"
    bl_description = "Export directly to the configured SKATE file"

    def execute(self, context):
        try:
            return _run_export(
                context, _resolved_output(context.scene.owned_world)
            )
        except Exception as error:
            traceback.print_exc()
            _store_export_failure(context.scene.owned_world, error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class SKATE_OT_export_dialog(Operator, ExportHelper):
    bl_idname = "skate_map.export_dialog"
    bl_label = "Export Skate 3 Custom Engine Map"
    bl_description = "Validate and export this scene as an SKATE package"
    filename_ext = ".skate"

    filter_glob: StringProperty(default="*.skate", options={"HIDDEN"})

    def invoke(self, context, event):
        self.filepath = str(_resolved_output(context.scene.owned_world))
        return ExportHelper.invoke(self, context, event)

    def execute(self, context):
        try:
            output = Path(self.filepath).resolve()
            if output.suffix.lower() != ".skate":
                output = output.with_suffix(".skate")
            return _run_export(context, output)
        except Exception as error:
            traceback.print_exc()
            _store_export_failure(context.scene.owned_world, error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class SKATE_OT_open_output_folder(Operator):
    bl_idname = "skate_map.open_output_folder"
    bl_label = "Open Output Folder"
    bl_description = "Open the folder containing the exported SKATE"

    def execute(self, context):
        folder = _resolved_output(context.scene.owned_world).parent
        folder.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.path_open(filepath=str(folder))
        return {"FINISHED"}


class OWMATERIAL_OT_apply_preset(Operator):
    bl_idname = "owmaterial.apply_preset"
    bl_label = "Apply Owned Material Preset"
    bl_options = {"REGISTER", "UNDO"}

    preset: EnumProperty(name="Preset", items=PRESET_ITEMS)

    def execute(self, context):
        material = context.material
        if material is None:
            self.report({"ERROR"}, "No active material")
            return {"CANCELLED"}
        audio, physics, pattern, roughness, metallic, alpha = PRESETS[
            self.preset
        ]
        settings = material.owned_world
        settings.audio_surface = str(audio)
        settings.physics_surface = str(physics)
        settings.surface_pattern = str(pattern)
        settings.roughness = roughness
        settings.metallic = metallic
        settings.alpha_mode = str(alpha)
        _sync(settings)
        self.report({"INFO"}, f"Applied {self.preset} to {material.name}")
        return {"FINISHED"}


class OWMATERIAL_PT_material(Panel):
    bl_label = "Owned World Material"
    bl_idname = "OWMATERIAL_PT_material"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        settings = context.material.owned_world

        presets = layout.box()
        presets.label(text="Quick Presets")
        grid = presets.grid_flow(columns=2, even_columns=True)
        for identifier, label, _description in PRESET_ITEMS:
            operator = grid.operator(
                "owmaterial.apply_preset", text=label
            )
            operator.preset = identifier

        game = layout.box()
        game.label(text="Skate 3 Contact")
        game.prop(settings, "audio_surface")
        game.prop(settings, "physics_surface")
        game.prop(settings, "surface_pattern")
        game.prop(settings, "collision_enabled")
        game.prop(settings, "friction")
        game.prop(settings, "restitution")

        pbr = layout.box()
        pbr.label(text="PBR Presentation")
        pbr.prop(settings, "albedo_image")
        pbr.prop(settings, "lightmap_image")
        pbr.prop(settings, "normal_image")
        pbr.prop(settings, "orm_image")
        pbr.prop(settings, "emissive_image")
        pbr.prop(settings, "roughness")
        pbr.prop(settings, "metallic")
        pbr.prop(settings, "baked_strength")
        pbr.prop(settings, "emissive_strength")
        pbr.prop(settings, "alpha_mode")
        if settings.alpha_mode == "1":
            pbr.prop(settings, "alpha_cutoff")
        pbr.label(text="ORM channels: R=AO, G=Roughness, B=Metallic")


class OWPHYSICS_PT_object(Panel):
    bl_label = "Owned World Physics"
    bl_idname = "OWPHYSICS_PT_object"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "physics"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == "MESH"

    def draw(self, context):
        layout = self.layout
        obj = context.object
        settings = obj.owned_world_physics
        collision = layout.box()
        collision.label(text="Collision Authoring")
        collision.prop(settings, "collision_material")
        collision.prop(settings, "upward_surface")
        layout.prop(settings, "physics_type")
        if settings.physics_type != "HINGED_DOOR":
            return
        hinge = layout.box()
        hinge.label(text="Hinge")
        hinge.operator("owphysics.hinge_from_cursor")
        hinge.prop(settings, "hinge_axis")
        if "ow_hinge_position" in obj:
            hinge.label(
                text="Hinge: {:.2f}, {:.2f}, {:.2f}".format(
                    *obj["ow_hinge_position"]
                )
            )
        else:
            hinge.label(text="Hinge uses object origin", icon="INFO")
        limits = layout.box()
        limits.label(text="Opening Limits")
        limits.prop(settings, "minimum_angle")
        limits.prop(settings, "maximum_angle")
        limits.prop(settings, "initial_angle")
        body = layout.box()
        body.label(text="Rigid Body")
        body.prop(settings, "mass")
        body.prop(settings, "angular_damping")
        body.prop(settings, "return_spring_strength")
        body.prop(settings, "maximum_angular_speed")
        body.prop(settings, "contact_impulse_scale")
        body.prop(settings, "friction")
        body.prop(settings, "restitution")
        body.label(text="Push motion is contact-driven at runtime")


class OWNPCPATH_PT_object(Panel):
    bl_label = "Owned World NPC Path (Experimental)"
    bl_idname = "OWNPCPATH_PT_object"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        obj = context.object
        collection = bpy.data.collections.get(exporter.NPC_PATH_COLLECTION)
        return (
            obj is not None
            and obj.type == "CURVE"
            and collection is not None
            and obj.name in collection.all_objects
        )

    def draw(self, context):
        layout = self.layout
        settings = context.object.owned_world_npc_path
        layout.label(text="Experimental Native AI Route", icon="ERROR")
        layout.label(text="Reliable route following is not yet supported.")
        layout.prop(settings, "skater_count")
        layout.prop(settings, "speed")
        layout.prop(settings, "spawn_spacing")
        layout.label(
            text="Enable Cyclic on the spline for a continuous loop.",
            icon="INFO",
        )


def _draw_map_panel(layout, context) -> None:
    settings = context.scene.owned_world

    intro = layout.box()
    intro.label(text="Skate 3 Custom Engine Map", icon="WORLD")
    intro.label(text="Open a Blender map, set spawn, then export.")
    intro.operator(
        "skate_map.auto_prepare_scene",
        text="Auto Prepare Blender Map",
        icon="IMPORT",
    )
    intro.label(
        text="Export also auto-prepares an untouched scene.",
        icon="INFO",
    )

    authoring = layout.box()
    authoring.label(text="Optional Overrides", icon="OBJECT_DATA")
    row = authoring.row(align=True)
    visual = row.operator(
        "skate_map.assign_selected", text="Visual", icon="MESH_CUBE"
    )
    visual.role = "VISUAL"
    collision = row.operator(
        "skate_map.assign_selected", text="Collision", icon="MOD_PHYSICS"
    )
    collision.role = "COLLISION"
    grind = row.operator(
        "skate_map.assign_selected", text="Grind", icon="CURVE_DATA"
    )
    grind.role = "GRIND"
    npc_path = row.operator(
        "skate_map.assign_selected",
        text="NPC Path (Experimental)",
        icon="OUTLINER_OB_CURVE",
    )
    npc_path.role = "NPC_PATH"
    authoring.operator("skate_map.create_uv_layers", icon="GROUP_UVS")
    authoring.operator(
        "skate_map.prepare_scene",
        text="Create Empty Authoring Structure",
        icon="TOOL_SETTINGS",
    )
    authoring.label(
        text="Material and door controls are in Properties.",
        icon="INFO",
    )

    spawn = layout.box()
    spawn.label(text="Player Spawn", icon="EMPTY_ARROWS")
    spawn.prop(settings, "spawn_heading")
    spawn.operator("skate_map.set_spawn", icon="PIVOT_CURSOR")

    world = layout.box()
    world.label(text="Map and Lighting", icon="LIGHT_SUN")
    world.label(
        text="Normal Blender lights export automatically.",
        icon="LIGHT",
    )
    world.prop(settings, "map_name")
    world.prop(settings, "cycle_seconds")
    world.prop(settings, "cycle_ping_pong")
    row = world.row(align=True)
    row.prop(settings, "start_hour")
    if settings.cycle_ping_pong:
        row.prop(settings, "end_hour")
    world.prop(settings, "orbit_azimuth")
    sky = world.column(align=True)
    sky.prop(settings, "sky_zenith")
    sky.prop(settings, "sky_horizon")
    sky.prop(settings, "sky_nadir")
    world.prop(
        settings,
        "show_advanced_lighting",
        icon="TRIA_DOWN" if settings.show_advanced_lighting else "TRIA_RIGHT",
    )
    if settings.show_advanced_lighting:
        grade = world.column(align=True)
        grade.label(text="Sky Colour")
        grade.prop(settings, "sky_tint")
        twilight = world.column(align=True)
        twilight.label(text="Twilight Palette")
        twilight.prop(settings, "twilight_zenith")
        twilight.prop(settings, "twilight_horizon")
        twilight.prop(settings, "twilight_nadir")
        night = world.column(align=True)
        night.label(text="Night Palette")
        night.prop(settings, "night_zenith")
        night.prop(settings, "night_horizon")
        night.prop(settings, "night_nadir")
        lights = world.column(align=True)
        lights.label(text="Directional and Ambient Light")
        lights.prop(settings, "sun_color")
        lights.prop(settings, "sun_intensity")
        lights.prop(settings, "moon_color")
        lights.prop(settings, "moon_intensity")
        lights.prop(settings, "day_ambient")
        lights.prop(settings, "night_ambient")

    output = layout.box()
    output.label(text="Validate and Export", icon="EXPORT")
    output.prop(settings, "output_path")
    output.prop(settings, "export_mode")
    row = output.row(align=True)
    row.operator("skate_map.validate", icon="CHECKMARK")
    row.operator("skate_map.quick_export", icon="FILE_TICK")
    row = output.row(align=True)
    row.operator("skate_map.export_dialog", text="Export As...", icon="EXPORT")
    row.operator(
        "skate_map.open_output_folder", text="Open Folder", icon="FILE_FOLDER"
    )

    status_icon = (
        "ERROR"
        if settings.last_status.startswith(("Map needs", "Export failed"))
        else "INFO"
    )
    output.label(text=settings.last_status, icon=status_icon)
    for line in settings.validation_details.splitlines()[:20]:
        icon = "ERROR" if line.startswith("ERROR:") else "INFO"
        output.label(text=line, icon=icon)
    remaining = len(settings.validation_details.splitlines()) - 20
    if remaining > 0:
        output.label(
            text=f"...and {remaining} more. Fix these first, then validate."
        )


class SKATE_PT_sidebar(Panel):
    bl_label = "Map Builder"
    bl_idname = "SKATE_PT_sidebar"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Skate 3 Map"

    def draw(self, context):
        _draw_map_panel(self.layout, context)


class SKATE_PT_scene(Panel):
    bl_label = "Skate 3 Custom Engine Map"
    bl_idname = "SKATE_PT_scene"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "scene"

    def draw(self, context):
        _draw_map_panel(self.layout, context)


def _hydrate_material(material: Material) -> None:
    settings = material.owned_world
    settings.audio_surface = str(
        max(0, min(93, int(material.get("ow_audio_surface", 3))))
    )
    settings.physics_surface = str(
        max(0, min(12, int(material.get("ow_physics_surface", 1))))
    )
    settings.surface_pattern = str(
        max(0, min(15, int(material.get("ow_surface_pattern", 0))))
    )
    settings.alpha_mode = str(
        max(0, min(2, int(material.get("ow_alpha_mode", 0))))
    )
    settings.alpha_cutoff = float(material.get("ow_alpha_cutoff", 0.5))
    settings.roughness = float(material.get("ow_roughness", 0.78))
    settings.metallic = float(material.get("ow_metallic", 0.0))
    settings.friction = float(material.get("ow_friction", 0.82))
    settings.restitution = float(material.get("ow_restitution", 0.0))
    settings.emissive_strength = float(material.get("ow_emissive", 0.0))
    settings.baked_strength = float(
        material.get("ow_baked_strength", 1.0)
    )
    settings.collision_enabled = bool(
        material.get("ow_collision_enabled", True)
    )
    for attribute, property_name in (
        ("albedo_image", "ow_albedo_image"),
        ("lightmap_image", "ow_lightmap_image"),
        ("normal_image", "ow_normal_image"),
        ("orm_image", "ow_orm_image"),
        ("emissive_image", "ow_emissive_image"),
    ):
        setattr(
            settings,
            attribute,
            bpy.data.images.get(str(material.get(property_name, ""))),
        )


def _hydrate_physics(obj: Object) -> None:
    settings = obj.owned_world_physics
    settings.collision_material = bpy.data.materials.get(
        str(obj.get("ow_material", ""))
    )
    settings.upward_surface = bool(obj.get("ow_upward_surface", False))
    physics_type = str(obj.get("ow_physics_type", "STATIC"))
    settings.physics_type = (
        physics_type if physics_type in {"STATIC", "HINGED_DOOR"} else "STATIC"
    )
    settings.hinge_axis = tuple(obj.get("ow_hinge_axis", (0.0, 0.0, 1.0)))
    settings.minimum_angle = math.radians(
        float(obj.get("ow_door_min_angle_degrees", -105.0))
    )
    settings.maximum_angle = math.radians(
        float(obj.get("ow_door_max_angle_degrees", 105.0))
    )
    settings.initial_angle = math.radians(
        float(obj.get("ow_door_initial_angle_degrees", 0.0))
    )
    settings.mass = float(obj.get("ow_door_mass", 32.0))
    settings.angular_damping = float(
        obj.get("ow_door_angular_damping", 2.2)
    )
    settings.return_spring_strength = float(
        obj.get("ow_door_return_spring_strength", 0.0)
    )
    settings.maximum_angular_speed = float(
        obj.get("ow_door_maximum_angular_speed", 8.0)
    )
    settings.contact_impulse_scale = float(
        obj.get("ow_door_contact_impulse_scale", 1.0)
    )
    settings.friction = float(obj.get("ow_door_friction", 0.55))
    settings.restitution = float(obj.get("ow_door_restitution", 0.02))


def _hydrate_npc_path(obj: Object) -> None:
    settings = obj.owned_world_npc_path
    settings.skater_count = max(
        1, min(32, int(obj.get("ow_npc_skater_count", 1)))
    )
    settings.speed = max(
        0.1, min(30.0, float(obj.get("ow_npc_speed", 5.5)))
    )
    settings.spawn_spacing = max(
        0.0, min(100.0, float(obj.get("ow_npc_spawn_spacing", 3.0)))
    )


def _hydrate_scene(scene: Scene) -> None:
    settings = scene.owned_world
    settings.map_name = str(scene.get("ow_map_name", "My Skate Map"))
    settings.cycle_seconds = float(scene.get("ow_cycle_seconds", 96.0))
    settings.start_hour = float(scene.get("ow_start_hour", 9.0))
    settings.end_hour = float(scene.get("ow_end_hour", 17.0))
    settings.cycle_ping_pong = bool(
        scene.get("ow_cycle_ping_pong", True)
    )
    settings.orbit_azimuth = float(scene.get("ow_orbit_azimuth", 0.62))
    settings.sky_zenith = tuple(
        scene.get("ow_sky_zenith", (0.09, 0.34, 0.72))
    )
    settings.sky_horizon = tuple(
        scene.get("ow_sky_horizon", (0.58, 0.78, 0.98))
    )
    settings.sky_nadir = tuple(
        scene.get("ow_sky_nadir", (0.18, 0.25, 0.34))
    )
    settings.twilight_zenith = tuple(
        scene.get("ow_twilight_zenith", (0.045, 0.10, 0.26))
    )
    settings.twilight_horizon = tuple(
        scene.get("ow_twilight_horizon", (1.0, 0.32, 0.10))
    )
    settings.twilight_nadir = tuple(
        scene.get("ow_twilight_nadir", (0.05, 0.035, 0.06))
    )
    settings.night_zenith = tuple(
        scene.get("ow_night_zenith", (0.007, 0.015, 0.045))
    )
    settings.night_horizon = tuple(
        scene.get("ow_night_horizon", (0.045, 0.085, 0.17))
    )
    settings.night_nadir = tuple(
        scene.get("ow_night_nadir", (0.008, 0.014, 0.032))
    )
    settings.sun_color = tuple(
        scene.get("ow_sun_color", (1.0, 0.92, 0.78))
    )
    settings.moon_color = tuple(
        scene.get("ow_moon_color", (0.42, 0.56, 0.92))
    )
    settings.sun_intensity = float(scene.get("ow_sun_intensity", 1.25))
    settings.moon_intensity = float(scene.get("ow_moon_intensity", 0.18))
    settings.day_ambient = float(scene.get("ow_day_ambient", 0.32))
    settings.night_ambient = float(scene.get("ow_night_ambient", 0.11))
    settings.sky_tint = tuple(
        scene.get("ow_sky_tint", (1.0, 1.0, 1.0))
    )
    spawn = bpy.data.objects.get(exporter.SPAWN_OBJECT)
    settings.spawn_heading = (
        float(spawn.get("ow_heading_radians", 0.0))
        if spawn is not None
        else 0.0
    )


def _hydrate_all() -> None:
    global _SYNCING
    _SYNCING = True
    try:
        for material in bpy.data.materials:
            _hydrate_material(material)
        for obj in bpy.data.objects:
            if obj.type == "MESH":
                _hydrate_physics(obj)
            elif obj.type == "CURVE":
                _hydrate_npc_path(obj)
        for scene in bpy.data.scenes:
            _hydrate_scene(scene)
    finally:
        _SYNCING = False


@persistent
def _load_post(_unused) -> None:
    _hydrate_all()


def _file_export_menu(self, _context) -> None:
    self.layout.operator(
        SKATE_OT_export_dialog.bl_idname,
        text="Skate 3 Custom Engine Map (.skate)",
    )


CLASSES = (
    OwnedWorldMaterialSettings,
    OwnedWorldPhysicsSettings,
    OwnedWorldNpcPathSettings,
    OwnedWorldSceneSettings,
    SKATE_OT_prepare_scene,
    SKATE_OT_auto_prepare_scene,
    SKATE_OT_assign_selected,
    SKATE_OT_create_uv_layers,
    SKATE_OT_set_spawn,
    SKATE_OT_validate,
    SKATE_OT_quick_export,
    SKATE_OT_export_dialog,
    SKATE_OT_open_output_folder,
    OWMATERIAL_OT_apply_preset,
    OWPHYSICS_OT_hinge_from_cursor,
    OWMATERIAL_PT_material,
    OWPHYSICS_PT_object,
    OWNPCPATH_PT_object,
    SKATE_PT_sidebar,
    SKATE_PT_scene,
)


def register() -> None:
    for cls in CLASSES:
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            pass
    if not hasattr(Material, "owned_world"):
        Material.owned_world = PointerProperty(
            type=OwnedWorldMaterialSettings
        )
    if not hasattr(Object, "owned_world_physics"):
        Object.owned_world_physics = PointerProperty(
            type=OwnedWorldPhysicsSettings
        )
    if not hasattr(Object, "owned_world_npc_path"):
        Object.owned_world_npc_path = PointerProperty(
            type=OwnedWorldNpcPathSettings
        )
    if not hasattr(Scene, "owned_world"):
        Scene.owned_world = PointerProperty(type=OwnedWorldSceneSettings)
    if _load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_load_post)
    bpy.types.TOPBAR_MT_file_export.append(_file_export_menu)
    _hydrate_all()


def unregister() -> None:
    try:
        bpy.types.TOPBAR_MT_file_export.remove(_file_export_menu)
    except RuntimeError:
        pass
    if _load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_load_post)
    if hasattr(Scene, "owned_world"):
        del Scene.owned_world
    if hasattr(Object, "owned_world_physics"):
        del Object.owned_world_physics
    if hasattr(Object, "owned_world_npc_path"):
        del Object.owned_world_npc_path
    if hasattr(Material, "owned_world"):
        del Material.owned_world
    for cls in reversed(CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass


if __name__ == "__main__":
    register()
