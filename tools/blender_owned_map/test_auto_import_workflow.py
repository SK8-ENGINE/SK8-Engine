"""Headless regression test for exporting an ordinary unconfigured .blend."""

from pathlib import Path
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


def make_plane(
    name: str,
    x_offset: float,
    material: bpy.types.Material | None = None,
) -> bpy.types.Object:
    mesh = bpy.data.meshes.new(f"{name}Mesh")
    mesh.from_pydata(
        [
            (x_offset - 1.0, -1.0, 0.0),
            (x_offset + 1.0, -1.0, 0.0),
            (x_offset + 1.0, 1.0, 0.0),
            (x_offset - 1.0, 1.0, 0.0),
        ],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()
    if material is not None:
        mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main() -> None:
    addon.register()
    try:
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        for collection in list(bpy.data.collections):
            bpy.data.collections.remove(collection)
        for material in list(bpy.data.materials):
            bpy.data.materials.remove(material)

        image = bpy.data.images.new(
            "AutoImportAlbedo", width=2, height=2, alpha=True
        )
        image.pixels = [
            0.8, 0.2, 0.1, 1.0,
            0.8, 0.2, 0.1, 1.0,
            0.8, 0.2, 0.1, 1.0,
            0.8, 0.2, 0.1, 1.0,
        ]
        image.pack()
        require(
            image.packed_file is not None,
            "Packed-image cache regression setup failed",
        )

        material = bpy.data.materials.new("Concrete_AutoImport")
        material.use_nodes = True
        shader = next(
            node
            for node in material.node_tree.nodes
            if node.type == "BSDF_PRINCIPLED"
        )
        shader.inputs["Roughness"].default_value = 0.63
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        material.node_tree.links.new(
            texture.outputs["Color"], shader.inputs["Base Color"]
        )

        floor = make_plane("OrdinaryVisibleFloor", 0.0, material)
        floor.location = (1.25, -0.75, 0.5)
        floor.rotation_euler = (0.1, 0.2, 0.3)
        floor.scale = (1.2, 0.8, 1.1)
        collider = make_plane("FloorCollider", 5.0)
        scale_reference = make_plane("character_size", 10.0)

        fake_light = bpy.data.objects.new("Point Light Imported Empty", None)
        bpy.context.scene.collection.objects.link(fake_light)

        light_data = bpy.data.lights.new("RealPointData", type="POINT")
        light_data.energy = 800.0
        real_light = bpy.data.objects.new("RealPoint", light_data)
        bpy.context.scene.collection.objects.link(real_light)
        real_light.location = (0.0, 0.0, 3.0)

        scene = bpy.context.scene
        scene.cursor.location = (0.0, 0.0, 1.0)
        require(
            bpy.ops.skate_map.set_spawn() == {"FINISHED"},
            "Spawn helper failed before automatic scene preparation",
        )

        output = Path(tempfile.gettempdir()) / "auto_import_workflow.skate"
        cache = output.with_name(output.name + ".export-cache.json")
        for path in (output, cache):
            path.unlink(missing_ok=True)
        scene.owned_world.map_name = "Automatic Import Test"
        scene.owned_world.output_path = str(output)

        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            scene.owned_world.validation_details
            or scene.owned_world.last_status,
        )
        require(output.is_file(), "Automatic export did not create a package")

        visual = bpy.data.collections.get(
            addon.exporter.VISUAL_COLLECTION
        )
        collision = bpy.data.collections.get(
            addon.exporter.COLLISION_COLLECTION
        )
        require(visual is not None, "Automatic export missed OW_VISUAL")
        require(collision is not None, "Automatic export missed OW_COLLISION")
        require(
            floor.name in visual.all_objects,
            "Ordinary visible mesh was not adopted",
        )
        require(
            collider.name not in visual.all_objects,
            "Explicit collider was incorrectly rendered",
        )
        require(
            scale_reference.name not in visual.all_objects
            and scale_reference.name not in collision.all_objects,
            "Character scale helper was incorrectly exported as map geometry",
        )
        require(
            floor.name in collision.all_objects
            and collider.name in collision.all_objects,
            "Automatic collision did not cover visuals and explicit proxies",
        )
        require(
            floor.data.uv_layers.get("UVMap") is not None
            and floor.data.uv_layers.get("Lightmap") is not None,
            "Automatic export did not create required UV layers",
        )
        require(
            material.get("ow_albedo_image") == image.name,
            "Principled Base Color image was not imported",
        )
        require(
            abs(float(material.get("ow_roughness", 0.0)) - 0.63) < 1.0e-5,
            "Principled roughness was not imported",
        )
        require(
            bool(material.get("ow_auto_imported", False)),
            "Automatically imported material was not marked refreshable",
        )

        # Older add-on builds could save an auto-imported material before its
        # Blender image links were available, leaving an empty albedo field
        # that every later export treated as authoritative. Automatic
        # materials must heal from their shader graph on the next export.
        material["ow_albedo_image"] = ""
        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            "Automatic export did not refresh stale material metadata",
        )
        require(
            material.get("ow_albedo_image") == image.name,
            "Stale auto-imported albedo metadata was not healed",
        )

        # A real UI edit transfers ownership to the author, preventing future
        # automatic preparation from overwriting deliberate choices.
        material.owned_world.roughness = 0.51
        require(
            not bool(material.get("ow_auto_imported", True)),
            "Manual material edit did not disable automatic overwrites",
        )
        require(
            not addon._auto_configure_material(material),
            "Automatic preparation overwrote a manually edited material",
        )
        require(
            abs(float(material.get("ow_roughness", 0.0)) - 0.51) < 1.0e-5,
            "Manual material value was not preserved",
        )
        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            "Automatic export failed after a manual material edit",
        )
        require(
            collider.get("ow_material") == material.name,
            "Collider-only mesh did not receive a visible fallback material",
        )

        _, _, counts = addon.exporter._read_package_header(output)
        require(
            counts[-2] == 1,
            "Only the real Blender Light should have been exported",
        )

        scalar_output = output.with_name(
            "auto_import_workflow_scalar.skate"
        )
        scalar_output.unlink(missing_ok=True)
        original_numpy = addon.exporter.numpy
        addon.exporter.numpy = None
        try:
            addon.exporter.export_scene(
                scalar_output, force_rebuild=True
            )
        finally:
            addon.exporter.numpy = original_numpy
        require(
            output.read_bytes() == scalar_output.read_bytes(),
            "Bulk geometry packing changed the SKATE package payload",
        )
        print(
            "AUTO_IMPORT_WORKFLOW_OK",
            output,
            output.stat().st_size,
            scene.owned_world.last_status,
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
