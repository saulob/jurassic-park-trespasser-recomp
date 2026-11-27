"""The catalog of a download category: what each entry has, how its files relate, and what of it is on disk."""
import hashlib
import json
import os
import re
import time

import community_level_source as community_source
from corpus_exclusions import EXCLUDED, Exclusions
from community_level_fetch import FetchError, safe_name, sha256_of

DOWNLOADED, PARTIAL, MANUAL = 'DOWNLOADED', 'PARTIALLY_DOWNLOADED', 'MANUAL_DOWNLOAD_REQUIRED'
FAILED, UNAVAILABLE, DUPLICATE, CATALOG_ONLY = 'DOWNLOAD_FAILED', 'UNAVAILABLE', 'DUPLICATE', 'CATALOG_ONLY'
NOT_NEEDED = 'NOT_NEEDED'
RELATION_OF_ROLE = {community_source.PATCH: 'PATCH_FOR', community_source.SPLIT_PART: 'PART_OF', community_source.ALTERNATIVE: 'ALTERNATIVE_TO', community_source.COMPLEMENT: 'REQUIRES', community_source.BASE: 'BASE'}


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.tmp', 'w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    os.replace(path + '.tmp', path)


class PageCache:
    """Pages kept beside the corpus, read again only when asked to."""

    def __init__(self, folder, fetcher, refresh):
        self.folder, self.fetcher, self.refresh = folder, fetcher, refresh
        self.fetched = {}

    def get(self, url):
        if url in self.fetched:
            return self.fetched[url]
        path = os.path.join(self.folder, hashlib.sha256(url.encode()).hexdigest()[:24] + '.html')
        if not self.refresh and os.path.exists(path):
            with open(path, encoding='utf-8') as stream:
                text = stream.read()
        else:
            text, _ = self.fetcher.page(url)
            write_text(path, text)
        self.fetched[url] = text
        return text


def listing_entries(cache, url):
    """Every entry of a category, through however many pages it has."""
    waiting, seen, entries = [url], set(), {}
    while waiting:
        page = waiting.pop(0)
        if page in seen:
            continue
        seen.add(page)
        markup = cache.get(page)
        for entry in community_source.parse_listing(markup, page):
            entries.setdefault(entry['id'], entry)
        waiting += [link for link in community_source.parse_pagination(markup, page) if link not in seen]
    return list(entries.values())


def plain(title):
    return re.sub(r'[^a-z0-9]+', ' ', title.lower()).strip()


def named_entries(text, entries, own):
    """Entries whose whole title a text names, longest first."""
    words = ' ' + plain(text) + ' '
    found = []
    for entry in sorted(entries, key=lambda e: -len(e['title'])):
        name = plain(entry['title'])
        if entry['id'] != own and len(name) >= 8 and ' ' + name + ' ' in words:
            found.append(entry)
            words = words.replace(' ' + name + ' ', ' ')
    return found


def quoted_names(text):
    return [name for name in re.findall(r'["“]([^"”]{6,60})["”]', text)]


def package_names(text):
    return [name.strip() for name in re.findall(r"\b((?:[A-Z][\w']*\s+){1,4})package\b", text)]


def level_words(text):
    """What a text says it takes a file from: the words of the name."""
    return [re.findall(r'[a-z0-9]+', name.lower()) for name in re.findall(r"(?:need|require)s?\b.*?\bfiles?\b.*?\bfrom (?:the )?([A-Z][\w~' ]{3,40}?) level\b", text)]


def needs_title(sentence, title):
    """The need comes before the name it is about."""
    return re.search(r'(need|requir|not include)\b.{0,120}' + r'\W+'.join(re.escape(w) for w in plain(title).split()), plain(sentence)) is not None


