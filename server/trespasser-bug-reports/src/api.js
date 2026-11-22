import { DUMP_FRAME, INCIDENT, readClaim, readClaims, readFeedback, readIncident } from './incidents.js';
import { ipGroup } from './limits.js';
import { PROTOCOL, SERVICE } from './service.js';

export const API_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'",
};

const RETRY_AFTER_SECONDS = '60';
const MAX_LOOKUP_BODY_BYTES = 16 * 1024;
const MAX_CHUNK_BODY_BYTES = DUMP_FRAME.headerBytes + INCIDENT.dumpChunkBytes;

const STATUS_OF_CODE = {
  ok: 200,
  bad_request: 400,
  origin_not_allowed: 403,
  not_found: 404,
  stale_request: 409,
  already_answered: 409,
  dump_incomplete: 409,
  too_large: 413,
  unsupported_media_type: 415,
  incident_hourly_limit: 429,
  incident_daily_limit: 429,
  limit: 429,
  unavailable: 502,
};

export function reply(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...API_HEADERS, 'Content-Type': 'application/json; charset=utf-8', ...extra },
  });
}

function failure(code, detail, extraBody = {}) {
  const status = STATUS_OF_CODE[code] ?? 500;
  const payload = detail === undefined ? { error: code, ...extraBody } : { error: code, detail, ...extraBody };
  return reply(payload, status, status === 429 ? { 'Retry-After': RETRY_AFTER_SECONDS } : {});
}

export const clientAddress = (request) => ipGroup(request.headers.get('CF-Connecting-IP') ?? 'local');

async function readBoundedBytes(request, maxBytes) {
  const declared = request.headers.get('Content-Length');
  if (declared !== null && !(/^[0-9]{1,12}$/.test(declared) && Number(declared) <= maxBytes)) return { code: 'too_large' };
  if (!request.body) return { bytes: new Uint8Array(0) };
  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > maxBytes) {
      await reader.cancel();
      return { code: 'too_large' };
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, at);
    at += chunk.byteLength;
  }
  return { bytes };
}

export async function readBoundedText(request, maxBytes) {
  const read = await readBoundedBytes(request, maxBytes);
  if (read.code) return read;
  try {
    return { text: new TextDecoder('utf-8', { fatal: true }).decode(read.bytes) };
  } catch {
    return { code: 'bad_request', detail: 'encoding' };
  }
}

const isJson = (request) => /^application\/json(\s*;|$)/i.test(request.headers.get('Content-Type') ?? '');
const isBinary = (request) => /^application\/octet-stream(\s*;|$)/i.test(request.headers.get('Content-Type') ?? '');

const JSON_ROUTES = {
  '/v1/incidents': { maxBytes: INCIDENT.frameChars, kind: 'file' },
  '/v1/incidents/dump/start': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'dump_start' },
  '/v1/incidents/dump/end': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'dump_end' },
  '/v1/incidents/status': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'status' },
  '/v1/incidents/read': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'read' },
  '/v1/incidents/feedback': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'feedback' },
};

export async function handlePublic(request, env, desk, log = () => {}) {
  const url = new URL(request.url);
  if (url.pathname === '/' || url.pathname === '/health') {
    if (request.method !== 'GET') return reply({ error: 'method_not_allowed' }, 405, { Allow: 'GET' });
    return reply({ service: SERVICE, protocol: PROTOCOL, status: 'ok' });
  }
  if (url.pathname === '/v1/incidents/dump/chunk') {
    if (request.method !== 'POST') return reply({ error: 'method_not_allowed' }, 405, { Allow: 'POST' });
    if (request.headers.has('Origin')) return failure('origin_not_allowed');
    if (!isBinary(request)) return failure('unsupported_media_type');
    const claim = readClaim({
      ticket: request.headers.get('X-JPT-Ticket'),
      receipt: request.headers.get('X-JPT-Receipt'),
    });
    if (!claim) return failure('not_found');
    const offsetText = request.headers.get('X-JPT-Dump-Offset') ?? '';
    if (!/^[0-9]{1,10}$/.test(offsetText)) return failure('bad_request', 'offset');
    const offset = Number(offsetText);
    const body = await readBoundedBytes(request, MAX_CHUNK_BODY_BYTES);
    if (body.code) return failure(body.code, body.detail);
    try {
      const answer = await desk.appendDumpChunk(claim, offset, body.bytes);
      if (answer.code !== 'ok') return failure(answer.code, answer.detail, answer.stored !== undefined ? { stored: answer.stored } : {});
      return reply({ stored: answer.stored, bytes: answer.bytes });
    } catch (error) {
      log(`incidents: dump chunk unavailable: ${error?.message ?? error}`);
      return failure('unavailable');
    }
  }

  const route = JSON_ROUTES[url.pathname];
  if (!route) return failure('not_found');
  if (request.method !== 'POST') return reply({ error: 'method_not_allowed' }, 405, { Allow: 'POST' });
  if (request.headers.has('Origin')) return failure('origin_not_allowed');
  if (!isJson(request)) return failure('unsupported_media_type');
  const address = clientAddress(request);
  const body = await readBoundedText(request, route.maxBytes);
  if (body.code) return failure(body.code, body.detail);
  let parsed;
  try {
    parsed = JSON.parse(body.text);
  } catch {
    return failure('bad_request', 'invalid JSON');
  }
  try {
    if (route.kind === 'file') {
      const { incident, dump, clientId, code, detail } = readIncident(parsed);
      if (!incident) return failure(code, detail);
      const filed = await desk.file(incident, dump, clientId, address);
      if (filed.code !== 'ok') return failure(filed.code);
      const { code: _ok, ...payload } = filed;
      return reply(payload, 201);
    }
    if (route.kind === 'dump_start') {
      const claim = readClaim(parsed);
      if (!claim) return failure('not_found');
      const answer = await desk.startDump(claim);
      if (answer.code !== 'ok') return failure(answer.code, answer.detail);
      const { code: _ok, ...payload } = answer;
      return reply(payload);
    }
    if (route.kind === 'dump_end') {
      const claim = readClaim(parsed);
      if (!claim) return failure('not_found');
      const answer = await desk.endDump(claim);
      if (answer.code !== 'ok') return failure(answer.code, answer.detail, answer.stored !== undefined ? { stored: answer.stored } : {});
      const { code: _ok, ...payload } = answer;
      return reply(payload);
    }
    if (route.kind === 'status') {
      const claims = readClaims(parsed);
      if (!claims) return failure('bad_request', 'reports');
      const answer = await desk.statuses(claims);
      return reply({ reports: answer.reports });
    }
    if (route.kind === 'read') {
      const claim = readClaim(parsed);
      if (!claim) return failure('bad_request', 'receipt');
      const answer = await desk.read(claim);
      return answer.code === 'ok' ? reply({ report: answer.report }) : failure(answer.code);
    }
    const { feedback, detail } = readFeedback(parsed);
    if (!feedback) return failure('bad_request', detail);
    const answer = await desk.feedback(feedback);
    if (answer.code !== 'ok') return failure(answer.code);
    const { code: _ok, ...payload } = answer;
    return reply(payload);
  } catch (error) {
    log(`incidents: desk unavailable: ${error?.message ?? error}`);
    return failure('unavailable');
  }
}
