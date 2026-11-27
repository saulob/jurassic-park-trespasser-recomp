"""Runs the custom level scanner over a downloaded corpus, with the archives it does not read extracted and the documented
bases, patches and requirements put together, and writes what came of it. The downloads are only read."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trescom_catalog as catalog  # noqa: E402
import trescom_corpus as corpus  # noqa: E402
import trescom_report as report  # noqa: E402

CLASSES = ('READY', 'REJECTED_MISSING', 'REJECTED_UNSUPPORTED', 'REJECTED_MALFORMED', 'REJECTED_OTHER')
HAVE = (catalog.DOWNLOADED, catalog.DUPLICATE)


def scan(exe, root, data):
    with tempfile.TemporaryDirectory(prefix='jpt-scan-') as folder:
        out = os.path.join(folder, 'scan.json')
        command = [exe, root, '--data', data, '--json', out]
        subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        with open(out, encoding='utf-8', errors='replace') as stream:
            return json.load(stream)['packages']


def blocker_of(issue):
    return issue['subject'] if issue['code'] in ('UNSUPPORTED_ACTION', 'UNSUPPORTED_FEATURE') else issue['code']


def level_row(level):
    blocking = [i for i in level['issues'] if i['blocks']]
    return {'scn': level['scn'], 'class': level['class'], 'version': level['scene_version'], 'features': level.get('features', []),
            'blockers': sorted({blocker_of(i) for i in blocking}), 'issues': [{k: i[k] for k in ('code', 'subject', 'detail')} | {'policy': i.get('policy', '')} for i in blocking]}


def package_row(package, prefix=''):
    return {'path': prefix + package['path'], 'sha256': package['sha256'], 'duplicate_of': prefix + package['duplicate_of'] if package['duplicate_of'] else '', 'unread': package['unread'],
            'import': package['import'], 'levels': [level_row(l) for l in package['levels']]}


def local(root, relative):
    return os.path.join(root, *relative.split('/'))


def related_files(entry, entries):
    """What an entry needs under it and what goes over it: the files of documented relations."""
    by_id = {e['id']: e for e in entries}
    got = lambda e, roles: [p['file'] for p in e['parts'] if p['role'] in roles and p['status'] in HAVE and p['file']]
    needs, patches, seen = [], [], {entry['id']}

    def follow(item):
        for relation in item['dependencies']:
            target = by_id.get(relation['id'])
            if target and target['id'] not in seen:
                seen.add(target['id'])
                follow(target)
                needs.extend(got(target, ('BASE', 'COMPLEMENT', 'PATCH')))
    follow(entry)
    needs.extend(got(entry, ('COMPLEMENT',)))
    patches.extend(got(entry, ('PATCH',)))
    for other in entries:
        if any(r['type'] == 'PATCH_FOR' and r['id'] == entry['id'] for r in other['relations']):
            patches.extend(got(other, ('BASE', 'PATCH')))
    return needs, patches


def files_below(folder):
    return sorted(os.path.relpath(os.path.join(base, name), folder).replace(os.sep, '/') for base, _, names in os.walk(folder) for name in names)


def level_folder(files):
    """Where the level files are: the folder of the first scene, a data folder before others."""
    scenes = sorted((f for f in files if f.lower().endswith('.scn')), key=lambda f: ('/data/' not in '/' + f.lower(), f.count('/'), f.lower()))
    return scenes[0].rpartition('/')[0] if scenes else ''


def put(source, target):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(source, target)


def game_root(files):
    """The folder that stands for the game's own: the one holding the data folder of the level files."""
    home = level_folder(files)
    return home.rpartition('/')[0] if home.rpartition('/')[2].lower() == 'data' else home


def below_root(name):
    """Its path from a folder of the game (data, maps, scripts), if it has one."""
    parts = name.split('/')
    for i, part in enumerate(parts[:-1]):
        if part.lower() in ('data', 'maps', 'scripts'):
            return '/'.join(parts[i:])
    return None


def script_target(base, files, name):
    """Trespasser CE reads scripts from the scripts folder of a game path: a patch's script goes there, never beside the levels."""
    inner = below_root(name)
    if not inner or inner.split('/')[0].lower() != 'scripts' or not name.lower().endswith('.nut'):
        return None
    root = game_root(files)
    folder = next((f.rpartition('/')[0] for f in files if f.lower().endswith('.nut') and f.rpartition('/')[0].rpartition('/')[2].lower() == 'scripts'), None)
    return os.path.join(base, folder if folder is not None else (root + '/scripts' if root else 'scripts'), name.rpartition('/')[2])


