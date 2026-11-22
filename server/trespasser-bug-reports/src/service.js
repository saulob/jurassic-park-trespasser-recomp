import { DUMP_FRAME, INCIDENT, IncidentDesk, isMinidump } from './incidents.js';
import { toHex } from './sha256.js';

export const SERVICE = 'trespasser-bug-reports';
export const PROTOCOL = 2;

const TICKET_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const TICKET_LENGTH = 6;

export class IncidentService {
  constructor(host) {
    this.host = host;
    host.sql.run('CREATE TABLE IF NOT EXISTS service_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)');
    let salt = host.sql.get('SELECT value FROM service_meta WHERE key = ?', 'salt')?.value;
    if (!salt) {
      salt = this.randomHex(32);
      host.sql.run('INSERT INTO service_meta (key, value) VALUES (?, ?)', 'salt', salt);
    }
    this.incidents = new IncidentDesk(host.sql, salt);
  }

  randomHex(byteCount) {
    return toHex(this.host.randomBytes(byteCount));
  }

  newTicket() {
    for (let attempt = 0; attempt < 32; attempt++) {
      const bytes = this.host.randomBytes(TICKET_LENGTH);
      let code = 'JPT-';
      for (let i = 0; i < TICKET_LENGTH; i++) code += TICKET_ALPHABET[bytes[i] & 31];
      if (!this.incidents.taken(code)) return code;
    }
    throw new Error('could not allocate ticket');
  }

  file(incident, dump, account, address) {
    const now = this.host.now();
    const keyAccount = account || address;
    return this.host.sql.transaction(() => {
      const refusal = this.incidents.refusal(keyAccount, address, now);
      if (refusal) return { code: refusal };
      const ticket = this.newTicket();
      const receipt = this.randomHex(32);
      this.incidents.file(incident, ticket, keyAccount, address, now, dump);
      this.incidents.track(ticket, receipt, now);
      return {
        code: 'ok',
        ticket,
        receipt,
        submitted_at: now,
        ...(dump && { dump: 'expected', dump_chunk_bytes: INCIDENT.dumpChunkBytes }),
      };
    });
  }

  startDump(claim) {
    if (!this.incidents.claimed(claim)) return { code: 'not_found' };
    this.incidents.trimDumps(this.host.now());
    const dump = this.incidents.dump(claim.ticket);
    if (!dump) return { code: 'bad_request', detail: 'no crash dump was announced for this report' };
    if (dump.complete) return { code: 'ok', stored: dump.bytes, bytes: dump.bytes, complete: true };
    const stored = this.incidents.dumpStored(claim.ticket);
    return { code: 'ok', stored, bytes: dump.bytes, complete: false };
  }

  appendDumpChunk(claim, offset, frameBytes) {
    if (!this.incidents.claimed(claim)) return { code: 'not_found' };
    this.incidents.trimDumps(this.host.now());
    const dump = this.incidents.dump(claim.ticket);
    if (!dump) return { code: 'bad_request', detail: 'no crash dump was announced for this report' };
    if (dump.complete) return { code: 'ok', stored: dump.bytes, bytes: dump.bytes };
    if (frameBytes.length <= DUMP_FRAME.headerBytes ||
        frameBytes[0] !== DUMP_FRAME.magic[0] ||
        frameBytes[1] !== DUMP_FRAME.magic[1] ||
        frameBytes[2] !== DUMP_FRAME.version ||
        frameBytes[3] !== DUMP_FRAME.chunk) {
      return { code: 'bad_request', detail: 'invalid dump frame header' };
    }
    const data = frameBytes.subarray(DUMP_FRAME.headerBytes);
    if (data.length === 0 || data.length > INCIDENT.dumpChunkBytes) {
      return { code: 'too_large', detail: 'chunk size' };
    }
    const stored = this.incidents.dumpStored(claim.ticket);
    if (!Number.isInteger(offset) || offset !== stored) {
      return { code: 'bad_request', detail: 'chunk offset mismatch', stored };
    }
    if (offset + data.length > dump.bytes) {
      return { code: 'too_large', detail: 'dump overflow' };
    }
    if (offset === 0 && !isMinidump(data)) {
      return { code: 'bad_request', detail: 'not a minidump' };
    }
    this.incidents.appendDump(claim.ticket, offset, data);
    return { code: 'ok', stored: offset + data.length, bytes: dump.bytes };
  }

  endDump(claim) {
    if (!this.incidents.claimed(claim)) return { code: 'not_found' };
    const dump = this.incidents.dump(claim.ticket);
    if (!dump) return { code: 'bad_request', detail: 'no crash dump was announced for this report' };
    if (!this.incidents.completeDump(claim.ticket)) {
      return { code: 'dump_incomplete', detail: 'the dump is not complete', stored: this.incidents.dumpStored(claim.ticket) };
    }
    return { code: 'ok', stored: dump.bytes, bytes: dump.bytes, complete: true };
  }

  statuses(claims) {
    return { code: 'ok', reports: this.incidents.statuses(claims) };
  }

  read(claim) {
    const report = this.incidents.report(claim);
    return report ? { code: 'ok', report } : { code: 'not_found' };
  }

  feedback(feedback) {
    return this.host.sql.transaction(() => {
      const { state, code } = this.incidents.answer(feedback, this.host.now());
      return state ? { code: 'ok', ...state } : { code };
    });
  }

  incidentsList() {
    return this.incidents.list();
  }

  incidentForAdmin(ticket) {
    const stored = this.incidents.get(ticket);
    if (!stored) return null;
    const status = this.incidents.status(ticket);
    return { ...stored, ...status, ...this.incidents.dumpSummary(ticket) };
  }

  incidentDump(ticket) {
    const bytes = this.incidents.dumpContent(ticket);
    return bytes ? { bytes } : null;
  }

  setIncidentStatus(ticket, change) {
    return this.incidents.setStatus(ticket, change, this.host.now());
  }

  deleteIncident(ticket) {
    return this.incidents.remove(ticket);
  }
}
