import { digestsMatch, sha256Hex } from './sha256.js';

export const INCIDENT = {
  frameChars: 160 * 1024,
  descriptionChars: 500,
  logBytes: 64 * 1024,
  metadataChars: 64,
  previewChars: 140,
  perAccountPerHour: 2,
  perAccountPerDay: 10,
  perAddressPerDay: 30,
  perDay: 300,
  kept: 500,
  messageChars: 500,
  statusBatch: 50,
  statusKept: 5000,
  dumpBytes: 8 * 1024 * 1024,
  dumpMinBytes: 32,
  dumpChunkBytes: 64 * 1024,
  dumpsKept: 100,
  dumpPendingMs: 24 * 3600 * 1000,
};

// Binary crash dump frame header: "DI", version 1, chunk type 1.
export const DUMP_FRAME = { magic: [0x44, 0x49], version: 1, chunk: 1, headerBytes: 4 };
const MINIDUMP_MAGIC = [0x4d, 0x44, 0x4d, 0x50];

export const INCIDENT_STATUSES = [
  'new', 'investigating', 'awaiting_player_test', 'fixed',
  'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed',
];
export const AWAITING_TEST = 'awaiting_player_test';
export const TEST_RESULTS = ['fixed', 'not_fixed'];

export const INCIDENT_LIMITED = {
  incident_hourly_limit: 'incident_hourly_limited',
  incident_daily_limit: 'incident_daily_limited',
  limit: 'incident_limited',
};

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;
const ENVELOPE = new Set(['client_id']);
const LOG_FILE = /^trespasser_recomp_[0-9]{3,10}\.log$/;
const DUMP_KEYS = new Set(['dump_file', 'dump_bytes']);
const VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const CLIENT_ID = /^[0-9a-f]{32,64}$/;
const TICKET = /^JPT-[A-HJ-NP-Z2-9]{6}$/;
const RECEIPT = /^[0-9a-f]{64}$/;
const STATUS_FIELDS = new Set(['status', 'fixed_version', 'player_message']);
const FEEDBACK_FIELDS = new Set(['ticket', 'receipt', 'revision', 'result', 'launcher_version', 'game_build']);

export const isTicket = (text) => typeof text === 'string' && TICKET.test(text);

export function logDownloadName(incident) {
  if (!isTicket(incident.ticket)) return 'trespasser-report.log';
  return LOG_FILE.test(incident.log_file) ? `${incident.ticket}-${incident.log_file}` : `${incident.ticket}.log`;
}

export const dumpNameOf = (logFile) => `${logFile.slice(0, -4)}.dmp`;

export function dumpDownloadName(incident) {
  return isTicket(incident.ticket) && LOG_FILE.test(incident.log_file)
    ? `${incident.ticket}-${dumpNameOf(incident.log_file)}`
    : `${incident.ticket}.dmp`;
}

export const isMinidump = (bytes) =>
  bytes.length >= MINIDUMP_MAGIC.length && MINIDUMP_MAGIC.every((b, i) => bytes[i] === b);

const utf8Bytes = (text) => new TextEncoder().encode(text).length;

// eslint-disable-next-line no-control-regex
const HAS_FORBIDDEN = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/;
// eslint-disable-next-line no-control-regex
const HAS_CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

class Refusal extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

function text(msg, field, maxChars) {
  const value = msg[field];
  if (typeof value !== 'string' || HAS_CONTROL.test(value) || value !== value.trim()) throw new Refusal('bad_request', field);
  if (Array.from(value).length > maxChars) throw new Refusal('too_large', field);
  return value;
}

function description(msg) {
  const value = msg.description;
  if (typeof value !== 'string' || HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'description');
  if (!value.trim()) throw new Refusal('bad_request', 'empty description');
  if (Array.from(value).length > INCIDENT.descriptionChars) throw new Refusal('too_large', 'description');
  return value;
}

function logText(msg) {
  const value = msg.log_text;
  if (typeof value !== 'string') throw new Refusal('bad_request', 'log_text');
  if (utf8Bytes(value) > INCIDENT.logBytes) throw new Refusal('too_large', 'log_text');
  if (HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'log_text');
  if (!value.trim()) throw new Refusal('bad_request', 'empty log');
  return value;
}

