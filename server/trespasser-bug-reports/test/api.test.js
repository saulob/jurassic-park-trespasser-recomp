import assert from 'node:assert/strict';
import test from 'node:test';

import { localService } from '../local/host.js';
import { handleAdmin } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { DUMP_FRAME } from '../src/incidents.js';

const TOKEN = 'test-admin-token-1234567890';

function deskAdapter(service) {
  return {
    file: (incident, dump, account, address) => service.file(incident, dump, account, address),
    startDump: (claim) => service.startDump(claim),
    appendDumpChunk: (claim, offset, bytes) => service.appendDumpChunk(claim, offset, bytes),
    endDump: (claim) => service.endDump(claim),
    statuses: (claims) => service.statuses(claims),
    read: (claim) => service.read(claim),
    feedback: (fb) => service.feedback(fb),
    incidents: () => service.incidentsList(),
    incident: (ticket) => service.incidentForAdmin(ticket),
    incidentDump: (ticket) => service.incidentDump(ticket),
    setIncidentStatus: (ticket, change) => service.setIncidentStatus(ticket, change),
    deleteIncident: (ticket) => service.deleteIncident(ticket),
  };
}

function jsonPost(path, body, ip = '203.0.113.10') {
  return new Request(`https://reports.example${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': ip },
    body: JSON.stringify(body),
  });
}

test('public API full lifecycle: file, chunked DMP upload, status, read and feedback', async () => {
  const { service, store } = localService();
  const desk = deskAdapter(service);
  try {
    const health = await handlePublic(new Request('https://reports.example/health'), {}, desk);
    assert.equal(health.status, 200);
    assert.equal((await health.json()).service, 'trespasser-bug-reports');

    const dump = new Uint8Array(100);
    dump.set([0x4d, 0x44, 0x4d, 0x50]);

    const filedRes = await handlePublic(jsonPost('/v1/incidents', {
      description: 'Crash when picking up crate in Lab.',
      launcher_version: '0.1.0',
      game_build: 'English (Retail 116)',
      game_version: '116',
      operating_system: 'Windows 11 (build 26200)',
      log_file: 'trespasser_recomp_042.log',
      log_clipped: 'no',
      log_text: '[0.100] [fault] crash\n',
      dump_file: 'trespasser_recomp_042.dmp',
      dump_bytes: dump.length,
      client_id: 'a'.repeat(64),
    }), {}, desk);
    assert.equal(filedRes.status, 201);
    const filed = await filedRes.json();
    assert.match(filed.ticket, /^JPT-[A-HJ-NP-Z2-9]{6}$/);
    assert.equal(filed.dump, 'expected');

    const startRes = await handlePublic(jsonPost('/v1/incidents/dump/start', { ticket: filed.ticket, receipt: filed.receipt }), {}, desk);
    assert.equal(startRes.status, 200);
    assert.equal((await startRes.json()).stored, 0);

    const frame = new Uint8Array(DUMP_FRAME.headerBytes + dump.length);
    frame.set([0x44, 0x49, 1, 1]);
    frame.set(dump, 4);
    const chunkRes = await handlePublic(new Request('https://reports.example/v1/incidents/dump/chunk', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-JPT-Ticket': filed.ticket,
        'X-JPT-Receipt': filed.receipt,
        'X-JPT-Dump-Offset': '0',
      },
      body: frame,
    }), {}, desk);
    assert.equal(chunkRes.status, 200);
    assert.equal((await chunkRes.json()).stored, dump.length);

    const endRes = await handlePublic(jsonPost('/v1/incidents/dump/end', { ticket: filed.ticket, receipt: filed.receipt }), {}, desk);
    assert.equal(endRes.status, 200);
    assert.equal((await endRes.json()).complete, true);

    const readRes = await handlePublic(jsonPost('/v1/incidents/read', { ticket: filed.ticket, receipt: filed.receipt }), {}, desk);
    assert.equal(readRes.status, 200);
    assert.equal((await readRes.json()).report.ticket, filed.ticket);

    // Admin requests player test, player replies 'fixed'.
    const saved = service.setIncidentStatus(filed.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: 'Try again.' });
    const statusRes = await handlePublic(jsonPost('/v1/incidents/status', { reports: [{ ticket: filed.ticket, receipt: filed.receipt }] }), {}, desk);
    assert.equal(statusRes.status, 200);
    const statuses = (await statusRes.json()).reports;
    assert.equal(statuses[0].status, 'awaiting_player_test');
    assert.equal(statuses[0].test_feedback, 'pending');

    const fbRes = await handlePublic(jsonPost('/v1/incidents/feedback', {
      ticket: filed.ticket,
      receipt: filed.receipt,
      revision: saved.status_updated_at,
      result: 'fixed',
      launcher_version: '0.1.1',
      game_build: 'English (Retail 116)',
    }), {}, desk);
    assert.equal(fbRes.status, 200);
    assert.equal((await fbRes.json()).test_feedback, 'fixed');
  } finally {
    store.close();
  }
});

test('admin API auth, list, detail, status update, log download, DMP download and delete', async () => {
  const { service, store } = localService();
  const desk = deskAdapter(service);
  const env = { JPT_BUG_REPORT_ADMIN_TOKEN: TOKEN };
  const auth = { Authorization: `Bearer ${TOKEN}` };
  try {
    // Static assets served without auth.
    const html = await handleAdmin(new Request('https://reports.example/admin'), env, desk);
    assert.equal(html.status, 200);
    const htmlBody = await html.text();
    assert.ok(htmlBody.includes('Jurassic Park: Trespasser'));
    assert.ok(htmlBody.includes('Download minidump'));

    // Unconfigured / unauthorized rejected.
    assert.equal((await handleAdmin(new Request('https://reports.example/admin/api/incidents'), {}, desk)).status, 503);
    assert.equal((await handleAdmin(new Request('https://reports.example/admin/api/incidents'), env, desk)).status, 401);
    assert.equal((await handleAdmin(new Request('https://reports.example/admin/api/incidents', { headers: { Authorization: 'Bearer wrong-token-0000000' } }), env, desk)).status, 401);

    // File report with dump.
    const dump = new Uint8Array(64);
    dump.set([0x4d, 0x44, 0x4d, 0x50]);
    const filed = service.file({
      description: 'Terrain flicker in Industrial Jungle.',
      launcher_version: '0.1.0',
      game_build: 'English (Retail 116)',
      game_version: '116',
      operating_system: 'Windows 11 (build 26200)',
      log_file: 'trespasser_recomp_105.log',
      log_clipped: 'no',
      log_text: '[0.100] [loader] ok\n',
    }, { file: 'trespasser_recomp_105.dmp', bytes: dump.length }, 'acct-admin-test', '203.0.113.20');
    const frame = new Uint8Array(4 + dump.length);
    frame.set([0x44, 0x49, 1, 1]);
    frame.set(dump, 4);
    service.appendDumpChunk({ ticket: filed.ticket, receipt: filed.receipt }, 0, frame);
    service.endDump({ ticket: filed.ticket, receipt: filed.receipt });

    // List.
    const listRes = await handleAdmin(new Request('https://reports.example/admin/api/incidents', { headers: auth }), env, desk);
    assert.equal(listRes.status, 200);
    const list = await listRes.json();
    assert.equal(list.incidents.length, 1);
    assert.equal(list.incidents[0].dump_status, 'stored');

    // Detail (receipt never exposed).
    const detailRes = await handleAdmin(new Request(`https://reports.example/admin/api/incidents/${filed.ticket}`, { headers: auth }), env, desk);
    assert.equal(detailRes.status, 200);
    const detail = await detailRes.json();
    assert.equal(detail.ticket, filed.ticket);
    assert.equal(detail.receipt, undefined);
    assert.equal(detail.receipt_hash, undefined);
    assert.equal(detail.dump_status, 'stored');

    // Save status.
    const saveRes = await handleAdmin(new Request(`https://reports.example/admin/api/incidents/${filed.ticket}/status`, {
      method: 'POST',
      headers: { ...auth, 'Content-Type': 'application/json' },
      body: JSON.stringify({ status: 'fixed', fixed_version: '0.1.1', player_message: 'Fixed terrain LOD.' }),
    }), env, desk);
    assert.equal(saveRes.status, 200);
    assert.equal((await saveRes.json()).fixed_version, '0.1.1');

    // Download log & DMP with safe server filenames.
    const logRes = await handleAdmin(new Request(`https://reports.example/admin/api/incidents/${filed.ticket}/log`, { headers: auth }), env, desk);
    assert.equal(logRes.status, 200);
    assert.equal(logRes.headers.get('Content-Disposition'), `attachment; filename="${filed.ticket}-trespasser_recomp_105.log"`);

    const dmpRes = await handleAdmin(new Request(`https://reports.example/admin/api/incidents/${filed.ticket}/dump`, { headers: auth }), env, desk);
    assert.equal(dmpRes.status, 200);
    assert.equal(dmpRes.headers.get('Content-Disposition'), `attachment; filename="${filed.ticket}-trespasser_recomp_105.dmp"`);
    assert.deepEqual(new Uint8Array(await dmpRes.arrayBuffer()), dump);

    // Delete removes incident, tracking, dump and chunks.
    const delRes = await handleAdmin(new Request(`https://reports.example/admin/api/incidents/${filed.ticket}`, { method: 'DELETE', headers: auth }), env, desk);
    assert.equal(delRes.status, 200);
    assert.equal(service.incidentForAdmin(filed.ticket), null);
    assert.equal(service.incidentDump(filed.ticket), null);
    assert.equal(service.statuses([{ ticket: filed.ticket, receipt: filed.receipt }]).reports.length, 0);
  } finally {
    store.close();
  }
});
