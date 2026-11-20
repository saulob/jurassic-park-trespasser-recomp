#!/usr/bin/env python3
"""Speech timing of the localized Smacker videos: where each dub speaks, and the cue table that follows from it.

Development aid only: nothing here runs in the game. The audio of each release's tpassintro.smk, newgame.smk and
win.smk is decoded offline (ffmpeg, never played) and the voices are separated from the music the four releases share:
the music is the same in all of them (the Spanish and Brazilian copies are shifted and louder by a constant), so the
difference of every pair of recordings holds only voices, and the energy of each voice follows from the six pairs.
The speech segments are then assigned, in order, to the cues of the English timeline, which fixes the narrative
(16, 9 and 7 cues): by the time the cue's text takes to say, the pause before it, and the spacing of the cue starts
the English timeline has. The result is a table of frames with one reading margin for every release.

    py -3 tools/smk_speech.py --report [--video newgame] [--release ru-116]   segments, pauses and speech spans
    py -3 tools/smk_speech.py --cues [--json out.json]                         the cue frames (margins applied)
    py -3 tools/smk_speech.py --c-source                                       the cue arrays of jpt_video_subtitles.c
    py -3 tools/smk_speech.py --check                                          compare with jpt_video_subtitles.c

Needs numpy, scipy and ffmpeg (--ffmpeg or PATH) and the game files under game/versions.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import percentile_filter
from scipy.signal import butter, fftconvolve, sosfilt

ROOT = Path(__file__).resolve().parents[1]
VERSIONS_DIR = ROOT / "game" / "versions"
SOURCE = ROOT / "src" / "common" / "jpt_video_subtitles.c"
TRANSLATIONS = ROOT / "assets" / "translations"

RELEASES = ["en-116", "ru-116", "es-117", "ptbr-117"]
DUBS = RELEASES[1:]
LANGUAGE = {"en-116": "en", "ru-116": "ru", "es-117": "es", "ptbr-117": "pt-BR"}
VIDEOS = {"tpassintro": "k_intro", "newgame": "k_new_game", "win": "k_win"}
SUFFIX = {"en-116": "", "ru-116": "_ru", "es-117": "_es", "ptbr-117": "_ptbr"}

RATE = 22050
FPS = 15
NFFT = 1024
HOP = 245
HOPS_PER_FRAME = RATE / FPS / HOP
BAND = (200, 3500)
SMOOTH_HOPS = 5
LEVEL_WINDOW = 450
LEVEL_PERCENTILE = 90
HIGH, LOW = 0.33, 0.19
MIN_SEGMENT_HOPS = 8

BLOCK_GAP_FRAMES = 20
EDGE_MARGIN_FRAMES = 10
END_MARGIN_FRAMES = 4
SYLLABLE_OFFSET_S = 0.2
RATIO_SIGMA = 0.3
PAUSE_WEIGHT = 1.0
SKIP_PER_S = 6.0
SPACING_SLACK = 8.0
SPACING_WEIGHT = 2.0
SPACING_CAP = 2.5

LEAD_FRAMES = 6
TRAIL_FRAMES = 3
MIN_CUE_FRAMES = 15

VOWELS = {"en": "aeiouy", "es": "aeiouáéíóúü", "pt-BR": "aeiouáéíóúâêôãõàü", "ru": "аеёиоуыэюя"}
NUMBER_SYLLABLES = {"en": (2, 5), "es": (3, 9), "pt-BR": (3, 10), "ru": (3, 12)}


def video_path(release, video):
    return VERSIONS_DIR / release / "data" / "menu" / (video + ".smk")


def read_source():
    return SOURCE.read_text(encoding="utf-8")


def parse_cue_arrays(source):
    macros = dict(re.findall(r'#define\s+(K_\w+)\s+("[^"]*")', source))
    arrays = {}
    for name, body in re.findall(r"static const jvs_cue_t (k_\w+)\[\] = \{(.*?)\n\};", source, re.S):
        rows = []
        for first, last, key in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(K_\w+|\"[^\"]*\")\s*\}", body):
            rows.append((int(first), int(last), macros.get(key, key).strip('"')))
        arrays[name] = rows
    return arrays


def english_timeline(arrays):
    return {video: arrays[array] for video, array in VIDEOS.items()}


def translated_text(release):
    text = {}
    path = TRANSLATIONS / ("%s.interface.txt" % LANGUAGE[release])
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("Video::"):
            key, value = line.split("\t", 1)
            text[key] = value
    return text


def syllables(text, language):
    lowered = text.lower()
    count = 0
    for digits in re.findall(r"\d+", lowered):
        count += NUMBER_SYLLABLES[language][0 if len(digits) <= 2 else 1]
    lowered = re.sub(r"\d+", " ", lowered)
    previous = False
    for ch in lowered:
        vowel = ch in VOWELS[language]
        if vowel and not previous:
            count += 1
        previous = vowel
    return max(count, 1)


def decode_audio(ffmpeg, path):
    command = [ffmpeg, "-v", "error", "-i", str(path), "-vn", "-f", "s16le", "-ar", str(RATE), "-ac", "2", "-"]
    result = subprocess.run(command, capture_output=True)
    if result.returncode:
        raise RuntimeError("ffmpeg could not decode %s: %s" % (path, result.stderr.decode(errors="replace")[:200]))
    pcm = np.frombuffer(result.stdout, dtype="<i2").astype(np.float64)
    return pcm.reshape(-1, 2).mean(axis=1)


def shifted(x, lag):
    out = np.zeros_like(x)
    if lag >= 0:
        out[: len(x) - lag] = x[lag:]
    else:
        out[-lag:] = x[: len(x) + lag]
    return out


def match_to_english(english, other, first_frame, last_frame):
    """Lag and gain that make `other` the same music as `english`: other[n + lag] * (1 / gain) ~ english[n]."""
    block = RATE * 2
    sos = butter(4, [60, 2500], "band", fs=RATE, output="sos")
    ref, oth = sosfilt(sos, english), sosfilt(sos, other)
    first, last = first_frame * RATE // FPS, last_frame * RATE // FPS
    found = []
    for start in range(0, min(len(ref), len(oth)) - block, block):
        a, b = ref[start:start + block], oth[start:start + block]
        corr = fftconvolve(b, a[::-1], mode="full")
        mid = len(a) - 1
        window = corr[mid - 4000:mid + 4001]
        k = int(np.argmax(np.abs(window)))
        if abs(window[k]) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9) > 0.97:
            found.append((max(0, first - (start + block), start - last), k - 4000))
    if not found:
        raise RuntimeError("no stretch of the music matches the English release")
    lags = [lag for _, lag in sorted(found)[:4]]
    lag = max(sorted(set(lags), key=abs), key=lags.count)
    aligned = shifted(other, lag)
    gains = []
    for start in range(0, min(len(english), len(aligned)) - RATE * 2, RATE * 2):
        a, b = english[start:start + RATE * 2], aligned[start:start + RATE * 2]
        if np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9) > 0.97:
            gains.append(np.dot(a, b) / np.dot(a, a))
    return lag, float(np.median(gains)) if gains else 1.0


def spectra(x):
    window = np.hanning(NFFT)
    count = (len(x) - NFFT) // HOP
    index = np.arange(NFFT)[None, :] + HOP * np.arange(count)[:, None]
    bins = np.fft.rfft(x[index] * window, axis=1)
    lo, hi = int(BAND[0] / (RATE / NFFT)), int(BAND[1] / (RATE / NFFT))
    return bins[:, lo:hi]


def voice_energies(signals):
    """The energy of each release's voice per hop, from the energy of the difference of every pair of recordings."""
    names = list(signals)
    spec = {k: spectra(s) for k, s in signals.items()}
    pair = {}
    for i in range(4):
        for j in range(i + 1, 4):
            pair[(i, j)] = pair[(j, i)] = (np.abs(spec[names[i]] - spec[names[j]]) ** 2).sum(axis=1)
    kernel = np.ones(SMOOTH_HOPS) / SMOOTH_HOPS
    out = {}
    for i in range(4):
        others = [k for k in range(4) if k != i]
        estimates = []
        for a in range(3):
            for b in range(a + 1, 3):
                j, k = others[a], others[b]
                estimates.append((pair[(i, j)] + pair[(i, k)] - pair[(j, k)]) / 2.0)
        out[names[i]] = np.convolve(np.maximum(np.mean(estimates, axis=0), 0), kernel, mode="same")
    return out


