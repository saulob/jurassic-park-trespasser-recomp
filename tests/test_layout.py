"""Headless checks of the structure offsets the host reads, per profile (no game files needed).

    py -3 -m unittest tests.test_layout

config/host_offsets.json holds, for every offset src/host hard-codes (a named `#define NAME 0x..` or a raw `+ 0x..` inside a
guest memory accessor), what is proven of it in each other build: unmoved, moved (with its value), not a guest structure,
or only used by code that does not run in that build. Nothing may be unproven, a field that moved must be read through a GLAYOUT_ descriptor, and an access the
manifest does not know must fail here.
"""
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gen_guest_layout  # noqa: E402
import gen_guest_va  # noqa: E402
import host_offsets  # noqa: E402

PROVEN = ("unmoved", "moved", "not_guest", "gated")


def manifest():
    return json.loads(host_offsets.MANIFEST.read_text(encoding="utf-8"))["offsets"]


def layout_table(profile):
    """(profile, struct, field, offset, status) of every offset the host reads."""
    out = []
    for row in manifest():
        entry = row[profile]
        out.append((profile, row.get("struct", "-"), f"{Path(row['file']).name}:{row['name']}", entry.get("value", row["value"]),
                    entry["status"]))
    return out


class LayoutTests(unittest.TestCase):
    def test_the_manifest_is_current(self):
        self.assertEqual(host_offsets.main([]), 0)

    def test_no_offset_is_unproven_in_any_other_build(self):
        for profile in host_offsets.PROFILES:
            bad = [f"{r['file']} {r['name']} {r['value']}: {r[profile].get('evidence', 'no evidence')}"
                   for r in manifest() if r[profile]["status"] not in PROVEN]
            self.assertEqual(bad, [], f"{profile}: {len(bad)} unproven")

    def test_every_row_says_what_it_proves(self):
        for profile in host_offsets.PROFILES:
            for row in manifest():
                entry = row[profile]
                self.assertTrue(entry["evidence"], f"{profile} {row['file']} {row['name']}")
                if entry["status"] == "moved":
                    self.assertNotEqual(entry["value"], row["value"], f"{profile} {row['name']}")

    def test_the_layout_table_lists_profile_struct_field_and_offset(self):
        for profile in host_offsets.PROFILES:
            table = layout_table(profile)
            self.assertEqual(len(table), len(manifest()))
            for _, struct, field, offset, status in table:
                self.assertRegex(offset, r"^0x[0-9A-F]+$")
                self.assertIn(status, PROVEN)
                self.assertTrue(struct and field)

    def test_a_field_that_moved_is_read_through_a_descriptor(self):
        for row in manifest():
            if any(row[p]["status"] == "moved" for p in host_offsets.PROFILES):
                self.assertIn("descriptor", row, f"{row['file']} {row['name']} moved but is a literal")

    def test_descriptor_tables_are_current_and_semantic(self):
        for lift, profile in gen_guest_layout.PROFILE_LIFT.items():
            if profile is not None:
                self.assertEqual(gen_guest_layout.values(profile)[1], [], lift)
        files = gen_guest_va.files_for()
        for name in ("guest_layout.h", "guest_profile_data.c"):
            path = ROOT / "src" / "host" / "profiles" / name
            self.assertEqual(path.read_text(encoding="utf-8"), files[name], name)
        for d in gen_guest_layout.descriptors():
            self.assertRegex(d["name"], r"^[A-Z][A-Z0-9]*(_[A-Z][A-Z0-9]*)+$")
            self.assertNotRegex(d["name"], r"(^|_)(OFFSET|OFF)_?[0-9A-F]+$")

    def test_a_descriptor_has_the_original_value_in_the_original_build(self):
        values = gen_guest_va.layout_values()["lift116"]
        for d in gen_guest_layout.descriptors():
            self.assertEqual(values[d["name"]], d["value"])

    def test_gated_offsets_are_only_used_by_code_that_does_not_run_in_their_build(self):
        for profile in host_offsets.PROFILES:
            groups = json.loads((host_offsets.CONFIG_DIR / "profiles" / f"{profile}.json").read_text(encoding="utf-8"))["patch117"]
            for row in manifest():
                if row[profile]["status"] != "gated":
                    continue
                lines = row.get("lines") or host_offsets.define_uses(row["file"], row["name"])
                self.assertTrue(host_offsets.compiled_out(row["file"], lines, groups), f"{profile} {row['file']} {row['name']}")

    def test_an_offset_the_manifest_does_not_know_fails(self):
        extra = {"file": "src/host/example.c", "name": "FIELD_NEW", "value": "0x44"}
        with mock.patch.object(host_offsets, "host_defines", return_value=host_offsets.host_defines() + [extra]):
            rows = host_offsets.build()
        new = [r for r in rows if r["name"] == "FIELD_NEW"][0]
        self.assertTrue(all(new[p]["status"] == "unproven" for p in host_offsets.PROFILES))
        with mock.patch.object(host_offsets, "host_defines", return_value=host_offsets.host_defines() + [extra]):
            self.assertEqual(host_offsets.main([]), 1)

    def test_a_raw_literal_the_manifest_does_not_know_fails(self):
        extra = {"file": "src/host/example.c", "name": "@0x4c4", "value": "0x4C4", "literal": True, "lines": [1]}
        with mock.patch.object(host_offsets, "host_literals", return_value=host_offsets.host_literals() + [extra]):
            rows = host_offsets.build()
        new = [r for r in rows if r["name"] == "@0x4c4"][0]
        self.assertTrue(all(new[p]["status"] == "unproven" for p in host_offsets.PROFILES))

    def test_no_moved_value_is_left_as_a_raw_literal(self):
        moved = set()
        for row in manifest():
            for p in host_offsets.PROFILES:
                if row[p]["status"] == "moved":
                    moved.add(row["value"])
                    moved.add(row[p]["value"])
        for row in manifest():
            if row.get("literal"):
                self.assertNotIn(row["value"], moved, f"{row['file']} reads {row['name']} raw: it moves in a build")

    def test_the_host_names_every_descriptor_it_defines(self):
        used = {m for path in (ROOT / "src" / "host").glob("*.c") for m in re.findall(r"GLAYOUT_[A-Z0-9_]+", path.read_text(encoding="utf-8"))}
        defined = {f"GLAYOUT_{d['name']}" for d in gen_guest_layout.descriptors()}
        self.assertEqual(used - defined, set())
        self.assertEqual(defined - used, set())


if __name__ == "__main__":
    unittest.main()
