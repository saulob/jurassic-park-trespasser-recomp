"""Every language of the port has every kind of text the runtime asks for (headless, read only):
    py -3 -I -B -m unittest discover -s tests -p "test_language_parity.py"

The languages are the rows of jpt_l10n_languages[] (src/common/jpt_l10n.c). For each: its credit, its interface file with
the same keys as English and a translation for each, its game text and its captions among the embedded files, no mixing of
alphabets and valid UTF-8. The content checks (placeholders, wrapping, game text coverage) are l10n_tests's.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gate_support as gs  # noqa: E402

TRANSLATIONS = gs.ROOT / "assets" / "translations"
LANGUAGES = gs.languages()
CODES = [l["code"] for l in LANGUAGES]
CYRILLIC = re.compile("[Ѐ-ӿ]")
MOJIBAKE = re.compile("Ã[\u0080-¿]|Â[\u0080-¿]|�")
RESULT = {code: [] for code in CODES}


def interface(code):
    path = TRANSLATIONS / f"{code}.interface.txt"
    keys, duplicates = {}, []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 2:
            RESULT[code].append(f"{path.name}:{number}: no tab between the key and its text")
            continue
        if fields[0] in keys:
            duplicates.append(fields[0])
        keys[fields[0]] = fields[1]
    return keys, duplicates


def note(code, text):
    RESULT[code].append(text)
    return text


class LanguageParity(unittest.TestCase):
    def test_the_port_has_the_languages_it_claims(self):
        self.assertEqual(len(LANGUAGES), 7)
        self.assertEqual(CODES[0], "en")
        self.assertEqual(len(set(CODES)), len(CODES))

    def test_every_language_has_a_credit(self):
        credited = {c["language"] for c in gs.credits()}
        for code in CODES:
            with self.subTest(language=code):
                if code not in credited:
                    self.fail(note(code, "no row in jpt_l10n_credits[]"))

    def test_interface_files_have_the_keys_of_english_and_a_translation_for_each(self):
        english, _ = interface("en")
        for code in CODES:
            keys, duplicates = interface(code)
            with self.subTest(language=code):
                if duplicates:
                    self.fail(note(code, f"duplicate keys: {duplicates[:3]}"))
                missing = sorted(set(english) - set(keys))
                extra = sorted(set(keys) - set(english))
                if missing or extra:
                    self.fail(note(code, f"keys differ from English: missing {missing[:3]}, extra {extra[:3]}"))
                empty = [k for k, v in keys.items() if not v.strip()]
                if empty:
                    self.fail(note(code, f"empty translations: {empty[:3]}"))

    def test_text_is_valid_utf8_and_each_language_keeps_to_its_alphabet(self):
        for code in CODES:
            for path in sorted(TRANSLATIONS.glob(f"{code}.*.txt")):
                with self.subTest(file=path.name):
                    raw = path.read_bytes()
                    text = raw.decode("utf-8")
                    self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), "a byte order mark")
                    if MOJIBAKE.search(text):
                        self.fail(note(code, f"{path.name}: text damaged by a wrong code page"))
                    if code == "ru":
                        self.assertRegex(text, CYRILLIC, "the Russian text has no Cyrillic")
                    elif "interface" in path.name or "strings" in path.name:
                        if CYRILLIC.search(text):
                            self.fail(note(code, f"{path.name}: Cyrillic in a Latin-alphabet language"))

    def test_every_language_has_game_text_and_captions(self):
        for code in CODES:
            with self.subTest(language=code):
                files = {p.name for p in TRANSLATIONS.glob(f"{code}.*.txt")}
                if code != "en":
                    if not files & {f"{code}.strings.txt", f"{code}.strings.port.txt"}:
                        self.fail(note(code, "no game text file"))
                if not files & {f"{code}.subtitles.txt", f"{code}.subtitles.port.txt"}:
                    self.fail(note(code, "no captions file"))
                self.assertIn(f"{code}.interface.txt", files)

    def test_code_pages_match_the_alphabet(self):
        for language in LANGUAGES:
            expected = 1251 if language["code"] == "ru" else 1252
            self.assertEqual(language["codepage"], expected, language["code"])

    def test_the_launcher_lists_every_language_and_translates_the_brazilian_page(self):
        launcher = gs.read(gs.LAUNCHER / "launcher.cpp")
        self.assertIn("jpt_l10n_language_listed", launcher)
        listed = re.search(r"k_listed\[JPT_L10N_LANGUAGE_COUNT\] = \{([^}]*)\}", gs.read(gs.COMMON / "jpt_l10n.c"))
        self.assertEqual(sorted(int(n) for n in listed.group(1).split(",")), list(range(len(LANGUAGES))))
        self.assertTrue((gs.LAUNCHER / "localization" / "pt_br_ui_text.cpp").exists())


def tearDownModule():
    gs.report("language_parity", {"languages": [{"code": l["code"], "name": l["name"], "problems": RESULT[l["code"]]} for l in LANGUAGES]})


if __name__ == "__main__":
    unittest.main()
