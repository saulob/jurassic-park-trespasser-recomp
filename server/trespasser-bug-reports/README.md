# Jurassic Park: Trespasser Recompiled: bug report service

A Cloudflare Worker with one SQLite-backed Durable Object. Players' launchers file reports here; only the developer reads
them, in the private dashboard at `/admin`. It is independent of every other project's service: its own Worker, its own
Durable Object and database, its own secret, no player identities.

Nothing in the repository's tooling deploys it. The steps below are run by hand.

## What it is

| Path | Method | Who | What |
|---|---|---|---|
| `/health` | GET | anyone | name, protocol and build; no counts |
| `/v1/reports` | POST | launcher | file a report; answers `201 {ticket, receipt, submitted_at}` |
| `/v1/reports/status` | POST | launcher | up to 30 `{ticket, receipt}` claims; answers the status of those that match |
| `/v1/reports/read` | POST | launcher | `{ticket, receipt}`; answers the stored report to its owner |
| `/admin` | GET | you | the dashboard page (static; holds no data) |
| `/admin/api/reports[/<ticket>[/log\|/status]]` | GET, POST | you | list, read, download the log, set the status; Bearer token |

Requests are JSON over HTTPS (`Content-Type: application/json`). A request that carries an `Origin` header (a web page)
is refused, there are no CORS headers, bodies are cut at 192 KiB while read, and fields are validated before the
Durable Object is touched. Answers: `400 bad_request`, `403 origin_not_allowed`, `404 not_found`, `413 too_large`,
`415 unsupported_media_type`, `429 hourly_limit | daily_limit | limit` (with `Retry-After`), `502 unavailable`.

Limits: 3 reports per hour and 10 per day per connection address (IPv6 by /64), 300 a day in all, in-memory bursts
(`src/limits.js`) and an optional edge limit (`REPORT_LIMITER`, 20 requests a minute per address). The newest 500 reports
are kept 180 days, their statuses 365 days. A report is the fields in `src/reports.js` (`READERS`), nothing else.

Statuses: Open (`new`), Investigating, Fixed (with a version), Cannot reproduce, Duplicate, Not a bug, Closed, and an
optional message to the player of up to 500 characters.

The service keeps four tables: `reports`, `report_tracking` (ticket, a salted hash of the receipt, the status),
`report_quota` (a salted hash of the address and a time, deleted after a day) and `meta` (the salt). No raw address, no
receipt, no token and no identity is stored or logged.

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

`wrangler.toml` creates the Durable Object class `BugReportDesk` (migration `v1`, SQLite) and the rate-limit binding
`REPORT_LIMITER`. Its `namespace_id` ships as the placeholder `"0"`: before the first deploy, set an integer that no other
rate-limit binding of your Cloudflare account uses (never the value of another port's Worker). Keep the block, it is the
edge abuse limit. Do not edit an applied
migration. The admin token is a Cloudflare secret you set yourself (`npx wrangler secret put JPT_BUG_REPORT_ADMIN_TOKEN`);
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
- To delete every report, deploy a migration that lists `BugReportDesk` under `deleted_classes` (and a new class name
  under `new_sqlite_classes`). This is irreversible.
- To remove the service, `npx wrangler delete`. Launchers then show "Cannot reach the report service" and keep working.

## Layout

| File | What |
|---|---|
| `src/worker.js` | the Worker, and the Durable Object class that holds the service |
| `src/api.js` | the public HTTP routes: origin, media type, size, edge limit, JSON, validation |
| `src/service.js` | the service: random values, limits, the transaction that files a report |
| `src/reports.js` | the request fields, tickets, receipts, statuses, the SQL tables, retention |
| `src/limits.js` | the in-memory rate limiter and address grouping |
| `src/admin.js`, `src/admin_page.js` | the dashboard's API and its static page |
| `src/sha256.js` | a synchronous SHA-256 |
| `local/host.js` | the Node host (node:sqlite) the tests use |
| `test/` | the tests; `test/fixtures/request.json` is the canonical request |
