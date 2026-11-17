#!/usr/bin/env python3
"""Inventory of the Smacker videos of the installed game versions.

Parses the SMK header and the frame tables only (no decoding, nothing is played) and prints, per version and video:
size, SHA-256 of the whole file and of its first 65536 bytes (the fingerprint the host uses for the subtitles),
signature, size, frame count, frame rate, duration, audio tracks and a summary of the per-frame sizes.

    py -3 tools/smk_inventory.py [--versions-dir game/versions] [--videos a.smk,b.smk] [--long|--csv] [--frames]
                                 [--compare en-116]
"""
import argparse
import hashlib
import os
import struct
import sys

FINGERPRINT_BYTES = 65536
VIDEOS = ["tpassintro.smk", "newgame.smk", "win.smk", "credits.smk"]
VERSIONS = ["en-116", "ru-116", "es-117", "ptbr-117"]


def frame_rate_ms(raw):
    """Smacker frame-rate field: >0 milliseconds per frame, <0 units of 1/100000 s, 0 ten frames per second."""
    if raw > 0:
        return float(raw)
    if raw < 0:
        return -raw / 100.0
    return 100.0


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    h = {"size": len(data)}
    h["sha256_full"] = hashlib.sha256(data).hexdigest()
    h["sha256_head"] = hashlib.sha256(data[:FINGERPRINT_BYTES]).hexdigest()
    sig = data[:4]
    h["signature"] = sig.decode("latin1")
    if sig not in (b"SMK2", b"SMK4"):
        raise ValueError("%s: not a Smacker file" % path)
    (width, height, frames, rate, flags) = struct.unpack_from("<IIIiI", data, 4)
    audio_max = struct.unpack_from("<7I", data, 24)
    (tree_size, mmap_size, mclr_size, full_size, type_size) = struct.unpack_from("<5I", data, 52)
    audio_rate = struct.unpack_from("<7I", data, 72)
    ring = flags & 1
    count = frames + ring
    pos = 104
    sizes = struct.unpack_from("<%dI" % count, data, pos)
    pos += 4 * count
    types = data[pos:pos + count]
    pos += count
    h.update(width=width, height=height, frames=frames, rate_raw=rate, flags=flags, ring=ring,
             ms=frame_rate_ms(rate), tree_size=tree_size, hdr_audio_max=audio_max)
    h["fps"] = 1000.0 / h["ms"]
    h["duration_s"] = frames * h["ms"] / 1000.0
    tracks = []
    for t in range(7):
        r = audio_rate[t]
        if r & 0x40000000:
            tracks.append({"track": t, "rate": r & 0xFFFFFF, "compressed": bool(r & 0x80000000),
                           "bits": 16 if r & 0x20000000 else 8, "stereo": bool(r & 0x10000000),
                           "max_unpacked": audio_max[t]})
    h["tracks"] = tracks
    h["frame_sizes"] = [s & ~3 for s in sizes]
    h["frame_flags"] = [s & 3 for s in sizes]
    h["frame_types"] = list(types)
    h["data_offset"] = pos + tree_size
    h["file_size_check"] = h["data_offset"] + sum(h["frame_sizes"]) == len(data)
    return h


def frame_chunks(path, h):
    """Yield (frame, palette_bytes, [audio chunk bytes per track present], video_bytes, video_digest, audio_digest)."""
    with open(path, "rb") as f:
        f.seek(h["data_offset"])
        for i, size in enumerate(h["frame_sizes"]):
            raw = f.read(size)
            t = h["frame_types"][i]
            p = 0
            pal = 0
            if t & 1:
                pal = raw[0] * 4
                p = pal
            audio = []
            audio_start = p
            for tr in range(7):
                if t & (2 << tr):
                    (n,) = struct.unpack_from("<I", raw, p)
                    audio.append(n)
                    p += n
            yield (i, pal, audio, size - p, hashlib.sha1(raw[p:]).digest(), hashlib.sha1(raw[audio_start:p]).digest())


