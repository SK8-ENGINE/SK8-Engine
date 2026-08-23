from __future__ import annotations

import struct
import sys
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from retail_collision_mesh import (
    decode_clustered_mesh,
    decode_rx2_clustered_meshes,
)


def _mesh_with_cluster(cluster: bytes, triangle_count: int) -> bytes:
    mesh = bytearray(148 + len(cluster))
    struct.pack_into(">3f", mesh, 0, -4.0, -5.0, -6.0)
    struct.pack_into(">3f", mesh, 16, 7.0, 8.0, 9.0)
    struct.pack_into(">I", mesh, 40, triangle_count)
    struct.pack_into(">I", mesh, 48, 96)
    struct.pack_into(">I", mesh, 52, 144)
    struct.pack_into(">f", mesh, 56, 0.001)
    struct.pack_into(">H", mesh, 60, 0x10)
    mesh[62] = 1
    mesh[63] = 2
    struct.pack_into(">I", mesh, 64, 1)
    struct.pack_into(">I", mesh, 80, len(mesh))
    struct.pack_into(">I", mesh, 144, 148)
    mesh[148:] = cluster
    return bytes(mesh)


def _rx2_with_mesh(mesh: bytes) -> bytes:
    section_offset = 0x80
    data = bytearray(section_offset + len(mesh))
    data[:7] = b"\x89RW4xb2"
    struct.pack_into(">I", data, 0x20, 1)
    struct.pack_into(">I", data, 0x30, 0x40)
    struct.pack_into(
        ">6I",
        data,
        0x40,
        section_offset,
        0,
        len(mesh),
        16,
        0,
        0x00080006,
    )
    data[section_offset:] = mesh
    return bytes(data)


class RetailCollisionMeshTests(unittest.TestCase):
    def assert_vec3_almost_equal(
        self,
        actual: tuple[float, float, float],
        expected: tuple[float, float, float],
    ) -> None:
        for actual_component, expected_component in zip(actual, expected):
            self.assertAlmostEqual(actual_component, expected_component, places=6)

    def test_decodes_16_bit_triangle_and_surface(self) -> None:
        unit = bytes((0xA1, 0, 1, 2, 0, 0, 0, 0x12, 0x83))
        cluster_size = 48 + len(unit)
        cluster = bytearray(cluster_size)
        struct.pack_into(">5H", cluster, 0, 1, len(unit), 2, 1, cluster_size)
        cluster[10] = 3
        cluster[12] = 1
        struct.pack_into(">3i", cluster, 16, 1000, 2000, -3000)
        struct.pack_into(">9h", cluster, 28, 0, 0, 0, 1000, 0, 0, 0, 1000, 0)
        cluster[48:] = unit
        decoded = decode_clustered_mesh(_mesh_with_cluster(cluster, 1))
        self.assertEqual(decoded.cluster_count, 1)
        self.assertEqual(decoded.vertex_count, 3)
        self.assertEqual(decoded.triangle_cluster_indices, (0,))
        self.assertEqual(decoded.compression_counts, ((1, 1),))
        self.assertEqual(decoded.mesh_flags, 0x10)
        self.assertEqual(decoded.group_id_width, 1)
        self.assertEqual(decoded.surface_id_width, 2)
        self.assertEqual(decoded.triangles[0].surface, 0x8312)
        self.assertEqual(decoded.triangles[0].edge_codes, (0, 0, 0))
        self.assertIsNone(decoded.triangles[0].group_id)
        self.assertEqual(decoded.triangles[0].unit_flags, 0xA1)
        self.assert_vec3_almost_equal(
            decoded.triangles[0].a, (1.0, 2.0, -3.0)
        )
        self.assert_vec3_almost_equal(
            decoded.triangles[0].b, (2.0, 2.0, -3.0)
        )
        self.assert_vec3_almost_equal(
            decoded.triangles[0].c, (1.0, 3.0, -3.0)
        )

    def test_decodes_32_bit_quad_with_native_split(self) -> None:
        unit = bytes(
            (0x82, 0, 1, 2, 3, 0x01, 0x81)
        )
        cluster_size = 64 + len(unit)
        cluster = bytearray(cluster_size)
        struct.pack_into(">5H", cluster, 0, 1, len(unit), 3, 2, cluster_size)
        cluster[10] = 4
        cluster[12] = 2
        struct.pack_into(
            ">12i",
            cluster,
            16,
            0,
            0,
            0,
            1000,
            0,
            0,
            1000,
            1000,
            0,
            0,
            1000,
            0,
        )
        cluster[64:] = unit
        decoded = decode_clustered_mesh(_mesh_with_cluster(cluster, 2))
        self.assertEqual(decoded.compression_counts, ((2, 1),))
        self.assertEqual(len(decoded.triangles), 2)
        self.assertEqual(decoded.triangle_cluster_indices, (0, 0))
        self.assertEqual(decoded.triangles[0].surface, 0x8101)
        self.assertIsNone(decoded.triangles[0].edge_codes)
        self.assertEqual(
            (decoded.triangles[0].a, decoded.triangles[0].b, decoded.triangles[0].c),
            ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0)),
        )
        self.assertEqual(
            (decoded.triangles[1].a, decoded.triangles[1].b, decoded.triangles[1].c),
            ((0.0, 1.0, 0.0), (1.0, 1.0, 0.0), (1.0, 0.0, 0.0)),
        )

    def test_finds_clustered_mesh_in_rx2_table(self) -> None:
        unit = bytes((0x81, 0, 1, 2, 0x83, 0x00))
        cluster = bytearray(48 + len(unit))
        struct.pack_into(">5H", cluster, 0, 1, len(unit), 2, 1, len(cluster))
        cluster[10] = 3
        cluster[12] = 1
        struct.pack_into(">3i", cluster, 16, 0, 0, 0)
        struct.pack_into(">9h", cluster, 28, 0, 0, 0, 1000, 0, 0, 0, 1000, 0)
        cluster[48:] = unit
        meshes = decode_rx2_clustered_meshes(
            _rx2_with_mesh(_mesh_with_cluster(cluster, 1))
        )
        self.assertEqual(len(meshes), 1)
        self.assertEqual(meshes[0].triangles[0].surface, 0x0083)

    def test_rejects_cluster_table_outside_mesh(self) -> None:
        mesh = bytearray(_mesh_with_cluster(bytes(16), 0))
        struct.pack_into(">I", mesh, 52, len(mesh) + 4)
        with self.assertRaisesRegex(ValueError, "cluster table"):
            decode_clustered_mesh(bytes(mesh))


if __name__ == "__main__":
    unittest.main()
