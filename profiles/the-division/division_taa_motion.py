"""Evaluate the two matched The Division TAA shaders' coordinate transforms.

Read one capture JSON object from a file or stdin. The `cb_hex` field is the
first 80 bytes of the selected uniform buffer. `samples` contain `dispatch_xy`
and `motion_xy`, where the latter is the decoded XY from the selected texture
texel. This utility uses host float precision for diagnostics.
"""

import argparse
import json
import math
import struct
import sys


def vec2(value):
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise ValueError("expected a pair")
    if not all(isinstance(component, (int, float)) and math.isfinite(component)
               for component in value):
        raise ValueError("expected two finite numbers")
    return value


def multiply(a, b):
    return [x * y for x, y in zip(a, b)]


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def affine(p, scale, bias):
    return add(multiply(p, scale), bias)


def diagnose(capture):
    raw = bytes.fromhex(capture["cb_hex"])
    history_scale_x_y, filter_data, render_data, reprojection_data = [
        struct.unpack_from("<4f", raw, i * 16) for i in range(4)
    ]
    output_origin = list(struct.unpack_from("<2I", raw, 64))

    history_scale = vec2(history_scale_x_y[:2])
    history_bias = vec2(history_scale_x_y[2:])
    inv_history_grid = vec2(filter_data[:2])
    history_grid = vec2(filter_data[2:])
    render_scale = vec2(render_data[2:])
    render_bias = vec2(reprojection_data[:2])
    reprojection_scale = vec2(reprojection_data[2:])
    history_uv_origin = affine((-1.0, 1.0), history_scale, history_bias)
    history_uv_slope = multiply(reprojection_scale, history_scale)

    result = {
        "current_texel_from_dispatch_center": {"scale": list(render_scale), "bias": list(render_bias)},
        "zero_motion_history_uv_from_dispatch_center": {"scale": history_uv_slope, "bias": history_uv_origin},
        "history_uv_per_motion_unit": list(history_scale),
        "history_grid_per_motion_unit": multiply(history_grid, history_scale),
        "history_grid_scale_times_inverse_grid_scale": multiply(history_grid, inv_history_grid),
        "equivalent_current_render_pixels_per_motion_unit": [
            r / n if n and a else None
            for r, n, a in zip(render_scale, reprojection_scale, history_scale)
        ],
        "output_origin": output_origin,
        "samples": [],
    }
    for sample in capture.get("samples", []):
        pixel = vec2(sample["dispatch_xy"])
        if not all(type(axis) is int and axis >= 0 for axis in pixel):
            raise ValueError("dispatch_xy must contain two nonnegative integer coordinates")
        motion = vec2(sample["motion_xy"])
        center = add(pixel, (0.5, 0.5))
        current = affine(center, render_scale, render_bias)
        base_uv = affine(center, history_uv_slope, history_uv_origin)
        delta_uv = multiply(motion, history_scale)
        history_uv = add(base_uv, delta_uv)
        history_position = multiply(history_uv, history_grid)
        result["samples"].append({
            "dispatch_xy": pixel,
            "decoded_motion_xy": motion,
            "current_texel": [math.trunc(x) for x in current],
            "output_texel": add(pixel, output_origin),
            "zero_motion_history_uv": base_uv,
            "reprojected_history_uv": history_uv,
            "delta_history_uv": delta_uv,
            "history_filter_base_texel": [math.floor(x - 0.5) for x in history_position],
        })
    return result


def check_example():
    width, height = 1280, 800
    cb = struct.pack("<16f4I",
                     0.5, -0.5, 0.5, 0.5,
                     1 / width, 1 / height, width, height,
                     0.0, 0.0, 0.75, 0.75,
                     2.0, 3.0, 2 / width, -2 / height,
                     13, 17, 0, 0)
    sample = {"dispatch_xy": [15, 20], "motion_xy": [0.50048828125, -0.251953125]}
    report = diagnose({"cb_hex": cb.hex(), "samples": [sample]})
    scale = report["equivalent_current_render_pixels_per_motion_unit"]
    assert all(math.isclose(x, y, rel_tol=1e-7) for x, y in zip(scale, [480, -300]))
    result = report["samples"][0]
    assert result["current_texel"] == [13, 18]
    assert result["output_texel"] == [28, 37]
    assert result["delta_history_uv"] == [0.250244140625, 0.1259765625]
    assert result["history_filter_base_texel"] == [335, 120]
    assert all(math.isclose(x, y, rel_tol=1e-7)
               for x, y in zip(report["history_grid_scale_times_inverse_grid_scale"], [1, 1]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=argparse.FileType("r"), default=sys.stdin)
    parser.add_argument("--check-example", action="store_true")
    args = parser.parse_args()
    if args.check_example:
        check_example()
        return
    try:
        print(json.dumps(diagnose(json.load(args.capture)), ensure_ascii=False, indent=2, allow_nan=False))
    except (KeyError, TypeError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
