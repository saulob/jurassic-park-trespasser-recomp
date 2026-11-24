import assert from 'node:assert/strict';
import test from 'node:test';

import { localService } from '../local/host.js';
import { DUMP_FRAME, INCIDENT, INCIDENT_STATUSES, dumpDownloadName, isMinidump, logDownloadName, readFeedback, readIncident, readStatusChange } from '../src/incidents.js';

function sampleIncident(overrides = {}) {
  return {
    description: 'Game froze near the harbour gate.',
    launcher_version: '0.1.0',
    game_build: 'English (Retail 116)',
    game_version: '116',
    operating_system: 'Windows 11 (build 26200)',
    log_file: 'trespasser_recomp_294.log',
    log_clipped: 'no',
    log_text: '[0.001] [loader] started\n[1.234] [fault] access violation\n',
    ...overrides,
  };
}

function makeMinidump(size = 128) {
  const bytes = new Uint8Array(size);
  bytes.set([0x4d, 0x44, 0x4d, 0x50]);
  for (let i = 4; i < size; i++) bytes[i] = i & 0xff;
  return bytes;
}

function frameOf(chunk) {
  const frame = new Uint8Array(DUMP_FRAME.headerBytes + chunk.length);
  frame.set([DUMP_FRAME.magic[0], DUMP_FRAME.magic[1], DUMP_FRAME.version, DUMP_FRAME.chunk]);
  frame.set(chunk, DUMP_FRAME.headerBytes);
  return frame;
}

test('incident validation, ticket format, receipt secrecy and download names', () => {
  const parsed = readIncident(sampleIncident({ dump_file: 'trespasser_recomp_294.dmp', dump_bytes: 128 }));
  assert.ok(parsed.incident);
  assert.deepEqual(parsed.dump, { file: 'trespasser_recomp_294.dmp', bytes: 128 });

  assert.equal(readIncident(sampleIncident({ extra: 'nope' })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ dump_file: 'other.dmp', dump_bytes: 128 })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ dump_file: 'trespasser_recomp_294.dmp', dump_bytes: 16 })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ dump_file: 'trespasser_recomp_294.dmp', dump_bytes: INCIDENT.dumpBytes + 1 })).code, 'too_large');
  assert.equal(readIncident(sampleIncident({ log_file: '../evil.log' })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ log_file: 'trespasser_001.log' })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ log_file: 'trespasser_windows_001.log' })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ log_file: 'trespasser_linux_12.log' })).code, 'bad_request');
  assert.equal(readIncident(sampleIncident({ log_file: 'trespasser_linux_001.txt' })).code, 'bad_request');

  const linuxCanonical = readIncident(sampleIncident({
    operating_system: 'Linux: SteamOS',
    log_file: 'trespasser_recomp_001.log',
  }));
  assert.ok(linuxCanonical.incident);
  assert.equal(linuxCanonical.incident.log_file, 'trespasser_recomp_001.log');

  const linuxLegacy = readIncident(sampleIncident({
    operating_system: 'Steam Deck: SteamOS',
    log_file: 'trespasser_linux_001.log',
    dump_file: 'trespasser_linux_001.dmp',
    dump_bytes: 128,
  }));
  assert.ok(linuxLegacy.incident);
  assert.equal(linuxLegacy.incident.log_file, 'trespasser_linux_001.log');
  assert.deepEqual(linuxLegacy.dump, { file: 'trespasser_linux_001.dmp', bytes: 128 });
  assert.equal(logDownloadName({ ticket: 'JPT-234567', log_file: 'trespasser_linux_001.log' }), 'JPT-234567-trespasser_linux_001.log');
  assert.equal(dumpDownloadName({ ticket: 'JPT-234567', log_file: 'trespasser_linux_001.log' }), 'JPT-234567-trespasser_linux_001.dmp');

  assert.equal(logDownloadName({ ticket: 'JPT-234567', log_file: 'trespasser_recomp_294.log' }), 'JPT-234567-trespasser_recomp_294.log');
  assert.equal(dumpDownloadName({ ticket: 'JPT-234567', log_file: 'trespasser_recomp_294.log' }), 'JPT-234567-trespasser_recomp_294.dmp');
  assert.equal(logDownloadName({ ticket: 'JPT-234567', log_file: '../bad.log' }), 'JPT-234567.log');
  assert.equal(isMinidump(makeMinidump(32)), true);
  assert.equal(isMinidump(new Uint8Array([1, 2, 3, 4])), false);
});