def voice_levels(energy):
    amplitude = np.sqrt(energy)
    return amplitude / (percentile_filter(amplitude, LEVEL_PERCENTILE, size=LEVEL_WINDOW, mode="nearest") + 1e-6)


def hysteresis(level):
    active = np.zeros(len(level), bool)
    on = False
    for i, value in enumerate(level):
        if not on and value > HIGH:
            on = True
        elif on and value < LOW:
            on = False
        active[i] = on
    return active


def segments(active, close_hops=1):
    runs, i = [], 0
    while i < len(active):
        if active[i]:
            j = i
            while j < len(active) and active[j]:
                j += 1
            runs.append([i, j])
            i = j
        else:
            i += 1
    merged = []
    for run in runs:
        if merged and run[0] - merged[-1][1] <= close_hops:
            merged[-1][1] = run[1]
        else:
            merged.append(run)
    return [(a, b) for a, b in merged if b - a >= MIN_SEGMENT_HOPS]


def blocks(timeline):
    groups = [[0]]
    for k in range(1, len(timeline)):
        if timeline[k][0] - timeline[k - 1][1] > BLOCK_GAP_FRAMES:
            groups.append([])
        groups[-1].append(k)
    return groups


def window_hops(timeline, group):
    last = group[-1] == len(timeline) - 1
    first = max(0, int((timeline[group[0]][0] - EDGE_MARGIN_FRAMES) * HOPS_PER_FRAME))
    end = int((timeline[group[-1]][1] + (END_MARGIN_FRAMES if last else EDGE_MARGIN_FRAMES)) * HOPS_PER_FRAME)
    return first, end


