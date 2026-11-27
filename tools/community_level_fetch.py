"""Polite HTTP: pages, and files that are only kept when they are what they claim to be."""
import email.message
import hashlib
import http.client
import os
import re
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

USER_AGENT = 'JptLevelCatalog/1.0 (custom level catalog; sequential)'
CHUNK = 1 << 18
MAX_PAGE = 16 << 20
MIN_ARCHIVE = 22
REDIRECTS = 8
RESERVED = {'con', 'prn', 'aux', 'nul'} | {f'com{i}' for i in range(1, 10)} | {f'lpt{i}' for i in range(1, 10)}


class FetchError(Exception):
    def __init__(self, reason, status=None, unavailable=False):
        super().__init__(reason)
        self.status = status
        self.unavailable = unavailable


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


def safe_name(name, fallback='download.bin'):
    name = (name or '').replace('\\', '/').rsplit('/', 1)[-1]
    name = re.sub(r'[\x00-\x1f<>:"|?*]', '_', name).strip(' .')
    if not name or name.split('.')[0].lower() in RESERVED:
        name = ('_' + name) if name else fallback
    stem, dot, extension = name.rpartition('.')
    return name if len(name) <= 120 else (stem[:100] + dot + extension[:16])


def disposition_name(header):
    if not header:
        return None
    message = email.message.Message()
    message['content-disposition'] = header
    name = message.get_filename()
    if not name:
        match = re.search(r'filename\s*=\s*"?([^";]+)', header)
        name = match.group(1) if match else None
    return urllib.parse.unquote(name.strip()) if name else None


def archive_kind(head):
    if head[:4] in (b'PK\x03\x04', b'PK\x05\x06', b'PK\x07\x08'):
        return 'zip'
    if head[:6] == b'7z\xbc\xaf\x27\x1c':
        return '7z'
    if head[:4] == b'Rar!':
        return 'rar'
    if head[:2] == b'MZ':
        return 'exe'
    if head[:4] == b'MSCF':
        return 'cab'
    if head[:2] == b'\x1f\x8b':
        return 'gz'
    if head[:3] == b'BZh':
        return 'bz2'
    if head[:6] == b'\xfd7zXZ\x00':
        return 'xz'
    if head[:8] == b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1':
        return 'msi'
    if head[257:262] == b'ustar':
        return 'tar'
    return None


def looks_like_html(head):
    start = head[:1024].lstrip(b'\xef\xbb\xbf \t\r\n').lower()
    return start.startswith((b'<!doctype', b'<html', b'<head', b'<?xml', b'<script', b'<body')) or b'<html' in start


def page_says(path):
    with open(path, 'rb') as stream:
        text = stream.read(1 << 18).decode('utf-8', 'replace')
    text = text[max(text.lower().find('<body'), 0):]
    text = re.sub(r'(?is)<(style|script)\b.*?</\1>', ' ', text)
    return re.sub(r'\s+', ' ', re.sub(r'<[^>]*>', ' ', text)).strip()[:160]


