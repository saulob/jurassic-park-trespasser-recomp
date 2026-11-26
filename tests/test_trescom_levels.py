import contextlib
import hashlib
import http.server
import io
import json
import os
import re
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import trescom_catalog as catalog  # noqa: E402
import trescom_compatibility as compatibility  # noqa: E402
import trescom_corpus as corpus  # noqa: E402
import trescom_fetch as fetch  # noqa: E402
import trescom_report as report  # noqa: E402
import trescom_site as site  # noqa: E402


def make_zip(text='level'):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as archive:
        archive.writestr('level.scn', text * 400)
    return stream.getvalue()


class Site:
    """A server whose answers are set by path; it counts what is asked."""

    def __init__(self):
        self.routes, self.hits, self.ranges = {}, {}, []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits[self.path] = outer.hits.get(self.path, 0) + 1
                route = outer.routes.get(self.path)
                if self.headers.get('Range'):
                    outer.ranges.append(self.headers['Range'])
                with contextlib.suppress(ConnectionError):
                    if route is None:
                        self.send_error(404)
                    else:
                        route(self)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, args=(0.02,), daemon=True)
        self.thread.start()
        self.base = f'http://127.0.0.1:{self.server.server_address[1]}'

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def serve(self, path, body, name=None, kind='application/octet-stream', status=200, headers=None):
        def answer(request):
            request.send_response(status)
            request.send_header('Content-Type', kind)
            request.send_header('Content-Length', str(len(body)))
            if name:
                request.send_header('Content-Disposition', f'attachment; filename="{name}"')
            for key, value in (headers or {}).items():
                request.send_header(key, value)
            request.end_headers()
            request.wfile.write(body)
        self.routes[path] = answer

    def redirect(self, path, target):
        def answer(request):
            request.send_response(302)
            request.send_header('Location', target)
            request.send_header('Content-Length', '0')
            request.end_headers()
        self.routes[path] = answer

    def ranged(self, path, body, name, cut=None):
        """Honours Range; the first answer may end early."""
        state = {'cut': cut}

        def answer(request):
            start = int((request.headers.get('Range') or 'bytes=0-')[6:].rstrip('-') or 0)
            request.send_response(206 if start else 200)
            request.send_header('Content-Disposition', f'attachment; filename="{name}"')
            request.send_header('Content-Length', str(len(body) - start))
            if start:
                request.send_header('Content-Range', f'bytes {start}-{len(body) - 1}/{len(body)}')
            request.end_headers()
            end, state['cut'] = state['cut'] or len(body), None
            request.wfile.write(body[start:end])
            if end != len(body):
                request.close_connection = True
        self.routes[path] = answer

    def sequence(self, path, *answers):
        left = list(answers)

        def answer(request):
            (left.pop(0) if len(left) > 1 else left[0])(request)
        self.routes[path] = answer


def failing(status, headers=None):
    def answer(request):
        request.send_response(status)
        for key, value in (headers or {}).items():
            request.send_header(key, value)
        request.send_header('Content-Length', '0')
        request.end_headers()
    return answer


def card(title, url, page, size='1.00 MB', label='Download', body='<p>Text</p>'):
    return f'''<!-- WPDM Link Template: X --><h3 class="package-title"><a href='{page}'>{title}</a></h3>
<div class="text-muted text-small"><i class="fas fa-copy"></i> 1 file(s) <i class="fas fa-hdd ml-3"></i> {size}</div>
<div>{body}</div></div><div class="ml-3 wpdmdl-btn"><a class='wpdm' href='#' data-downloadurl="{url}&refresh=abc123">{label}</a></div>'''


