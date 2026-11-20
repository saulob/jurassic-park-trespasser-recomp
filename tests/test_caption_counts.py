"""The caption and restored-audio numbers the documentation states are the numbers of the data (headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_caption_counts.py"

The sources are assets/translations/en.subtitles.txt and assets/restored_audio/restored_audio.manifest.txt, which the
program embeds; the retail comparison reads the installed Stream.tpa and is skipped without it.
"""
import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402

README = gs.read(ROOT / "README.md")
LEGAL = gs.read(ROOT / "docs" / "LEGAL.md")
RETAIL_CANDIDATES = (ROOT / "game" / "data" / "Stream.tpa", ROOT / "game" / "versions" / "en-116" / "data" / "Stream.tpa")


def canonical_captions():
    samples, current = {}, None
    for line in gs.read(ROOT / "assets" / "translations" / "en.subtitles.txt").splitlines():
        fields = line.split("\t")
        if fields[0] == "S":
            current = int(fields[2], 16)
            samples[current] = []
        elif fields[0] == "C":
            samples[current].append((int(fields[1]), fields[2]))
    return samples


def restored_records():
    records = {}
    for line in gs.read(ROOT / "assets" / "restored_audio" / "restored_audio.manifest.txt").splitlines():
        if not line.startswith("sample\t"):
            continue
        fields = line.split("\t")
        attributes = dict(f.split("=", 1) for f in fields[2:])
        records[int(fields[1], 16)] = attributes
    return records


def retail_captions(path):
    with open(path, "rb") as fh:
        magic, count, header = struct.unpack("<III", fh.read(12))
        assert magic == 0x150 and header == 0x18
        fh.seek(0x18)
        directory = fh.read(count * 0x38)
        captions = {}
        for i in range(count):
            entry = directory[i * 0x38:(i + 1) * 0x38]
            sample, offset = struct.unpack("<II", entry[:8])
            block_header = struct.unpack("<I", entry[0x1C:0x20])[0]
            if sample in captions or block_header <= 0x24:
                continue
            fh.seek(offset + 0x24)
            length, lines = struct.unpack("<II", fh.read(8))
            body, at, found = fh.read(length - 8), 0, []
            for _ in range(lines):
                units = struct.unpack("<I", body[at:at + 4])[0]
                end = body.index(b"\0", at + 4)
                found.append((units, body[at + 4:end].decode("latin-1")))
                at = end + 1
            captions[sample] = found
    return captions


CANONICAL = canonical_captions()
RESTORED = restored_records()
NEW = {s for s, a in RESTORED.items() if a["replaces"] == "-"}
REPLACING = set(RESTORED) - NEW
RETAIL_PATH = next((p for p in RETAIL_CANDIDATES if p.exists()), None)
FACTS = {"canonical": len(CANONICAL), "restored": len(RESTORED), "new": len(NEW), "replacing": len(REPLACING),
         "restored_bytes": sum(int(a["record"]) for a in RESTORED.values()), "retail_data": RETAIL_PATH is not None}


class CaptionCounts(unittest.TestCase):
    def test_restored_records_are_captioned_and_each_one_is_new_or_replaces_one_retail_payload(self):
        self.assertTrue(RESTORED)
        for sample in RESTORED:
            self.assertIn(sample, CANONICAL, "a restored record without a canonical caption")
        self.assertEqual(len(NEW) + len(REPLACING), len(RESTORED))
        for sample in REPLACING:
            self.assertRegex(RESTORED[sample]["replaces"], r"^[0-9a-f]{64}$")

    def test_the_documentation_states_the_restored_records_and_their_size(self):
        words = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven"}
        self.assertIn(f"{words[len(RESTORED)]} audio records, {FACTS['restored_bytes']:,} bytes", LEGAL)
        self.assertIn(f"These {words[len(RESTORED)]} records, {FACTS['restored_bytes']:,}", README)

    def test_the_documentation_states_how_many_samples_are_captioned_in_every_language(self):
        per_language = {}
        for path in sorted((ROOT / "assets" / "translations").glob("*.subtitles*.txt")):
            language = path.name.split(".")[0]
            for line in gs.read(path).splitlines():
                if line.startswith("S	"):
                    _, pack, sample = line.split("	")[:3]
                    per_language.setdefault(language, set()).add((pack, sample))
        english = {(pack, f"{s:08x}") for (pack, s) in ((("stream"), s) for s in CANONICAL)}
        stated = int(re.search(r"\((\d+) samples, the restored dialogue included\)", gs.read(ROOT / "assets" / "translations" / "README.md")).group(1))
        for language in ("pt-BR", "fr", "de", "es"):
            self.assertEqual(len(per_language[language] | english), stated, language)

    @unittest.skipUnless(RETAIL_PATH, "the retail Stream.tpa is not installed: the retail comparison cannot be made (DATA UNAVAILABLE)")
    def test_the_documentation_states_the_retail_comparison(self):
        retail = retail_captions(RETAIL_PATH)
        self.assertTrue(NEW <= set(CANONICAL) - set(retail), "a new restored sample is already in the retail file")
        self.assertEqual(set(retail) - set(CANONICAL), set(), "a retail caption the canonical table lacks")
        shared = [s for s in CANONICAL if s in retail]
        identical = [s for s in shared if CANONICAL[s] == retail[s]]
        timing_only = [s for s in shared if CANONICAL[s] != retail[s] and [t for _, t in CANONICAL[s]] == [t for _, t in retail[s]]]
        corrected = len(shared) - len(identical)
        other = corrected - len(timing_only)
        match = re.search(r"Of the (\d+) voice captions of the retail `Stream\.tpa`, (\d+) are corrected \((\d+) only in\s+timing, (\d+) also", README)
        self.assertIsNotNone(match, "README's caption sentence changed shape")
        self.assertEqual(tuple(int(g) for g in match.groups()), (len(retail), corrected, len(timing_only), other))
        added = len(CANONICAL) - len(retail) - len(RESTORED)
        self.assertGreaterEqual(added, 0)
        flat = " ".join(README.split())
        self.assertIn(f"with the {added} captions the retail file lacked for audio it has and the {len(RESTORED)} restored records the canonical table holds {len(CANONICAL)} samples", flat)


def tearDownModule():
    gs.report("caption_counts", FACTS)


if __name__ == "__main__":
    unittest.main()