def add_below(base, extra):
    """Files that the base does not have: in the game's folders they name, else beside its level files."""
    files = files_below(base)
    have = {f.rpartition('/')[2].lower() for f in files}
    home, root = level_folder(files), game_root(files)
    for name in files_below(extra):
        file = name.rpartition('/')[2]
        inner = below_root(name)
        if script_target(base, files, name):
            target = script_target(base, files, name)
        elif inner and inner.split('/')[0].lower() == 'maps':
            target = os.path.join(base, root, inner)
        elif '/map/' in '/' + name.lower():
            target = os.path.join(base, home, name[('/' + name.lower()).index('/map/'):])
        elif file.lower().endswith(('.grf', '.wtd', '.pid', '.swp', '.spz', '.spp', '.tpa', '.asa', '.sky')) and file.lower() not in have:
            target = os.path.join(base, home, file)
        else:
            continue
        if not os.path.exists(target):
            put(os.path.join(extra, name), target)


def add_over(base, patch):
    """A patch's files in place of the base's of the same name; its others beside them."""
    files = files_below(base)
    places = {}
    for name in files:
        places.setdefault(name.rpartition('/')[2].lower(), []).append(name)
    home = None
    for name in files_below(patch):
        same = places.get(name.rpartition('/')[2].lower(), [])
        if len(same) == 1:
            home = home if home is not None else same[0].rpartition('/')[0]
            put(os.path.join(patch, name), os.path.join(base, same[0]))
    home = home if home is not None else level_folder(files)
    for name in files_below(patch):
        if name.rpartition('/')[2].lower() not in places:
            put(os.path.join(patch, name), script_target(base, files, name) or os.path.join(base, home, name.rpartition('/')[2]))


def compose(root, entry, entries, target, notes):
    needs, patches = related_files(entry, entries)
    main = entry['parts'][0]
    base_file = main['file'] if main['status'] in HAVE else next((p['file'] for p in entry['parts'] if p['role'] == 'ALTERNATIVE' and p['status'] in HAVE), '')
    if not base_file or not (needs or patches):
        return False
    work = target + '.work'
    try:
        problem = corpus.extract(local(root, base_file), target)
        for number, (file, over) in enumerate([(f, False) for f in needs] + [(f, True) for f in patches]):
            part = os.path.join(work, str(number))
            problem = problem or corpus.extract(local(root, file), part)
            if problem:
                break
            corpus.expand_nested(part)
            (add_over if over else add_below)(target, part)
        if problem:
            notes.append(f'{entry["id"]}: {problem}')
            shutil.rmtree(target, ignore_errors=True)
            return False
        return corpus.has_levels(target)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def analyse(root, exe, data, temporary):
    with open(os.path.join(root, 'TRESCOM_LEVELS_CATALOG.json'), encoding='utf-8') as stream:
        entries = json.load(stream)['entries']
    notes = []
    downloads = os.path.join(root, 'downloads')
    files = files_below(downloads)
    packages = [package_row(p) for p in scan(exe, downloads, data)]
    known = {p['path'] for p in packages if p['levels'] or p['duplicate_of']}
    extra = os.path.join(temporary, 'extra')
    os.makedirs(extra)
    for name in files:
        path = os.path.join(downloads, *name.split('/'))
        if name in known or not name.lower().endswith(corpus.ARCHIVES):
            continue
        where = os.path.join(extra, *name.split('/'))
        problem = corpus.extract(path, where)
        if problem:
            notes.append(f'{name}: {problem}')
            shutil.rmtree(where, ignore_errors=True)
            continue
        corpus.expand_nested(where)
        if not corpus.has_levels(where):
            shutil.rmtree(where, ignore_errors=True)
    if os.listdir(extra):
        packages += [package_row(p) for p in scan(exe, extra, data)]
    composed = os.path.join(temporary, 'composed')
    os.makedirs(composed)
    for entry in entries:
        compose(root, entry, entries, os.path.join(composed, entry['id']), notes)
    if os.listdir(composed):
        packages += [package_row(p, 'composed/') for p in scan(exe, composed, data)]
    return entries, packages, notes


def entry_of(path, by_id):
    parts = path.split('/')
    return by_id.get(parts[1] if parts[0] == 'composed' and len(parts) > 1 else parts[0])


def levels_of(entries, packages):
    """One row for each level of an entry; one put together stands for the parts it is made of."""
    by_id = {e['id']: e for e in entries}
    rows, whole = {}, {}
    for package in packages:
        if package['duplicate_of'] and not package['path'].startswith('composed/'):
            continue
        entry = entry_of(package['path'], by_id)
        if not entry:
            continue
        together = package['path'].startswith('composed/')
        for level in package['levels']:
            row = dict(level, package=package['path'], entry=entry['id'], title=entry['title'], together=together)
            key = (entry['id'], level['scn'].rpartition('/')[2].lower())
            if together:
                whole.setdefault(entry['id'], set()).add(key)
                rows[key + ('',)] = row
            else:
                rows[key + (package['path'],)] = row
    dropped = {key for key in rows if key[2] and (key[0], key[1]) in whole.get(key[0], set())}
    return [row for key, row in rows.items() if key not in dropped]