def assign_block(level, group, timeline, weights, rate, spaced):
    first, end = window_hops(timeline, group)
    active = hysteresis(level)
    active[:first] = False
    active[end:] = False
    runs = segments(active)
    n, m = len(group), len(runs)
    if m < n:
        raise RuntimeError("fewer speech segments (%d) than cues (%d)" % (m, n))
    start = [a / HOPS_PER_FRAME for a, _ in runs]
    cumulative = np.concatenate([[0], np.cumsum([(b - a) / 90.0 for a, b in runs])])
    gap = [(runs[i + 1][0] - runs[i][1]) / 90.0 for i in range(m - 1)]
    sigma2 = 2 * RATIO_SIGMA * RATIO_SIGMA
    inf = 1e18
    best = np.full((n + 1, m + 1), inf)
    back = np.zeros((n + 1, m + 1), int)
    for j in range(m):
        best[0][j] = SKIP_PER_S * cumulative[j]
    for k in range(n):
        for j in range(k, m):
            if best[k][j] >= inf:
                continue
            expected = rate * weights[group[k]] + SYLLABLE_OFFSET_S
            for i in range(j + 1, m + 1):
                cost = math.log(max(cumulative[i] - cumulative[j], 0.05) / expected) ** 2 / sigma2
                if k < n - 1:
                    if i >= m:
                        continue
                    cost -= PAUSE_WEIGHT * math.log(1.0 + gap[i - 1] * 12.0)
                    if spaced is not None:
                        drift = (start[i] - start[j]) - (spaced[group[k + 1]] - spaced[group[k]])
                        over = max(0.0, abs(drift) - SPACING_SLACK) / 8.0
                        cost += min(SPACING_WEIGHT * over * over, SPACING_CAP)
                total = best[k][j] + cost
                if k == n - 1:
                    total += SKIP_PER_S * (cumulative[m] - cumulative[i])
                    slot = n
                else:
                    slot = k + 1
                if total < best[slot][i]:
                    best[slot][i] = total
                    back[slot][i] = j
    stop = int(np.argmin(best[n]))
    firsts, stops = [0] * n, [0] * n
    i = stop
    for k in range(n - 1, -1, -1):
        j = back[n][stop] if k == n - 1 else back[k + 1][i]
        firsts[k], stops[k] = j, i
        i = j
    return [(runs[firsts[k]][0] / HOPS_PER_FRAME, runs[stops[k] - 1][1] / HOPS_PER_FRAME) for k in range(n)]