def fmt_table(rows, headers):
    widths = [max(len(str(r[i])) for r in rows + [headers]) for i in range(len(headers))]
    out = []
    for r in [headers] + rows:
        out.append("  ".join(str(c).ljust(widths[i]) for i, c in enumerate(r)))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--versions-dir", default=os.path.join(root, "game", "versions"))
    ap.add_argument("--videos", default=",".join(VIDEOS))
    ap.add_argument("--versions", default=",".join(VERSIONS))
    ap.add_argument("--long", action="store_true", help="one field per line instead of the table")
    ap.add_argument("--csv", action="store_true", help="one line per file, semicolon separated")
    ap.add_argument("--frames", action="store_true", help="also print the per-frame size table")
    ap.add_argument("--compare", metavar="VERSION", help="also compare every file with the same video of this version "
                    "(frame by frame: same picture data, same audio chunk)")
    args = ap.parse_args()

    rows = []
    details = {}
    for v in args.versions.split(","):
        for name in args.videos.split(","):
            path = os.path.join(args.versions_dir, v, "data", "menu", name)
            if not os.path.isfile(path):
                print("missing: %s" % path, file=sys.stderr)
                continue
            h = parse(path)
            audio = ",".join("t%d:%dHz/%db/%s/%s" % (t["track"], t["rate"], t["bits"], "stereo" if t["stereo"] else "mono",
                                                     "dpcm" if t["compressed"] else "pcm") for t in h["tracks"]) or "none"
            fs = h["frame_sizes"]
            rows.append([v, name, h["size"], h["sha256_full"], h["sha256_head"], h["signature"], h["width"], h["height"],
                         h["frames"], "%d = %.2f ms (%.3f fps)" % (h["rate_raw"], h["ms"], h["fps"]),
                         "%.2f s" % h["duration_s"], audio, "%d..%d" % (min(fs), max(fs)),
                         "ok" if h["file_size_check"] else "MISMATCH"])
            details[(v, name)] = (path, h)
    headers = ["version", "video", "size", "sha256 (whole file)", "sha256 (first 64 KiB)", "sig", "w", "h", "frames",
               "rate field", "duration", "audio", "frame bytes", "tables"]
    if args.csv:
        print(";".join(headers))
        for r in rows:
            print(";".join(str(c) for c in r))
    elif args.long:
        for r in rows:
            print("\n".join("%-22s %s" % (hd, c) for hd, c in zip(headers, r)))
            print()
    else:
        print(fmt_table(rows, headers))
    if args.compare:
        print()
        for (v, name), (path, h) in details.items():
            ref = details.get((args.compare, name))
            if v == args.compare or not ref:
                continue
            rh = ref[1]
            same_head = [h[k] for k in ("width", "height", "frames", "rate_raw", "flags")] == \
                [rh[k] for k in ("width", "height", "frames", "rate_raw", "flags")]
            a = list(frame_chunks(path, h))
            b = list(frame_chunks(ref[0], rh))
            n = min(len(a), len(b))
            same_pic = sum(1 for x, y in zip(a, b) if x[4] == y[4])
            same_aud = sum(1 for x, y in zip(a, b) if x[5] == y[5])
            same_sizes = sum(1 for x, y in zip(a, b) if x[2] == y[2])
            print("%s %s vs %s: same size/frames/rate/flags %s; picture data identical in %d/%d frames; audio chunk "
                  "identical in %d/%d (same chunk size in %d)" % (v, name, args.compare, same_head, same_pic, n, same_aud, n,
                                                                   same_sizes))
    if args.frames:
        for (v, name), (path, h) in details.items():
            print("\n== %s %s: frame, frame bytes, palette bytes, audio chunk bytes, picture bytes" % (v, name))
            for i, pal, audio, video, _, _ in frame_chunks(path, h):
                print("%d %d %d %s %d" % (i, h["frame_sizes"][i], pal, "/".join(map(str, audio)) or "-", video))


if __name__ == "__main__":
    main()
