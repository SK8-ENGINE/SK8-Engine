from __future__ import annotations

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector

from scene_groups import world_bounds


PREVIEW_WIDTH = 1280
PREVIEW_HEIGHT = 800
PREVIEW_MARGIN = 1.08


def _scene_bounds() -> tuple[Vector, Vector]:
    bounds = [
        world_bounds(obj)
        for obj in bpy.context.scene.objects
        if obj.type == "MESH"
        and len(obj.data.polygons) > 0
        and not obj.hide_render
    ]
    bounds = [entry for entry in bounds if entry is not None]
    if not bounds:
        raise ValueError("scene has no visible mesh geometry to preview")
    minimum = Vector(
        min(float(entry["minimum"][axis]) for entry in bounds)
        for axis in range(3)
    )
    maximum = Vector(
        max(float(entry["maximum"][axis]) for entry in bounds)
        for axis in range(3)
    )
    return minimum, maximum


def _rounded(values) -> list[float]:
    return [round(float(value), 4) for value in values]


def _configure_workbench(scene: bpy.types.Scene) -> None:
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = PREVIEW_WIDTH
    scene.render.resolution_y = PREVIEW_HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = True
    scene.display.shading.cavity_type = "WORLD"
    scene.display.shading.show_specular_highlight = False
    scene.display.shading.background_type = "VIEWPORT"
    scene.display.shading.background_color = (0.035, 0.045, 0.06)


def _render_view(
    scene: bpy.types.Scene,
    camera: bpy.types.Object,
    output: Path,
    center: Vector,
    direction: Vector,
    horizontal_size: float,
    vertical_size: float,
) -> None:
    camera.location = center - direction.normalized() * (
        max(horizontal_size, vertical_size) * 2.0 + 10.0
    )
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = max(
        vertical_size,
        horizontal_size * PREVIEW_HEIGHT / PREVIEW_WIDTH,
        1.0,
    ) * PREVIEW_MARGIN
    scene.render.filepath = str(output)
    bpy.ops.render.render(write_still=True)


def render_scene_previews(output_directory: Path) -> dict:
    output_directory.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    minimum, maximum = _scene_bounds()
    center = (minimum + maximum) * 0.5
    size = maximum - minimum
    diagonal = max(size.length, 1.0)

    original_camera = scene.camera
    camera_data = bpy.data.cameras.new("SK8_AgentPreviewCamera_Data")
    camera = bpy.data.objects.new("SK8_AgentPreviewCamera", camera_data)
    scene.collection.objects.link(camera)
    scene.camera = camera
    camera_data.clip_start = 0.01
    camera_data.clip_end = max(1000.0, diagonal * 8.0)

    _configure_workbench(scene)
    views = [
        {
            "name": "top",
            "filename": "scene_preview_top.png",
            "direction": Vector((0.0, 0.0, -1.0)),
            "horizontal_axis": "+X",
            "vertical_axis": "+Y",
            "horizontal_size": max(size.x, 1.0),
            "vertical_size": max(size.y, 1.0),
        },
        {
            "name": "south",
            "filename": "scene_preview_south.png",
            "direction": Vector((0.0, 1.0, 0.0)),
            "horizontal_axis": "+X",
            "vertical_axis": "+Z",
            "horizontal_size": max(size.x, 1.0),
            "vertical_size": max(size.z, 1.0),
        },
        {
            "name": "east",
            "filename": "scene_preview_east.png",
            "direction": Vector((-1.0, 0.0, 0.0)),
            "horizontal_axis": "+Y",
            "vertical_axis": "+Z",
            "horizontal_size": max(size.y, 1.0),
            "vertical_size": max(size.z, 1.0),
        },
        {
            "name": "isometric",
            "filename": "scene_preview_isometric.png",
            "direction": Vector((-1.0, 1.0, -0.72)),
            "horizontal_axis": "visual",
            "vertical_axis": "visual",
            "horizontal_size": max(size.x, size.y, 1.0) * math.sqrt(2.0),
            "vertical_size": max(size.z + max(size.x, size.y) * 0.5, 1.0),
        },
    ]

    try:
        for view in views:
            _render_view(
                scene,
                camera,
                output_directory / view["filename"],
                center,
                view["direction"],
                view["horizontal_size"],
                view["vertical_size"],
            )
    finally:
        scene.camera = original_camera
        bpy.data.objects.remove(camera, do_unlink=True)
        bpy.data.cameras.remove(camera_data)

    metadata = {
        "version": 1,
        "format": "sk8-agent-scene-previews",
        "resolution": [PREVIEW_WIDTH, PREVIEW_HEIGHT],
        "world_bounds": {
            "minimum": _rounded(minimum),
            "maximum": _rounded(maximum),
            "center": _rounded(center),
            "size": _rounded(size),
        },
        "views": [
            {
                key: value
                for key, value in view.items()
                if key not in {
                    "direction",
                    "horizontal_size",
                    "vertical_size",
                }
            }
            for view in views
        ],
    }
    metadata_path = output_directory / "scene_previews.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2) + "\n",
        encoding="utf-8",
    )
    return metadata