def speech_spans(level, timeline, weights, spaced):
    """(first, last) frame of the speech of every cue: the stretch of speech segments the assignment gave it."""
    active = hysteresis(level)
    groups = blocks(timeline)
    seconds = 0.0
    total_weight = 0.0
    for group in groups:
        first, end = window_hops(timeline, group)
        seconds += active[first:end].sum() / 90.0
        total_weight += sum(weights[k] for k in group)
    rate = (seconds - SYLLABLE_OFFSET_S * len(timeline)) / total_weight
    spans = [None] * len(timeline)
    for group in groups:
        for k, span in zip(group, assign_block(level, group, timeline, weights, rate, spaced)):
            spans[k] = span
    return spans


def margins(spans, frames):
    """Cue frames: a reading lead before the speech and a trail after it (split at a shorter pause), one second at least."""
    first = [int(math.floor(a)) - LEAD_FRAMES for a, _ in spans]
    last = [int(math.ceil(b)) + TRAIL_FRAMES for _, b in spans]
    for k in range(1, len(spans)):
        if first[k] <= last[k - 1]:
            before, after = int(math.ceil(spans[k - 1][1])), int(math.floor(spans[k][0]))
            cut = before + max(0, after - before) * TRAIL_FRAMES // (LEAD_FRAMES + TRAIL_FRAMES)
            last[k - 1], first[k] = cut, cut + 1
    first[0] = max(first[0], 0)
    last[-1] = min(last[-1], frames - 1)
    for k in range(len(spans)):
        room = (first[k + 1] - 1) if k + 1 < len(spans) else frames - 1
        last[k] = max(last[k], min(first[k] + MIN_CUE_FRAMES - 1, room))
    return [(a, b) for a, b in zip(first, last)]


class Analysis:
    def __init__(self, ffmpeg, arrays):
        self.ffmpeg = ffmpeg
        self.timeline = english_timeline(arrays)
        self.text = {release: translated_text(release) for release in RELEASES}
        self.keys = {video: [row[2] for row in rows] for video, rows in self.timeline.items()}
        self.cache = {}

    def levels(self, video):
        if video not in self.cache:
            rows = self.timeline[video]
            audio = {release: decode_audio(self.ffmpeg, video_path(release, video)) for release in RELEASES}
            n = min(len(a) for a in audio.values())
            english = audio["en-116"][:n]
            signals = {"en-116": english}
            for release in DUBS:
                x = audio[release][:n]
                lag, gain = match_to_english(english, x, max(0, rows[0][0] - 15), rows[-1][1] + 10)
                x = shifted(x, lag)
                signals[release] = x / gain if release in ("es-117", "ptbr-117") else x
            energy = voice_energies(signals)
            self.cache[video] = {release: voice_levels(energy[release]) for release in RELEASES}
        return self.cache[video]

    def weights(self, video, release):
        return [syllables(self.text[release].get(key, key), LANGUAGE[release]) for key in self.keys[video]]

    def spans(self, video, release):
        rows = self.timeline[video]
        spaced = [row[0] for row in rows] if release != "en-116" else None
        return speech_spans(self.levels(video)[release], rows, self.weights(video, release), spaced)

    def cues(self, video, release):
        frames = frame_count(video_path(release, video))
        return margins(self.spans(video, release), frames)