def relations_of(entry, entries, index):
    relations = []

    def add(kind, target, evidence):
        if target['id'] != entry['id'] and not any(r['type'] == kind and r['id'] == target['id'] for r in relations):
            relations.append({'type': kind, 'id': target['id'], 'title': target['title'], 'source_page': target['source_page'], 'evidence': evidence})

    everything = entries + [e for e in index if all(e['id'] != own['id'] for own in entries)]
    for sentence in community_source.needed_sentences(entry['description']):
        for target in named_entries(sentence, everything, entry['id']):
            if needs_title(sentence, target['title']):
                add('REQUIRES', target, sentence)
        for name in package_names(sentence):
            matches = [e for e in everything if plain(name) in plain(e['title']) and e['id'] != entry['id'] and 'package' not in plain(e['title'])]
            if len(matches) == 1:
                add('REQUIRES', matches[0], sentence)
        for name in (n for n in quoted_names(sentence) if ' ' in n.strip()):
            for target in everything:
                if plain(name) in plain(target['title']):
                    add('REQUIRES', target, sentence)
    for sentence in community_source.sentences(entry['description']):
        for words in level_words(sentence):
            matches = [e for e in everything if len(words) > 1 and e['id'] != entry['id'] and all(any(t.startswith(w) for t in plain(e['title']).split()) for w in words)]
            if len(matches) == 1:
                add('REQUIRES', matches[0], sentence)
    series = re.match(r'(.+?)[:,]?\s+part\s+(\d+|[ivx]+)\b', entry['title'], re.I)
    if series:
        for other in entries:
            match = re.match(r'(.+?)[:,]?\s+part\s+(\d+|[ivx]+)\b', other['title'], re.I)
            if match and plain(match.group(1)) == plain(series.group(1)):
                add('PART_OF', other, 'title')
                if re.search(r"you('ll| will)? need in order to play", other['description'], re.I):
                    add('REQUIRES', other, next(s for s in community_source.sentences(other['description']) if re.search('need in order to play', s, re.I)))
    follow = re.match(r'(.+?)\s+[-–]\s+(.*\b(?:update|fix|patch|translation)\b.*)$', entry['title'], re.I)
    if follow:
        for other in entries:
            base = plain(follow.group(1))
            if plain(other['title']) == base or plain(other['title']).startswith(base + ' ') and not re.search(r'\b(update|fix|patch|translation)\b', other['title'], re.I):
                add('PATCH_FOR', other, 'title')
    for link in entry['other_links']:
        for other in entries:
            if link['url'].rstrip('/') == other['source_page'].rstrip('/'):
                add('RELATED_TO', other, link['label'])
    return relations


def new_part(role, label, url):
    return {'role': role, 'relation': RELATION_OF_ROLE[role], 'label': label, 'url': url, 'resolved_url': '', 'host': community_source.host_of(url),
            'status': CATALOG_ONLY, 'file': '', 'size': None, 'sha256': '', 'kind': '', 'check': '', 'etag': '', 'modified': '', 'http_status': None, 'error': '', 'versions': []}