function matching(msg, field, pattern) {
  const value = text(msg, field, INCIDENT.metadataChars);
  if (!pattern.test(value)) throw new Refusal('bad_request', field);
  return value;
}

function yesNo(msg, field) {
  if (msg[field] !== 'yes' && msg[field] !== 'no') throw new Refusal('bad_request', field);
  return msg[field];
}

const READERS = {
  description,
  launcher_version: (msg) => matching(msg, 'launcher_version', VERSION),
  game_build: (msg) => text(msg, 'game_build', INCIDENT.metadataChars),
  game_version: (msg) => text(msg, 'game_version', INCIDENT.metadataChars),
  operating_system: (msg) => matching(msg, 'operating_system', /./),
  log_file: (msg) => matching(msg, 'log_file', LOG_FILE),
  log_clipped: (msg) => yesNo(msg, 'log_clipped'),
  log_text: logText,
};

function readDump(msg, logFile) {
  const named = Object.hasOwn(msg, 'dump_file');
  const sized = Object.hasOwn(msg, 'dump_bytes');
  if (!named && !sized) return null;
  if (!named || !sized) throw new Refusal('bad_request', 'dump_file and dump_bytes go together');
  if (msg.dump_file !== dumpNameOf(logFile)) throw new Refusal('bad_request', 'dump_file');
  const bytes = msg.dump_bytes;
  if (!Number.isInteger(bytes) || bytes < INCIDENT.dumpMinBytes) throw new Refusal('bad_request', 'dump_bytes');
  if (bytes > INCIDENT.dumpBytes) throw new Refusal('too_large', 'dump_bytes');
  return { file: msg.dump_file, bytes };
}

export function readIncident(msg) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) return { code: 'bad_request', detail: 'body' };
  for (const key of Object.keys(msg)) {
    if (!ENVELOPE.has(key) && !DUMP_KEYS.has(key) && !Object.hasOwn(READERS, key)) return { code: 'bad_request', detail: key };
  }
  let clientId = '';
  if (Object.hasOwn(msg, 'client_id')) {
    if (typeof msg.client_id !== 'string' || !CLIENT_ID.test(msg.client_id)) return { code: 'bad_request', detail: 'client_id' };
    clientId = msg.client_id;
  }
  try {
    const incident = {};
    for (const [field, read] of Object.entries(READERS)) incident[field] = read(msg);
    return { incident, dump: readDump(msg, incident.log_file), clientId };
  } catch (error) {
    if (error instanceof Refusal) return { code: error.code, detail: error.message };
    throw error;
  }
}

export function readClaim(entry) {
  if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) return null;
  const { ticket, receipt } = entry;
  return isTicket(ticket) && typeof receipt === 'string' && RECEIPT.test(receipt) ? { ticket, receipt } : null;
}

export function readClaims(msg) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) return null;
  if (Object.keys(msg).some((key) => key !== 'reports')) return null;
  const { reports } = msg;
  if (!Array.isArray(reports) || reports.length > INCIDENT.statusBatch) return null;
  const claims = reports.map(readClaim);
  return claims.includes(null) ? null : claims;
}

export function readStatusChange(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { detail: 'body' };
  for (const key of Object.keys(body)) if (!STATUS_FIELDS.has(key)) return { detail: key };
  const { status, fixed_version: version = '', player_message: message = '' } = body;
  if (!INCIDENT_STATUSES.includes(status)) return { detail: 'status' };
  if (typeof version !== 'string' || (status === 'fixed' && version && !VERSION.test(version))) return { detail: 'fixed_version' };
  if (typeof message !== 'string' || HAS_FORBIDDEN.test(message) || message.includes('\r')) return { detail: 'player_message' };
  if (Array.from(message).length > INCIDENT.messageChars) return { detail: 'player_message' };
  return { change: { status, fixed_version: status === 'fixed' ? version : '', player_message: message.trim() } };
}

