"""The catalog as a document to read: headings and short lists, no wide tables."""
import time

import trescom_catalog as catalog
import trescom_site as site


def link(label, url):
    return f'[{label}](<{url}>)' if any(c in url for c in ' ()') else f'[{label}]({url})'


def megabytes(size):
    if not size:
        return '0 KB'
    return f'{size / (1 << 30):.2f} GB' if size >= 1 << 30 else f'{size / (1 << 20):.1f} MB' if size >= 1 << 20 else f'{size / 1024:.1f} KB'


def shorten(text, most=280):
    text = ' '.join(text.split())
    return text if len(text) <= most else text[:most].rsplit(' ', 1)[0] + '…'


def external(part):
    return part['resolved_url'] or part['url']


def part_lines(part, name):
    lines = [f'- {name}: {part["status"]}']
    if part['file']:
        lines.append(f'  - File: `{part["file"]}`')
        lines.append(f'  - Size: {megabytes(part["size"])}')
        lines.append(f'  - SHA-256: `{part["sha256"]}`')
    lines.append(f'  - Download: {link(part["label"] or "file", part["url"])}')
    if part['resolved_url'] and part['resolved_url'] != part['url'] and site.host_of(part['resolved_url']) != site.host_of(part['url']):
        lines.append(f'  - Served by: {link(site.host_of(part["resolved_url"]), part["resolved_url"])}')
    if part['http_status']:
        lines.append(f'  - HTTP result: {part["http_status"]}')
    if part['error']:
        lines.append(f'  - Reason: {part["error"]}')
    for older in part['versions']:
        lines.append(f'  - Earlier version kept: `{older["file"]}`')
    return lines


def entry_lines(item, status_word):
    main = item['parts'][0]
    lines = [f'### {item["title"]}', '', f'- Status: {status_word}']
    if main['file']:
        lines += [f'- File: `{main["file"]}`', f'- Size: {megabytes(main["size"])}']
    lines.append(f'- Source: {link("Source on TresCom", item["source_page"])}')
    lines.append(f'- Download: {link("Download", main["url"])}')
    if main['resolved_url'] and site.host_of(main['resolved_url']) != site.host_of(main['url']):
        lines.append(f'- Host: {link(site.manual_host(main["resolved_url"]) or site.host_of(main["resolved_url"]), main["resolved_url"])}')
    for url in item['forum_urls']:
        lines.append(f'- Forum: {link("Forum topic", url)}')
    if item['author']:
        lines.append(f'- Author: {item["author"]}')
    if item['created']:
        lines.append(f'- Date: {item["created"]}' + (f' (updated {item["updated"]})' if item['updated'] and item['updated'] != item['created'] else ''))
    if main['sha256']:
        lines.append(f'- SHA-256: `{main["sha256"]}`')
    engines = [name for name, on in (('CE', item['requires_ce']), ('ATX', item['requires_atx'])) if on]
    if engines:
        lines.append(f'- Engine in its description: {", ".join(engines)}')
    for relation in item['dependencies']:
        lines.append(f'- Requires: {link(relation["title"], relation["source_page"])}')
    for note in item['notes'][:4]:
        lines.append(f'- Note: {shorten(note)}')
    return lines


def failure_lines(item):
    main = item['parts'][0]
    lines = entry_lines(item, 'Download failed' if item['download_status'] == catalog.FAILED else 'Unavailable')
    lines.append(f'- Attempted URL: {link(external(main), external(main))}')
    if main['http_status']:
        lines.append(f'- HTTP result: {main["http_status"]}')
    lines.append(f'- Reason: {main["error"] or "unknown"}')
    return lines


def manual_lines(item):
    main = item['parts'][0]
    lines = entry_lines(item, 'Manual download required')
    lines.append(f'- Download page: {link(external(main), external(main))}')
    for url in main.get('candidates', []):
        lines.append(f'  - Link found there: {link(url, url)}')
    lines.append(f'- Expected name: {site.path_name(external(main).removesuffix("/file")) or "unknown"}')
    lines.append(f'- Expected size: {item["reported_size"] or "unknown"}')
    lines.append(f'- Reason: {main["error"]}')
    return lines


