import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from './admin_page.js';
import { dumpDownloadName, isTicket, logDownloadName, readStatusChange } from './incidents.js';
import { sha256Hex } from './sha256.js';

const MIN_TOKEN_CHARS = 16;
const MAX_STATUS_BODY_CHARS = 8 * 1024;

const SECURITY_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Resource-Policy': 'same-origin',
  'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; " +
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
};

const ASSETS = {
  '/admin': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/app.css': [ADMIN_CSS, 'text/css; charset=utf-8'],
  '/admin/app.js': [ADMIN_JS, 'text/javascript; charset=utf-8'],
};

export const isAdminPath = (pathname) => pathname === '/admin' || pathname.startsWith('/admin/');

function reply(body, status, type, extra = {}) {
  return new Response(body, { status, headers: { ...SECURITY_HEADERS, 'Content-Type': type, ...extra } });
}

const json = (value, status = 200, extra = {}) => reply(JSON.stringify(value), status, 'application/json; charset=utf-8', extra);

export function tokenMatches(authorization, token) {
  if (typeof token !== 'string' || token.length < MIN_TOKEN_CHARS) return false;
  const given = typeof authorization === 'string' && authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
  const a = sha256Hex(given);
  const b = sha256Hex(token);
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

const INCIDENTS_PATH = '/admin/api/incidents';
const METHODS = { incident: ['GET', 'DELETE'], incident_status: ['POST'] };

function adminApi(pathname) {
  if (pathname === INCIDENTS_PATH) return { name: 'incidents' };
  if (pathname.startsWith(`${INCIDENTS_PATH}/`)) {
    const [ticket, rest, ...more] = pathname.slice(INCIDENTS_PATH.length + 1).split('/');
    if (more.length === 0 && rest === undefined) return { name: 'incident', ticket };
    if (more.length === 0 && rest === 'log') return { name: 'incident_log', ticket };
    if (more.length === 0 && rest === 'dump') return { name: 'incident_dump', ticket };
    if (more.length === 0 && rest === 'status') return { name: 'incident_status', ticket };
  }
  return null;
}

async function saveIncidentStatus(request, ticket, desk) {
  const text = await request.text();
  if (text.length > MAX_STATUS_BODY_CHARS) return json({ error: 'too_large' }, 413);
  let body = null;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ error: 'bad_request', detail: 'invalid JSON' }, 400);
  }
  const { change, detail } = readStatusChange(body);
  if (!change) return json({ error: 'bad_request', detail }, 400);
  const saved = await desk.setIncidentStatus(ticket, change);
  return saved ? json(saved) : json({ error: 'not_found' }, 404);
}

export async function handleAdmin(request, env, desk, log = () => {}) {
  const url = new URL(request.url);
  const api = adminApi(url.pathname);
  const methods = METHODS[api?.name] ?? ['GET'];
  if (!methods.includes(request.method)) return json({ error: 'method_not_allowed' }, 405, { Allow: methods.join(', ') });
  const asset = ASSETS[url.pathname];
  if (asset) return reply(asset[0], 200, asset[1]);
  if (!api) return json({ error: 'not_found' }, 404);
  if (typeof env.JPT_BUG_REPORT_ADMIN_TOKEN !== 'string' || env.JPT_BUG_REPORT_ADMIN_TOKEN.length < MIN_TOKEN_CHARS) {
    return json({ error: 'not_configured' }, 503);
  }
  if (!tokenMatches(request.headers.get('Authorization'), env.JPT_BUG_REPORT_ADMIN_TOKEN)) {
    return json({ error: 'unauthorized' }, 401, { 'WWW-Authenticate': 'Bearer' });
  }
  if (api.ticket !== undefined && !isTicket(api.ticket)) return json({ error: 'bad_request', detail: 'invalid report id' }, 400);
  try {
    if (api.name === 'incidents') return json(await desk.incidents());
    if (api.name === 'incident_status') return await saveIncidentStatus(request, api.ticket, desk);
    if (request.method === 'DELETE') {
      return await desk.deleteIncident(api.ticket) ? json({ deleted: api.ticket }) : json({ error: 'not_found' }, 404);
    }
    const incident = await desk.incident(api.ticket);
    if (!incident) return json({ error: 'not_found' }, 404);
    if (api.name === 'incident') return json(incident);
    if (api.name === 'incident_dump') {
      const dump = await desk.incidentDump(api.ticket);
      if (!dump) return json({ error: 'not_found' }, 404);
      return reply(dump.bytes, 200, 'application/octet-stream',
        { 'Content-Disposition': `attachment; filename="${dumpDownloadName(incident)}"` });
    }
    return reply(incident.log_text, 200, 'text/plain; charset=utf-8',
      { 'Content-Disposition': `attachment; filename="${logDownloadName(incident)}"` });
  } catch (error) {
    log(`admin: desk unavailable: ${error?.message ?? error}`);
    return json({ error: 'unavailable' }, 502);
  }
}