export function readFeedback(msg) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) return { detail: 'body' };
  if (Object.keys(msg).some((key) => !FEEDBACK_FIELDS.has(key))) return { detail: 'fields' };
  const claim = readClaim(msg);
  if (!claim) return { detail: 'receipt' };
  if (!Number.isSafeInteger(msg.revision) || msg.revision <= 0) return { detail: 'revision' };
  if (!TEST_RESULTS.includes(msg.result)) return { detail: 'result' };
  const launcher = msg.launcher_version ?? '';
  if (typeof launcher !== 'string' || (launcher && !VERSION.test(launcher))) return { detail: 'launcher_version' };
  const build = msg.game_build ?? '';
  if (typeof build !== 'string' || HAS_CONTROL.test(build) || build !== build.trim() || Array.from(build).length > INCIDENT.metadataChars) {
    return { detail: 'game_build' };
  }
  return { feedback: { claim, revision: msg.revision, result: msg.result, launcher_version: launcher, game_build: build } };
}

const LIST_COLUMNS = `incidents.ticket AS ticket, ms, launcher_version, game_build, game_version, operating_system, log_file,
  substr(description, 1, ${INCIDENT.previewChars}) AS preview, COALESCE(t.status, 'new') AS status,
  COALESCE(t.status_ms, ms) AS status_updated_at,
  CASE WHEN t.status = '${AWAITING_TEST}' THEN CASE WHEN t.feedback_revision = t.status_ms THEN t.feedback ELSE 'pending' END
  ELSE '' END AS player_feedback,
  COALESCE((SELECT CASE complete WHEN 1 THEN 'stored' ELSE 'uploading' END FROM incident_dumps d
    WHERE d.ticket = incidents.ticket), 'none') AS dump_status`;

const feedbackOf = (row) => {
  if (row.status !== AWAITING_TEST) return '';
  return row.feedback_revision === row.status_ms ? row.feedback : 'pending';
};

const feedbackDetails = (row) => {
  const answer = feedbackOf(row);
  if (answer !== 'fixed' && answer !== 'not_fixed') return {};
  return {
    player_feedback_at: row.feedback_ms,
    player_feedback_launcher_version: row.feedback_launcher_version,
    player_feedback_game_build: row.feedback_game_build,
  };
};

const statusOf = (row) => ({
  status: row.status,
  status_updated_at: row.status_ms,
  fixed_version: row.fixed_version,
  player_message: row.player_message,
  player_feedback: feedbackOf(row),
  ...feedbackDetails(row),
});

const FEEDBACK_COLUMNS = [
  ['feedback', `TEXT NOT NULL DEFAULT 'pending'`],
  ['feedback_revision', 'INTEGER NOT NULL DEFAULT 0'],
  ['feedback_ms', 'INTEGER NOT NULL DEFAULT 0'],
  ['feedback_launcher_version', `TEXT NOT NULL DEFAULT ''`],
  ['feedback_game_build', `TEXT NOT NULL DEFAULT ''`],
];

export class IncidentDesk {
  constructor(sql, salt) {
    this.sql = sql;
    this.salt = salt;
    sql.run(`CREATE TABLE IF NOT EXISTS incidents (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, game_version TEXT NOT NULL,
      operating_system TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL,
      log_text TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_quota (account_key TEXT NOT NULL, address_key TEXT NOT NULL,
      ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL,
      submitted_ms INTEGER NOT NULL, status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL,
      player_message TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_dumps (ticket TEXT PRIMARY KEY, file TEXT NOT NULL, bytes INTEGER NOT NULL,
      complete INTEGER NOT NULL, ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_dump_chunks (ticket TEXT NOT NULL, pos INTEGER NOT NULL, data BLOB NOT NULL,
      PRIMARY KEY (ticket, pos))`);
    const have = new Set(sql.all('PRAGMA table_info(incident_tracking)').map((c) => c.name));
    for (const [column, type] of FEEDBACK_COLUMNS) {
      if (!have.has(column)) sql.run(`ALTER TABLE incident_tracking ADD COLUMN ${column} ${type}`);
    }
  }

  key(kind, value) {
    return sha256Hex(`${this.salt}|${kind}|${value}`).slice(0, 32);
  }

