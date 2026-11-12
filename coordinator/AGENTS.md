# coordinator — Agent Guide

> The fleet coordination service: the Rails app dongles enrol with and operators drive.
> **Read the repo-root `AGENTS.md` first** — its principles (§2), working rules (§8) and
> maintenance rules (§10) bind here unchanged. This file carries only what is specific to
> this tree. Design: `docs/design/fleet.md`. Roadmap and decisions: issue #68.
>
> Section numbers mirror the root file's, so §4 here extends root §4; a gap (§2, §5) means the
> root section applies unchanged.

## 1. What this is

One Rails app, two **editions** from one codebase (`AIRKVM_EDITION`):

- `self_hosted` — a homelab or an operator's own infrastructure. Normally one Account.
- `cloud` — app.airkvm.io. One Account per customer.

Status: early. It boots, fails fast on missing configuration, serves Prometheus metrics, and
has an `Account` model. There is no authentication, no `User`, and no device enrolment yet —
the next interstitials in issue #68.

## 3. Stack

| Layer | Choice | Notes |
|---|---|---|
| Runtime | Rails 8.1, Ruby 4.0.6 | pinned exactly in the root `mise.toml`; `.ruby-version` and the Dockerfile agree |
| Database | Postgres 18 | dev container on **port 25433**; production needs three URLs (below) |
| Keys | UUIDv7 everywhere | native `uuidv7()` in Postgres 18 — see §4 |
| Tests | rspec, factory_bot, capybara + cuprite | cuprite drives system Chromium over CDP; unproven on this Pi (root §6.1: headless Chromium hangs here) |
| Lint | rubocop-omakase, brakeman, bundler-audit, `importmap audit` | four tools; `co:lint` runs all of them |
| Metrics | yabeda, `/metrics` on port 9394 | loopback by default; see §6.1 for the control socket |
| Frontend | importmap + propshaft, no build step | same rule as the console (root §4) |

## 4. Critical rules

### Tenancy: every record is reached through its Account

Every domain table `belongs_to :account`. Every read goes through the current account's
association — `current_account.devices.find(id)` — and never through the bare class,
`Device.find(id)`. The bare form is how one tenant's session reaches another tenant's device,
and on a service that can type on every enrolled machine that is the whole ballgame.

A record belonging to another Account is a **404, not a 403**. A 403 confirms the id exists.

