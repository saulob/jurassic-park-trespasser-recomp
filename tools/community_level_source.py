"""The pages of a WordPress Download Manager category, read into plain entries."""
import html
import re
import urllib.parse

ARCHIVE_EXTENSIONS = ('.zip', '.7z', '.rar', '.exe', '.msi', '.tar', '.gz', '.tgz', '.bz2', '.xz', '.cab', '.lzh', '.ace')
MANUAL_HOSTS = {'mediafire.com': 'MediaFire', 'moddb.com': 'ModDB', 'drive.google.com': 'Google Drive', 'dropbox.com': 'Dropbox',
                'mega.nz': 'MEGA', 'mega.co.nz': 'MEGA', 'onedrive.live.com': 'OneDrive', '1drv.ms': 'OneDrive'}

BASE, PATCH, SPLIT_PART, ALTERNATIVE, COMPLEMENT = 'BASE', 'PATCH', 'SPLIT_PART', 'ALTERNATIVE', 'COMPLEMENT'


def text_of(markup):
    markup = re.sub(r'(?i)<br\s*/?>|</p>|</li>|</div>', '\n', markup)
    plain = html.unescape(re.sub(r'<[^>]+>', ' ', markup))
    lines = (re.sub(r'\s+', ' ', line).strip() for line in plain.splitlines())
    return '\n'.join(line for line in lines if line)


def links_of(markup, page):
    found = []
    for match in re.finditer(r'<a\b[^>]*?\bhref\s*=\s*(["\'])(.*?)\1[^>]*>(.*?)</a>', markup, re.S | re.I):
        url = urllib.parse.urljoin(page, html.unescape(match.group(2)).strip())
        if url.startswith(('http://', 'https://')):
            found.append((url, text_of(match.group(3)).replace('\n', ' '), match.start()))
    return found


def host_of(url):
    host = (urllib.parse.urlsplit(url).hostname or '').lower()
    return host[4:] if host.startswith('www.') else host


def manual_host(url):
    host = host_of(url)
    for domain, name in MANUAL_HOSTS.items():
        if host == domain or host.endswith('.' + domain):
            return name
    return None


def path_name(url):
    return urllib.parse.unquote(urllib.parse.urlsplit(url).path).rstrip('/').rsplit('/', 1)[-1]


def is_archive_link(url):
    return path_name(url).lower().endswith(ARCHIVE_EXTENSIONS)


def is_forum(url):
    return 'viewtopic.php' in url or 'viewforum.php' in url


def role_of(label, url, sentence):
    words = (label + ' ' + path_name(url)).lower()
    if re.search(r'split|part\s*0*\d|\.part\d', words):
        return SPLIT_PART
    if re.search(r'mirror|moddb page|ratio|\barchive\b|alternat', words):
        return ALTERNATIVE
    if re.search(r'update|patch|fix', words):
        return PATCH
    if manual_host(url) and not is_archive_link(url) and 'download' not in words:
        return ALTERNATIVE
    return COMPLEMENT


def description_links(markup, page):
    downloads, forums, others = [], [], []
    for url, label, at in links_of(markup, page):
        if re.search(r'\.(png|jpe?g|gif|webp|bmp)$', path_name(url), re.I):
            continue
        before = text_of(markup[:at])
        sentence = (before[-200:] + ' ' + label).strip()
        if is_forum(url):
            forums.append({'label': label, 'url': url})
        elif is_archive_link(url) or manual_host(url):
            if not any(d['url'] == url for d in downloads):
                downloads.append({'label': label or path_name(url), 'url': url, 'role': role_of(label, url, sentence)})
        else:
            others.append({'label': label, 'url': url})
    return downloads, forums, others


def drop_refresh(url):
    return re.sub(r'&refresh=[^&]*', '', html.unescape(url))