def ranked(rows):
    users = {}
    for row in rows:
        for name in row['blockers']:
            users.setdefault(name, []).append(row)
    return sorted(users.items(), key=lambda item: (-len(item[1]), item[0]))


def package_names(rows):
    return sorted({row['title'] for row in rows}, key=str.lower)


FEATURES = {}


def read_features(source):
    """The feature table's names, titles and status, for the report."""
    import re
    with open(source, encoding='utf-8') as stream:
        for name, title, status, _, note in re.findall(r'\{ "(\w+)", "([^"]*)", (\w+), ([^,]*), "([^"]*)" \}', stream.read()):
            FEATURES[name] = {'title': title, 'status': status, 'note': note}


def feature_of(blocker):
    return FEATURES.get('ATX_ACTION_' + blocker.split()[-1] if blocker.startswith('ActionType ') else blocker)


def source_of(blocker):
    if blocker.startswith('ActionType '):
        number = int(blocker.split()[-1])
        return 'CE (script)' if number == 99 else 'ATX'
    if blocker.startswith('CE_'):
        return 'CE'
    return 'ATX' if blocker.startswith('ATX_') else 'unknown'


def markdown(entries, rows, packages, notes, baseline):
    by_id = {e['id']: e for e in entries}
    count = lambda name, chosen=rows: sum(row['class'] == name for row in chosen)
    analysed = {row['entry'] for row in rows}
    duplicates = [p for p in packages if p['duplicate_of'] and not p['path'].startswith('composed/')]
    covered = analysed | {p['path'].split('/')[0] for p in duplicates}
    outside = [e for e in entries if e['id'] not in covered]
    sources = sum(1 for e in entries for p in e['parts'] if p['status'] in HAVE)
    lines = ['# TresCom Level Compatibility', '', f'Generated: {time.strftime("%Y-%m-%d %H:%M")}', '', 'READY means that everything the level needs is there and is implemented. It does not mean that it was played.', '',
             '## Summary', '', f'- Downloaded files: {sources}', f'- Entries analysed: {len(covered)} of {len(entries)}', f'- Packages with levels: {len({row["package"] for row in rows})}', f'- Levels: {len(rows)}']
    lines += [f'- {name}: {count(name)}' for name in CLASSES]
    lines += [f'- Not analyzed (entries): {len(outside)}', '', '## Progress', '']
    was = {(r['entry'], r['scn'].rpartition('/')[2].lower(), r['package']): r for r in baseline} if baseline else {}
    before = lambda row: was.get((row['entry'], row['scn'].rpartition('/')[2].lower(), row['package']))
    if baseline:
        lines += [f'- Baseline: {sum(r["class"] == "READY" for r in baseline)} READY of {len(baseline)}', f'- Now: {count("READY")} READY of {len(rows)}',
                  f'- Gain: {count("READY") - sum(r["class"] == "READY" for r in baseline):+d}', '', '### Features implemented in this run', '']
        solved = {}
        for row in rows:
            old = before(row)
            for name in (set(old['blockers']) - set(row['blockers']) if old else ()):
                solved.setdefault(name, []).append(row)
        for name, users in sorted(solved.items(), key=lambda item: (-len(item[1]), item[0])):
            feature = feature_of(name)
            unlocked = [r for r in users if r['class'] == 'READY']
            lines += [f'#### {name}' + (f' ({feature["title"]})' if feature else ''), '', f'- Levels it no longer blocks: {len(users)}', f'- Levels now READY among them: {len(unlocked)}',
                      f'- Packages: {", ".join(package_names(users))}', '']
        together = [row for row in rows if row['together'] and row['class'] == 'READY']
        if together:
            lines += ['#### Bases, patches and requirements put together', '', f'- Levels now READY: {len(together)}', f'- Packages: {", ".join(package_names(together))}', '']
    else:
        lines += [f'- Now: {count("READY")} READY of {len(rows)}', '']
    lines += ['### Remaining blockers', '']
    lines += [f'- {name}: {len(users)} level(s), {sum(1 for r in users if r["blockers"] == [name])} of them blocked by nothing else' for name, users in ranked(rows)] + ['']

    def grouped(chosen):
        groups = {}
        for row in chosen:
            groups.setdefault(row['entry'], []).append(row)
        return sorted(groups.items(), key=lambda item: by_id[item[0]]['title'].lower())

    def head(entry_id):
        entry = by_id[entry_id]
        return [f'### {entry["title"]}', '', f'- Source: {report.link("Source on TresCom", entry["source_page"])}']

    lines += ['## Ready', '']
    for entry_id, group in grouped([r for r in rows if r['class'] == 'READY']):
        lines += head(entry_id)
        lines += [f'- {r["scn"].rpartition("/")[2]}: READY' + (' (base, patches and requirements put together)' if r['together'] else '') for r in group]
        features = sorted({f for r in group for f in r['features']})
        lines += ([f'- Features: {", ".join(features)}'] if features else []) + ['']
    for title, name, policy in (('Rejected: missing files', 'REJECTED_MISSING', 'missing'), ('Rejected: unsupported features', 'REJECTED_UNSUPPORTED', ''), ('Rejected: malformed', 'REJECTED_MALFORMED', ''),
                                ('Rejected: other', 'REJECTED_OTHER', '')):
        lines += [f'## {title}', '']
        chosen = grouped([r for r in rows if r['class'] == name])
        if not chosen:
            lines += ['None.', '']
        for entry_id, group in chosen:
            lines += head(entry_id)
            for row in group:
                lines.append(f'- Level: {row["scn"].rpartition("/")[2]}' + (' (put together)' if row['together'] else ''))
                for issue in row['issues']:
                    word = 'Missing' if issue['policy'] == 'missing' else 'Requires' if issue['policy'] == 'unsupported' else 'Problem'
                    label = blocker_of(issue) if issue['policy'] == 'unsupported' else f'{issue["code"]} {issue["subject"]}'
                    lines.append(f'  - {word}: {label}: {report.shorten(issue["detail"], 320)}')
            lines.append('')
    lines += ['## Not analyzed', '']
    for entry in sorted(outside, key=lambda e: e['title'].lower()):
        main = entry['parts'][0]
        about = [n.split(': ', 1)[1] for n in notes if n.startswith(entry['id'])]
        mine = [p for p in packages if p['path'].split('/')[0] == entry['id']]
        reason = main['error'] or (about[0] if about else '') or next((p['unread'] or p['import'] for p in mine), '') or 'no level files in it'
        lines += [f'### {entry["title"]}', '', f'- Download status: {entry["download_status"]}', f'- Reason: {reason}', f'- Source: {report.link("Source on TresCom", entry["source_page"])}',
                  f'- Download: {report.link("Download", report.external(main))}', '']
    lines += ['## Generic blockers', '', 'By the number of levels.', '']
    for name, users in ranked(rows):
        feature = feature_of(name)
        lines += [f'### {name}' + (f' ({feature["title"]})' if feature else ''), '', f'- Affected levels: {len(users)}', f'- Levels that nothing else blocks: {sum(1 for r in users if r["blockers"] == [name])}']
        lines += [f'- Affected packages: {", ".join(package_names(users))}', f'- Implementation source: {source_of(name)}']
        if feature:
            status = {'NONE': 'not implemented', 'PART': 'partially implemented', 'FULL': 'implemented', 'FREE': 'not required', 'UNKNOWN': 'unknown'}.get(feature['status'], feature['status'])
            lines.append(f'- Status: {status}' + (f': {feature["note"]}' if feature['note'] else ''))
        else:
            lines.append('- Status: ' + ('files that no documented source has' if name.startswith('MISSING') else 'see the levels above'))
        lines.append('')
    return '\n'.join(lines).rstrip() + '\n'


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    parser.add_argument('--exe', required=True)
    parser.add_argument('--data', required=True)
    parser.add_argument('--baseline', default='', help='an earlier state, for the progress')
    parser.add_argument('--state', default='', help='where this state is written')
    parser.add_argument('--temp', default='')
    parser.add_argument('--no-report', action='store_true')
    options = parser.parse_args(arguments)
    root = os.path.abspath(options.root)
    read_features(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'common', 'jpt_level_features.c'))
    corpus.READER[:] = [os.path.abspath(options.exe)]
    with tempfile.TemporaryDirectory(prefix='jpt-corpus-', dir=options.temp or None) as temporary:
        entries, packages, notes = analyse(root, os.path.abspath(options.exe), os.path.abspath(options.data), temporary)
    rows = levels_of(entries, packages)
    baseline = []
    if options.baseline:
        with open(options.baseline, encoding='utf-8') as stream:
            baseline = json.load(stream)['rows']
    if options.state:
        catalog.write_text(options.state, json.dumps({'rows': rows, 'packages': packages, 'notes': notes}, indent=1, ensure_ascii=False))
    if not options.no_report:
        catalog.write_text(os.path.join(root, 'TRESCOM_LEVEL_COMPATIBILITY.md'), markdown(entries, rows, packages, notes, baseline))
    print(f'{len(packages)} packages, {len(rows)} levels')
    for name in CLASSES:
        print(f'{name}: {sum(row["class"] == name for row in rows)}')
    for name, users in ranked(rows)[:25]:
        print(f'  {len(users):4} {name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