def frame_count(path):
    with open(path, "rb") as f:
        head = f.read(16)
    return int.from_bytes(head[12:16], "little")


def find_ffmpeg(option):
    found = option or os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not found:
        sys.exit("ffmpeg not found: pass --ffmpeg or put it on PATH")
    return found


def print_report(analysis, videos, releases):
    for video in videos:
        rows = analysis.timeline[video]
        print("== %s: %d cues, English timeline %d-%d" % (video, len(rows), rows[0][0], rows[-1][1]))
        levels = analysis.levels(video)
        for release in releases:
            active = hysteresis(levels[release])
            first, end = window_hops(rows, [0, len(rows) - 1])
            active[:first] = False
            active[end:] = False
            pieces = segments(active)
            line, previous = [], None
            for a, b in pieces:
                if previous is not None and (a - previous) / 90.0 >= 0.25:
                    line.append("|%.2fs|" % ((a - previous) / 90.0))
                line.append("%.0f-%.0f" % (a / HOPS_PER_FRAME, b / HOPS_PER_FRAME))
                previous = b
            print("%s speech %.0f-%.0f in %d segments (pauses of 0.25 s or more shown)" % (
                release, pieces[0][0] / HOPS_PER_FRAME, pieces[-1][1] / HOPS_PER_FRAME, len(pieces)))
            print("   " + " ".join(line))
            spans = analysis.spans(video, release)
            cues = analysis.cues(video, release)
            for k, (span, cue) in enumerate(zip(spans, cues)):
                print("   cue %2d speech %6.1f-%-6.1f cue %4d-%-4d" % (k + 1, span[0], span[1], cue[0], cue[1]))


def c_source(analysis, videos):
    lines = []
    for video in videos:
        for release in DUBS:
            array = VIDEOS[video] + SUFFIX[release]
            lines.append("static const jvs_cue_t %s[] = {" % array)
            for (first, last), key in zip(analysis.cues(video, release), analysis.keys[video]):
                lines.append("    { %d, %d, %s }," % (first, last, key_macro(video, analysis.keys[video].index(key))))
            lines.append("};")
            lines.append("")
    return "\n".join(lines)


def key_macro(video, index):
    return "%s_%d" % (VIDEOS[video].upper(), index)


def check(analysis, videos, arrays):
    bad = 0
    for video in videos:
        for release in DUBS:
            expected = analysis.cues(video, release)
            found = arrays.get(VIDEOS[video] + SUFFIX[release])
            if found is None:
                print("MISSING %s" % (VIDEOS[video] + SUFFIX[release]))
                bad += 1
            elif [(a, b) for a, b, _ in found] != expected:
                print("DIFFERENT %s" % (VIDEOS[video] + SUFFIX[release]))
                bad += 1
    print("cue tables match the analysis" if not bad else "%d tables differ" % bad)
    return 1 if bad else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--video", choices=list(VIDEOS))
    parser.add_argument("--release", choices=RELEASES)
    parser.add_argument("--ffmpeg")
    parser.add_argument("--json")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--report", action="store_true")
    mode.add_argument("--cues", action="store_true")
    mode.add_argument("--c-source", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    videos = [args.video] if args.video else list(VIDEOS)
    releases = [args.release] if args.release else RELEASES
    arrays = parse_cue_arrays(read_source())
    analysis = Analysis(find_ffmpeg(args.ffmpeg), arrays)
    if args.report:
        print_report(analysis, videos, releases)
    elif args.cues:
        table = {video: {release: analysis.cues(video, release) for release in DUBS} for video in videos}
        if args.json:
            Path(args.json).write_text(json.dumps(table, indent=1), encoding="utf-8")
        for video, per_release in table.items():
            for release, cues in per_release.items():
                print("%-10s %-8s %s" % (video, release, " ".join("%d-%d" % c for c in cues)))
    elif args.c_source:
        print(c_source(analysis, videos))
    else:
        return check(analysis, videos, arrays)
    return 0


if __name__ == "__main__":
    sys.exit(main())