def parse_listing(markup, page):
    entries = []
    for block in re.split(r'<!-- WPDM Link Template:[^>]*-->', markup)[1:]:
        title = re.search(r'class="package-title">\s*<a\s[^>]*?href=(["\'])(.*?)\1[^>]*>(.*?)</a>', block, re.S)
        button = re.search(r'data-downloadurl="([^"]+)"[^>]*>(.*?)</a>', block, re.S)
        if not title or not button:
            continue
        size = re.search(r'fa-hdd[^>]*></i>\s*([^<]*)<', block)
        files = re.search(r'(\d+)\s*file\(s\)', block)
        body = re.search(r'text-small">.*?</div>\s*<div>(.*?)<div class="ml-3 wpdmdl-btn">', block, re.S)
        body = body.group(1) if body else ''
        source = urllib.parse.urljoin(page, html.unescape(title.group(2)))
        downloads, forums, others = description_links(body, page)
        entries.append({
            'id': urllib.parse.urlsplit(source).path.strip('/').rsplit('/', 1)[-1],
            'title': text_of(title.group(3)),
            'source_page': source,
            'download_url': drop_refresh(button.group(1)),
            'download_label': text_of(button.group(2)),
            'reported_size': size.group(1).strip() if size else '',
            'reported_files': int(files.group(1)) if files else None,
            'description': text_of(body),
            'links': downloads, 'forums': forums, 'other_links': others,
        })
    return entries


def parse_pagination(markup, page):
    pages = []
    for url, _, _ in links_of(markup, page):
        base = urllib.parse.urlsplit(page)
        here = urllib.parse.urlsplit(url)
        if here.netloc == base.netloc and re.search(r'/page/\d+/?$|[?&](cp|paged)=\d+', url) and here.path.startswith(base.path.split('/page/')[0]):
            if url not in pages:
                pages.append(url)
    return pages


def parse_detail(markup):
    fields = {}
    for name, value in re.findall(r'<li class="list-group-item[^"]*">\s*([^<]+?)\s*<span class="badge">(.*?)</span>', markup, re.S):
        fields[name.strip().lower()] = text_of(value)
    return {'author': fields.get('author', ''), 'created': fields.get('create date', ''), 'updated': fields.get('last updated', ''),
            'size': fields.get('file size', '')}


def parse_size(text):
    match = re.match(r'\s*([\d.,]+)\s*(bytes|[kmg]?b)', text or '', re.I)
    if not match:
        return 0
    number = float(match.group(1).replace(',', '.'))
    return int(number * {'bytes': 1, 'b': 1, 'kb': 1 << 10, 'mb': 1 << 20, 'gb': 1 << 30}[match.group(2).lower()])


def category_links(markup, page):
    found = {}
    for url, label, _ in links_of(markup, page):
        match = re.search(r'/download-category/([^/]+)/?$', url)
        if match:
            found.setdefault(match.group(1), url)
    return found


def mediafire_direct(markup):
    match = re.search(r'href="(https?://download[^"/]*\.mediafire\.com/[^"]+)"', markup)
    return html.unescape(match.group(1)) if match else None


def moddb_start(markup, page):
    links = {url for url, _, _ in links_of(markup, page) if re.search(r'/downloads/start/\d+', url)}
    return links.pop() if len(links) == 1 else None


def moddb_mirror(markup, page):
    links = {url for url, _, _ in links_of(markup, page) if '/downloads/mirror/' in url}
    return links.pop() if len(links) == 1 else None


def sentences(text):
    return [part.strip() for part in re.split(r'(?<=[.!?])\s+|\n', text) if part.strip()]


def engine_need(text, engine):
    for sentence in sentences(text):
        if re.search(r'\b' + engine + r'\b', sentence) and re.search(
                r'requir|need|pre-packed|pre-packaged|works? (as a standalone mod )?for|standalone|under ' + engine + r'|with the ' + engine + r'|v1\.0\d|custom levels', sentence, re.I):
            return sentence
    return None


def needed_sentences(text):
    return [s for s in sentences(text) if re.search(r'\bneed|requir|make sure to download|not include', s, re.I) and not re.search(r"(don't|not) need|not required", s, re.I)]
