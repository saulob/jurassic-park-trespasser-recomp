"""Archives of a corpus that the import does not read, extracted to a temporary folder to be analysed; the originals are only read."""
import os
import shutil
import subprocess
import zipfile

from trescom_fetch import archive_kind, safe_name

ARCHIVES = ('.zip', '.7z', '.rar', '.exe')
MAX_DEPTH = 3
MAX_BYTES = 8 << 30


def kind_of(path):
    with open(path, 'rb') as stream:
        return archive_kind(stream.read(1024))


def tar_path():
    return os.path.join(os.environ.get('SystemRoot', ''), 'System32', 'tar.exe')


def embedded(path):
    """The archive inside a self-extracting one: its bytes after the program, never run."""
    with open(path, 'rb') as stream:
        data = stream.read()
    for magic, extension in ((b'Rar!\x1a\x07', '.rar'), (b'7z\xbc\xaf\x27\x1c', '.7z')):
        at = data.find(magic)
        if at > 0:
            return data[at:], extension
    return None, None


def safe_members(folder):
    """False when something of it points outside."""
    root = os.path.realpath(folder)
    for base, folders, files in os.walk(folder):
        for name in folders + files:
            path = os.path.join(base, name)
            if os.path.islink(path) or not os.path.realpath(path).startswith(root + os.sep):
                return False
    return True


READER = []  # the scanner, which reads ZIP, 7z and RAR as the import does


def extract(path, folder):
    """None, or why it was not extracted."""
    os.makedirs(folder, exist_ok=True)
    kind = kind_of(path)
    if kind == 'exe':
        if zipfile.is_zipfile(path):
            kind = 'zip'
        else:
            data, extension = embedded(path)
            if not data:
                return 'an installer without a readable archive inside'
            inner = os.path.join(folder, '__embedded' + extension)
            with open(inner, 'wb') as stream:
                stream.write(data)
            problem = extract(inner, folder)
            if os.path.exists(inner):
                os.remove(inner)
            return problem
    if READER and kind in ('zip', '7z', 'rar'):
        result = subprocess.run([READER[0], '--extract', path, folder], stdin=subprocess.DEVNULL, capture_output=True, timeout=3600)
        if result.returncode == 0:
            return None
        shutil.rmtree(folder, ignore_errors=True)
        return result.stderr.decode('utf-8', 'replace').strip()[:200] or 'it could not be read'
    if kind == 'zip':
        try:
            with zipfile.ZipFile(path) as archive:
                if sum(i.file_size for i in archive.infolist()) > MAX_BYTES:
                    return 'too large'
                archive.extractall(folder)
        except Exception as error:
            return str(error) or type(error).__name__
    else:
        result = subprocess.run([tar_path(), '-xf', path, '-C', folder], stdin=subprocess.DEVNULL, capture_output=True, timeout=1800)
        if result.returncode:
            return result.stderr.decode('utf-8', 'replace').strip().splitlines()[0][:200] if result.stderr else 'tar could not read it'
    if not safe_members(folder):
        shutil.rmtree(folder, ignore_errors=True)
        return 'it points outside its folder'
    return None


def has_levels(folder):
    return any(name.lower().endswith('.scn') for _, _, files in os.walk(folder) for name in files)


def inner_archives(folder):
    found = []
    for base, _, files in os.walk(folder):
        found += [os.path.join(base, name) for name in files if name.lower().endswith(('.zip', '.7z', '.rar'))]
    return sorted(found)


def expand_nested(folder, depth=0):
    """Archives inside a package, beside it when it has no level of its own."""
    if depth >= MAX_DEPTH:
        return
    for path in inner_archives(folder):
        target = os.path.join(os.path.dirname(path), safe_name(os.path.splitext(os.path.basename(path))[0], 'inner'))
        if os.path.exists(target):
            target += '_inner'
        if extract(path, target) is None:
            os.remove(path)
            expand_nested(target, depth + 1)