class Downloader:
    def __init__(self, root, fetcher, registry, refresh=False, retry_failed=False, log=print, exclusions=None):
        self.root, self.fetcher, self.registry = root, fetcher, registry
        self.refresh, self.retry_failed, self.log = refresh, retry_failed, log
        self.exclusions = exclusions if exclusions is not None else Exclusions()
        self.by_sha = {}

    def local(self, part):
        return os.path.join(self.root, *part['file'].split('/')) if part['file'] else ''

    def intact(self, part):
        path = self.local(part)
        return bool(path) and os.path.exists(path) and os.path.getsize(path) == part['size'] and sha256_of(path) == part['sha256']

    def changed_at_source(self, part):
        try:
            facts, _ = self.fetcher.peek(part['url'])
        except FetchError as error:
            part['error'] = f'could not be checked again: {error}'
            return False
        differs = lambda now, was: bool(now) and bool(was) and now != was
        return (facts['size'] is not None and facts['size'] != part['size']) or differs(facts['etag'], part['etag']) or differs(facts['modified'], part['modified'])

    def fetch(self, part, folder):
        """Brings one part up to date; its status says the rest."""
        old = self.registry.get(part['url'])
        if old:
            for key in ('resolved_url', 'file', 'size', 'sha256', 'kind', 'check', 'etag', 'modified', 'versions', 'status', 'http_status', 'error'):
                part[key] = old.get(key, part[key])
            if part['status'] == EXCLUDED or self.exclusions.blocks_hash(part['sha256']):
                part['status'] = old['status'] = EXCLUDED
                return 'skipped'
            if part['status'] in (DOWNLOADED, DUPLICATE) and self.intact(part):
                if not self.refresh or not self.changed_at_source(part):
                    return 'skipped'
                part['versions'] = part['versions'] + [{'file': part['file'], 'size': part['size'], 'sha256': part['sha256']}]
                self.log('  changed at its source, the earlier file is kept')
            elif part['status'] in (FAILED, UNAVAILABLE, MANUAL) and not self.retry_failed and not self.refresh:
                return 'skipped'
        part.update(status=CATALOG_ONLY, error='', http_status=None)
        self.attempt(part, folder)
        self.registry[part['url']] = {key: value for key, value in part.items() if key not in ('role', 'relation', 'label')}
        return 'fetched'

    def attempt(self, part, folder):
        try:
            url = self.direct(part)
            if not url:
                return
            facts = self.fetcher.download(url, folder, part['label'])
        except FetchError as error:
            external = community_source.manual_host(part['resolved_url'] or part['url'])
            if not external and part['resolved_url'] and (community_source.is_forum(part['resolved_url']) or community_source.host_of(part['resolved_url']) != community_source.host_of(part['url'])):
                external = community_source.host_of(part['resolved_url'])
            part['status'] = UNAVAILABLE if error.unavailable else MANUAL if external else FAILED
            part['http_status'], part['error'] = error.status, f'{external}: {error}' if external else str(error)
            return
        if self.exclusions.blocks_hash(facts['sha256']):
            # Its hash was unknown before the download: the bytes are dropped before anything reads them.
            os.remove(facts.pop('partial'))
            part.update(status=EXCLUDED, file='', size=facts['size'], sha256=facts['sha256'])
            return
        twin = self.by_sha.get(facts['sha256'])
        if twin and os.path.exists(os.path.join(self.root, *twin.split('/'))):
            os.remove(facts.pop('partial'))
            part['status'], relative = DUPLICATE, twin
        else:
            relative = os.path.relpath(self.fetcher.keep(facts, folder), self.root).replace(os.sep, '/')
            part['status'] = DOWNLOADED
            self.by_sha[facts['sha256']] = relative
        part.update(file=relative, size=facts['size'], sha256=facts['sha256'], kind=facts['kind'], check=facts['check'], etag=facts['etag'], modified=facts['modified'])

    def direct(self, part):
        """The address that serves the file itself; None when only a person can get it."""
        url = part['url']
        if not community_source.manual_host(url) and not community_source.is_forum(url):
            final = self.fetcher.resolve(url, lambda to: to != url and (bool(community_source.manual_host(to)) or community_source.is_forum(to) or community_source.host_of(to) != community_source.host_of(url) and not community_source.is_archive_link(to)))
            part['resolved_url'] = final
            if community_source.host_of(final) == community_source.host_of(url) or not (community_source.manual_host(final) or community_source.is_forum(final)) and community_source.is_archive_link(final):
                return url
            url = final
        part['resolved_url'] = url
        host = community_source.manual_host(url)

        def manual(reason):
            part['status'], part['error'] = MANUAL, reason

        if community_source.is_forum(url):
            markup, final = self.fetcher.page(url)
            found = sorted({link for link, _, _ in community_source.links_of(markup, final) if community_source.is_archive_link(link) or community_source.manual_host(link)})
            part['versions'] = part['versions']
            part['candidates'] = found
            if len(found) == 1 and not community_source.manual_host(found[0]):
                return found[0]
            return manual('the download is in a forum topic' + (f' ({len(found)} links in it)' if found else ''))
        if host == 'MediaFire':
            markup, _ = self.fetcher.page(url)
            return community_source.mediafire_direct(markup) or manual('MediaFire: its page has no plain link to the file')
        if host == 'ModDB':
            markup, final = self.fetcher.page(url)
            start = community_source.moddb_start(markup, final)
            if not start:
                return manual('ModDB: the page offers no single file')
            markup, final = self.fetcher.page(start)
            return community_source.moddb_mirror(markup, final) or manual('ModDB: its page has no plain link to the file')
        return manual(f'{host or community_source.host_of(url)}: interactive download')

    def entry(self, entry):
        folder = os.path.join(self.root, 'downloads', safe_name(entry['id'], 'entry'))
        main = entry['parts'][0]
        outcome = {self.fetch(main, folder)}
        have_main = main['status'] in (DOWNLOADED, DUPLICATE)
        for part in entry['parts'][1:]:
            if part['role'] in (community_source.PATCH, community_source.COMPLEMENT):
                outcome.add(self.fetch(part, folder))
        installer = have_main and main['kind'] in ('exe', 'msi')
        others = [p for p in entry['parts'] if p['role'] == community_source.ALTERNATIVE]
        for part in others:
            if not have_main or installer:
                outcome.add(self.fetch(part, folder))
            else:
                part['status'] = NOT_NEEDED
        got = have_main or any(p['status'] in (DOWNLOADED, DUPLICATE) for p in others)
        for part in (p for p in entry['parts'] if p['role'] == community_source.SPLIT_PART):
            if not got:
                outcome.add(self.fetch(part, folder))
            else:
                part['status'] = NOT_NEEDED
        if os.path.isdir(folder) and not os.listdir(folder):
            os.rmdir(folder)
        entry['download_status'] = status_of(entry)
        return 'fetched' in outcome