def markdown(items, source, folder, pages=1):
    by = {}
    for item in items:
        by.setdefault(item['download_status'], []).append(item)
    count = lambda *names: sum(len(by.get(name, [])) for name in names)
    files = {p['file']: p['size'] for item in items for p in item['parts'] if p['file'] and p['status'] in (catalog.DOWNLOADED, catalog.DUPLICATE)}
    levels = [item for item in items if not item.get('category')]
    lines = ['# TresCom Custom Levels Catalog', '', f'Generated: {time.strftime("%Y-%m-%d %H:%M")}', '', f'Source: {link(source, source)}', '', f'Download directory: `{folder}`', '',
             'A downloaded level is not a supported level: see `TRESCOM_LEVEL_COMPATIBILITY.md`.', '', '## Summary', '',
             f'- Entries found in the category: {len(levels)}', f'- Entries from other categories that they require: {len(items) - len(levels)}',
             f'- Downloaded: {count(catalog.DOWNLOADED)}', f'- Duplicates of another entry: {count(catalog.DUPLICATE)}', f'- Partially downloaded: {count(catalog.PARTIAL)}',
             f'- Manual download required: {count(catalog.MANUAL)}', f'- Failed: {count(catalog.FAILED)}', f'- Unavailable: {count(catalog.UNAVAILABLE)}',
             f'- Catalog only: {count(catalog.CATALOG_ONLY)}', f'- Total downloaded size: {megabytes(sum(size or 0 for size in files.values()))} in {len(files)} files', '']

    def section(title, chosen, writer):
        lines.extend([f'## {title}', ''])
        if not chosen:
            lines.extend(['None.', ''])
        for item in chosen:
            lines.extend(writer(item) + [''])

    ordered = lambda *names: sorted((item for name in names for item in by.get(name, [])), key=lambda i: i['title'].lower())
    section('Manual downloads required', ordered(catalog.MANUAL), manual_lines)
    section('Failed downloads', ordered(catalog.FAILED, catalog.UNAVAILABLE), failure_lines)
    section('Partially downloaded', ordered(catalog.PARTIAL), lambda i: entry_lines(i, 'Partially downloaded') + [x for n, p in enumerate(i['parts']) for x in part_lines(p, f'{p["role"]} {p["label"]}'.strip())])
    section('Downloaded', ordered(catalog.DOWNLOADED), lambda i: entry_lines(i, 'Downloaded'))
    section('Catalog only', ordered(catalog.CATALOG_ONLY), lambda i: entry_lines(i, 'Catalog only'))
    lines.extend(['## Patches and multipart downloads', ''])
    for item in sorted(items, key=lambda i: i['title'].lower()):
        extra = [p for p in item['parts'][1:]]
        relations = [r for r in item['relations'] if r['type'] in ('PATCH_FOR', 'PART_OF')]
        if not extra and not relations:
            continue
        lines.extend([f'### {item["title"]}', '', f'- Source: {link("Source on TresCom", item["source_page"])}'])
        for relation in relations:
            lines.append(f'- {relation["type"]}: {link(relation["title"], relation["source_page"])}')
        for part in extra:
            lines.extend(part_lines(part, f'{part["relation"]} — {part["label"]}'))
        lines.append('')
    lines.extend(['## Duplicate or alternate packages', ''])
    for item in sorted(items, key=lambda i: i['title'].lower()):
        related = [r for r in item['relations'] if r['type'] in ('RELATED_TO', 'ALTERNATIVE_TO')]
        if item['download_status'] != catalog.DUPLICATE and not related:
            continue
        lines.extend([f'### {item["title"]}', '', f'- Status: {item["download_status"]}', f'- Source: {link("Source on TresCom", item["source_page"])}'])
        if item['download_status'] == catalog.DUPLICATE:
            lines.append(f'- Same file as: `{item["parts"][0]["file"]}`')
        for relation in related:
            lines.append(f'- {relation["type"]}: {link(relation["title"], relation["source_page"])}')
        lines.append('')
    return '\n'.join(lines).rstrip() + '\n'