test('dump upload in chunks, validation, resume, completion, expiry and retention', () => {
  let now = 1_700_000_000_000;
  const { service, store } = localService({ now: () => now });
  try {
    const dumpBytes = makeMinidump(90_000);
    const { incident, dump } = readIncident(sampleIncident({ dump_file: 'trespasser_recomp_294.dmp', dump_bytes: dumpBytes.length }));
    const filed = service.file(incident, dump, 'acct-1', '198.51.100.1');
    assert.equal(filed.code, 'ok');
    assert.match(filed.ticket, /^JPT-[A-HJ-NP-Z2-9]{6}$/);
    assert.match(filed.receipt, /^[0-9a-f]{64}$/);
    assert.equal(filed.dump, 'expected');
    assert.equal(filed.dump_chunk_bytes, INCIDENT.dumpChunkBytes);

    const claim = { ticket: filed.ticket, receipt: filed.receipt };
    const wrongClaim = { ticket: filed.ticket, receipt: '0'.repeat(64) };
    assert.equal(service.startDump(wrongClaim).code, 'not_found');

    // Non-MDMP rejected at offset 0.
    const badChunk = new Uint8Array(64);
    assert.equal(service.appendDumpChunk(claim, 0, frameOf(badChunk)).code, 'bad_request');

    // Invalid DI header rejected.
    const badHeader = new Uint8Array([0x00, 0x00, 1, 1, 0x4d, 0x44, 0x4d, 0x50]);
    assert.equal(service.appendDumpChunk(claim, 0, badHeader).code, 'bad_request');

    // Upload chunk 1 (64 KiB).
    const started = service.startDump(claim);
    assert.deepEqual(started, { code: 'ok', stored: 0, bytes: dumpBytes.length, complete: false });
    const part1 = dumpBytes.subarray(0, INCIDENT.dumpChunkBytes);
    const r1 = service.appendDumpChunk(claim, 0, frameOf(part1));
    assert.deepEqual(r1, { code: 'ok', stored: part1.length, bytes: dumpBytes.length });

    // Out-of-order offset rejected.
    assert.equal(service.appendDumpChunk(claim, 0, frameOf(part1)).code, 'bad_request');

    // Incomplete dump cannot finish.
    assert.equal(service.endDump(claim).code, 'dump_incomplete');
    assert.equal(service.incidentDump(filed.ticket), null);
    assert.equal(service.incidentForAdmin(filed.ticket).dump_status, 'uploading');

    // Resume reports stored offset.
    const resumed = service.startDump(claim);
    assert.deepEqual(resumed, { code: 'ok', stored: part1.length, bytes: dumpBytes.length, complete: false });

    // Upload chunk 2 and finalize.
    const part2 = dumpBytes.subarray(INCIDENT.dumpChunkBytes);
    const r2 = service.appendDumpChunk(claim, part1.length, frameOf(part2));
    assert.deepEqual(r2, { code: 'ok', stored: dumpBytes.length, bytes: dumpBytes.length });
    const ended = service.endDump(claim);
    assert.deepEqual(ended, { code: 'ok', stored: dumpBytes.length, bytes: dumpBytes.length, complete: true });

    const downloaded = service.incidentDump(filed.ticket);
    assert.ok(downloaded);
    assert.deepEqual(downloaded.bytes, dumpBytes);
    assert.equal(service.incidentForAdmin(filed.ticket).dump_status, 'stored');

    // Pending dump expires after 24h and removes orphan chunks.
    const { incident: inc2, dump: dump2 } = readIncident(sampleIncident({ dump_file: 'trespasser_recomp_294.dmp', dump_bytes: 128 }));
    const f2 = service.file(inc2, dump2, 'acct-2', '198.51.100.2');
    service.appendDumpChunk({ ticket: f2.ticket, receipt: f2.receipt }, 0, frameOf(makeMinidump(64)));
    now += INCIDENT.dumpPendingMs + 1000;
    service.incidents.trimDumps(now);
    assert.equal(service.incidents.dump(f2.ticket), null);
    assert.equal(service.incidents.dumpStored(f2.ticket), 0);
  } finally {
    store.close();
  }
});