class Base(unittest.TestCase):
    def setUp(self):
        self.site = Site()
        self.addCleanup(self.site.close)
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = folder.name
        self.slept = []
        self.fetcher = fetch.Fetcher(timeout=5, retries=2, delay=0, sleep=self.slept.append)

    def url(self, path):
        return self.site.base + path

    def get(self, path):
        facts = self.fetcher.download(self.url(path), self.root)
        return facts, self.fetcher.keep(facts, self.root)

    def entry(self, *links, title='Some Level', path='/get?id=1', identifier='some-level'):
        entry = {'id': identifier, 'title': title, 'source_page': self.url('/download/' + identifier + '/'), 'download_url': self.url(path), 'download_label': 'Download',
                 'reported_size': '1 KB', 'reported_files': 1, 'description': '', 'links': [], 'forums': [], 'other_links': [], 'detail': {}, 'relations': [], 'download_status': ''}
        entry['parts'] = [catalog.new_part(site.BASE, 'Download', entry['download_url'])] + [catalog.new_part(role, label, url) for role, label, url in links]
        return entry

    def downloader(self, registry=None, **options):
        return catalog.Downloader(self.root, self.fetcher, {} if registry is None else registry, log=lambda text: None, **options)


class Fetching(Base):
    def test_direct_download_with_hash(self):
        body = make_zip()
        self.site.serve('/a', body, 'Level One.zip', 'application/zip')
        facts, path = self.get('/a')
        self.assertEqual(os.path.basename(path), 'Level One.zip')
        self.assertEqual(facts['sha256'], hashlib.sha256(body).hexdigest())
        self.assertEqual(facts['check'], 'readable')
        self.assertFalse([name for name in os.listdir(self.root) if name.endswith('.partial')])

    def test_redirect(self):
        self.site.redirect('/a', '/b.zip')
        self.site.serve('/b.zip', make_zip())
        self.assertEqual(os.path.basename(self.get('/a')[1]), 'b.zip')

    def test_redirect_to_another_scheme_is_refused(self):
        self.site.redirect('/a', 'file:///C:/Windows/win.ini')
        with self.assertRaises(fetch.FetchError):
            self.get('/a')

    def test_missing_is_unavailable_and_not_retried(self):
        with self.assertRaises(fetch.FetchError) as caught:
            self.get('/none')
        self.assertTrue(caught.exception.unavailable)
        self.assertEqual(self.site.hits['/none'], 1)

    def test_timeout_is_retried_then_reported(self):
        self.fetcher.timeout = 0.2

        def slow(request):
            time.sleep(0.6)
        self.site.routes['/slow'] = slow
        with self.assertRaises(fetch.FetchError):
            self.get('/slow')
        self.assertEqual(self.site.hits['/slow'], 3)

    def test_server_error_is_retried(self):
        good = make_zip()
        self.site.serve('/ok', good, 'x.zip')
        self.site.sequence('/a', failing(503), self.site.routes['/ok'])
        self.assertEqual(self.get('/a')[0]['size'], len(good))

    def test_rate_limit_waits_as_told(self):
        self.site.serve('/ok', make_zip(), 'x.zip')
        self.site.sequence('/a', failing(429, {'Retry-After': '7'}), self.site.routes['/ok'])
        self.get('/a')
        self.assertIn(7.0, self.slept)

    def test_backoff_grows(self):
        self.site.routes['/a'] = failing(500)
        with self.assertRaises(fetch.FetchError):
            self.get('/a')
        self.assertEqual(self.slept, [2.0, 4.0])

    def test_interrupted_download_resumes(self):
        body = make_zip('abc' * 5000)
        self.site.ranged('/a', body, 'big.zip', cut=len(body) // 2)
        facts, path = self.get('/a')
        self.assertEqual(Path(path).read_bytes(), body)
        self.assertEqual(self.site.ranges, [f'bytes={len(body) // 2}-'])

    def test_server_without_ranges_starts_again(self):
        body = make_zip('abc' * 5000)
        partial = os.path.join(self.root, hashlib.sha256(self.url('/a').encode()).hexdigest()[:16] + '.partial')
        Path(partial).write_bytes(b'old')
        self.site.serve('/a', body, 'big.zip')
        self.assertEqual(Path(self.get('/a')[1]).read_bytes(), body)

    def test_html_is_no_archive(self):
        for kind in ('text/html', 'application/zip'):
            self.site.serve('/a', b'<!DOCTYPE html><html><body>Sorry</body></html>' * 4, 'level.zip', kind)
            with self.assertRaises(fetch.FetchError):
                self.get('/a')
        self.assertEqual(os.listdir(self.root), [])

    def test_page_that_says_the_file_is_gone(self):
        self.site.serve('/a', b'<!DOCTYPE html><html><head><style>p{}</style></head><body><p>Sorry! File not found!</p></body></html>', kind='text/html')
        with self.assertRaises(fetch.FetchError) as caught:
            self.get('/a')
        self.assertTrue(caught.exception.unavailable)
        self.assertIn('Sorry! File not found!', str(caught.exception))

    def test_damaged_archive_is_set_aside(self):
        self.site.serve('/a', make_zip()[:-40], 'level.zip')
        with self.assertRaises(fetch.FetchError):
            self.get('/a')
        self.assertEqual(os.listdir(self.root), ['level.zip.damaged'])

    def test_names_cannot_leave_the_folder(self):
        for name, expected in (('../../evil.zip', 'evil.zip'), ('..\\..\\evil.zip', 'evil.zip'), ('C:\\x\\evil.zip', 'evil.zip'), ('a:b?.zip', 'a_b_.zip'), ('nul.zip', '_nul.zip'),
                               ('..', 'download.bin'), ('', 'download.bin')):
            self.assertEqual(fetch.safe_name(name), expected)
        self.site.serve('/a', make_zip(), '../../evil.zip')
        self.assertEqual(os.path.dirname(self.get('/a')[1]), self.root)


class Parsing(Base):
    def test_listing_and_pages(self):
        page = self.url('/cat/')
        first = card('One &amp; Only', self.url('/download/one/?wpdmdl=1'), self.url('/download/one/')) + f'<a href="{self.url("/cat/page/2/")}">2</a>'
        self.site.serve('/cat/', first.encode(), kind='text/html')
        self.site.serve('/cat/page/2/', (card('Two', self.url('/download/two/?wpdmdl=2'), self.url('/download/two/')) + f'<a href="{page}">1</a>').encode(), kind='text/html')
        cache = catalog.PageCache(os.path.join(self.root, 'pages'), self.fetcher, False)
        entries = catalog.listing_entries(cache, page)
        self.assertEqual([e['title'] for e in entries], ['One & Only', 'Two'])
        self.assertEqual(entries[0]['download_url'], self.url('/download/one/?wpdmdl=1'))
        self.assertEqual(entries[0]['reported_size'], '1.00 MB')
        hits = dict(self.site.hits)
        catalog.listing_entries(catalog.PageCache(os.path.join(self.root, 'pages'), self.fetcher, False), page)
        self.assertEqual(self.site.hits, hits)
        catalog.listing_entries(catalog.PageCache(os.path.join(self.root, 'pages'), self.fetcher, True), page)
        self.assertEqual(self.site.hits['/cat/'], 2)

    def test_link_roles(self):
        body = ('<p><a href="http://x.org/a.part01.rar">Split Download - Part 1</a> <a href="http://x.org/fix.zip">Optional Update</a> '
                '<a href="http://x.org/terrain.rar">Terrain File</a> <a href="https://www.moddb.com/members/a/downloads/b">MODDB Page</a> '
                '<a href="http://x.org/wide.rar">Widescreen Ratio</a> <a href="https://forum.org/viewtopic.php?t=1">Topic</a> <a href="http://x.org/p.png">pic</a></p>')
        entry = site.parse_listing(card('T', 'http://x.org/download/t/?wpdmdl=1', 'http://x.org/download/t/', body=body), 'http://x.org/')[0]
        self.assertEqual([l['role'] for l in entry['links']], [site.SPLIT_PART, site.PATCH, site.COMPLEMENT, site.ALTERNATIVE, site.ALTERNATIVE])
        self.assertEqual([f['url'] for f in entry['forums']], ['https://forum.org/viewtopic.php?t=1'])

    def test_sizes(self):
        self.assertEqual(site.parse_size('20,82 MB'), int(20.82 * (1 << 20)))
        self.assertEqual(site.parse_size('0.00 KB'), 0)
        self.assertEqual(site.parse_size('nothing'), 0)

    def test_relations_need_evidence(self):
        def entry(identifier, title, text=''):
            return {'id': identifier, 'title': title, 'source_page': 'http://x/' + identifier, 'description': text, 'other_links': []}
        engine = entry('engine', 'Winter Contest - Engine')
        pack = entry('pack', 'Week (WoT) Contest Level')
        entries = [entry('a', 'A', 'You\'ll need the "Winter Contest Engine" for it.'), entry('b', 'Raptor Nest Deluxe'), entry('c', 'C', 'Raptor Nest Deluxe triggers (those need to be redone).'),
                   entry('d', 'D', 'You need the files from the official Contest Level package.'), entry('e', 'Snow Fall'), entry('f', 'Snow Fall - SCN Update'), entry('g', 'Saga Part 1: Engine'), entry('h', 'Saga Part 2: Streams'),
                   entry('i', 'Isle', 'NOTE: This level needs the terrain (.wtd) file from Saga~Stream level in order to work.'), entry('j', 'J', 'This level requires the terrain file from the Summit level (sum.wtd).'), entry('k', 'Summit Test')]
        relations = {e['id']: [(r['type'], r['id']) for r in catalog.relations_of(e, entries, [engine, pack])] for e in entries}
        self.assertEqual(relations['a'], [('REQUIRES', 'engine')])
        self.assertEqual(relations['c'], [])
        self.assertEqual(relations['d'], [('REQUIRES', 'pack')])
        self.assertEqual(relations['f'], [('PATCH_FOR', 'e')])
        self.assertEqual(relations['h'], [('PART_OF', 'g')])
        self.assertEqual(relations['i'], [('REQUIRES', 'h')])
        self.assertEqual(relations['j'], [])


class Entries(Base):
    def test_rerun_downloads_nothing(self):
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        registry = {}
        entry = self.entry()
        self.assertTrue(self.downloader(registry).entry(entry))
        hits = dict(self.site.hits)
        again = self.entry()
        self.assertFalse(self.downloader(registry).entry(again))
        self.assertEqual(again['download_status'], catalog.DOWNLOADED)
        self.assertEqual(self.site.hits, hits)

    def test_changed_local_file_is_downloaded_again(self):
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        registry = {}
        entry = self.entry()
        self.downloader(registry).entry(entry)
        local = os.path.join(self.root, *entry['parts'][0]['file'].split('/'))
        Path(local).write_bytes(b'broken')
        again = self.entry()
        self.assertTrue(self.downloader(registry).entry(again))
        self.assertEqual(Path(os.path.join(self.root, *again['parts'][0]['file'].split('/'))).read_bytes(), make_zip())
        self.assertEqual(Path(local).read_bytes(), b'broken')

    def test_new_version_is_kept_beside_the_old(self):
        self.site.serve('/get?id=1', make_zip('one'), 'level.zip')
        registry = {}
        self.downloader(registry).entry(self.entry())
        self.site.serve('/get?id=1', make_zip('another'), 'level.zip')
        quiet = self.entry()
        self.downloader(registry).entry(quiet)
        self.assertEqual(quiet['parts'][0]['file'], 'downloads/some-level/level.zip')
        fresh = self.entry()
        self.downloader(registry, refresh=True).entry(fresh)
        self.assertEqual(fresh['parts'][0]['file'], 'downloads/some-level/level.v2.zip')
        self.assertEqual(fresh['parts'][0]['versions'][0]['file'], 'downloads/some-level/level.zip')
        self.assertTrue(os.path.exists(os.path.join(self.root, 'downloads', 'some-level', 'level.zip')))

    def test_failed_waits_for_retry_failed(self):
        registry = {}
        entry = self.entry()
        self.downloader(registry).entry(entry)
        self.assertEqual(entry['download_status'], catalog.UNAVAILABLE)
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        self.assertFalse(self.downloader(registry).entry(self.entry()))
        entry = self.entry()
        self.assertTrue(self.downloader(registry, retry_failed=True).entry(entry))
        self.assertEqual(entry['download_status'], catalog.DOWNLOADED)

    def test_duplicate_is_not_stored_twice(self):
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        self.site.serve('/get?id=2', make_zip(), 'copy.zip')
        downloader = self.downloader()
        downloader.entry(self.entry())
        second = self.entry(path='/get?id=2', identifier='other')
        downloader.entry(second)
        self.assertEqual(second['download_status'], catalog.DUPLICATE)
        self.assertEqual(second['parts'][0]['file'], 'downloads/some-level/level.zip')
        self.assertFalse(os.path.exists(os.path.join(self.root, 'downloads', 'other')))

    def test_full_package_is_preferred_to_its_parts(self):
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        entry = self.entry((site.SPLIT_PART, 'Part 1', self.url('/p1.rar')), (site.ALTERNATIVE, 'Mirror', self.url('/m.zip')))
        self.downloader().entry(entry)
        self.assertEqual([p['status'] for p in entry['parts']], [catalog.DOWNLOADED, catalog.NOT_NEEDED, catalog.NOT_NEEDED])
        self.assertNotIn('/p1.rar', self.site.hits)

    def test_parts_replace_a_lost_full_package(self):
        self.site.serve('/p1.zip', make_zip('one'))
        self.site.serve('/p2.zip', make_zip('two'))
        entry = self.entry((site.SPLIT_PART, 'Part 1', self.url('/p1.zip')), (site.SPLIT_PART, 'Part 2', self.url('/p2.zip')))
        self.downloader().entry(entry)
        self.assertEqual(entry['download_status'], catalog.DOWNLOADED)
        broken = self.entry((site.SPLIT_PART, 'Part 1', self.url('/p1.zip')), (site.SPLIT_PART, 'Part 2', self.url('/lost.zip')), identifier='broken')
        self.downloader().entry(broken)
        self.assertEqual(broken['download_status'], catalog.PARTIAL)

    def test_mirror_replaces_a_lost_download(self):
        self.site.serve('/m.zip', make_zip())
        entry = self.entry((site.ALTERNATIVE, 'Mirror', self.url('/m.zip')))
        self.downloader().entry(entry)
        self.assertEqual(entry['download_status'], catalog.DOWNLOADED)

    def test_patch_is_kept_apart_and_related(self):
        self.site.serve('/get?id=1', make_zip('base'), 'level.zip')
        self.site.serve('/fix.zip', make_zip('fix'))
        entry = self.entry((site.PATCH, 'Update', self.url('/fix.zip')))
        self.downloader().entry(entry)
        self.assertEqual([p['relation'] for p in entry['parts']], ['BASE', 'PATCH_FOR'])
        self.assertEqual(len({p['file'] for p in entry['parts']}), 2)
        self.assertEqual(Path(self.root, 'downloads', 'some-level', 'level.zip').read_bytes(), make_zip('base'))

    def test_lost_patch_is_partial(self):
        self.site.serve('/get?id=1', make_zip('base'), 'level.zip')
        entry = self.entry((site.PATCH, 'Update', self.url('/lost.zip')))
        self.downloader().entry(entry)
        self.assertEqual(entry['download_status'], catalog.PARTIAL)

    def test_external_hosts_are_manual(self):
        for url, name, page in (('https://www.mediafire.com/file/x/level.zip/file', 'MediaFire', '<a id="downloadButton" href="javascript:void(0)" data-scrambled-url="aHR0">Download</a>'),
                                ('https://www.moddb.com/mods/x/downloads', 'ModDB', '<a href="/downloads/start/1">a</a><a href="/downloads/start/2">b</a>')):
            with self.subTest(name):
                downloader = self.downloader()
                downloader.fetcher = type('External', (), {'resolve': lambda s, u, stop=None, to=url: to, 'page': lambda s, u, text=page: (text, u)})()
                entry = self.entry()
                downloader.entry(entry)
                self.assertEqual(entry['download_status'], catalog.MANUAL)
                self.assertEqual(entry['parts'][0]['resolved_url'], url)
                text = report.markdown([catalog.catalog_item(entry)], 'http://x/', self.root)
                self.assertIn(url, text)
                self.assertIn(name, text)

    def test_refusing_host_is_manual_with_its_address(self):
        self.site.redirect('/get?id=1', 'https://www.moddb.com/mods/x/downloads')
        entry = self.entry()
        downloader = self.downloader()
        asked = []

        def refused(url):
            asked.append(url)
            raise fetch.FetchError('HTTP 403', 403)
        downloader.fetcher.page = refused
        downloader.entry(entry)
        self.assertEqual(asked, ['https://www.moddb.com/mods/x/downloads'])
        self.assertEqual(entry['download_status'], catalog.MANUAL)
        self.assertEqual(entry['parts'][0]['resolved_url'], 'https://www.moddb.com/mods/x/downloads')
        self.assertEqual(entry['parts'][0]['http_status'], 403)

    def test_refusing_forum_is_manual(self):
        self.site.redirect('/get?id=1', 'https://forum.example/viewtopic.php?t=1')
        entry = self.entry()
        downloader = self.downloader()

        def refused(url):
            raise fetch.FetchError('HTTP 403', 403)
        downloader.fetcher.page = refused
        downloader.entry(entry)
        self.assertEqual(entry['download_status'], catalog.MANUAL)

    def test_another_site_is_not_asked(self):
        self.site.redirect('/get?id=1', 'https://example.invalid/page')
        entry = self.entry()
        self.downloader().entry(entry)
        self.assertEqual(entry['download_status'], catalog.MANUAL)
        self.assertEqual(entry['parts'][0]['resolved_url'], 'https://example.invalid/page')

    def test_plain_external_link_is_followed(self):
        self.assertEqual(site.mediafire_direct('<a href="https://download1.mediafire.com/abc/x/level.zip" id="downloadButton">'), 'https://download1.mediafire.com/abc/x/level.zip')
        self.assertEqual(site.moddb_start('<a href="/downloads/start/7">Download</a>', 'https://www.moddb.com/x'), 'https://www.moddb.com/downloads/start/7')


class Report(Base):
    def test_document_is_readable_and_complete(self):
        self.site.serve('/get?id=1', make_zip(), 'level.zip')
        good, lost = self.entry(), self.entry(title='Lost One', path='/lost', identifier='lost')
        downloader = self.downloader()
        downloader.entry(good)
        downloader.entry(lost)
        items = [catalog.catalog_item(e) for e in (good, lost)]
        text = report.markdown(items, 'http://x/', self.root)
        self.assertNotRegex(text, re.compile(r'^\|', re.M))
        self.assertIn('### Lost One', text)
        self.assertIn(f']({self.url("/lost")})', text)
        self.assertIn('HTTP result: 404', text)
        for key in ('id', 'title', 'source_page', 'download_urls', 'external_urls', 'forum_url', 'description', 'reported_size', 'download_status', 'local_files', 'sha256', 'dependencies',
                    'alternatives', 'patches', 'requires_ce', 'requires_atx', 'notes', 'error'):
            self.assertIn(key, items[0])
        path = os.path.join(self.root, 'catalog.json')
        catalog.write_json(path, items, 'http://x/')
        self.assertEqual(len(json.loads(Path(path).read_text('utf-8'))['entries']), 2)


class Compatibility(Base):
    def tree(self, name, files):
        for path, text in files.items():
            target = Path(self.root, name, *path.split('/'))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
        return os.path.join(self.root, name)

    def text(self, *path):
        return Path(self.root, *path).read_text()

    def test_a_patch_replaces_files_of_the_same_name(self):
        base = self.tree('base', {'Mod/Data/a.grf': 'old', 'Mod/Data/a.scn': 'old', 'Mod/readme.txt': 'r'})
        compatibility.add_over(base, self.tree('patch', {'fix/a.grf': 'new', 'fix/more.tpa': 'more'}))
        self.assertEqual((self.text('base', 'Mod', 'Data', 'a.grf'), self.text('base', 'Mod', 'Data', 'a.scn'), self.text('base', 'Mod', 'Data', 'more.tpa')), ('new', 'old', 'more'))

    def test_a_requirement_adds_only_what_the_level_lacks(self):
        base = self.tree('base', {'data/a.scn': 'a', 'data/a.grf': 'own'})
        compatibility.add_below(base, self.tree('engine', {'E/a.grf': 'other', 'E/t.wtd': 'terrain', 'E/b.scn': 'b', 'E/run.exe': 'x', 'E/Map/t/p.bmp': 'p', 'E/MAPS/LVL/fx.tpa': 'fx'}))
        self.assertEqual(compatibility.files_below(base), ['MAPS/LVL/fx.tpa', 'data/Map/t/p.bmp', 'data/a.grf', 'data/a.scn', 'data/t.wtd'])
        self.assertEqual(self.text('base', 'data', 'a.grf'), 'own')

    def test_only_documented_files_are_put_together(self):
        base, patch, engine, other = self.entry(identifier='base'), self.entry(identifier='patch'), self.entry(identifier='engine'), self.entry(identifier='other')
        for entry in (base, patch, engine, other):
            entry['parts'][0].update(status=catalog.DOWNLOADED, file=f'downloads/{entry["id"]}/f.zip')
            entry['dependencies'], entry['relations'] = [], []
        base['dependencies'] = [{'type': 'REQUIRES', 'id': 'engine'}]
        engine['dependencies'] = [{'type': 'REQUIRES', 'id': 'base'}]
        patch['relations'] = [{'type': 'PATCH_FOR', 'id': 'base'}]
        self.assertEqual(compatibility.related_files(base, [base, patch, engine, other]), (['downloads/engine/f.zip'], ['downloads/patch/f.zip']))
        self.assertEqual(compatibility.related_files(other, [base, patch, engine, other]), ([], []))

    def test_self_extracting_archive_is_read_not_run(self):
        path = os.path.join(self.root, 'setup.exe')
        Path(path).write_bytes(b'MZ' + b'\0' * 600 + make_zip())
        self.assertIsNone(corpus.extract(path, os.path.join(self.root, 'out')))
        self.assertTrue(corpus.has_levels(os.path.join(self.root, 'out')))
        Path(path).write_bytes(b'MZ' + b'\0' * 600)
        self.assertIn('installer', corpus.extract(path, os.path.join(self.root, 'none')))

    def test_report_is_readable(self):
        entry = self.entry()
        entry['parts'][0].update(status=catalog.DOWNLOADED, file='downloads/some-level/f.zip')
        entry['download_status'] = catalog.DOWNLOADED
        level = {'scn': 'a.scn', 'class': 'READY', 'scene_version': 22, 'features': ['CE_SKY'], 'issues': []}
        bad = {'scn': 'b.scn', 'class': 'REJECTED_UNSUPPORTED', 'scene_version': 22, 'features': [], 'issues': [{'code': 'UNSUPPORTED_ACTION', 'subject': 'ActionType 99', 'detail': 'one trigger', 'blocks': True, 'policy': 'unsupported'}]}
        item = catalog.catalog_item(entry)
        packages = [compatibility.package_row({'path': 'some-level/f.zip', 'sha256': '', 'duplicate_of': '', 'unread': '', 'import': '', 'levels': [level, bad]})]
        rows = compatibility.levels_of([item], packages)
        text = compatibility.markdown([item], rows, packages, [], [dict(rows[0], **{'class': 'REJECTED_UNSUPPORTED'}, blockers=['CE_SKY'])])
        self.assertNotRegex(text, re.compile(r'^\|', re.M))
        for part in ('## Summary', '## Progress', '## Ready', '## Rejected: missing files', '## Rejected: unsupported features', '## Not analyzed', '## Generic blockers', 'READY: 1', 'ActionType 99', 'Gain: +1',
                     '#### CE_SKY', f']({entry["source_page"]})'):
            self.assertIn(part, text)


if __name__ == '__main__':
    unittest.main()
