export const ADMIN_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta name="robots" content="noindex, nofollow">
<title>JPT Bug Reports Admin</title>
<link rel="stylesheet" href="/admin/app.css">
</head>
<body>
<header class="top">
  <div class="top-bar">
    <h1>Jurassic Park: Trespasser · Bug Reports Admin</h1>
    <div id="session" class="session" hidden>
      <button id="menu-button" type="button" class="icon-button" aria-label="Admin session menu" aria-haspopup="true" aria-expanded="false" aria-controls="session-menu"><span class="bars" aria-hidden="true"></span></button>
      <div id="session-menu" class="menu" role="menu" aria-label="Admin session" hidden>
        <p class="menu-title">Admin session</p>
        <p id="session-state" class="muted"></p>
        <button id="signout" type="button" role="menuitem" class="quiet">Sign out</button>
      </div>
    </div>
  </div>
  <p class="muted">Bug reports sent from the Jurassic Park: Trespasser launcher, timestamps in America/Sao_Paulo.</p>
</header>
<main>
  <form id="login" class="panel login" autocomplete="off">
    <label for="token">Admin token</label>
    <input id="token" type="password" autocomplete="off" spellcheck="false" required>
    <label class="check"><input id="remember" type="checkbox"> Keep for this tab</label>
    <button type="submit">Open admin</button>
    <p id="login-error" class="error" role="alert"></p>
  </form>
  <section id="dash" hidden>
    <nav class="tabs" role="tablist" aria-label="Admin sections">
      <button id="tab-bugs" type="button" role="tab" aria-selected="true" aria-controls="view-bugs">Bug Reports <span id="bug-new-count" class="tab-count" title="Reports with status New">–</span></button>
    </nav>
    <div id="view-bugs" role="tabpanel" aria-labelledby="tab-bugs">
      <div class="panel">
        <p class="muted">Reports players sent from the launcher, newest first. The log of a report is stored as the player previewed it and is shown and downloaded exactly as stored. No account or address is kept with a report, and the newest 500 are kept.</p>
        <div class="toolbar">
          <label for="bug-version">Launcher version</label>
          <select id="bug-version"></select>
          <label for="bug-build">Game build</label>
          <select id="bug-build"></select>
          <label for="bug-day">Date</label>
          <input id="bug-day" type="date">
          <label for="bug-status-filter">Status</label>
          <select id="bug-status-filter">
            <option value="">All statuses</option>
            <option value="new">New</option>
            <option value="investigating">Investigating</option>
            <option value="awaiting_player_test">Awaiting player test</option>
            <option value="fixed">Fixed</option>
            <option value="cannot_reproduce">Cannot reproduce</option>
            <option value="duplicate">Duplicate</option>
            <option value="not_a_bug">Not a bug</option>
            <option value="closed">Closed</option>
          </select>
          <label for="bug-feedback-filter">Player feedback</label>
          <select id="bug-feedback-filter">
            <option value="">All</option>
            <option value="pending">Awaiting response</option>
            <option value="fixed">Confirmed fixed</option>
            <option value="not_fixed">Still happening</option>
          </select>
          <button id="bug-refresh" type="button">Refresh</button>
          <span id="bug-count" class="muted" role="status"></span>
        </div>
        <div class="scroll bug-list"><table id="bug-list"></table></div>
      </div>
      <div id="bug-detail" class="panel bug-detail" hidden>
        <div class="toolbar bug-header">
          <h2 id="bug-title"></h2>
          <button id="bug-download" type="button">Download log</button>
          <button id="bug-download-dump" type="button" hidden>Download minidump</button>
          <button id="bug-copy" type="button" class="quiet">Copy log</button>
          <span id="bug-note" class="muted" role="status"></span>
          <button id="bug-close" type="button" class="quiet bug-close">Close report</button>
        </div>
        <p id="bug-text" class="bug-text"></p>
        <dl id="bug-facts" class="bug-facts"></dl>
        <h3>Report status</h3>
        <div class="bug-status-form">
          <label for="bug-status">Status</label>
          <select id="bug-status">
            <option value="new">New</option>
            <option value="investigating">Investigating</option>
            <option value="awaiting_player_test">Awaiting player test</option>
            <option value="fixed">Fixed</option>
            <option value="cannot_reproduce">Cannot reproduce</option>
            <option value="duplicate">Duplicate</option>
            <option value="not_a_bug">Not a bug</option>
            <option value="closed">Closed</option>
          </select>
          <p id="bug-status-meaning" class="muted"></p>
          <div id="bug-feedback" class="bug-feedback" hidden>
            <h3>Player feedback</h3>
            <p id="bug-feedback-state" class="bug-feedback-state"></p>
            <dl id="bug-feedback-facts" class="bug-facts"></dl>
          </div>
          <div id="bug-fixed-field" class="bug-status-field" hidden>
            <label for="bug-fixed">Fixed in version</label>
            <input id="bug-fixed" type="text" maxlength="32" autocomplete="off" placeholder="0.1.1">
          </div>
          <div class="bug-status-field">
            <label for="bug-message">Message to player</label>
            <textarea id="bug-message" rows="3" maxlength="500"></textarea>
          </div>
          <div class="toolbar">
            <button id="bug-status-save" type="button">Save status</button>
            <span id="bug-status-note" class="muted" role="status"></span>
          </div>
        </div>
        <h3 id="bug-log-name"></h3>
        <p class="muted bug-snapshot">This is the sanitized log snapshot captured when the report was sent. Previous report IDs may appear if they were already present in the selected log.</p>
        <pre id="bug-log" class="bug-log" tabindex="0"></pre>
        <div class="bug-danger">
          <h3>Delete report</h3>
          <p class="muted">Permanently removes this report, its log, its crash dump and the status its player sees. Meant for test and unwanted reports.</p>
          <button id="bug-delete" type="button" class="danger">Delete report</button>
        </div>
      </div>
    </div>
  </section>
