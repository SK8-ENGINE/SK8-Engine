"""Headless smoke test for the complete Blender addon workflow."""

from pathlib import Path
import math
import sys
import tempfile

import bpy


TOOL_ROOT = Path(__file__).resolve().parent
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

import owned_world_material_addon as addon


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def collision_object(
    name: str,
    vertices: list[tuple[float, float, float]],
    faces: list[tuple[int, int, int]],
    material_name: str,
) -> bpy.types.Object:
    mesh = bpy.data.meshes.new(f"{name}Mesh")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.data.collections[addon.exporter.COLLISION_COLLECTION].objects.link(obj)
    obj["ow_material"] = material_name
    return obj


def main() -> None:
    addon.register()
    try:
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        for collection in list(bpy.data.collections):
            bpy.data.collections.remove(collection)

        require(
            bpy.ops.skate_map.prepare_scene() == {"FINISHED"},
            "Prepare Scene operator failed",
        )

        bpy.ops.mesh.primitive_plane_add(size=20.0, location=(0.0, 0.0, 0.0))
        floor = bpy.context.object
        floor.name = "TestFloor"
        material = bpy.data.materials.new("TestConcrete")
        floor.data.materials.append(material)
        material.owned_world.roughness = 0.67

        require(
            bpy.ops.skate_map.assign_selected(role="VISUAL") == {"FINISHED"},
            "Visual assignment failed",
        )
        require(
            bpy.ops.skate_map.assign_selected(role="COLLISION") == {"FINISHED"},
            "Collision assignment failed",
        )
        floor["ow_upward_surface"] = True
        require(
            bpy.ops.skate_map.create_uv_layers() == {"FINISHED"},
            "UV layer helper failed",
        )

        # Old add-on versions could leave imported scale-reference meshes in
        # both export collections. Existing scenes must ignore them without
        # requiring the author to repair collection membership manually.
        helper_mesh = bpy.data.meshes.new("character_size_mesh")
        helper_mesh.from_pydata(
            [(0.0, 0.0, 0.0), (0.0, 0.0, 2.0), (0.25, 0.0, 0.0)],
            [],
            [(0, 1, 2)],
        )
        helper_mesh.update()
        helper = bpy.data.objects.new("character_size", helper_mesh)
        bpy.data.collections[
            addon.exporter.VISUAL_COLLECTION
        ].objects.link(helper)
        bpy.data.collections[
            addon.exporter.COLLISION_COLLECTION
        ].objects.link(helper)

        cleanup_proxy = collision_object(
            "CollisionCleanupRegression",
            [
                (30.0, 0.0, 0.0),
                (31.0, 0.0, 0.0),
                (30.0, 1.0, 0.0),
                # A separate vertex set with identical positions verifies
                # position-based duplicate cleanup.
                (30.0, 0.0, 0.0),
                (31.0, 0.0, 0.0),
                (30.0, 1.0, 0.0),
                # Collinear source geometry must be skipped without editing
                # this mesh or any visual UVs.
                (35.0, 0.0, 0.0),
                (36.0, 0.0, 0.0),
                (37.0, 0.0, 0.0),
            ],
            [(0, 1, 2), (3, 4, 5), (6, 7, 8)],
            material.name,
        )

        downward_a = collision_object(
            "WrongFacingA",
            [(40.0, 0.0, 0.0), (40.0, 1.0, 0.0), (41.0, 0.0, 0.0)],
            [(0, 1, 2)],
            material.name,
        )
        downward_b = collision_object(
            "WrongFacingB",
            [(45.0, 0.0, 0.0), (45.0, 1.0, 0.0), (46.0, 0.0, 0.0)],
            [(0, 1, 2)],
            material.name,
        )
        downward_a["ow_upward_surface"] = True
        downward_b["ow_upward_surface"] = True
        _triangles, wrong_facing_audit = (
            addon.exporter.audit_collision_geometry(
                [downward_a, downward_b]
            )
        )
        require(
            sum(
                "face downward or vertically" in issue
                for issue in wrong_facing_audit.issues
            )
            == 2,
            "Collision audit did not report every bad mesh in one pass",
        )
        for obj in (downward_a, downward_b):
            mesh = obj.data
            bpy.data.objects.remove(obj, do_unlink=True)
            bpy.data.meshes.remove(mesh)

        scene = bpy.context.scene
        point_data = bpy.data.lights.new("TestPointData", type="POINT")
        point_data.color = (0.15, 0.45, 1.0)
        point_data.energy = 1200.0
        point_data.cutoff_distance = 12.0
        point = bpy.data.objects.new("TestPoint", point_data)
        scene.collection.objects.link(point)
        point.location = (-2.0, 0.0, 3.0)

        spot_data = bpy.data.lights.new("TestSpotData", type="SPOT")
        spot_data.color = (1.0, 0.18, 0.08)
        spot_data.energy = 1600.0
        spot_data.cutoff_distance = 16.0
        spot_data.spot_size = 0.9
        spot_data.spot_blend = 0.35
        spot = bpy.data.objects.new("TestSpot", spot_data)
        scene.collection.objects.link(spot)
        spot.location = (2.0, -2.0, 4.0)

        area_data = bpy.data.lights.new("TestAreaData", type="AREA")
        area_data.color = (0.35, 1.0, 0.25)
        area_data.energy = 900.0
        area_data.cutoff_distance = 10.0
        area_data.shape = "RECTANGLE"
        area_data.size = 2.0
        area_data.size_y = 1.0
        area = bpy.data.objects.new("TestArea", area_data)
        scene.collection.objects.link(area)
        area.location = (0.0, 3.0, 3.0)
        area.rotation_euler = (0.35, 0.0, math.pi)

        sun_data = bpy.data.lights.new("TestSunData", type="SUN")
        sun_data.color = (1.0, 0.72, 0.42)
        sun_data.energy = 2.5
        sun = bpy.data.objects.new("TestSun", sun_data)
        scene.collection.objects.link(sun)
        sun.rotation_euler = (0.7, 0.0, -0.5)

        scene.cursor.location = (0.0, 0.0, 1.0)
        require(
            bpy.ops.skate_map.set_spawn() == {"FINISHED"},
            "Spawn helper failed",
        )

        output = Path(tempfile.gettempdir()) / "addon_workflow_test.skate"
        cache = output.with_name(output.name + ".export-cache.json")
        for path in (output, cache):
            path.unlink(missing_ok=True)

        settings = scene.owned_world
        settings.map_name = "Addon Workflow Test"
        settings.output_path = str(output)
        settings.cycle_seconds = 30.0
        settings.cycle_ping_pong = True
        settings.start_hour = 8.0
        settings.end_hour = 18.0

        # AI routes are experimental and optional. Their collection must not
        # be required for validation or export.
        npc_collection = bpy.data.collections.get(
            addon.exporter.NPC_PATH_COLLECTION
        )
        require(npc_collection is not None, "Prepare Scene missed NPC paths")
        bpy.data.collections.remove(npc_collection)
        grind_collection = bpy.data.collections.get(
            addon.exporter.GRIND_COLLECTION
        )
        require(grind_collection is not None, "Prepare Scene missed grinds")
        bpy.data.collections.remove(grind_collection)

        require(
            bpy.ops.skate_map.validate() == {"FINISHED"},
            settings.validation_details or "Validation failed",
        )
        require(
            "zero-area collision triangle" in settings.validation_details
            and "duplicate collision triangle" in settings.validation_details,
            "Validation did not explain automatic collision cleanup",
        )
        require(
            "OW_NPC_PATHS" not in settings.validation_details,
            "Optional NPC paths were reported as an export problem",
        )
        require(
            "OW_GRIND" not in settings.validation_details,
            "Optional grind collection was reported as an export problem",
        )
        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            settings.last_status,
        )
        require(output.is_file(), "Quick Export did not create an SKATE")
        require(cache.is_file(), "Quick Export did not create its cache")
        require(
            output.read_bytes()[:8] == b"SKATE08\0",
            "Exported package has the wrong magic",
        )
        _, _, counts = addon.exporter._read_package_header(output)
        collision_triangle_count = counts[4]
        local_light_count = counts[-2]
        npc_route_count = counts[-1]
        require(
            collision_triangle_count == 3,
            "Degenerate or duplicate collision reached the package "
            f"(got {collision_triangle_count}, expected 3)",
        )
        require(
            local_light_count == 3,
            "Point, Spot, and Area lights were not exported",
        )
        require(npc_route_count == 0, "Unexpected NPC routes were exported")
        require(
            abs(float(material.get("ow_roughness", 0.0)) - 0.67) < 1.0e-5,
            "Material panel did not synchronize exporter metadata",
        )
        require(
            settings.last_status.startswith("Exported successfully"),
            "Friendly export result was not retained",
        )
        require(
            "No mesh dissolve or UV changes are required"
            in settings.validation_details,
            "Quick Export did not retain collision cleanup guidance",
        )
        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            "Incremental export failed after collision sanitation",
        )
        _, _, cached_counts = addon.exporter._read_package_header(output)
        require(
            cached_counts == counts,
            "Incremental cache changed sanitized package counts",
        )
        blend_path = output.with_suffix(".blend")
        bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
        require(blend_path.is_file(), "Workflow test scene was not saved")
        print(
            "ADDON_WORKFLOW_OK",
            output,
            output.stat().st_size,
            settings.last_status,
        )
    finally:
        addon.unregister()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback

        traceback.print_exc()
        sys.exit(1)
