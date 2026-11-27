# Jurassic Park: Trespasser Recompiled: bug report service

A Cloudflare Worker with one SQLite-backed Durable Object. Players' launchers file reports here; only the developer reads
them, in the private dashboard at `/admin`. It is independent of every other project's service: its own Worker, its own
Durable Object and database, its own secret, no player identities.

Nothing in the repository's tooling deploys it. The steps below are run by hand.

## What it is

| Path | Method | Who | What |
|---|---|---|---|
| `/health` | GET | anyone | name, protocol and status; no counts |
| `/v1/incidents` | POST | launcher | file a report; answers its ticket and receipt |
| `/v1/incidents/dump/start`, `/dump/chunk`, `/dump/end` | POST | launcher | the crash dump of a report, in chunks of 64 KiB, at most 8 MiB |
| `/v1/incidents/status` | POST | launcher | `{ticket, receipt}` claims; answers the status of those that match |
| `/v1/incidents/read` | POST | launcher | `{ticket, receipt}`; answers the stored report to its owner |
| `/v1/incidents/feedback` | POST | launcher | the player's answer to "Awaiting player test" (`fixed` or `not_fixed`) |
| `/admin` | GET | you | the dashboard page (static; holds no data) |
| `/admin/api/incidents[/<ticket>[/log\|/dump\|/status]]` | GET, POST, DELETE | you | list, read, download the log or dump, set the status, delete; Bearer token |

Requests are JSON over HTTPS (`Content-Type: application/json`). A request that carries an `Origin` header (a web page)
is refused, there are no CORS headers, a report body is cut at 160 KiB while read, and fields are validated before the
Durable Object is touched. Answers: `400 bad_request`, `403 origin_not_allowed`, `404 not_found`, `413 too_large`,
`415 unsupported_media_type`, `429 incident_hourly_limit | incident_daily_limit | limit` (with `Retry-After`).

Limits (`INCIDENT` in `src/incidents.js`): 2 reports per hour and 10 per day per launcher (its random `client_id`, else
its address), 30 a day per connection address (IPv6 by /64), 300 a day in all, and in-memory bursts (`PROTECTION` in
`src/limits.js`). There is no edge rate-limit binding. The newest 500 reports, the newest 100 crash dumps and the
statuses of the newest 5000 tickets are kept; a dump that is not complete after a day is dropped. A report is the fields
in `src/incidents.js` (`READERS`), nothing else.

Statuses: Open (`new`), Investigating, Awaiting player test, Fixed (with a version), Cannot reproduce, Duplicate, Not a bug,
Closed, and an optional message to the player of up to 500 characters.

The service keeps six tables: `incidents`, `incident_tracking` (ticket, a salted hash of the receipt, the status, the
player's test result), `incident_quota` (salted hashes of the launcher and the address and a time, deleted after a day),
`incident_dumps` and `incident_dump_chunks` (crash dumps) and `service_meta` (the salt). No raw address, no receipt, no
token and no identity is stored or logged.

## Tests (headless, no network, nothing deployed)

```powershell
cd server\trespasser-bug-reports
node --test "test/*.test.js"          # Node 22 or newer; an in-memory database
```

## Deploy (manual; nothing in this repository deploys)

Production address: `https://trespasser-bug-reports.saulobenigno.workers.dev`. The launcher points at it by default (override with `JPT_BUG_REPORT_URL`; `kBugReportServiceUrl` in
`launcher/game_profile.h`). SQLite-backed Durable Objects are offered on Cloudflare's free and paid Workers plans; check
Cloudflare's current limits for yours.

Prerequisites: Node.js 22.12 or newer (with npm), then the Worker's dependencies once, from the repository root:

```powershell
npm ci --prefix server\trespasser-bug-reports   # installs the Wrangler version pinned in package-lock.json
```

`tools\worker.ps1` deploys with that local Wrangler (`-DryRun` bundles and uploads nothing). It runs from any folder,
never uses a global Wrangler or `npx`, downloads nothing, and returns Wrangler's exit code. Inside this folder, after
`npm ci`, `npx wrangler` also picks the local copy.

```powershell
cd server\trespasser-bug-reports
npx wrangler whoami                       # must be the account that owns saulobenigno.workers.dev

# a random 64-character token; copy it into your password manager
$bytes = New-Object byte[] 32; [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
($bytes | ForEach-Object { $_.ToString('x2') }) -join ''

npx wrangler secret put JPT_BUG_REPORT_ADMIN_TOKEN    # paste the token when asked
npx wrangler deploy
curl.exe https://trespasser-bug-reports.saulobenigno.workers.dev/health
start https://trespasser-bug-reports.saulobenigno.workers.dev/admin
```

If `secret put` says the Worker does not exist yet, run `npx wrangler deploy` first and then `secret put`; until the secret
exists `/admin` answers `503 not_configured`. The token needs 24 or more characters.

`wrangler.toml` creates the Durable Object class `IncidentDeskObject` (binding `INCIDENTS`, migration `v1`, SQLite).
Do not edit an applied migration. The admin token is a Cloudflare secret you set yourself (`npx wrangler secret put JPT_BUG_REPORT_ADMIN_TOKEN`);
nothing in the repository holds one.

Validate with a real submission: see the last section of `docs/BUG_REPORTS.md`.

### Troubleshooting `tools\worker.ps1`

| Message | Fix |
|---|---|
| `Node.js not found` | install Node.js 22.12 or newer and open a new terminal |
| `dependencies are not installed` | run the `npm ci --prefix ...` command it prints |
| `npm was not found` | reinstall Node.js with npm, then `npm ci` |
| `local Wrangler is X but package-lock.json pins Y` | run `npm ci` again; it replaces `node_modules` with the locked versions |
| `Node.js ... is too old` | update Node.js to the version it names |
| `wrangler exited with code N` | Wrangler's own error, printed above it (login, configuration, network) |

## Operate

- `npx wrangler tail trespasser-bug-reports` shows the Worker's log. It records refusals and errors, never report text,
  receipts, tokens or addresses.
- To rotate the admin token, run `npx wrangler secret put JPT_BUG_REPORT_ADMIN_TOKEN` again and deploy nothing else.
- Wrong tokens are limited per address (10, then one every 30 seconds, per isolate).
- To delete every report, deploy a migration that lists `IncidentDeskObject` under `deleted_classes` (and a new class name
  under `new_sqlite_classes`). This is irreversible.
- To remove the service, `npx wrangler delete`. Launchers then show "Cannot reach the report service" and keep working.

## Layout

| File | What |
|---|---|
| `src/worker.js` | the Worker, and the Durable Object class that holds the service |
| `src/api.js` | the public HTTP routes: origin, media type, size, JSON, validation |
| `src/service.js` | the service: random values, limits, the transaction that files a report |
| `src/incidents.js` | the request fields, tickets, receipts, statuses, crash dumps, the SQL tables, retention |
| `src/limits.js` | the in-memory rate limiter and address grouping |
| `src/admin.js`, `src/admin_page.js` | the dashboard's API and its static page |
| `src/sha256.js` | a synchronous SHA-256 |
| `local/host.js` | the Node host (node:sqlite) the tests use |
| `test/` | the tests; `test/fixtures/request.json` is the canonical request |
