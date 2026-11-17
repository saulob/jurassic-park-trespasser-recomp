#!/usr/bin/env python3
"""Maps the guest addresses of the 1.0.116 engine (the lifted one) onto another build of the same game.

    py -3 tools/guest_map.py <profile id>... [--list] [--write]

Both executables are decoded (pcrecomp's function catalogue gives the bounds), every instruction is normalised
(absolute image addresses and branch displacements masked), functions are aligned in link order and through their
call graph, and the instructions of each aligned pair are aligned again. An address of src/host or src/common is then
resolved as one of:

  identical  its function is byte-identical (after masking) and the address is at the same offset
  aligned    its instruction has an equal instruction in the aligned function of the other build
  data       a global that the aligned instructions of both builds reference as a pair (majority vote, no conflict)
  unresolved anything else: the function has no counterpart, the instruction sits in a changed region, the global is
             never referenced by aligned code

--write stores config/guest_maps/<id>.json (every address with its class and evidence). Read only otherwise.
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import json
import pickle
import re
import struct
import sys
from pathlib import Path

from common import ANALYSIS_DIR, CONFIG_DIR, GUEST_MAPS_DIR, PCRECOMP_DIR, PROFILES_DIR, ROOT, TARGET, sha256_of

ADDRESS = re.compile(r"(?:0x|sub_|lift_hook_|GUEST_OVERRIDE\(|LIFT_HOOK\()([0-9A-Fa-f]{6,8})[uUlL]*\b")
GUEST_LO, GUEST_HI = 0x401000, 0x750000
MAPS = ANALYSIS_DIR / "maps"


def read_pe(path):
    b = Path(path).read_bytes()
    o = struct.unpack_from("<I", b, 0x3C)[0]
    count, opt_size = struct.unpack_from("<H", b, o + 6)[0], struct.unpack_from("<H", b, o + 20)[0]
    opt = o + 24
    base, size = struct.unpack_from("<I", b, opt + 28)[0], struct.unpack_from("<I", b, opt + 56)[0]
    sections = []
    for i in range(count):
        s = opt + opt_size + 40 * i
        name = b[s:s + 8].rstrip(b"\0").decode()
        vsize, va, rsize, raw = struct.unpack_from("<IIII", b, s + 8)
        sections.append((name, base + va, max(vsize, rsize), raw))
    return b, base, size, sections


def profile_exe(profile_id):
    from_version = ROOT / "game" / "versions" / profile_id / "trespass.exe"
    if from_version.exists():
        return from_version
    sys.exit(f"{from_version} is missing: import the {profile_id} disc with the launcher, or pass --exe")


class Image:
    """Decoded functions of one executable: instructions as (address, masked bytes), references and calls."""

    def __init__(self, exe, catalog, rtti=None):
        from capstone import CS_ARCH_X86, CS_MODE_32, Cs
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        self.exe = Path(exe)
        self.data, self.base, self.size, self.sections = read_pe(exe)
        self.lo, self.hi = self.base, self.base + self.size
        self.funcs = {}
        for f in catalog:
            a, e = f["address"], f["end"]
            o = self.offset(a)
            if o is None or not a < e <= a + 200000:
                continue
            insns, refs, calls = [], [], []
            for ins in md.disasm(self.data[o:o + e - a], a):
                raw = bytearray(ins.bytes)
                kind = None
                for op in ins.operands:
                    if op.type == X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFF
                        if ins.mnemonic == "call" or ins.mnemonic.startswith("j") or ins.mnemonic.startswith("loop"):
                            n = 4 if ins.size >= 5 and ins.mnemonic in ("call", "jmp") else (4 if ins.size >= 6 else 1)
                            raw[-n:] = bytes(n)
                            if ins.mnemonic == "call":
                                calls.append((len(insns), v))
                        elif self.lo <= v < self.hi:
                            p = bytes(ins.bytes).rfind(struct.pack("<I", v))
                            if p >= 0:
                                raw[p:p + 4] = b"\xAA" * 4
                                refs.append((len(insns), v))
                    elif op.type == X86_OP_MEM:
                        v = op.mem.disp & 0xFFFFFFFF
                        if self.lo <= v < self.hi:
                            p = bytes(ins.bytes).find(struct.pack("<I", v))
                            if p >= 0:
                                raw[p:p + 4] = b"\xBB" * 4
                                refs.append((len(insns), v))
                insns.append((ins.address, bytes(raw)))
            self.funcs[a] = {"end": e, "insns": insns, "refs": refs, "calls": calls,
                             "hash": hashlib.sha1(b"".join(t for _, t in insns)).hexdigest()}
        self.starts = sorted(self.funcs)
        self.vtables = {}
        for name, c in (rtti or {}).get("classes", {}).items():
            va, count = int(c["vtable"], 16), c["slots"]
            o = self.offset(va)
            if o is not None and o + 4 * count <= len(self.data):
                self.vtables[name] = (va, list(struct.unpack_from(f"<{count}I", self.data, o)))

    def offset(self, va):
        for _, sva, span, raw in self.sections:
            if sva <= va < sva + span:
                return raw + va - sva
        return None

    def function_of(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and self.starts[i] <= va < self.funcs[self.starts[i]]["end"]:
            return self.starts[i]
        return None


def catalog_path(digest):
    """The seeded catalogue of the build (analysis/<id>/functions.json), else a plain one under analysis/catalogs."""
    if digest == TARGET["sha256"]:
        return ROOT / "analysis" / "functions.json"
    for profile in sorted(PROFILES_DIR.glob("*.json")):
        if json.loads(profile.read_text()).get("sha256") == digest:
            return ROOT / "analysis" / profile.stem / "functions.json"
    return ANALYSIS_DIR / "catalogs" / f"{digest}.functions.json"


def catalog_for(exe, digest):
    path = catalog_path(digest)
    if not path.exists():
        import subprocess
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"[*] cataloguing {exe} (about 15 minutes)", flush=True)
        subprocess.run([sys.executable, str(PCRECOMP_DIR / "tools" / "disasm" / "disasm32.py"), str(exe), "-o", str(path)],
                       check=True, stdout=subprocess.DEVNULL)
    return json.loads(path.read_text())["functions"]


def load_image(exe):
    digest = sha256_of(exe)
    catalog = catalog_path(digest)
    stamp = catalog.stat().st_size if catalog.exists() else 0
    cache = MAPS / f"{digest}.{stamp}.v2.image.pkl"
    if cache.exists():
        return pickle.loads(cache.read_bytes())
    rtti_file = catalog.parent / "rtti.json"
    image = Image(exe, catalog_for(exe, digest), json.loads(rtti_file.read_text()) if rtti_file.exists() else None)
    MAPS.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(pickle.dumps(image, protocol=4))
    return image


class Mapper:
    def __init__(self, a, b):
        self.a, self.b = a, b
        self.match = {}       # function start in a -> in b
        self.reverse = {}
        self.insn = {}        # function start in a -> {address in a -> address in b} for equal instructions
        self.data_votes = collections.defaultdict(collections.Counter)
        self.slot_agree, self.slot_conflict = 0, []
        self.how = {}

    def pair(self, fa, fb, how):
        if fa in self.match or fb in self.reverse:
            return False
        self.match[fa] = fb
        self.reverse[fb] = fa
        self.how[fa] = how
        return True

    def align_sequence(self):
        ka, kb = self.a.starts, self.b.starts
        sm = difflib.SequenceMatcher(None, [self.a.funcs[k]["hash"] for k in ka], [self.b.funcs[k]["hash"] for k in kb],
                                     autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1):
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    self.pair(ka[i], kb[j], "hash" if tag == "equal" else "gap")

    def align_instructions(self, fa):
        fb = self.match[fa]
        A, B = self.a.funcs[fa], self.b.funcs[fb]
        ta, tb = [t for _, t in A["insns"]], [t for _, t in B["insns"]]
        if A["hash"] == B["hash"]:
            blocks = [(0, 0, len(ta))]
        else:
            sm = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
            blocks = [(m.a, m.b, m.size) for m in sm.get_matching_blocks()]
        table = {}
        index_b = {}
        for i0, j0, n in blocks:
            for k in range(n):
                table[i0 + k] = j0 + k
        self.insn[fa] = {A["insns"][i][0]: B["insns"][j][0] for i, j in table.items()}
        ra = collections.defaultdict(list)
        rb = collections.defaultdict(list)
        for i, v in A["refs"]:
            ra[i].append(v)
        for j, v in B["refs"]:
            rb[j].append(v)
        for i, j in table.items():
            for x, y in zip(ra.get(i, ()), rb.get(j, ())):
                self.data_votes[x][y] += 1
        ca = {i: t for i, t in A["calls"]}
        cb = {j: t for j, t in B["calls"]}
        grown = 0
        for i, j in table.items():
            if i in ca and j in cb and ca[i] in self.a.funcs and cb[j] in self.b.funcs and self.pair(ca[i], cb[j], "call"):
                grown += 1
        return grown

    def unique_hash(self):
        ha, hb = collections.defaultdict(list), collections.defaultdict(list)
        for k in self.a.starts:
            if k not in self.match:
                ha[self.a.funcs[k]["hash"]].append(k)
        for k in self.b.starts:
            if k not in self.reverse:
                hb[self.b.funcs[k]["hash"]].append(k)
        grown = 0
        for h, la in ha.items():
            lb = hb.get(h, [])
            if len(la) == 1 and len(lb) == 1:
                grown += self.pair(la[0], lb[0], "hash")
        return grown

    def similar(self, fa, fb):
        A, B = self.a.funcs[fa]["insns"], self.b.funcs[fb]["insns"]
        if not A or not B or not 0.5 <= len(A) / len(B) <= 2.0:
            return 0.0
        return difflib.SequenceMatcher(None, [t for _, t in A], [t for _, t in B], autojunk=False).ratio()

    def gaps(self):
        """Unmatched runs between matched neighbours in link order pair one to one when both builds have as many."""
        ka, kb = self.a.starts, self.b.starts
        pos_b = {k: i for i, k in enumerate(kb)}
        grown, i = 0, 0
        while i < len(ka):
            if ka[i] in self.match:
                i += 1
                continue
            j = i
            while j < len(ka) and ka[j] not in self.match:
                j += 1
            if 0 < i and j < len(ka):
                p, n = pos_b[self.match[ka[i - 1]]], pos_b[self.match[ka[j]]]
                free = [k for k in kb[p + 1:n] if k not in self.reverse]
                run = ka[i:j]
                if p < n and len(free) == len(run):
                    for fa, fb in zip(run, free):
                        if self.similar(fa, fb) >= 0.55 or len(self.a.funcs[fa]["insns"]) <= 4:
                            grown += self.pair(fa, fb, "gap")
            i = j
        return grown

    def neighbours(self):
        """A function right after (or before) a matched pair, whose counterpart is the unmatched function right after
        (before) the other one, when they are alike."""
        ka, kb = self.a.starts, self.b.starts
        pos_a = {k: i for i, k in enumerate(ka)}
        pos_b = {k: i for i, k in enumerate(kb)}
        grown = 0
        for fa, fb in list(self.match.items()):
            for step in (1, -1):
                ia, ib = pos_a[fa] + step, pos_b[fb] + step
                if 0 <= ia < len(ka) and 0 <= ib < len(kb) and ka[ia] not in self.match and kb[ib] not in self.reverse:
                    if self.a.funcs[ka[ia]]["hash"] == self.b.funcs[kb[ib]]["hash"] or self.similar(ka[ia], kb[ib]) >= 0.8:
                        grown += self.pair(ka[ia], kb[ib], "neighbour")
        return grown

    def by_similarity(self, threshold=0.75, margin=0.08):
        """Modified functions: the unmatched candidate of about the same size with the highest, clearly best ratio."""
        free_b = [k for k in self.b.starts if k not in self.reverse]
        by_size = collections.defaultdict(list)
        for k in free_b:
            by_size[len(self.b.funcs[k]["insns"]).bit_length()].append(k)
        counters = {k: collections.Counter(t for _, t in self.b.funcs[k]["insns"]) for k in free_b}
        grown = 0
        for fa in self.a.starts:
            if fa in self.match or len(self.a.funcs[fa]["insns"]) < 12:
                continue
            n = len(self.a.funcs[fa]["insns"]).bit_length()
            ca = collections.Counter(t for _, t in self.a.funcs[fa]["insns"])
            best = []
            for size in (n - 1, n, n + 1):
                for fb in by_size.get(size, ()):
                    if fb in self.reverse:
                        continue
                    cb = counters[fb]
                    shared = sum((ca & cb).values())
                    if shared / max(sum(ca.values()), sum(cb.values())) < threshold:
                        continue
                    best.append((self.similar(fa, fb), fb))
            best.sort(reverse=True)
            if best and best[0][0] >= threshold and (len(best) == 1 or best[0][0] - best[1][0] >= margin):
                grown += self.pair(fa, best[0][1], "similarity")
        return grown

    def vtable_slots(self):
        """A class whose matched virtual slots all agree between the builds: its unmatched slots pair by index. A class
        with any disagreement (a virtual added or removed shifts the slots) is left to the code itself."""
        grown = 0
        for name, (va, slots) in self.a.vtables.items():
            other = self.b.vtables.get(name)
            if not other or len(other[1]) != len(slots):
                continue
            agree = conflict = 0
            free = []
            for fa, fb in zip(slots, other[1]):
                if fa not in self.a.funcs or fb not in self.b.funcs:
                    continue
                if fa in self.match or fb in self.reverse:
                    if self.match.get(fa) == fb:
                        agree += 1
                    else:
                        conflict += 1
                else:
                    free.append((fa, fb))
            if conflict:
                self.slot_conflict.append((name, conflict, agree))
                continue
            self.slot_agree += agree
            if agree:
                self.data_votes[va][other[0]] += 1000
                for fa, fb in free:
                    grown += self.pair(fa, fb, "vtable")
        return grown

    def propagate(self, done):
        grown = 0
        pending = [f for f in self.match if f not in done]
        while pending:
            fa = pending.pop()
            if fa in done:
                continue
            done.add(fa)
            before = set(self.match)
            self.align_instructions(fa)
            added = set(self.match) - before
            grown += len(added)
            pending.extend(added)
        return grown

    def run(self):
        done = set()
        self.unique_hash()
        self.align_sequence()
        while True:
            self.propagate(done)
            grown = self.unique_hash() + self.vtable_slots() + self.gaps() + self.neighbours()
            if not grown:
                grown = self.by_similarity()
            if not grown and not [f for f in self.match if f not in done]:
                break
        return self

    def data(self, va):
        votes = self.data_votes.get(va)
        if not votes:
            return None
        ranked = votes.most_common(2)
        best, n = ranked[0]
        second = ranked[1][1] if len(ranked) > 1 else 0
        return best, n, second

    def proven(self, fa):
        """A matched function is proven when its body is identical or the match rests on the code or the class: its
        callers' aligned call, or the same virtual slot of a class whose other slots agree. Neighbourhood and similarity
        only suggest."""
        fb = self.match[fa]
        return self.a.funcs[fa]["hash"] == self.b.funcs[fb]["hash"] or self.how.get(fa) in ("hash", "call", "vtable")

    def resolve(self, va):
        fa = self.a.function_of(va)
        if fa is not None:
            if fa not in self.match:
                return {"class": "unresolved", "why": "function has no counterpart", "function": fa}
            fb = self.match[fa]
            same = self.a.funcs[fa]["hash"] == self.b.funcs[fb]["hash"]
            kind = "identical" if same else "aligned" if self.proven(fa) else "candidate"
            via = self.how.get(fa)
            if va == fa:
                return {"class": kind, "to": fb, "function": fa, "via": via}
            mapped = self.insn.get(fa, {}).get(va)
            if mapped is None:
                return {"class": "unresolved", "why": "instruction in a changed region", "function": fa, "to_function": fb}
            return {"class": kind, "to": mapped, "function": fa, "via": via}
        vote = self.data(va)
        if vote is None:
            return {"class": "unresolved", "why": "no aligned code references it"}
        best, n, second = vote
        if second and n < 4 * second:
            return {"class": "unresolved", "why": f"conflicting pairs ({n} against {second})", "to": best, "votes": n}
        return {"class": "data", "to": best, "votes": n}


COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
COLOUR_FILES = {"src/host/pad_glyphs.c", "src/host/guest_profile.c"}


def excluded_literals():
    path = CONFIG_DIR / "guest_va_exclude.json"
    items = json.loads(path.read_text(encoding="utf-8"))["literals"] if path.exists() else []
    return {(i["file"], i["literal"].lower()) for i in items}


STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def decisions_for(profile):
    path = CONFIG_DIR / "guest_decisions" / f"{profile}.json"
    if not path.exists():
        return {}
    return {int(d["va"], 16): d for d in json.loads(path.read_text(encoding="utf-8"))["decisions"]}


def host_addresses():
    """Guest addresses the host's code uses (comments are not code; pad_glyphs.c only holds RGB colours and guest_profile.c only labels the profiler buckets)."""
    refs = collections.defaultdict(list)
    skip = excluded_literals()
    for folder in ("src/host", "src/common"):
        for path in sorted((ROOT / folder).rglob("*")):
            rel = path.relative_to(ROOT).as_posix()
            if path.suffix not in (".c", ".h", ".cpp") or rel in COLOUR_FILES or "/profiles/" in rel:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            code = COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
            code = STRING.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), code)
            for n, line in enumerate(code.splitlines(), 1):
                for m in ADDRESS.finditer(line):
                    v = int(m.group(1), 16)
                    if (rel, "0x" + m.group(1).lower()) in skip:
                        continue
                    if GUEST_LO <= v < GUEST_HI:
                        refs[v].append(f"{rel}:{n}")
    return refs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profiles", nargs="+")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--exe", type=Path)
    args = ap.parse_args()
    engine = ROOT / "game" / TARGET["installed_name"]
    if not engine.exists():
        engine = ROOT / "game" / "versions" / "en-116" / "trespass.exe"
    base = load_image(engine)
    refs = host_addresses()
    for profile in args.profiles:
        exe = args.exe or profile_exe(profile)
        other = load_image(exe)
        mapper = Mapper(base, other).run()
        decisions = decisions_for(profile)
        counts = collections.Counter()
        out = {}
        for va in sorted(refs):
            r = mapper.resolve(va)
            decided = decisions.get(va)
            if decided and r["class"] in ("candidate", "unresolved"):
                r = {"class": "absent" if decided["category"] == "NO_EQUIVALENT" else "decided",
                     "category": decided["category"], "evidence": decided["evidence"], "was": r["class"]}
                if "to" in decided:
                    r["to"] = int(decided["to"], 16)
            counts[r["class"]] += 1
            out[f"{va:08X}"] = {k: (f"{v:08X}" if k in ("to", "function", "to_function") else v)
                                for k, v in dict(r, where=refs[va][0]).items()}
        total = len(refs)
        print(f"{profile}: {len(mapper.match)} of {len(base.funcs)} functions matched; {total} host addresses: "
              + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
        if args.list:
            for va, r in out.items():
                if r["class"] == "unresolved":
                    print(f"  {va} {r['why']:36} {r['where']}")
        if args.write:
            MAPS.mkdir(parents=True, exist_ok=True)
            GUEST_MAPS_DIR.mkdir(parents=True, exist_ok=True)
            (GUEST_MAPS_DIR / f"{profile}.json").write_text(json.dumps(
                {"profile": profile, "sha256": sha256_of(exe), "addresses": out}, indent=1) + chr(10), encoding="utf-8")


if __name__ == "__main__":
    import guest_map
    guest_map.main()