def status_of(entry):
    parts = entry['parts']
    if any(p['status'] == EXCLUDED for p in parts):
        return EXCLUDED
    main = parts[0]
    done = lambda p: p['status'] in (DOWNLOADED, DUPLICATE)
    splits = [p for p in parts if p['role'] == community_source.SPLIT_PART]
    needed = [p for p in parts if p['role'] in (community_source.PATCH, community_source.COMPLEMENT)]
    base = done(main) or any(done(p) for p in parts if p['role'] == community_source.ALTERNATIVE) or bool(splits) and all(done(p) for p in splits)
    if base and all(done(p) for p in needed):
        return main['status'] if done(main) else DOWNLOADED
    if base or any(done(p) for p in parts):
        return PARTIAL
    return main['status'] if main['status'] != NOT_NEEDED else FAILED


def build_entries(cache, category, detail=True, log=print, excluded=lambda identifier: False):
    """The entries of a category; the page of an excluded one is never fetched and its listing text is dropped."""
    entries = listing_entries(cache, category)
    for number, entry in enumerate(entries, 1):
        entry['detail'] = {}
        if excluded(entry['id']):
            entry['description'] = ''
        elif detail:
            if number % 25 == 1:
                log(f'Pages {number}/{len(entries)}')
            try:
                markup = cache.get(entry['source_page'])
                entry['detail'] = community_source.parse_detail(markup)
            except FetchError as error:
                entry['detail'] = {'error': str(error)}
        entry['parts'] = [new_part(community_source.BASE, entry['download_label'], entry['download_url'])]
        for link in entry['links']:
            entry['parts'].append(new_part(link['role'], link['label'], link['url']))
        entry['download_status'] = CATALOG_ONLY
    return entries


def catalog_item(entry):
    text = '' if entry['download_status'] == EXCLUDED else entry['description']
    parts = entry['parts']
    have = [p for p in parts if p['file'] and p['status'] in (DOWNLOADED, DUPLICATE)]
    external = []
    for part in parts:
        for url in (part['resolved_url'], part['url'], *part.get('candidates', [])):
            if url and community_source.host_of(url) != community_source.host_of(entry['source_page']) and url not in external:
                external.append(url)
    related = entry.get('relations', [])
    return {
        'id': entry['id'], 'title': entry['title'], 'category': entry.get('category', ''), 'source_page': entry['source_page'],
        'author': entry['detail'].get('author', ''), 'created': entry['detail'].get('created', ''), 'updated': entry['detail'].get('updated', ''),
        'download_urls': [p['url'] for p in parts], 'external_urls': external,
        'forum_url': entry['forums'][0]['url'] if entry['forums'] else '', 'forum_urls': [f['url'] for f in entry['forums']],
        'description': text, 'reported_size': entry['reported_size'], 'reported_files': entry['reported_files'],
        'download_status': entry['download_status'],
        'local_files': [p['file'] for p in have], 'sha256': {p['file']: p['sha256'] for p in have},
        'dependencies': [r for r in related if r['type'] == 'REQUIRES'],
        'relations': [r for r in related if r['type'] != 'REQUIRES'],
        'alternatives': [p['url'] for p in parts if p['role'] in (community_source.ALTERNATIVE, community_source.SPLIT_PART)],
        'patches': [p['url'] for p in parts if p['role'] == community_source.PATCH],
        'requires_ce': bool(community_source.engine_need(text, 'CE')), 'requires_atx': bool(community_source.engine_need(text, 'ATX')),
        'engine_evidence': [s for s in (community_source.engine_need(text, 'CE'), community_source.engine_need(text, 'ATX')) if s],
        'notes': community_source.needed_sentences(text), 'error': next((p['error'] for p in parts if p['error']), ''),
        'parts': parts,
    }


def read_registry(path):
    """What earlier runs got, by address."""
    registry = {}
    if os.path.exists(path):
        with open(path, encoding='utf-8') as stream:
            registry = json.load(stream).get('parts', {})
    return registry


def write_registry(path, registry):
    write_text(path, json.dumps({'parts': registry}, indent=1, ensure_ascii=False))


def write_json(path, items, source):
    document = {'generated': time.strftime('%Y-%m-%d %H:%M:%S'), 'source': source, 'entries': items}
    write_text(path, json.dumps(document, indent=1, ensure_ascii=False) + '\n')
