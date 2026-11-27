"""Packages excluded from the development corpus, by catalog id or SHA-256, read from a local file that is never versioned.

An excluded entry is never downloaded, extracted, scanned or put together with another. Exclusion spreads to patches and
parts of an excluded entry and to every entry that shares a file with it. A missing file excludes nothing, and that is not a
permission to give any package to an AI tool (docs/PROVENANCE.md)."""
import argparse
import json
import os
import re
import sys

FILE_NAME = 'corpus_exclusions.json'
ENVIRONMENT = 'JPT_CORPUS_EXCLUSIONS'
CATALOG_NAME = 'COMMUNITY_LEVELS_CATALOG.json'
LEGACY_CATALOG_NAMES = ('TRESCOM_LEVELS_CATALOG.json',)  # read when a corpus has not been catalogued again since the rename
EXCLUDED = 'EXCLUDED'
SPREADS_THROUGH = ('PATCH_FOR', 'PART_OF')
REPOSITORY = os.path.realpath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ID = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$')
SHA256 = re.compile(r'^[0-9a-f]{64}$')


class Exclusions:
    def __init__(self, ids=(), hashes=(), source=''):
        self.ids, self.hashes, self.source = set(ids), set(hashes), source

    def __bool__(self):
        return bool(self.ids or self.hashes)

    def blocks_hash(self, sha256):
        return bool(sha256) and sha256.lower() in self.hashes

    def resolve(self, entries, registry=None):
        """The excluded ids among entries, with every hash they hold added to the blocked ones."""
        excluded = {e['id'] for e in entries if e['id'] in self.ids}
        changed = True
        while changed:
            changed = False
            for entry in entries:
                hashes = hashes_of(entry, registry)
                if entry['id'] in excluded:
                    if not hashes <= self.hashes:
                        self.hashes |= hashes
                        changed = True
                elif hashes & self.hashes or any(r.get('type') in SPREADS_THROUGH and r.get('id') in excluded for r in entry.get('relations', [])):
                    excluded.add(entry['id'])
                    changed = True
        self.ids |= excluded
        return excluded


def hashes_of(entry, registry=None):
    """Every SHA-256 known for an entry's parts: their own, earlier versions, and what the download registry recorded."""
    found = set()
    for part in entry.get('parts', []):
        records = [part] + ([registry[part['url']]] if registry and part.get('url') in registry else [])
        for record in records:
            found.update(v.get('sha256', '') for v in [record] + list(record.get('versions', [])))
    found.update((entry.get('sha256') or {}).values() if isinstance(entry.get('sha256'), dict) else ())
    return {h.lower() for h in found if h}


def inside(path, folder):
    try:
        return os.path.commonpath([os.path.normcase(path), os.path.normcase(folder)]) == os.path.normcase(folder)
    except ValueError:  # another drive
        return False


def path_for(root, explicit=''):
    return os.path.abspath(explicit or os.environ.get(ENVIRONMENT, '') or os.path.join(root, FILE_NAME))


def load(path):
    """The exclusions of a file; none when it does not exist. A file that cannot be trusted stops the caller (ValueError)."""
    if not os.path.exists(path):
        return Exclusions(source='')
    if inside(os.path.realpath(path), REPOSITORY):
        raise ValueError(f'{path}: keep the exclusions outside the repository, they are local and never versioned')
    try:
        with open(path, encoding='utf-8') as stream:
            document = json.load(stream)
    except (OSError, ValueError) as error:
        raise ValueError(f'{path}: {error}') from None
    if not isinstance(document, dict) or set(document) - {'ids', 'sha256'}:
        raise ValueError(f'{path}: expected an object with "ids" and "sha256" lists only')
    ids, hashes = document.get('ids', []), document.get('sha256', [])
    if not isinstance(ids, list) or not all(isinstance(i, str) and ID.match(i) for i in ids):
        raise ValueError(f'{path}: "ids" must be a list of catalog ids')
    if not isinstance(hashes, list) or not all(isinstance(h, str) and SHA256.match(h.lower()) for h in hashes):
        raise ValueError(f'{path}: "sha256" must be a list of 64 hexadecimal digits')
    return Exclusions(ids, {h.lower() for h in hashes}, path)


def describe(rules, path):
    if rules.source:
        return f'Exclusions: {len(rules.ids)} id(s), {len(rules.hashes)} hash(es) from {path}'
    return f'Exclusions: none ({path} does not exist). No exclusion is not a permission to share a package with AI tools.'


def catalog_path(root):
    """The corpus's catalog: the current name, else a legacy one; the current name when there is none."""
    for name in (CATALOG_NAME,) + LEGACY_CATALOG_NAMES:
        if os.path.exists(os.path.join(root, name)):
            return os.path.join(root, name)
    return os.path.join(root, CATALOG_NAME)


def corpus_root_of(path):
    """The development corpus a path belongs to: the nearest folder, itself or above, with a catalog or an exclusions file."""
    here = os.path.realpath(path)
    while True:
        if os.path.isdir(here) and any(os.path.exists(os.path.join(here, name)) for name in (CATALOG_NAME, FILE_NAME) + LEGACY_CATALOG_NAMES):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            return ''
        here = parent


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-path', required=True, help='exits 3 when the path is inside a development corpus')
    options = parser.parse_args(arguments)
    root = corpus_root_of(options.check_path)
    if root:
        print(f'{options.check_path} belongs to the development corpus at {root}: use tools/community_level_compatibility.py, which applies its exclusions.')
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
