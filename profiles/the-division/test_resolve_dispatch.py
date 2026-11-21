import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest

from resolve_dispatch import ProfileError, SHADERS, resolve_dispatch


def image(role, kind, element, fmt="R16G16B16A16_FLOAT", mip=0):
    resource_extent = [1280, 800, 1]
    return {
        "class": kind, "set": 1, "binding": 1, "element": element,
        "resource": f"resource_{role}", "view": f"view_{role}",
        "resource_format": fmt, "view_format": fmt,
        "resource_extent": resource_extent, "mip_extent": [max(1, n >> mip) for n in resource_extent],
        "mip": mip, "mip_count": 1, "layer": 0, "layer_count": 1,
    }


def fixture(shader_hash):
    # Distinct slots and shuffled input ensure the profile follows index arithmetic.
    push = bytearray(40)
    struct.pack_into("<QII", push, 0, 0x123456789, 100, 7)
    struct.pack_into("<I", push, 20, 470)
    struct.pack_into("<I", push, 32, 800)
    prefix = bytes(range(80)).hex()
    uniform = {
        "class": "constant", "set": 2, "binding": 0, "element": 7,
        "resource": "coords", "view": "ResourceId::0", "offset": 2048, "length": 128,
        "prefix_hex": prefix,
    }
    return {
        "event": 52, "groups": [160, 100, 1], "shader_sha256": shader_hash,
        "push_hex": push.hex(),
        "indirect": {"resource": "indices", "offset": 16, "indices": [17, 12, 52, 3]},
        "entries": [
            dict(uniform, element=5, prefix_hex="ff" * 80),
            image("output", "read_write", 470),
            image("history", "read_only", 112),
            image("wrong_class", "read_write", 103, "R32_UINT"),
            image("flags", "read_only", 815, "R8_UINT"),
            image("aux", "read_write", 471),
            image("depth", "read_only", 826, "R32_FLOAT"),
            image("current", "read_only", 117, mip=1),
            image("motion", "read_only", 103, "R16G16_FLOAT"),
            image("wrong_set", "read_only", 117) | {"set": 4},
            image("scalar", "read_only", 152, "R8_UNORM"),
            uniform,
        ],
    }, uniform


class ResolveDispatchTest(unittest.TestCase):
    def setUp(self):
        self.hash = next(iter(SHADERS))
        self.capture, self.uniform = fixture(self.hash)

    def test_known_variants_map_all_eight_roles_and_first_mip(self):
        expected = {
            "current": 117, "history": 112, "scalar": 152, "motion": 103,
            "depth_candidate": 826, "flags": 815, "output": 470, "aux": 471,
        }
        for shader_hash, label in SHADERS.items():
            with self.subTest(label=label):
                capture, uniform = fixture(shader_hash)
                result = resolve_dispatch(capture)
                roles = result["roles"]
                self.assertEqual(roles["current"]["view"], "view_current")
                self.assertEqual(roles["current"]["mip_extent"], [640, 400, 1])
                self.assertEqual(roles["motion"]["view_format"], "R16G16_FLOAT")
                self.assertEqual(roles["output"]["class"], "read_write")
                self.assertEqual({role: view["element"] for role, view in roles.items()}, expected)
                self.assertEqual(result["cb_hex"], uniform["prefix_hex"])
                self.assertEqual(result["shader_label_hash"], label)

    def test_roles_follow_array_position_not_element_order_or_format(self):
        self.capture["indirect"]["indices"] = [12, 17, 3, 52]
        roles = resolve_dispatch(self.capture)["roles"]
        self.assertEqual(roles["current"]["view"], "view_history")
        self.assertEqual(roles["history"]["view"], "view_current")
        self.assertEqual(roles["scalar"]["view"], "view_motion")
        self.assertEqual(roles["motion"]["view"], "view_scalar")

    def test_missing_or_wrong_class_texture_is_unresolved(self):
        self.capture["entries"] = [row for row in self.capture["entries"] if row.get("view") != "view_motion"]
        with self.assertRaisesRegex(ProfileError, r"motion: missing read_only descriptor"):
            resolve_dispatch(self.capture)

    def test_partial_capture_is_unresolved(self):
        self.capture["push_hex"] = self.capture["push_hex"][:64]
        with self.assertRaisesRegex(ProfileError, "push constants"):
            resolve_dispatch(self.capture)
        self.capture, self.uniform = fixture(self.hash)
        self.capture["indirect"]["indices"].pop()
        with self.assertRaisesRegex(ProfileError, "expected four indices"):
            resolve_dispatch(self.capture)
        self.capture, self.uniform = fixture(self.hash)
        self.uniform["prefix_hex"] = "00" * 79
        with self.assertRaisesRegex(ProfileError, r"coordinate uniform: expected at least 80 bytes"):
            resolve_dispatch(self.capture)

    def test_foreign_shader_is_rejected_before_other_fields(self):
        with self.assertRaisesRegex(ProfileError, "SHA-256"):
            resolve_dispatch({"shader_sha256": "f" * 64})

    def test_identical_texture_duplicates_and_ubo_aliases_are_supported(self):
        self.capture["entries"].extend([
            copy.deepcopy(self.capture["entries"][7]),
            dict(self.uniform, type="scalar", length=256, prefix_hex=self.uniform["prefix_hex"] + "00"),
        ])
        result = resolve_dispatch(self.capture)
        self.assertEqual(result["coordinate_uniform"]["lengths"], [128, 256])
        self.assertEqual(result["cb_hex"], self.uniform["prefix_hex"])

    def test_ubo_conflicts_across_alias_rows_are_unresolved(self):
        for override in ({"resource": "other"}, {"offset": 8}, {"prefix_hex": "ff" * 80}):
            with self.subTest(override=override):
                capture, uniform = fixture(self.hash)
                capture["entries"].append(dict(uniform, **override))
                with self.assertRaisesRegex(ProfileError, r"coordinate uniform: ambiguous"):
                    resolve_dispatch(capture)

    def test_different_views_at_same_element_are_unresolved(self):
        self.capture["entries"].append(image("conflict", "read_only", 117))
        with self.assertRaisesRegex(ProfileError, r"current: ambiguous image view"):
            resolve_dispatch(self.capture)

    def test_cli_reports_unresolved_status_and_fails(self):
        script = Path(__file__).with_name("resolve_dispatch.py")
        result = subprocess.run([sys.executable, script], input="{}", text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["status"], "unresolved")


if __name__ == "__main__":
    unittest.main()
