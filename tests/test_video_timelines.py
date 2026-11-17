#!/usr/bin/env python3
"""The cue tables of the dubbed videos, headless: py -3 -m unittest discover -s tests -p "test_video_timelines.py"

The table checks read src/common/jpt_video_subtitles.c only. The analysis check decodes the videos of
game/versions/<id> with ffmpeg and is skipped without them."""
import os
import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import smk_speech as S  # noqa: E402

FRAMES = {"tpassintro": 1585, "newgame": 1085, "win": 606}
CUE_COUNT = {"tpassintro": 16, "newgame": 9, "win": 7}


def tables():
    return S.parse_cue_arrays(S.read_source())


class CueTables(unittest.TestCase):
    def test_every_release_has_a_table_for_every_dialogue_video(self):
        arrays = tables()
        for video, array in S.VIDEOS.items():
            for release in S.RELEASES:
                self.assertIn(array + S.SUFFIX[release], arrays, (video, release))
                self.assertEqual(len(arrays[array + S.SUFFIX[release]]), CUE_COUNT[video], (video, release))

    def test_cues_are_ordered_inside_the_video_and_read_long_enough(self):
        arrays = tables()
        for video, array in S.VIDEOS.items():
            for release in S.RELEASES:
                rows = arrays[array + S.SUFFIX[release]]
                for k, (first, last, _) in enumerate(rows):
                    self.assertLess(first, last, (video, release, k))
                    self.assertLess(last, FRAMES[video], (video, release, k))
                    self.assertGreaterEqual(last - first + 1, 15, (video, release, k))
                    if k:
                        self.assertGreater(first, rows[k - 1][1], (video, release, k))

    def test_a_dub_keeps_the_english_lines_and_has_its_own_timing(self):
        arrays = tables()
        for video, array in S.VIDEOS.items():
            english = arrays[array]
            for release in S.DUBS:
                rows = arrays[array + S.SUFFIX[release]]
                self.assertEqual([r[2] for r in rows], [r[2] for r in english], (video, release))
                self.assertNotEqual([(r[0], r[1]) for r in rows], [(r[0], r[1]) for r in english], (video, release))

    def test_the_margins_are_one_policy(self):
        spans = [(100.2, 130.0), (140.0, 170.4), (171.0, 200.0)]
        cues = S.margins(spans, 1000)
        self.assertEqual(cues[0], (int(100.2) - S.LEAD_FRAMES, 130 + S.TRAIL_FRAMES))
        self.assertEqual(cues[1][0], 140 - S.LEAD_FRAMES)
        self.assertEqual(cues[1][1] + 1, cues[2][0])
        self.assertGreater(cues[2][0], int(170.4))
        self.assertTrue(all(b - a + 1 >= S.MIN_CUE_FRAMES for a, b in cues))
        self.assertEqual(S.margins([(2.0, 4.0)], 1000)[0][0], 0)


@unittest.skipUnless(shutil.which("ffmpeg") or os.environ.get("FFMPEG"), "ffmpeg is not installed")
@unittest.skipUnless(all(S.video_path(r, v).exists() for r in S.RELEASES for v in S.VIDEOS), "the release videos are not installed")
class Analysis(unittest.TestCase):
    def test_tables_are_what_the_analysis_derives(self):
        analysis = S.Analysis(S.find_ffmpeg(None), tables())
        for video in S.VIDEOS:
            for release in S.DUBS:
                found = tables()[S.VIDEOS[video] + S.SUFFIX[release]]
                self.assertEqual([(a, b) for a, b, _ in found], analysis.cues(video, release), (video, release))

    def test_the_english_timeline_is_recovered_from_the_english_audio(self):
        analysis = S.Analysis(S.find_ffmpeg(None), tables())
        for video in S.VIDEOS:
            english = analysis.timeline[video]
            spans = analysis.spans(video, "en-116")
            for k, ((first, last, _), (a, b)) in enumerate(zip(english, spans)):
                middle = (a + b) / 2
                self.assertTrue(first - 4 <= middle <= last + 4, (video, k, first, last, a, b))


if __name__ == "__main__":
    unittest.main()
