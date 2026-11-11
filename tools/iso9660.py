"""Read-only ISO 9660 reader. Prefers the Joliet tree, which keeps the long names the game asks for."""
import struct
from dataclasses import dataclass

SECTOR = 2048
JOLIET_ESCAPES = (b"%/@", b"%/C", b"%/E")


@dataclass(frozen=True)
class Entry:
    path: str
    extent: int
    size: int
    is_dir: bool


class Iso:
    def __init__(self, path):
        self._fh = open(path, "rb")
        self.joliet = False
        self._root = self._find_root()

    def close(self):
        self._fh.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read(self, sector, size):
        self._fh.seek(sector * SECTOR)
        return self._fh.read(size)

    def _find_root(self):
        primary = None
        for sector in range(16, 64):
            vd = self._read(sector, SECTOR)
            if len(vd) < SECTOR or vd[1:6] != b"CD001" or vd[0] == 255:
                break
            if vd[0] == 1:
                primary = vd[156:190]
            elif vd[0] == 2 and vd[88:91] in JOLIET_ESCAPES:
                self.joliet = True
                return vd[156:190]
        if primary is None:
            raise ValueError("not an ISO 9660 image")
        return primary

    def _decode_name(self, raw):
        name = raw.decode("utf-16-be") if self.joliet else raw.decode("latin-1")
        name = name.split(";")[0]
        return name[:-1] if name.endswith(".") and not self.joliet else name

    def _children(self, record, parent):
        extent, size = struct.unpack_from("<I4xI", record, 2)
        block = self._read(extent, size)
        pos = 0
        while pos < len(block):
            length = block[pos]
            if length == 0:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = block[pos:pos + length]
            pos += length
            name_raw = rec[33:33 + rec[32]]
            if name_raw in (b"\x00", b"\x01"):
                continue
            child_extent, child_size = struct.unpack_from("<I4xI", rec, 2)
            path = parent + self._decode_name(name_raw)
            is_dir = bool(rec[25] & 2)
            yield Entry(path, child_extent, child_size, is_dir), rec

    def walk(self):
        stack = [(self._root, "")]
        while stack:
            record, parent = stack.pop()
            for entry, rec in self._children(record, parent):
                if entry.is_dir:
                    stack.append((rec, entry.path + "/"))
                else:
                    yield entry

    def copy_to(self, entry, out_file, hasher=None, chunk=1 << 20):
        self._fh.seek(entry.extent * SECTOR)
        remaining = entry.size
        while remaining:
            data = self._fh.read(min(chunk, remaining))
            if not data:
                raise IOError(f"truncated image while reading {entry.path}")
            out_file.write(data)
            if hasher:
                hasher.update(data)
            remaining -= len(data)
