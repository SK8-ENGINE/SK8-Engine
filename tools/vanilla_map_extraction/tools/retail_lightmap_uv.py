"""Decode the secondary Xenos texture coordinate used by retail lightmaps."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Protocol

import numpy


class VertexAttribute(Protocol):
    offset: int
    descriptor: bytes


@dataclass(frozen=True)
class LightmapUvDecode:
    values: numpy.ndarray
    format_code: int
    offset: int
    usage_index: int


def _secondary_texcoord(
    attributes: Iterable[VertexAttribute],
) -> tuple[VertexAttribute, int] | None:
    texcoords: list[tuple[int, int, VertexAttribute]] = []
    for declaration_index, attribute in enumerate(attributes):
        descriptor = bytes(attribute.descriptor)
        if len(descriptor) != 16:
            raise ValueError("Xenos vertex descriptors must contain 16 bytes")
        # D3DDECLUSAGE is byte 9 and the usage index is byte 10.
        if descriptor[9] == 5:
            texcoords.append(
                (int(descriptor[10]), declaration_index, attribute)
            )
    if len(texcoords) < 2:
        return None
    texcoords.sort(key=lambda item: (item[0], item[1]))
    usage_index, _declaration_index, attribute = texcoords[1]
    return attribute, usage_index


def decode_lightmap_uvs(
    data: bytes,
    *,
    vertex_buffer_offset: int,
    vertex_count: int,
    vertex_stride: int,
    attributes: Iterable[VertexAttribute],
) -> LightmapUvDecode | None:
    """Decode the second TEXCOORD without changing its retail sign bits.

    Static world shaders use ``abs(uv)`` because the signs also carry tangent
    frame handedness. Keeping the signed values here lets the Blender importer
    apply the correct shader-family policy rather than destroying provenance
    during extraction.
    """

    selected = _secondary_texcoord(attributes)
    if selected is None:
        return None
    attribute, usage_index = selected
    descriptor = bytes(attribute.descriptor)
    format_code = int.from_bytes(descriptor[4:8], "big")
    xenos_format = format_code & 0x3F
    offset = int(attribute.offset)
    if vertex_count < 0 or vertex_stride <= 0 or offset < 0:
        raise ValueError("invalid retail vertex-buffer dimensions")

    if xenos_format in (25, 26):
        component_bytes = 2
        dtype = numpy.dtype(">i2")
    elif xenos_format in (31, 32):
        component_bytes = 2
        dtype = numpy.dtype(">f2")
    elif xenos_format in (37, 38):
        component_bytes = 4
        dtype = numpy.dtype(">f4")
    else:
        raise ValueError(
            f"unsupported lightmap UV Xenos format 0x{format_code:08X}"
        )

    required_end = (
        vertex_buffer_offset
        + max(0, vertex_count - 1) * vertex_stride
        + offset
        + component_bytes * 2
    )
    if required_end > len(data):
        raise ValueError(
            "retail lightmap UV data extends past the RX2 vertex buffer"
        )
    if vertex_count == 0:
        values = numpy.empty((0, 2), dtype=numpy.float32)
    else:
        values = numpy.ndarray(
            shape=(vertex_count, 2),
            dtype=dtype,
            buffer=data,
            offset=vertex_buffer_offset + offset,
            strides=(vertex_stride, component_bytes),
        ).astype(numpy.float32, copy=True)
    if xenos_format in (25, 26):
        values /= numpy.float32(32767.0)
    if not numpy.isfinite(values).all():
        raise ValueError("retail lightmap UVs contain non-finite values")
    return LightmapUvDecode(
        values=numpy.ascontiguousarray(values, dtype=numpy.float32),
        format_code=format_code,
        offset=offset,
        usage_index=usage_index,
    )
