"""Catalogs a community level download category (a WordPress Download Manager site) and downloads what a plain link serves.
Downloaded files are never run or changed."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus_exclusions  # noqa: E402
import community_level_catalog as catalog  # noqa: E402
import community_level_report as report  # noqa: E402
import community_level_source as community_source  # noqa: E402
from community_level_fetch import Fetcher, FetchError  # noqa: E402


def size_text(size):
    return report.megabytes(size) if size else '?'


class Progress:
    def __init__(self, tty):
        self.tty = tty
        self.step = -1

    def __call__(self, label, done, total):
        percent = int(100 * done / total) if total else None
        text = f'  {percent}% {size_text(done)} / {size_text(total)}' if percent is not None else f'  {size_text(done)}'
        if self.tty:
            print('\r' + text + '    ', end='', flush=True)
        elif percent is not None and percent // 25 != self.step:
            self.step = percent // 25
            print(text, flush=True)


def index_entries(cache, categories, skip):
    entries = []
    for name, url in categories.items():
        if url.rstrip('/') == skip.rstrip('/'):
            continue
        try:
            for entry in catalog.listing_entries(cache, url):
                entry['category'] = name
                entries.append(entry)
        except FetchError as error:
            print(f'Category {name}: {error}')
    return entries


def required_entries(entries, index):
    known = {e['id'] for e in entries}
    extra = {}
    for entry in entries:
        for relation in entry['relations']:
            if relation['type'] == 'REQUIRES' and relation['id'] not in known:
                extra[relation['id']] = next(e for e in index if e['id'] == relation['id'])
    for entry in extra.values():
        entry['detail'], entry['relations'], entry['download_status'] = {}, [], catalog.CATALOG_ONLY
        entry['parts'] = [catalog.new_part(community_source.BASE, entry['download_label'], entry['download_url'])]
        entry['parts'] += [catalog.new_part(l['role'], l['label'], l['url']) for l in entry['links']]
    return list(extra.values())


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='https://www.trescom.org/download-category/levels/')
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--refresh-catalog', action='store_true', help='read every page again and compare downloaded files with their source')
    parser.add_argument('--retry-failed', action='store_true')
    parser.add_argument('--resume', action='store_true', help='the default: what is on disk and intact is not downloaded again')
    parser.add_argument('--catalog-only', action='store_true')
    parser.add_argument('--only', default='', help='ids, separated by commas')
    parser.add_argument('--delay', type=float, default=0.8)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--retries', type=int, default=3)
    parser.add_argument('--exclusions', default='', help=f'the local exclusions file (default: ${corpus_exclusions.ENVIRONMENT} or {corpus_exclusions.FILE_NAME} in the output root)')
    options = parser.parse_args(arguments)

    root = os.path.abspath(options.output_root)
    exclusions_file = corpus_exclusions.path_for(root, options.exclusions)
    try:
        rules = corpus_exclusions.load(exclusions_file)
    except ValueError as error:
        print(f'Stopped: {error}')
        return 2
    print(corpus_exclusions.describe(rules, exclusions_file), flush=True)
    os.makedirs(root, exist_ok=True)
    fetcher = Fetcher(timeout=options.timeout, retries=options.retries, delay=options.delay, progress=Progress(sys.stdout.isatty()))
    cache = catalog.PageCache(os.path.join(root, 'metadata', 'pages'), fetcher, options.refresh_catalog)
    try:
        entries = catalog.build_entries(cache, options.source, log=lambda text: print(text, flush=True), excluded=rules.ids.__contains__)
    except FetchError as error:
        print(f'The category could not be read: {error}')
        return 1
    categories = community_source.category_links(cache.get(options.source), options.source)
    index = index_entries(cache, categories, options.source)
    for entry in entries:
        entry['relations'] = catalog.relations_of(entry, entries, index)
    entries += required_entries(entries, index)
    print(f'{len(entries)} entries', flush=True)

    registry_file = os.path.join(root, 'metadata', 'registry.json')
    registry = catalog.read_registry(registry_file)
    excluded = rules.resolve(entries, registry)
    for entry in entries:
        if entry['id'] in excluded:
            entry['download_status'], entry['description'] = corpus_exclusions.EXCLUDED, ''
    downloader = catalog.Downloader(root, fetcher, registry, options.refresh_catalog, options.retry_failed, exclusions=rules)
    for part in registry.values():
        if part['status'] == catalog.DOWNLOADED and part['sha256'] and not rules.blocks_hash(part['sha256']):
            downloader.by_sha.setdefault(part['sha256'], part['file'])
    only = {name for name in options.only.split(',') if name}

    def save():
        catalog.write_registry(registry_file, registry)
        items = [catalog.catalog_item(entry) for entry in entries]
        catalog.write_json(os.path.join(root, 'COMMUNITY_LEVELS_CATALOG.json'), items, options.source)
        catalog.write_text(os.path.join(root, 'COMMUNITY_LEVELS_CATALOG.md'), report.markdown(items, options.source, root))
        return items

    if not options.catalog_only:
        for number, entry in enumerate(entries, 1):
            if only and entry['id'] not in only:
                continue
            if entry['id'] in excluded:
                print(f'[{number}/{len(entries)}] {entry["id"]}: excluded, not downloaded', flush=True)
                continue
            print(f'[{number}/{len(entries)}] {entry["title"]} ({entry["reported_size"] or "?"})', flush=True)
            fetched = downloader.entry(entry)
            if entry['download_status'] == corpus_exclusions.EXCLUDED:
                excluded |= rules.resolve(entries, registry)
                for other in entries:
                    if other['id'] in excluded:
                        other['download_status'], other['description'] = corpus_exclusions.EXCLUDED, ''
            main_part = entry['parts'][0]
            word = entry['download_status'] if fetched else f'Skipped ({entry["download_status"]})'
            print(f'\n  {word}' + (f' {size_text(main_part["size"])} {main_part["file"]}' if main_part['file'] else f' {main_part["error"]}' if main_part['error'] else ''), flush=True)
            if fetched:
                catalog.write_registry(registry_file, registry)
    items = save()
    totals = {}
    for item in items:
        totals[item['download_status']] = totals.get(item['download_status'], 0) + 1
    print('\n' + '\n'.join(f'{name}: {count}' for name, count in sorted(totals.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
