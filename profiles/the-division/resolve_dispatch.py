"""Resolve The Division build 21161679 TAA offline from inspect_dispatch.py JSON.

Generate the input with --index-count 4. Pass its path here or pipe it to
stdin. The successful result includes cb_hex for division_taa_motion.py.
"""

import argparse
import json
import struct
import sys


SHADERS = {
    "f9f53bbbb48a38b1a8c71ee99fd22820afb68d0aaef722e62996d9bb9cbd39b4": "454827a21acbab8d",
    "77d430846e46b5f81cb2ef28f4b0cb102680646bd441fbbf04c35da47de23811": "eb009069ee26b2be",
}
UINT32_MAX = 0xffffffff
CB_BYTES = 80


class ProfileError(ValueError):
    """The dispatch does not satisfy the observations needed by this profile."""


def hex_bytes(value, count, name):
    try:
        raw = bytes.fromhex(value)
    except (TypeError, ValueError) as error:
        raise ProfileError(f"{name}: expected hexadecimal bytes") from error
    if len(raw) < count:
        raise ProfileError(f"{name}: expected at least {count} bytes, got {len(raw)}")
    return raw


def unsigned(value, name, maximum=UINT32_MAX, *, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProfileError(f"{name}: expected an integer between {minimum} and {maximum}")
    return value


def identifier(row, field, name):
    value = row.get(field)
    if not isinstance(value, str) or value.strip().lower() in {
        "", "0", "resourceid::0", "null", "undefined", "unknown"
    }:
        raise ProfileError(f"{name}: missing {field}")
    return value


def descriptor_rows(entries, key, name):
    matching = [row for row in entries if
                (row.get("class"), row.get("set"), row.get("binding"), row.get("element")) == key]
    if not matching:
        kind, descriptor_set, binding, element = key
        raise ProfileError(f"{name}: missing {kind} descriptor at ({descriptor_set}, {binding}, {element})")
    return matching


def extent(row, field, name):
    value = row.get(field)
    if not isinstance(value, list) or len(value) != 3:
        raise ProfileError(f"{name}: missing {field}")
    return [unsigned(axis, f"{name}.{field}", minimum=1) for axis in value]


def texture_view(row, role):
    result = {field: identifier(row, field, role)
              for field in ("resource", "view", "resource_format", "view_format")}
    result.update({field: unsigned(row.get(field), f"{role}.{field}")
                   for field in ("mip", "layer")})
    result.update({field: unsigned(row.get(field), f"{role}.{field}", minimum=1)
                   for field in ("mip_count", "layer_count")})
    result.update({field: extent(row, field, role) for field in ("resource_extent", "mip_extent")})
    if result["mip_extent"] != [max(1, axis >> result["mip"]) for axis in result["resource_extent"]]:
        raise ProfileError(f"{role}: mip_extent disagrees with resource_extent and mip")
    return result


def resolve_texture(entries, role, kind, index):
    key = kind, 1, 1, index & UINT32_MAX
    views = [texture_view(row, role) for row in descriptor_rows(entries, key, role)]
    view = views[0]
    if any(other != view for other in views[1:]):
        raise ProfileError(f"{role}: ambiguous image view for descriptor {key}")
    return dict(zip(("class", "set", "binding", "element"), key), **view)


def resolve_uniform(entries, element):
    name = "coordinate uniform"
    rows = descriptor_rows(entries, ("constant", 2, 0, element), name)
    aliases = set()
    lengths = set()
    for row in rows:
        resource = identifier(row, "resource", name)
        offset = unsigned(row.get("offset"), f"{name}.offset", (1 << 64) - 1)
        length = unsigned(row.get("length"), f"{name}.length", (1 << 64) - 1, minimum=CB_BYTES)
        raw = hex_bytes(row.get("prefix_hex"), CB_BYTES, name)
        aliases.add((resource, offset, raw[:CB_BYTES]))
        lengths.add(length)
    if len(aliases) != 1:
        raise ProfileError("coordinate uniform: ambiguous buffer, offset or captured prefix")
    resource, offset, raw = aliases.pop()
    return {"set": 2, "binding": 0, "element": element, "resource": resource, "offset": offset,
            "lengths": sorted(lengths)}, raw.hex()


def resolve_dispatch(dispatch):
    """Return the resolved profile or raise ProfileError on insufficient evidence."""
    if not isinstance(dispatch, dict):
        raise ProfileError("expected one inspect_dispatch.py JSON object")
    shader_hash = dispatch.get("shader_sha256")
    if not isinstance(shader_hash, str) or shader_hash not in SHADERS:
        raise ProfileError("shader SHA-256 is outside the The Division TAA profile")
    push = hex_bytes(dispatch.get("push_hex"), 36, "push constants")
    index_address, image_base, coordinate_element = struct.unpack_from("<QII", push, 0)
    output_base, = struct.unpack_from("<I", push, 20)
    global_base, = struct.unpack_from("<I", push, 32)
    indirect = dispatch.get("indirect")
    indices = indirect.get("indices") if isinstance(indirect, dict) else None
    if not index_address or not isinstance(indices, list) or len(indices) != 4:
        raise ProfileError("expected four indices read through the nonzero buffer device address")
    indices = [unsigned(index, f"indirect.indices[{i}]") for i, index in enumerate(indices)]
    entries = dispatch.get("entries")
    if not isinstance(entries, list) or any(not isinstance(row, dict) for row in entries):
        raise ProfileError("expected an array of descriptor entries")
    roles = {
        role: resolve_texture(entries, role, "read_only", image_base + index)
        for role, index in zip(("current", "history", "scalar", "motion"), indices)
    }
    roles["depth_candidate"] = resolve_texture(entries, "depth_candidate", "read_only", global_base + 26)
    roles["flags"] = resolve_texture(entries, "flags", "read_only", global_base + 15)
    roles["output"] = resolve_texture(entries, "output", "read_write", output_base)
    roles["aux"] = resolve_texture(entries, "aux", "read_write", output_base + 1)
    uniform, raw = resolve_uniform(entries, coordinate_element)
    return {
        "status": "resolved",
        "steam_build": "21161679",
        "event": dispatch.get("event"),
        "shader_sha256": shader_hash,
        "shader_label_hash": SHADERS[shader_hash],
        "roles": roles,
        "coordinate_uniform": uniform,
        "cb_hex": raw,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dispatch", type=argparse.FileType("r"), nargs="?", default=sys.stdin)
    args = parser.parse_args()
    try:
        result = resolve_dispatch(json.load(args.dispatch))
    except (ProfileError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "unresolved", "error": str(error)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