test('all 8 statuses, player_message, fixed_version, awaiting_player_test and revision feedback', () => {
  let now = 1_700_000_000_000;
  const { service, store } = localService({ now: () => now });
  try {
    const { incident } = readIncident(sampleIncident());
    const filed = service.file(incident, null, 'acct-1', '198.51.100.1');
    const claim = { ticket: filed.ticket, receipt: filed.receipt };

    for (const status of INCIDENT_STATUSES) {
      now += 1000;
      const { change } = readStatusChange({
        status,
        fixed_version: status === 'fixed' ? '0.1.1' : '0.9.9',
        player_message: `Status is ${status}`,
      });
      const saved = service.setIncidentStatus(filed.ticket, change);
      assert.equal(saved.status, status);
      assert.equal(saved.fixed_version, status === 'fixed' ? '0.1.1' : '');
      assert.equal(saved.player_message, `Status is ${status}`);
    }

    // Move to awaiting_player_test -> player_feedback is pending.
    now += 1000;
    const req1 = service.setIncidentStatus(filed.ticket, {
      status: 'awaiting_player_test',
      fixed_version: '',
      player_message: 'Please test build 0.1.1.',
    });
    assert.equal(req1.player_feedback, 'pending');
    const rev1 = req1.status_updated_at;

    // Wrong revision refused.
    const wrongRev = readFeedback({
      ticket: filed.ticket, receipt: filed.receipt, revision: rev1 - 1,
      result: 'fixed', launcher_version: '0.1.1', game_build: 'English (Retail 116)',
    });
    assert.equal(service.feedback(wrongRev.feedback).code, 'stale_request');

    // First answer succeeds and is final for rev1.
    now += 500;
    const okAnswer = readFeedback({
      ticket: filed.ticket, receipt: filed.receipt, revision: rev1,
      result: 'not_fixed', launcher_version: '0.1.1', game_build: 'English (Retail 116)',
    });
    const ans1 = service.feedback(okAnswer.feedback);
    assert.equal(ans1.code, 'ok');
    assert.equal(ans1.test_feedback, 'not_fixed');

    // Same answer again is idempotent; different answer is refused.
    assert.equal(service.feedback(okAnswer.feedback).code, 'ok');
    const flipAnswer = readFeedback({
      ticket: filed.ticket, receipt: filed.receipt, revision: rev1,
      result: 'fixed', launcher_version: '0.1.1', game_build: 'English (Retail 116)',
    });
    assert.equal(service.feedback(flipAnswer.feedback).code, 'already_answered');

    const adminDetail = service.incidentForAdmin(filed.ticket);
    assert.equal(adminDetail.player_feedback, 'not_fixed');
    assert.equal(adminDetail.player_feedback_launcher_version, '0.1.1');

    // New awaiting_player_test request gets a newer revision and resets feedback to pending.
    const req2 = service.setIncidentStatus(filed.ticket, {
      status: 'awaiting_player_test',
      fixed_version: '',
      player_message: 'Please test build 0.1.2.',
    });
    assert.ok(req2.status_updated_at > rev1);
    assert.equal(req2.player_feedback, 'pending');

    // Old revision cannot answer new request.
    assert.equal(service.feedback(okAnswer.feedback).code, 'stale_request');

    // New revision accepts 'fixed'.
    const ans2 = service.feedback(readFeedback({
      ticket: filed.ticket, receipt: filed.receipt, revision: req2.status_updated_at,
      result: 'fixed', launcher_version: '0.1.2', game_build: 'English (Retail 116)',
    }).feedback);
    assert.equal(ans2.code, 'ok');
    assert.equal(ans2.test_feedback, 'fixed');

    const lookedUp = service.statuses([claim]).reports;
    assert.equal(lookedUp.length, 1);
    assert.equal(lookedUp[0].test_feedback, 'fixed');
  } finally {
    store.close();
  }
});

test('rate limits enforce hourly and daily per-account quotas', () => {
  let now = 1_700_000_000_000;
  const { service, store } = localService({ now: () => now });
  try {
    const { incident } = readIncident(sampleIncident());
    assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'ok');
    assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'ok');
    assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'incident_hourly_limit');

    // After 1 hour, can send again until daily limit (10) is hit.
    for (let i = 0; i < 4; i++) {
      now += 3600 * 1000 + 1000;
      assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'ok');
      assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'ok');
    }
    now += 3600 * 1000 + 1000;
    assert.equal(service.file(incident, null, 'acct-1', '198.51.100.1').code, 'incident_daily_limit');
  } finally {
    store.close();
  }
});