def archive_problem(path, kind):
    """None when its listing can be read, else why; '' when nothing here can tell."""
    if kind == 'zip':
        try:
            with zipfile.ZipFile(path) as archive:
                bad = archive.testzip() if all(not i.flag_bits & 1 for i in archive.infolist()) else None
                return f'damaged entry {bad}' if bad else None
        except NotImplementedError:
            return ''
        except Exception as error:
            return str(error) or type(error).__name__
    tar = os.path.join(os.environ.get('SystemRoot', ''), 'System32', 'tar.exe')
    if kind in ('7z', 'rar', 'cab', 'tar', 'gz', 'bz2', 'xz') and os.path.exists(tar):
        try:
            result = subprocess.run([tar, '-tf', path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300, stdin=subprocess.DEVNULL)
            return None if result.returncode == 0 else ''
        except (OSError, subprocess.SubprocessError):
            return ''
    return ''


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while block := stream.read(CHUNK):
            digest.update(block)
    return digest.hexdigest()


class Fetcher:
    def __init__(self, timeout=60, retries=3, delay=0.6, backoff=2.0, max_backoff=120, sleep=time.sleep, progress=None):
        self.timeout, self.retries, self.delay, self.backoff, self.max_backoff = timeout, retries, delay, backoff, max_backoff
        self.sleep, self.progress = sleep, progress
        self.last = {}
        self.opener = urllib.request.build_opener(NoRedirect)
        self.requests = 0

    def _pace(self, url):
        host = urllib.parse.urlsplit(url).netloc
        wait = self.last.get(host, 0) + self.delay - time.monotonic()
        if wait > 0:
            self.sleep(wait)
        self.last[host] = time.monotonic()

    def _open(self, url, headers, stop=None):
        """The response and where it came from, after redirects; no response for an address that stops."""
        for _ in range(REDIRECTS):
            if stop and stop(url):
                return None, url
            if urllib.parse.urlsplit(url).scheme not in ('http', 'https'):
                raise FetchError(f'refused address {url}')
            url = urllib.parse.quote(url, safe="%/:=&?~#+!$,;'@()*[]")
            self._pace(url)
            self.requests += 1
            request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept-Encoding': 'identity', **headers})
            try:
                return self.opener.open(request, timeout=self.timeout), url
            except urllib.error.HTTPError as error:
                location = error.headers.get('Location')
                if error.code in (301, 302, 303, 307, 308) and location:
                    error.close()
                    url = urllib.parse.urljoin(url, location)
                    continue
                raise
        raise FetchError('too many redirects')

    def resolve(self, url, stop=None):
        """Where a download link leads, without its body; an address that stops is not asked."""
        def once():
            response, final = self._open(url, {}, stop)
            if response:
                response.close()
            return final
        return self._retrying(once)

    def _retrying(self, action):
        wait = self.backoff
        for attempt in range(self.retries + 1):
            try:
                return action()
            except urllib.error.HTTPError as error:
                with error:
                    retry = error.headers.get('Retry-After', '')
                if error.code in (404, 410):
                    raise FetchError(f'HTTP {error.code}', error.code, unavailable=True) from None
                if error.code != 429 and error.code < 500 or attempt == self.retries:
                    raise FetchError(f'HTTP {error.code}', error.code) from None
                pause = float(retry) if retry.isdigit() else wait
            except (urllib.error.URLError, TimeoutError, socket.timeout, ConnectionError, http.client.HTTPException) as error:
                if attempt == self.retries:
                    reason = getattr(error, 'reason', None) or error
                    raise FetchError(f'{type(error).__name__}: {reason}') from None
                pause = wait
            self.sleep(min(pause, self.max_backoff))
            wait *= 2
        raise FetchError('no attempt left')

    def page(self, url):
        def once():
            response, final = self._open(url, {})
            with response:
                body = response.read(MAX_PAGE + 1)
                if len(body) > MAX_PAGE:
                    raise FetchError('page too large')
                charset = response.headers.get_content_charset() or 'utf-8'
                return body.decode(charset, errors='replace'), final
        return self._retrying(once)

    def peek(self, url):
        """Name, size and validators of what a link serves now."""
        def once():
            response, final = self._open(url, {})
            with response:
                head = response.read(1024)
                return self._facts(response, final), head
        return self._retrying(once)

    @staticmethod
    def _facts(response, final):
        headers = response.headers
        length = headers.get('Content-Length', '')
        return {'url': final, 'name': safe_name(disposition_name(headers.get('Content-Disposition')) or urllib.parse.unquote(urllib.parse.urlsplit(final).path)),
                'size': int(length) if length.isdigit() else None, 'etag': headers.get('ETag', ''), 'modified': headers.get('Last-Modified', ''),
                'type': (headers.get_content_type() or '').lower()}

    def download(self, url, folder, label=''):
        """A finished file in the folder: its facts, with 'path', 'sha256', 'kind' and 'check'."""
        os.makedirs(folder, exist_ok=True)
        partial = os.path.join(folder, hashlib.sha256(url.encode()).hexdigest()[:16] + '.partial')
        facts = self._retrying(lambda: self._download_once(url, partial, label))
        with open(partial, 'rb') as stream:
            head = stream.read(1024)
        size = os.path.getsize(partial)
        kind = archive_kind(head)
        problem = None
        gone = False
        if looks_like_html(head) or (kind is None and facts['type'] in ('text/html', 'application/xhtml+xml')):
            said = page_says(partial)
            gone = bool(re.search(r'file not found|no file is attached', said, re.I))
            problem = f'the site answers: {said}' if gone else 'an HTML page instead of a file'
        elif size < MIN_ARCHIVE:
            problem = f'only {size} bytes'
        elif kind is None and facts['name'].lower().endswith(('.zip', '.7z', '.rar', '.exe')):
            problem = 'not the archive its name says'
        if problem:
            os.remove(partial)
            raise FetchError(problem, unavailable=gone)
        check = archive_problem(partial, kind)
        if check:
            os.replace(partial, os.path.join(folder, facts['name'] + '.damaged'))
            raise FetchError(f'damaged archive: {check}')
        facts.update(size=size, sha256=sha256_of(partial), kind=kind or 'file', check='readable' if check is None else 'not checked')
        facts['partial'] = partial
        return facts

    @staticmethod
    def keep(facts, folder, previous=None):
        """Moves the finished download to its name; another version's file stays."""
        name = facts['name']
        target = os.path.join(folder, name)
        version = 1
        while os.path.exists(target) and sha256_of(target) != facts['sha256']:
            version += 1
            stem, dot, extension = name.rpartition('.')
            target = os.path.join(folder, f'{stem or extension}.v{version}{dot if stem else ""}{extension if stem else ""}')
        os.replace(facts.pop('partial'), target)
        return target

    def _download_once(self, url, partial, label):
        have = os.path.getsize(partial) if os.path.exists(partial) else 0
        response, final = self._open(url, {'Range': f'bytes={have}-'} if have else {})
        with response:
            facts = self._facts(response, final)
            total = facts['size']
            mode = 'wb'
            if have and response.status == 206:
                match = re.match(r'bytes (\d+)-\d+/(\d+)', response.headers.get('Content-Range', ''))
                if match and int(match.group(1)) == have:
                    mode, total = 'ab', int(match.group(2))
                else:
                    have = 0
                    os.remove(partial)
                    raise ConnectionError('unusable range')
            else:
                have = 0
            done = have
            shown = 0.0
            with open(partial, mode) as stream:
                while block := response.read(CHUNK):
                    stream.write(block)
                    done += len(block)
                    if self.progress and time.monotonic() - shown > 1:
                        shown = time.monotonic()
                        self.progress(label, done, total)
            if total is not None and done != total:
                raise ConnectionError(f'ended at {done} of {total} bytes')
            facts['size'] = total if total is not None else done
            return facts