</main>
<div id="delete-backdrop" class="backdrop" hidden>
  <div id="delete-dialog" class="dialog" role="alertdialog" aria-modal="true" aria-labelledby="delete-title" aria-describedby="delete-text">
    <h2 id="delete-title"></h2>
    <p id="delete-text">This removes the stored report, log and crash dump and cannot be undone.</p>
    <p id="delete-error" class="error" role="alert"></p>
    <div class="dialog-actions">
      <button id="delete-cancel" type="button" class="quiet">Cancel</button>
      <button id="delete-confirm" type="button" class="danger">Delete report</button>
    </div>
  </div>
</div>
<script src="/admin/app.js"></script>
</body>
</html>
`;

export const ADMIN_CSS = String.raw`:root {
  color-scheme: light;
  --surface-0: #f3f3f1;
  --surface-1: #fcfcfb;
  --border: #dcdbd6;
  --grid: #e7e6e2;
  --text-primary: #0b0b0b;
  --text-secondary: #52514e;
  --text-muted: #6d6c67;
  --accent: #b8731e;
  --series-3: #1baf7a;
  --error: #b42318;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) {
    color-scheme: dark;
    --surface-0: #111110;
    --surface-1: #1a1a19;
    --border: #34332f;
    --grid: #2a2a27;
    --text-primary: #f0efec;
    --text-secondary: #c3c2b7;
    --text-muted: #9a998f;
    --accent: #d99b38;
    --series-3: #199e70;
    --error: #f97066;
  }
}
* { box-sizing: border-box; }
[hidden] { display: none !important; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  background: var(--surface-0);
  color: var(--text-primary);
  font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
.top, main { max-width: 1120px; margin: 0 auto; padding: 0 16px; }
.top { padding-top: 24px; }
.top-bar { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; }
h1 { font-size: 22px; margin: 0 0 4px; min-width: 0; }
.session { position: relative; flex: none; }
.icon-button { padding: 8px 10px; background: transparent; color: var(--text-secondary); border-color: var(--border); line-height: 0; }
.icon-button[aria-expanded="true"] { color: var(--text-primary); border-color: var(--accent); }
.bars, .bars::before, .bars::after { display: block; width: 18px; height: 2px; border-radius: 1px; background: currentColor; }
.bars { position: relative; }
.bars::before, .bars::after { content: ""; position: absolute; left: 0; }
.bars::before { top: -6px; }
.bars::after { top: 6px; }
.menu { position: absolute; right: 0; top: calc(100% + 6px); z-index: 20; width: 260px; padding: 12px; background: var(--surface-1); border: 1px solid var(--border); border-radius: 10px; box-shadow: 0 6px 20px rgba(0, 0, 0, .18); }
.menu-title { margin: 0 0 4px; font-weight: 600; }
.menu .muted { margin: 0 0 12px; }
.menu button { width: 100%; }
h2 { font-size: 15px; margin: 0 0 8px; }
.muted { color: var(--text-muted); margin: 0 0 12px; font-size: 13px; }
.panel {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px;
  margin: 0 0 16px;
  min-width: 0;
}
.login { max-width: 420px; display: grid; gap: 10px; }
.check { display: flex; gap: 8px; align-items: center; color: var(--text-secondary); }
input[type="password"], input[type="date"], input[type="text"], textarea, select {
  font: inherit;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--surface-0);
  color: var(--text-primary);
  width: 100%;
}
select, input[type="date"] { width: auto; }
button {
  font: inherit;
  padding: 8px 14px;
  border-radius: 6px;
  border: 1px solid var(--accent);
  background: var(--accent);
  color: #fff;
  cursor: pointer;
}
button.quiet { background: transparent; color: var(--text-secondary); border-color: var(--border); }
.error { color: var(--error); margin: 0; min-height: 1.2em; }
.toolbar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin: 0 0 16px; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { padding: 6px 8px; text-align: left; border-bottom: 1px solid var(--grid); white-space: nowrap; vertical-align: top; }
th { color: var(--text-secondary); font-weight: 600; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin: 0 0 16px; border-bottom: 1px solid var(--border); }
.tabs button[role="tab"] { background: transparent; color: var(--text-secondary); border: 0; border-bottom: 3px solid transparent; border-radius: 0; padding: 8px 14px; }
.tabs button[role="tab"][aria-selected="true"] { color: var(--text-primary); border-bottom-color: var(--accent); font-weight: 600; }
.bug-feedback { margin: 0 0 24px; }
.bug-feedback-state { margin: 0 0 8px; font-weight: 600; }
.bug-feedback .bug-facts { margin: 0; }
#bug-list td.col-feedback { white-space: normal; min-width: 9em; }
.tab-count { display: inline-block; min-width: 2.2em; margin-left: 4px; padding: 0 6px; border-radius: 999px; border: 1px solid var(--border); font-size: 12px; line-height: 1.5; text-align: center; font-variant-numeric: tabular-nums; color: var(--text-muted); }
.tab-count.active { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
.feedback-badge { display: inline-block; padding: 0 8px; border-radius: 999px; border: 1px solid var(--border); font-size: 12px; white-space: nowrap; color: var(--text-primary); }
.feedback-badge::before { content: ""; display: inline-block; width: 8px; height: 8px; margin-right: 6px; border-radius: 50%; background: var(--text-muted); }
.feedback-badge.fixed { border-color: var(--series-3); font-weight: 600; }
.feedback-badge.fixed::before { background: var(--series-3); }
.feedback-badge.not_fixed { border-color: var(--error); font-weight: 600; }
.feedback-badge.not_fixed::before { background: var(--error); }
.bug-detail h2 { margin: 0; }
.bug-list { max-height: 420px; overflow-y: auto; }
#bug-list td.col-build, #bug-list td.col-os { white-space: normal; overflow-wrap: anywhere; }
#bug-list td.col-build { max-width: 12em; }
#bug-list td.col-os { max-width: 10em; }
#bug-list td.bug-summary { white-space: normal; overflow-wrap: anywhere; width: 100%; min-width: 14em; }
@media (max-width: 1000px) {
  #bug-list .col-os { display: none; }
}
@media (max-width: 760px) {
  #bug-list .col-launcher { display: none; }
  #bug-list td.bug-summary { min-width: 10em; }
}
#bug-list tr.selected td { background: var(--grid); }
button.ticket { padding: 0; background: transparent; border: 0; color: var(--accent); text-decoration: underline; font-variant-numeric: tabular-nums; }
.bug-detail { margin-top: 16px; }
.bug-header { margin-bottom: 20px; }
.bug-close { margin-left: auto; }
.bug-detail h3 { font-size: 14px; margin: 0 0 4px; }
.bug-text { white-space: pre-wrap; overflow-wrap: anywhere; margin: 0 0 24px; }
.bug-facts { display: grid; grid-template-columns: max-content 1fr; gap: 4px 12px; margin: 0 0 24px; font-size: 13px; }
.bug-snapshot { margin-bottom: 8px; }
.bug-status-form { margin: 0 0 24px; }
.bug-status-form > .muted { margin: 4px 0 12px; }
.bug-status-form .toolbar { margin: 12px 0 0; }
.bug-status-field { display: flex; flex-direction: column; gap: 4px; margin: 0 0 12px; }
.bug-status-field[hidden] { display: none; }
.bug-status-field input { max-width: 200px; }
.bug-status-field textarea { max-width: 640px; font: inherit; }
.bug-facts dt { color: var(--text-secondary); }
.bug-facts dd { margin: 0; overflow-wrap: anywhere; }
.bug-danger { margin: 28px 0 0; padding: 16px 0 0; border-top: 1px solid var(--border); }
.bug-danger .muted { margin: 0 0 10px; }
button.danger { background: var(--error); border-color: var(--error); color: #fff; }
.backdrop { position: fixed; inset: 0; z-index: 30; display: flex; align-items: center; justify-content: center; padding: 16px; background: rgba(0, 0, 0, .5); }
.dialog { width: 100%; max-width: 420px; padding: 20px; background: var(--surface-1); border: 1px solid var(--border); border-radius: 10px; }
.dialog h2 { font-size: 17px; margin: 0 0 8px; overflow-wrap: anywhere; }
.dialog p { margin: 0 0 12px; color: var(--text-secondary); }
.dialog p.error { margin: 0 0 12px; }
.dialog-actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 10px; }
.bug-log { max-height: 480px; overflow: auto; margin: 0; padding: 10px; background: var(--surface-0); border: 1px solid var(--border); border-radius: 6px; font: 12px/1.45 ui-monospace, Consolas, monospace; white-space: pre; }
`;

export const ADMIN_JS = String.raw`(function () {
  'use strict';
  var KEY = 'jpt-bug-report-admin-token';
  var ZONE = 'America/Sao_Paulo';
  var token = '';
  var kept = false;
  var menuOpen = false;
  var deleteTicket = '';
  var deleting = false;
  var bugReports = [];
  var bugTicket = '';
  var bugLoadingTicket = '';
  var bugLogText = '';
  var $ = function (id) { return document.getElementById(id); };
  var stampLabel = new Intl.DateTimeFormat('en-GB', { timeZone: ZONE, day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  var bugDayLabel = new Intl.DateTimeFormat('en-CA', { timeZone: ZONE, year: 'numeric', month: '2-digit', day: '2-digit' });

  var BUG_STATUSES = {
    new: ['New', 'The report has not been reviewed yet.'],
    investigating: ['Investigating', 'The report is being investigated.'],
    awaiting_player_test: ['Awaiting player test', 'The player is asked in the launcher whether this fixed the issue. The message below is what they see, and saving a change asks again.'],
    fixed: ['Fixed', 'The problem has been corrected.'],
    cannot_reproduce: ['Cannot reproduce', 'The problem could not be reproduced with the available information.'],
    duplicate: ['Duplicate', 'The same problem is already tracked by another report.'],
    not_a_bug: ['Not a bug', 'The reported behavior is expected or is not a Jurassic Park: Trespasser bug.'],
    closed: ['Closed', 'The report was intentionally finished without a specific fix or another more precise status. It does not mean Fixed.']
  };
  var FEEDBACK_LABELS = { pending: 'Awaiting response', fixed: 'Player confirmed fixed', not_fixed: 'Still happening' };
  var BUG_COLUMNS = ['Report ID', 'Status', 'Player feedback', 'Submitted', 'Launcher', 'Game build', 'Operating system', 'Description'];
  var BUG_COLUMN_CLASSES = ['col-id', 'col-status', 'col-feedback', 'col-submitted', 'col-launcher', 'col-build', 'col-os', ''];

  function readStored(key) {
    try { return sessionStorage.getItem(key) || ''; } catch (e) { return ''; }
  }
  function writeStored(key, value) {
    try { if (value) sessionStorage.setItem(key, value); else sessionStorage.removeItem(key); } catch (e) { /* storage blocked */ }
  }
  function storeToken(value) {
    writeStored(KEY, value);
    kept = Boolean(value) && readStored(KEY) === value;
  }

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }
  function fmt(n) { return Number(n || 0).toLocaleString('en-US'); }

  function AuthError() {}

  function request(path, body, method) {
    return fetch(path, { method: method || (body ? 'POST' : 'GET'), body: body, headers: { Authorization: 'Bearer ' + token }, cache: 'no-store', credentials: 'omit' })
      .then(function (r) {
        if (r.status === 401) throw new AuthError();
        if (!r.ok) {
          var failure = new Error('The service answered ' + r.status + '.');
          failure.status = r.status;
          throw failure;
        }
        return r;
      });
  }
  function api(path) {
    return request(path).then(function (r) { return r.json(); });
  }

  function table(node, head, body, rowClass, columns) {
    clear(node);
    var thead = el('thead');
    var tr = el('tr');
    head.forEach(function (h, i) { tr.appendChild(el('th', columns ? columns[i] : '', h)); });
    thead.appendChild(tr);
    node.appendChild(thead);
    var tbody = el('tbody');
    body.forEach(function (cells, i) {
      var row = el('tr', rowClass ? rowClass(i) : '');
      cells.forEach(function (c, j) {
        var td = typeof c === 'string' ? el('td', '', c) : c;
        if (columns && columns[j]) td.className = td.className ? td.className + ' ' + columns[j] : columns[j];
        row.appendChild(td);
      });
      tbody.appendChild(row);
    });
    node.appendChild(tbody);
  }

  function bugOptions(id, key) {
    var select = $(id);
    var chosen = select.value;
    var seen = {};
    bugReports.forEach(function (r) { if (r[key]) seen[r[key]] = true; });
    clear(select);
    var all = el('option', '', 'All');
    all.value = '';
    select.appendChild(all);
    Object.keys(seen).sort().forEach(function (value) {
      var option = el('option', '', value);
      option.value = value;
      select.appendChild(option);
    });
    select.value = seen[chosen] ? chosen : '';
  }

  function bugShown() {
    var version = $('bug-version').value;
    var build = $('bug-build').value;
    var day = $('bug-day').value;
    var status = $('bug-status-filter').value;
    var feedback = $('bug-feedback-filter').value;
    return bugReports.filter(function (r) {
      return (!version || r.launcher_version === version) && (!build || r.game_build === build) &&
        (!day || bugDayLabel.format(r.ms) === day) && (!status || r.status === status) && (!feedback || r.player_feedback === feedback);
    });
  }

  function showNewCount() {
    var count = bugReports.filter(function (r) { return r.status === 'new'; }).length;
    var badge = $('bug-new-count');
    badge.textContent = fmt(count);
    badge.className = count > 0 ? 'tab-count active' : 'tab-count';
    badge.title = count + ' ' + (count === 1 ? 'report' : 'reports') + ' with status New';
  }

  function feedbackBadge(feedback) {
    return FEEDBACK_LABELS[feedback] ? el('span', 'feedback-badge ' + feedback, FEEDBACK_LABELS[feedback]) : null;
  }

  function bugFact(list, name, value) {
    list.appendChild(el('dt', '', name));
    list.appendChild(el('dd', '', value || '–'));
  }

  function bugFeedbackFacts(r) {
    var label = FEEDBACK_LABELS[r.player_feedback];
    var list = $('bug-feedback-facts');
    clear(list);
    $('bug-feedback').hidden = !label;
    $('bug-feedback-state').textContent = label || '';
    if (!label || r.player_feedback === 'pending') return;
    bugFact(list, 'Response time', stampLabel.format(r.player_feedback_at) + ' (Sao Paulo)');
    bugFact(list, 'Launcher version', r.player_feedback_launcher_version);
    bugFact(list, 'Game build', r.player_feedback_game_build);
  }

  function showBugStatusMeaning() {
    var status = $('bug-status').value;
    $('bug-status-meaning').textContent = BUG_STATUSES[status][1];
    $('bug-fixed-field').hidden = status !== 'fixed';
  }

  function bugStatusForm(r) {
    $('bug-status').value = r.status;
    $('bug-fixed').value = r.fixed_version || '';
    $('bug-message').value = r.player_message || '';
    $('bug-status-note').textContent = '';
    showBugStatusMeaning();
    bugFeedbackFacts(r);
  }

  function bugDumpLabel(r) {
    if (r.dump_status === 'stored') return 'Stored, ' + r.dump_file + ' (' + fmt(r.dump_bytes) + ' bytes)';
    if (r.dump_status === 'uploading') return 'Announced, upload not finished (' + r.dump_file + ')';
    return 'No crash report';
  }

  function bugDetail(r) {
    bugTicket = r.ticket;
    bugLoadingTicket = '';
    bugLogText = r.log_text;
    $('bug-title').textContent = r.ticket;
    $('bug-text').textContent = r.description;
    var facts = $('bug-facts');
    clear(facts);
    bugFact(facts, 'Report ID', r.ticket);
    bugFact(facts, 'Submitted', stampLabel.format(r.ms) + ' (Sao Paulo)');
    bugFact(facts, 'Launcher version', r.launcher_version);
    bugFact(facts, 'Game build', r.game_build);
    bugFact(facts, 'Game version', r.game_version);
    bugFact(facts, 'Operating system', r.operating_system);
    bugFact(facts, 'Original log file', r.log_file);
    bugFact(facts, 'Log clipped', r.log_clipped === 'yes' ? 'Yes, only its end was sent' : 'No');
    bugFact(facts, 'Crash report', bugDumpLabel(r));
    $('bug-download-dump').hidden = r.dump_status !== 'stored';
    $('bug-log-name').textContent = 'Sanitized log';
    $('bug-log').textContent = bugLogText;
    $('bug-note').textContent = '';
    bugStatusForm(r);
    $('bug-detail').hidden = false;
    renderBugList();
  }

  function openBugReport(ticket) {
    bugLoadingTicket = ticket;
    return api('/admin/api/incidents/' + encodeURIComponent(ticket)).then(function (report) {
      if (report.ticket === bugLoadingTicket) bugDetail(report);
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      if (ticket !== bugLoadingTicket) return;
      bugLoadingTicket = '';
      $('bug-count').textContent = 'Could not open ' + ticket + '.';
    });
  }

  function forgetBugReport() {
    bugTicket = '';
    bugLoadingTicket = '';
    bugLogText = '';
    $('bug-detail').hidden = true;
  }

  function closeBugReport() {
    forgetBugReport();
    renderBugList();
  }

  function toggleBugReport(ticket) {
    if (ticket === bugTicket) closeBugReport();
    else openBugReport(ticket);
  }

  function downloadBugFile(path, extension, what) {
    var ticket = bugTicket;
    if (!ticket) return Promise.resolve();
    return request('/admin/api/incidents/' + encodeURIComponent(ticket) + '/' + path).then(function (r) {
      var named = /filename="([^"]+)"/.exec(r.headers.get('Content-Disposition') || '');
      return r.blob().then(function (blob) {
        var url = URL.createObjectURL(blob);
        var link = el('a');
        link.href = url;
        link.download = named ? named[1] : ticket + extension;
        link.click();
        setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
      });
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('bug-note').textContent = 'Could not download ' + what + '.';
    });
  }

  function copyBugLog() {
    if (!bugLogText) return Promise.resolve();
    var say = function (text) { $('bug-note').textContent = text; };
    if (!navigator.clipboard || !navigator.clipboard.writeText) { say('Copying is not available in this browser.'); return Promise.resolve(); }
    return navigator.clipboard.writeText(bugLogText).then(function () { say('Log copied.'); }, function () { say('Could not copy the log.'); });
  }

  function saveBugStatus() {
    var ticket = bugTicket;
    if (!ticket) return Promise.resolve();
    var status = $('bug-status').value;
    var change = { status: status, fixed_version: status === 'fixed' ? $('bug-fixed').value.trim() : '', player_message: $('bug-message').value };
    return request('/admin/api/incidents/' + encodeURIComponent(ticket) + '/status', JSON.stringify(change)).then(function (r) {
      return r.json();
    }).then(function (saved) {
      bugReports.forEach(function (r) {
        if (r.ticket === ticket) { r.status = saved.status; r.status_updated_at = saved.status_updated_at; r.player_feedback = saved.player_feedback; }
      });
      renderBugList();
      if (ticket !== bugTicket) return;
      bugStatusForm(saved);
      $('bug-status-note').textContent = 'Saved.';
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      if (ticket === bugTicket) $('bug-status-note').textContent = 'Could not save the status. Check the version and the message.';
    });
  }

  function removeBugReport(ticket) {
    bugReports = bugReports.filter(function (r) { return r.ticket !== ticket; });
    if (bugTicket === ticket || bugLoadingTicket === ticket) forgetBugReport();
    bugOptions('bug-version', 'launcher_version');
    bugOptions('bug-build', 'game_build');
    renderBugList();
    $('bug-count').textContent = ticket + ' deleted. ' + $('bug-count').textContent;
  }

  function setDeleteBusy(busy) {
    deleting = busy;
    $('delete-cancel').disabled = busy;
    $('delete-confirm').disabled = busy;
  }

  function openDelete() {
    if (!bugTicket) return;
    deleteTicket = bugTicket;
    $('delete-title').textContent = 'Delete ' + deleteTicket + ' permanently?';
    $('delete-error').textContent = '';
    setDeleteBusy(false);
    $('delete-backdrop').hidden = false;
    $('delete-cancel').focus();
  }

  function closeDelete() {
    if (deleting) return;
    deleteTicket = '';
    $('delete-backdrop').hidden = true;
  }

  function confirmDelete() {
    var ticket = deleteTicket;
    if (!ticket || deleting) return Promise.resolve();
    setDeleteBusy(true);
    var finish = function () {
      setDeleteBusy(false);
      deleteTicket = '';
      $('delete-backdrop').hidden = true;
      removeBugReport(ticket);
    };
    return request('/admin/api/incidents/' + encodeURIComponent(ticket), undefined, 'DELETE').then(finish, function (error) {
      if (error instanceof AuthError) { setDeleteBusy(false); signOut('The token was not accepted.'); return; }
      if (error && error.status === 404) { finish(); return; }
      setDeleteBusy(false);
      $('delete-error').textContent = 'Could not delete the report. Try again.';
    });
  }

  function bugTicketCell(r) {
    var td = el('td');
    var button = el('button', 'ticket', r.ticket);
    button.type = 'button';
    button.addEventListener('click', function () { toggleBugReport(r.ticket); });
    td.appendChild(button);
    return td;
  }

  function bugFeedbackCell(r) {
    var badge = feedbackBadge(r.player_feedback);
    if (!badge) return el('td', 'muted', '–');
    var td = el('td');
    td.appendChild(badge);
    return td;
  }

  function renderBugList() {
    var shown = bugShown();
    showNewCount();
    table($('bug-list'), BUG_COLUMNS, shown.map(function (r) {
      return [bugTicketCell(r), BUG_STATUSES[r.status][0], bugFeedbackCell(r), stampLabel.format(r.ms), r.launcher_version, r.game_build || '–',
        r.operating_system, el('td', 'bug-summary', r.preview)];
    }), function (i) { return shown[i].ticket === bugTicket ? 'selected' : ''; }, BUG_COLUMN_CLASSES);
    $('bug-count').textContent = !bugReports.length ? 'No reports yet.'
      : shown.length === bugReports.length ? fmt(shown.length) + ' reports' : fmt(shown.length) + ' of ' + fmt(bugReports.length) + ' reports';
  }

  function loadBugReports() {
    $('bug-count').textContent = 'Loading…';
    return api('/admin/api/incidents').then(function (r) {
      bugReports = r.incidents;
      bugOptions('bug-version', 'launcher_version');
      bugOptions('bug-build', 'game_build');
      renderBugList();
    }).catch(function (error) {
      if (error instanceof AuthError) { signOut('The token was not accepted.'); return; }
      $('bug-count').textContent = error && error.message ? error.message : 'Could not load the reports.';
    });
  }

  function setMenu(open) {
    menuOpen = open;
    $('session-menu').hidden = !open;
    $('menu-button').setAttribute('aria-expanded', String(open));
  }

  function showSessionState() {
    $('session-state').textContent = kept
      ? 'Signed in. The token is kept for this tab only, until you sign out or close the tab.'
      : 'Signed in. The token is held in memory only; reloading the page asks for it again.';
  }

  function showDashboard() {
    $('login').hidden = true;
    $('dash').hidden = false;
    $('session').hidden = false;
    setMenu(false);
    showSessionState();
    loadBugReports();
  }

  function signOut(message) {
    token = '';
    storeToken('');
    bugReports = [];
    $('bug-new-count').textContent = '–';
    $('bug-new-count').className = 'tab-count';
    deleting = false;
    deleteTicket = '';
    $('delete-backdrop').hidden = true;
    forgetBugReport();
    setMenu(false);
    $('session').hidden = true;
    $('dash').hidden = true;
    $('login').hidden = false;
    $('remember').checked = false;
    $('token').value = '';
    $('login-error').textContent = message || '';
    $('token').focus();
  }

  $('login').addEventListener('submit', function (event) {
    event.preventDefault();
    token = $('token').value.trim();
    $('token').value = '';
    if (!token) return;
    storeToken($('remember').checked ? token : '');
    $('login-error').textContent = '';
    showDashboard();
  });
  $('bug-refresh').addEventListener('click', loadBugReports);
  $('bug-download').addEventListener('click', function () { downloadBugFile('log', '.log', 'the log'); });
  $('bug-download-dump').addEventListener('click', function () { downloadBugFile('dump', '.dmp', 'the minidump'); });
  $('bug-copy').addEventListener('click', copyBugLog);
  $('bug-close').addEventListener('click', closeBugReport);
  $('bug-status').addEventListener('change', showBugStatusMeaning);
  $('bug-status-save').addEventListener('click', saveBugStatus);
  ['bug-version', 'bug-build', 'bug-day', 'bug-status-filter', 'bug-feedback-filter'].forEach(function (id) { $(id).addEventListener('change', renderBugList); });
  $('signout').addEventListener('click', function () { signOut(''); });
  $('menu-button').addEventListener('click', function () {
    setMenu(!menuOpen);
    if (menuOpen) $('signout').focus();
  });
  $('bug-delete').addEventListener('click', openDelete);
  $('delete-cancel').addEventListener('click', closeDelete);
  $('delete-confirm').addEventListener('click', confirmDelete);
  $('delete-backdrop').addEventListener('click', function (event) { if (event.target === $('delete-backdrop')) closeDelete(); });
  document.addEventListener('click', function (event) {
    if (menuOpen && !$('session').contains(event.target)) setMenu(false);
  });
  document.addEventListener('keydown', function (event) {
    if (deleteTicket && event.key === 'Tab') {
      event.preventDefault();
      ($('delete-cancel') === document.activeElement ? $('delete-confirm') : $('delete-cancel')).focus();
    } else if (event.key === 'Escape' && deleteTicket) {
      closeDelete();
    } else if (event.key === 'Escape' && menuOpen) {
      setMenu(false);
      $('menu-button').focus();
    }
  });

  token = readStored(KEY);
  if (token) {
    kept = true;
    $('remember').checked = true;
    showDashboard();
  }
}());
`;
