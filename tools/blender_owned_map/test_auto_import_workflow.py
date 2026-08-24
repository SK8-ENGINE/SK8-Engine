"""Headless regression test for exporting an ordinary unconfigured .blend."""

from pathlib import Path
import sys
import tempfile

import bpy


TOOL_ROOT = Path(__file__).resolve().parent
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

import owned_world_material_addon as addon
from compare_skate import compare_packages


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
        texture_group = bpy.data.node_groups.new(
            "ImportedAlbedoNodeGroup", "ShaderNodeTree"
        )
        texture_group.interface.new_socket(
            name="Color",
            in_out="OUTPUT",
            socket_type="NodeSocketColor",
        )
        group_output = texture_group.nodes.new("NodeGroupOutput")
        texture = texture_group.nodes.new("ShaderNodeTexImage")
        texture.image = image
        texture_group.links.new(
            texture.outputs["Color"], group_output.inputs["Color"]
        )
        group_node = material.node_tree.nodes.new("ShaderNodeGroup")
        group_node.node_tree = texture_group
        material.node_tree.links.new(
            group_node.outputs["Color"], shader.inputs["Base Color"]
        )

        floor = make_plane("OrdinaryVisibleFloor", 0.0, material)
        floor.location = (1.25, -0.75, 0.5)
        floor.rotation_euler = (0.1, 0.2, 0.3)
        floor.scale = (1.2, 0.8, 1.1)
        solidify = floor.modifiers.new("ExportedSolidify", "SOLIDIFY")
        solidify.thickness = 0.2
        collider = make_plane("FloorCollider", 5.0)
        scale_reference = make_plane("character_size", 10.0)
        legacy_visual = bpy.data.collections.new(
            addon.exporter.LEGACY_VISUAL_COLLECTION
        )
        legacy_collision = bpy.data.collections.new(
            addon.exporter.LEGACY_COLLISION_COLLECTION
        )
        bpy.context.scene.collection.children.link(legacy_visual)
        bpy.context.scene.collection.children.link(legacy_collision)
        legacy_visual.objects.link(floor)
        legacy_collision.objects.link(floor)
        legacy_collision.objects.link(collider)

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
        spawn = bpy.data.objects[addon.exporter.SPAWN_OBJECT]
        require(spawn.type == "MESH", "Spawn locator is not a movable mesh")
        require(
            abs(spawn.dimensions.x - 4.0) < 1.0e-5
            and abs(spawn.dimensions.y - 4.0) < 1.0e-5,
            "Spawn locator is not a 4x4 metre pad",
        )
        require(
            tuple(spawn.location) == (0.0, 0.0, 0.0),
            "Spawn locator still followed the 3D cursor",
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

        default_group = bpy.data.collections.get(
            addon.exporter.PRESENTATION_COLLISION_COLLECTION
        )
        collider_group = bpy.data.collections.get(
            addon.exporter.NO_PRESENTATION_COLLECTION
        )
        no_collision_group = bpy.data.collections.get(
            addon.exporter.NO_COLLISION_COLLECTION
        )
        require(default_group is not None, "Automatic export missed Group 1")
        require(collider_group is not None, "Automatic export missed Group 2")
        require(no_collision_group is not None, "Automatic export missed Group 3")
        require(
            bpy.data.collections.get(
                addon.exporter.LEGACY_VISUAL_COLLECTION
            )
            is None
            and bpy.data.collections.get(
                addon.exporter.LEGACY_COLLISION_COLLECTION
            )
            is None,
            "Legacy role collections were not cleaned up after migration",
        )
        require(
            addon._object_groups(floor)
            == [addon.exporter.PRESENTATION_COLLISION_COLLECTION],
            "Ordinary mesh was not assigned exclusively to Group 1",
        )
        require(
            addon._object_groups(collider)
            == [addon.exporter.NO_PRESENTATION_COLLECTION],
            "Explicit collider was not assigned exclusively to Group 2",
        )
        require(
            not addon._object_groups(scale_reference),
            "Character scale helper was incorrectly exported as map geometry",
        )
        require(
            floor.name in default_group.all_objects
            and collider.name in collider_group.all_objects,
            "Five-group automatic classification is incomplete",
        )
        scene.owned_world.material_list_index = bpy.data.materials.find(
            material.name
        )
        require(
            floor.select_get()
            and bpy.context.view_layer.objects.active == floor
            and not collider.select_get(),
            "Clicking a material-list row did not select its scene meshes",
        )
        require(
            bpy.ops.skate_map.set_material_group(
                group=addon.exporter.NO_COLLISION_COLLECTION
            )
            == {"FINISHED"}
            and addon._object_groups(floor)
            == [addon.exporter.NO_COLLISION_COLLECTION],
            "Material-list Group 3 button did not move its mesh users",
        )
        require(
            bpy.ops.skate_map.set_material_group(
                group=addon.exporter.PRESENTATION_COLLISION_COLLECTION
            )
            == {"FINISHED"}
            and addon._object_groups(floor)
            == [addon.exporter.PRESENTATION_COLLISION_COLLECTION],
            "Material-list Group 1 button did not restore its mesh users",
        )
        no_collision_group.objects.link(floor)
        exclusive_issues, _warnings, _stats = addon._validate_scene(
            bpy.context, inspect_geometry=False
        )
        require(
            any(
                "must belong to exactly one map group" in issue
                for issue in exclusive_issues
            ),
            "Validation did not reject multiple group memberships",
        )
        addon._move_to_group(
            scene,
            floor,
            addon.exporter.PRESENTATION_COLLISION_COLLECTION,
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
        normal = bpy.data.images.get(
            str(material.get("ow_normal_image", ""))
        )
        orm = bpy.data.images.get(str(material.get("ow_orm_image", "")))
        require(
            normal is not None and orm is not None,
            "Auto Prepare did not generate normal and ORM maps",
        )
        require(
            normal.packed_file is not None
            and orm.packed_file is not None
            and normal.use_fake_user
            and orm.use_fake_user,
            "Generated material maps were not made persistent",
        )
        require(
            scene.owned_world.day_ambient == 0.0
            and scene.owned_world.night_ambient == 0.0,
            "Local lights did not disable default ambient light",
        )
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_mesh, evaluated_object = addon.exporter._mesh_for_export(
            floor, depsgraph, preserve_all_data_layers=True
        )
        try:
            require(
                len(evaluated_mesh.polygons) > len(floor.data.polygons),
                "Build did not evaluate the Solidify modifier",
            )
        finally:
            if evaluated_object is not None:
                evaluated_object.to_mesh_clear()

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
        material["ow_albedo_image"] = ""
        require(
            addon._auto_configure_material(
                material, generate_maps=True
            )
            and material.get("ow_albedo_image") == image.name
            and abs(float(material.get("ow_roughness", 0.0)) - 0.51)
            < 1.0e-5
            and not bool(material.get("ow_auto_imported", True)),
            "Auto Prepare did not fill a missing authored albedo while "
            "preserving manual scalar settings",
        )

        # Older add-on builds saved generated images as transient datablocks.
        # On reload Blender retained their names and metadata but restored
        # black pixels. Authored materials preserve deliberate source maps,
        # but add-on-owned maps must still be regenerated and packed.
        generated_names = (
            str(material["ow_normal_image"]),
            str(material["ow_orm_image"]),
        )
        for name, kind, size in zip(
            generated_names,
            ("NORMAL", "ORM"),
            (32, 4),
            strict=True,
        ):
            previous = bpy.data.images.get(name)
            require(previous is not None, f"Missing generated {kind} setup")
            bpy.data.images.remove(previous)
            broken = bpy.data.images.new(
                name, width=size, height=size, alpha=True
            )
            broken["ow_generated_map"] = kind
            broken["ow_generated_for"] = material.name
            broken.use_fake_user = True
        _stats, repair_warnings = addon._auto_prepare_scene(bpy.context)
        require(not repair_warnings, "Legacy generated-map repair warned")
        repaired_normal = bpy.data.images[generated_names[0]]
        repaired_orm = bpy.data.images[generated_names[1]]
        repaired_normal_pixels = list(repaired_normal.pixels[:])
        repaired_orm_pixels = list(repaired_orm.pixels[:])
        require(
            any(
                repaired_normal_pixels[index]
                or repaired_normal_pixels[index + 1]
                or repaired_normal_pixels[index + 2]
                for index in range(0, len(repaired_normal_pixels), 4)
            )
            and any(
                repaired_orm_pixels[index]
                or repaired_orm_pixels[index + 1]
                or repaired_orm_pixels[index + 2]
                for index in range(0, len(repaired_orm_pixels), 4)
            )
            and repaired_normal.packed_file is not None
            and repaired_orm.packed_file is not None,
            "Auto Prepare did not repair black legacy generated maps",
        )
        require(
            abs(float(material.get("ow_roughness", 0.0)) - 0.51) < 1.0e-5
            and not bool(material.get("ow_auto_imported", True)),
            "Generated-map repair overwrote authored material settings",
        )

        shader.inputs["Roughness"].default_value = 0.41
        bpy.ops.object.select_all(action="DESELECT")
        floor.select_set(True)
        bpy.context.view_layer.objects.active = floor
        require(
            bpy.ops.owmaterial.apply_preset(preset="TREE") == {"FINISHED"},
            "Tree material preset failed",
        )
        require(
            material.get("ow_presentation_type") == "VEGETATION"
            and int(material.get("ow_alpha_mode", 0)) == 1
            and not bool(material.get("ow_collision_enabled", True)),
            "Tree preset did not configure optimized cutout presentation",
        )
        require(
            bpy.ops.owmaterial.sync_materials() == {"FINISHED"},
            "Sync Materials operator failed",
        )
        require(
            abs(float(material.get("ow_roughness", 0.0)) - 0.41) < 1.0e-5,
            "Sync Materials did not refresh shader roughness",
        )
        orm = bpy.data.images[str(material["ow_orm_image"])]
        orm_pixels = [0.0] * (orm.size[0] * orm.size[1] * 4)
        orm.pixels.foreach_get(orm_pixels)
        require(
            abs(orm_pixels[1] - 0.41) < 0.01,
            "Sync Materials did not overwrite generated roughness data",
        )

        sharp_mesh = bpy.data.meshes.new("SharpRailMesh")
        sharp_mesh.from_pydata(
            [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (2.0, 0.0, 0.0)],
            [(0, 1), (1, 2)],
            [],
        )
        sharp_mesh.update()
        sharp_attribute = sharp_mesh.attributes.new(
            "sharp_edge", "BOOLEAN", "EDGE"
        )
        for value in sharp_attribute.data:
            value.value = True
        sharp_object = bpy.data.objects.new("SharpRail", sharp_mesh)
        scene.collection.objects.link(sharp_object)
        bpy.ops.object.select_all(action="DESELECT")
        sharp_object.select_set(True)
        bpy.context.view_layer.objects.active = sharp_object
        require(
            bpy.ops.skate_map.grinds_from_sharp_edges() == {"FINISHED"},
            "Sharp-edge grind generation failed",
        )
        grind_object = bpy.context.object
        require(
            grind_object.type == "CURVE"
            and len(grind_object.data.splines) == 1
            and len(grind_object.data.splines[0].points) == 3,
            "Sharp edges were not consolidated into one three-point spline",
        )
        require(
            addon._object_groups(grind_object)
            == [addon.exporter.GRIND_COLLECTION],
            "Generated grind spline was not placed exclusively in Group 4",
        )
        grind_data = grind_object.data
        bpy.data.objects.remove(grind_object, do_unlink=True)
        bpy.data.curves.remove(grind_data)
        bpy.data.objects.remove(sharp_object, do_unlink=True)
        bpy.data.meshes.remove(sharp_mesh)
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
        _, _, scalar_counts = addon.exporter._read_package_header(
            scalar_output
        )
        require(
            scalar_counts == counts,
            "Bulk and scalar geometry paths produced different counts",
        )
        semantic_failures, _, _, _ = compare_packages(
            scalar_output, output
        )
        require(
            not semantic_failures,
            "Bulk and scalar geometry paths produced different decoded "
            f"content: {'; '.join(semantic_failures)}",
        )

        persistence_blend = Path(tempfile.gettempdir()) / (
            "auto_import_generated_map_persistence.blend"
        )
        persistence_blend.unlink(missing_ok=True)
        persisted_material_name = material.name
        require(
            bpy.ops.wm.save_as_mainfile(filepath=str(persistence_blend))
            == {"FINISHED"},
            "Could not save generated-map persistence fixture",
        )
        require(
            bpy.ops.wm.open_mainfile(
                filepath=str(persistence_blend), load_ui=False
            )
            == {"FINISHED"},
            "Could not reopen generated-map persistence fixture",
        )
        reloaded_material = bpy.data.materials[persisted_material_name]
        reloaded_normal = bpy.data.images[
            str(reloaded_material["ow_normal_image"])
        ]
        reloaded_orm = bpy.data.images[
            str(reloaded_material["ow_orm_image"])
        ]
        for reloaded, label in (
            (reloaded_normal, "normal"),
            (reloaded_orm, "ORM"),
        ):
            pixels = list(reloaded.pixels[:])
            require(
                reloaded.packed_file is not None
                and any(
                    pixels[index]
                    or pixels[index + 1]
                    or pixels[index + 2]
                    for index in range(0, len(pixels), 4)
                ),
                f"Generated {label} map became black after save/reopen",
            )
        reloaded_scene = bpy.context.scene
        require(
            bpy.ops.skate_map.quick_export() == {"FINISHED"},
            reloaded_scene.owned_world.validation_details
            or "Export failed after generated-map save/reopen",
        )
        print(
            "AUTO_IMPORT_WORKFLOW_OK",
            output,
            output.stat().st_size,
            reloaded_scene.owned_world.last_status,
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