**Stated, not yet enforced.** `current_account` does not exist until operator authentication
lands (issue #68's next interstitial). Until then this rule has nothing to apply to; when it
lands, it lands with a spec proving a cross-account lookup is a 404.

### An edition changes policy, never data shape

`Airkvm.edition.self_hosted?` / `.cloud?` may decide *who* may do something — whether sign-up
exists, whether the first-boot setup page is reachable, limits. It must never decide *what the
data is*: no edition-only column, no edition-only table, no record that is tenant-scoped in one
edition and global in the other. Both editions run the same migrations against the same schema.
Code that branches on the edition to add or drop a column is the seam being misused.

### Every table has a UUIDv7 primary key, and every reference is a uuid

v7 rather than bigint so ids are not enumerable — device and enrolment ids must not be guessable
by counting — and rather than v4 because v7 is time-ordered, so inserts stay at the right-hand
edge of the index instead of fragmenting it.

**What v7 gives away: when the record was created.** The first 48 bits are a millisecond
timestamp, so any id in a URL, a log line or a support ticket dates its record. Guessing one is
still infeasible (~62 random bits remain). Accepted; if a record's creation time is ever itself
sensitive, it needs a second, opaque identifier rather than a different key type.

**Enforced by `spec/db/primary_keys_spec.rb`**, which reads the real schema and names any table
whose key is not a uuid defaulting to exactly `uuidv7()`, and any `*_id` or foreign-key column
that is not a uuid. The reference half matters as much as the key: a bigint `account_id` given a
uuid is cast with `to_i`, which keeps only the leading digits (`"01998a3c-…"` → `1998`), and v7
ids made close together share them — so a scoped query would match other tenants' rows. The
generator config in `config/application.rb` and the migration template in `lib/templates/` make
generated migrations right by default, but those are politeness: a hand-written migration
bypasses both, and the spec is what catches it. Proven with planted tables: a bigint
`t.references :account`, a bigint `owner_id`, and a `gen_random_uuid()` key each failed it.

Two cases it will flag that are not mistakes of yours:
- **Join tables.** `create_join_table` makes a table with no primary key, and the spec reports
  it. Give the table a uuidv7 `id` — this service has no reason to use HABTM over `has_many
  :through` with a model.
- **Rails install generators** (`action_text:install` and the like) emit `id: :uuid` with no
  default, which Postgres fills with `gen_random_uuid()` (v4). Add `default: -> { "uuidv7()" }`
  to the generated migration before running it.

### Required configuration fails fast at boot

`DATABASE_URL`, `CACHE_DATABASE_URL`, `QUEUE_DATABASE_URL` and `AIRKVM_EDITION` are required in
production and raise at boot when missing (`config/initializers/`). None has a production
default, for reasons worth knowing before adding one:

- **Three database URLs, not one.** Rails applies `DATABASE_URL` to the *primary* database
  only. A deploy setting just that one boots healthy and sends every cache write and background
  job to a local socket.
- **No default edition.** Both editions boot and serve normally, so a wrong guess does not fail
  — a cloud deploy running as `self_hosted` would expose the first-boot setup page instead. The
  local default is an allowlist (development and test), so staging fails closed.

## 6. Development

```sh
mise run co:db       # the development Postgres (docker compose, port 25433)
mise run co:setup    # bundle install + db:prepare — idempotent
mise run co:dev      # bin/rails server
mise run co:test     # rspec (needs co:db)
mise run co:lint     # rubocop, brakeman, bundler-audit, importmap audit
```

**`co:*` is deliberately not in the root `test`/`lint`/`fmt` umbrellas.** Its checks need a
container and the network, and a mise umbrella kills its siblings on the first failure (root
§6.1) — so wiring it in would redden a firmware-only change on any machine without Postgres.
CI runs it as its own job.

### 6.1 Gotchas

- **Puma cannot put its control socket in `tmp/`.** A unix socket path is capped at 108 bytes
  and this repo's agent worktrees make `<app>/tmp/pumactl.sock` 110. Puma then refuses to start
  at all. It lives in a private directory under `$TMPDIR` instead — `config/puma_control_socket.rb`.
- **Puma creates unix sockets world-writable** (umask 0). The old in-app path was only safe
  because the home directory above it is `0700`. The socket's *directory* is now what protects
  it, verified at boot with `lstat` (not a symlink, owned by us, mode `0700`), plus Puma's
  control token rather than `no_token`.
- **Puma's `?umask=` does not work with yabeda.** Its stats fetcher opens the control URL as a
  literal path, so it looks for a file named `pumactl.sock?umask=0077` and the *entire* `/metrics`
  response fails with ENOENT — not just the Puma gauges.
- **`postgres:18` refuses the pre-18 `/var/lib/postgresql/data` mount** with a hard error, and
  the container then vanishes from `docker compose ps`, so it looks like it never started.
  Mount `/var/lib/postgresql`.
- **ERB renders `config/database.yml` in every environment.** An `ENV.fetch` for a
  production-only variable there raises during development boot. Enforce in an initializer.
- **`0 examples, 0 failures` is not a pass — it is a suite that never ran.** A probe migration
  dated 2999 was rejected outright (Rails 8.1: `Invalid timestamp … for migration file`, exit 1),
  which is loud — but the probe had redirected that output to `/dev/null`, so the rejection was
  invisible. The pending migration then made rspec abort before any example, and a check looking
  only at the failure count read it as green. The first draft of this very entry blamed Rails for
  "silently skipping" the migration; re-running with the output visible showed the silence was
  the probe's. Read the example count, not just the failures, and never hide a setup step's
  output in a check whose conclusion depends on that step having happened.

## 7. CI

The `coordinator` job in the root `.github/workflows/ci.yml`: specs and `co:lint` against a
`postgres:18` service on port 25433, matching the local config. A third job rather than steps in
`checks`, because a `services:` block is per-job — the one case the root §7 billing logic cannot
satisfy.