  refusal(account, address, now) {
    this.sql.run('DELETE FROM incident_quota WHERE ms <= ?', now - DAY_MS);
    const accountKey = this.key('account', account);
    const used = this.sql.get(`SELECT COUNT(*) AS everyone, COALESCE(SUM(account_key = ?), 0) AS account_day,
      COALESCE(SUM(account_key = ? AND ms > ?), 0) AS account_hour, COALESCE(SUM(address_key = ?), 0) AS address
      FROM incident_quota`, accountKey, accountKey, now - HOUR_MS, this.key('address', address));
    if (used.account_hour >= INCIDENT.perAccountPerHour) return 'incident_hourly_limit';
    if (used.account_day >= INCIDENT.perAccountPerDay) return 'incident_daily_limit';
    if (used.everyone >= INCIDENT.perDay || used.address >= INCIDENT.perAddressPerDay) return 'limit';
    return null;
  }

  file(incident, ticket, account, address, now, dump = null) {
    this.sql.run(`INSERT INTO incidents (ticket, ms, launcher_version, game_build, game_version, operating_system,
      description, log_file, log_clipped, log_text) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, ticket, now, incident.launcher_version,
    incident.game_build, incident.game_version, incident.operating_system, incident.description, incident.log_file,
    incident.log_clipped, incident.log_text);
    this.sql.run('INSERT INTO incident_quota (account_key, address_key, ms) VALUES (?, ?, ?)', this.key('account', account),
      this.key('address', address), now);
    this.sql.run(`DELETE FROM incidents WHERE rowid IN
      (SELECT rowid FROM incidents ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, INCIDENT.kept);
    if (dump) this.sql.run('INSERT INTO incident_dumps (ticket, file, bytes, complete, ms) VALUES (?, ?, ?, 0, ?)', ticket, dump.file, dump.bytes, now);
    this.trimDumps(now);
  }

  trimDumps(now) {
    this.sql.run('DELETE FROM incident_dumps WHERE complete = 0 AND ms <= ?', now - INCIDENT.dumpPendingMs);
    this.sql.run('DELETE FROM incident_dumps WHERE ticket NOT IN (SELECT ticket FROM incidents)');
    this.sql.run(`DELETE FROM incident_dumps WHERE rowid IN
      (SELECT rowid FROM incident_dumps ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, INCIDENT.dumpsKept);
    this.sql.run('DELETE FROM incident_dump_chunks WHERE ticket NOT IN (SELECT ticket FROM incident_dumps)');
  }

  dump(ticket) {
    return this.sql.get('SELECT * FROM incident_dumps WHERE ticket = ?', ticket) ?? null;
  }

  dumpStored(ticket) {
    return this.sql.get('SELECT COALESCE(MAX(pos + LENGTH(data)), 0) AS n FROM incident_dump_chunks WHERE ticket = ?', ticket).n;
  }

  appendDump(ticket, pos, bytes) {
    this.sql.run('INSERT INTO incident_dump_chunks (ticket, pos, data) VALUES (?, ?, ?)', ticket, pos, bytes);
  }

  completeDump(ticket) {
    const dump = this.dump(ticket);
    if (!dump) return false;
    if (dump.complete) return true;
    if (this.dumpStored(ticket) !== dump.bytes) return false;
    const first = this.sql.get('SELECT data FROM incident_dump_chunks WHERE ticket = ? AND pos = 0', ticket);
    if (!first || !isMinidump(new Uint8Array(first.data))) return false;
    this.sql.run('UPDATE incident_dumps SET complete = 1 WHERE ticket = ?', ticket);
    return true;
  }

  dumpContent(ticket) {
    const dump = this.dump(ticket);
    if (!dump?.complete) return null;
    const out = new Uint8Array(dump.bytes);
    let at = 0;
    for (const chunk of this.sql.all('SELECT data FROM incident_dump_chunks WHERE ticket = ? ORDER BY pos', ticket)) {
      const data = new Uint8Array(chunk.data);
      out.set(data, at);
      at += data.length;
    }
    return at === dump.bytes ? out : null;
  }

  dumpSummary(ticket) {
    const dump = this.dump(ticket);
    return { dump_status: dump ? (dump.complete ? 'stored' : 'uploading') : 'none', dump_file: dump?.file ?? '', dump_bytes: dump?.bytes ?? 0 };
  }

  receiptHash(receipt) {
    return sha256Hex(`${this.salt}|receipt|${receipt}`);
  }

  track(ticket, receipt, now) {
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, ?, ?, 'new', ?, '', '')`, ticket, this.receiptHash(receipt), now, now);
    this.sql.run(`DELETE FROM incident_tracking WHERE rowid IN
      (SELECT rowid FROM incident_tracking ORDER BY submitted_ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, INCIDENT.statusKept);
  }

  claimed({ ticket, receipt }) {
    const hash = this.receiptHash(receipt);
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    return row && digestsMatch(row.receipt_hash, hash) ? row : null;
  }

  statuses(claims) {
    return claims.map((claim) => this.claimed(claim)).filter(Boolean).map((row) => ({
      ticket: row.ticket,
      status: row.status,
      status_updated_at: row.status_ms,
      ...(row.fixed_version ? { fixed_version: row.fixed_version } : {}),
      ...(row.player_message ? { player_message: row.player_message } : {}),
      ...this.testState(row),
    }));
  }

  testState(row) {
    if (row.status !== AWAITING_TEST) return {};
    const feedback = feedbackOf(row);
    return { test_feedback: feedback, ...(feedback === 'pending' ? {} : { test_feedback_at: row.feedback_ms }) };
  }

  answer({ claim, revision, result, launcher_version: launcher, game_build: build }, now) {
    const row = this.claimed(claim);
    if (!row) return { code: 'not_found' };
    if (row.status !== AWAITING_TEST || row.status_ms !== revision) return { code: 'stale_request' };
    const given = feedbackOf(row);
    if (given !== result) {
      if (given !== 'pending') return { code: 'already_answered' };
      this.sql.run(`UPDATE incident_tracking SET feedback = ?, feedback_revision = ?, feedback_ms = ?, feedback_launcher_version = ?,
        feedback_game_build = ? WHERE ticket = ?`, result, revision, now, launcher, build, row.ticket);
    }
    const stored = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', row.ticket);
    return { state: { ticket: row.ticket, revision, ...this.testState(stored) } };
  }

  report(claim) {
    const row = this.claimed(claim);
    const stored = row && this.get(row.ticket);
    if (!stored) return null;
    const { ms, ...report } = stored;
    return { ...report, submitted_at: ms };
  }

  status(ticket) {
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    if (row) return statusOf(row);
    const stored = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket);
    return stored ? { status: 'new', status_updated_at: stored.ms, fixed_version: '', player_message: '', player_feedback: '' } : null;
  }

  setStatus(ticket, change, now) {
    const current = this.status(ticket);
    if (!current) return null;
    if (Object.keys(change).every((field) => current[field] === change[field])) return current;
    const submitted = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket)?.ms ?? current.status_updated_at;
    const changedAt = change.status === AWAITING_TEST ? Math.max(now, current.status_updated_at + 1) : now;
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, '', ?, ?, ?, ?, ?) ON CONFLICT(ticket) DO UPDATE SET status = excluded.status, status_ms = excluded.status_ms,
      fixed_version = excluded.fixed_version, player_message = excluded.player_message`, ticket, submitted, change.status, changedAt,
    change.fixed_version, change.player_message);
    return this.status(ticket);
  }

  remove(ticket) {
    return this.sql.transaction(() => {
      const existed = this.taken(ticket);
      this.sql.run('DELETE FROM incidents WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_tracking WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_dumps WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_dump_chunks WHERE ticket = ?', ticket);
      return existed;
    });
  }

  taken(ticket) {
    return Boolean(this.sql.get(`SELECT 1 AS x FROM incidents WHERE ticket = ?
      UNION ALL SELECT 1 AS x FROM incident_tracking WHERE ticket = ?`, ticket, ticket));
  }

  list() {
    return {
      incidents: this.sql.all(`SELECT ${LIST_COLUMNS} FROM incidents LEFT JOIN incident_tracking t ON t.ticket = incidents.ticket
        ORDER BY ms DESC, incidents.rowid DESC`),
      kept: INCIDENT.kept,
    };
  }

  get(ticket) {
    return this.sql.get('SELECT * FROM incidents WHERE ticket = ?', ticket) ?? null;
  }
}
