"""Decode retail Pegasus tSplineData grind paths from Skate 3 RX2 assets."""

from __future__ import annotations

import math
import struct


RX2_TOC_RECORD_SIZE = 24
RX2_TYPE_SPLINE_DATA = 0x00EB0004
SPLINE_HEADER_SIZE = 16
SPLINE_RAIL_SIZE = 32
SPLINE_SEGMENT_SIZE = 144
SPLINE_SEGMENT_PAYLOAD_SIZE = 120
SPLINE_CONTINUITY_EPSILON = 1.0e-3


def _vec3(data: bytes, offset: int) -> tuple[float, float, float]:
    return struct.unpack_from(">3f", data, offset)


def _add(
    *values: tuple[float, float, float],
) -> tuple[float, float, float]:
    return tuple(sum(value[axis] for value in values) for axis in range(3))


def _distance(
    left: tuple[float, float, float],
    right: tuple[float, float, float],
) -> float:
    return math.sqrt(
        sum((left[axis] - right[axis]) ** 2 for axis in range(3))
    )


def decode_grind_splines(data: bytes) -> list[dict[str, object]]:
    """Return every native grind spline in one simulation RX2 resource.

    Retail spline segments are cubic polynomials:

    ``position(t) = D + C*t + B*t^2 + A*t^3``

    The first 120 bytes of each 144-byte segment contain the four coefficient
    vectors plus retail auxiliary data. The final 24 bytes are relocated
    runtime links and are deliberately not copied into the portable manifest.
    """

    if len(data) < 0x34 or data[:7] != b"\x89RW4xb2":
        raise ValueError("asset is not an Xbox 360 RW4 RX2 resource")
    file_count = struct.unpack_from(">I", data, 0x20)[0]
    file_table = struct.unpack_from(">I", data, 0x30)[0]
    if file_table + file_count * RX2_TOC_RECORD_SIZE > len(data):
        raise ValueError("RX2 section table extends beyond the file")

    result: list[dict[str, object]] = []
    for record_index in range(file_count):
        record_offset = file_table + record_index * RX2_TOC_RECORD_SIZE
        section_offset, _, section_size, _, _ = struct.unpack_from(
            ">5I",
            data,
            record_offset,
        )
        section_type = struct.unpack_from(">I", data, record_offset + 20)[0]
        if section_type != RX2_TYPE_SPLINE_DATA:
            continue
        section_end = section_offset + section_size
        if (
            section_offset + SPLINE_HEADER_SIZE > len(data)
            or section_end > len(data)
        ):
            raise ValueError("RX2 spline section extends beyond the file")

        rail_count, segment_count, rail_table, segment_table = (
            struct.unpack_from(">4I", data, section_offset)
        )
        expected_segment_table = (
            SPLINE_HEADER_SIZE + rail_count * SPLINE_RAIL_SIZE
        )
        expected_size = (
            expected_segment_table + segment_count * SPLINE_SEGMENT_SIZE
        )
        if (
            rail_table != SPLINE_HEADER_SIZE
            or segment_table != expected_segment_table
            or section_size != expected_size
        ):
            raise ValueError(
                "retail tSplineData has an inconsistent table layout"
            )

        covered_segments: set[int] = set()
        for rail_index in range(rail_count):
            rail_offset = section_offset + rail_table + (
                rail_index * SPLINE_RAIL_SIZE
            )
            (
                spline_id,
                type_signature,
                flags,
                first_segment,
                last_segment,
                trailing_word,
            ) = struct.unpack_from(">QQ4I", data, rail_offset)
            if (
                first_segment < segment_table
                or last_segment < first_segment
                or (first_segment - segment_table) % SPLINE_SEGMENT_SIZE
                or (last_segment - first_segment) % SPLINE_SEGMENT_SIZE
                or last_segment + SPLINE_SEGMENT_SIZE > section_size
            ):
                raise ValueError(
                    f"retail grind rail {rail_index} has invalid segment links"
                )

            payloads: list[str] = []
            first_start: tuple[float, float, float] | None = None
            previous_end: tuple[float, float, float] | None = None
            for relative_segment in range(
                first_segment,
                last_segment + 1,
                SPLINE_SEGMENT_SIZE,
            ):
                segment_index = (
                    relative_segment - segment_table
                ) // SPLINE_SEGMENT_SIZE
                if segment_index in covered_segments:
                    raise ValueError(
                        "retail tSplineData rails share a segment record"
                    )
                covered_segments.add(segment_index)
                segment_offset = section_offset + relative_segment
                native_values = struct.unpack_from(
                    ">30f",
                    data,
                    segment_offset,
                )
                if not all(math.isfinite(value) for value in native_values):
                    raise ValueError(
                        f"retail grind rail {rail_index} contains "
                        "non-finite native data"
                    )
                coefficient_a = _vec3(data, segment_offset)
                coefficient_b = _vec3(data, segment_offset + 16)
                coefficient_c = _vec3(data, segment_offset + 32)
                coefficient_d = _vec3(data, segment_offset + 48)
                segment_end = _add(
                    coefficient_d,
                    coefficient_c,
                    coefficient_b,
                    coefficient_a,
                )
                if first_start is None:
                    first_start = coefficient_d
                if (
                    previous_end is not None
                    and _distance(previous_end, coefficient_d)
                    > SPLINE_CONTINUITY_EPSILON
                ):
                    raise ValueError(
                        f"retail grind rail {rail_index} is discontinuous"
                    )
                previous_end = segment_end
                payloads.append(
                    data[
                        segment_offset : (
                            segment_offset + SPLINE_SEGMENT_PAYLOAD_SIZE
                        )
                    ].hex()
                )

            assert first_start is not None and previous_end is not None
            result.append(
                {
                    "section_index": record_index,
                    "section_offset": section_offset,
                    "rail_index": rail_index,
                    "spline_id": f"0x{spline_id:016X}",
                    "type_signature": f"0x{type_signature:016X}",
                    "flags": flags,
                    "trailing_word": trailing_word,
                    "closed": (
                        _distance(first_start, previous_end)
                        <= SPLINE_CONTINUITY_EPSILON
                    ),
                    "segment_count": len(payloads),
                    "native_segment_payloads": payloads,
                }
            )

        if len(covered_segments) != segment_count:
            raise ValueError(
                "retail tSplineData contains unreferenced segment records"
            )
    return result
