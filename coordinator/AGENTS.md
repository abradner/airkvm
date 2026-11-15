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

Status: early. It boots, fails fast on missing configuration, serves Prometheus metrics, signs
operators in, and bootstraps a self-hosted install through a one-time setup page. There is no
device enrolment yet — issue #68's next bullet.

## 3. Stack

| Layer | Choice | Notes |
|---|---|---|
| Runtime | Rails 8.1, Ruby 4.0 | pinned exactly in the root `mise.toml`; `.ruby-version` and the Dockerfile agree |
| Database | Postgres 18 | dev container on **port 25433**; production needs three URLs (below) |
| Keys | UUIDv7 on every application (primary-database) table | native `uuidv7()` in Postgres 18; the Solid Cache/Queue databases keep their gems' bigint keys — see §4 |
| Tests | rspec, factory_bot, capybara + cuprite | cuprite drives system Chromium over CDP, and **works on this Pi** despite root §6.1's `--headless` hang (probed 2026-09-26, §6.1 below). No system specs exist yet |
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

**Enforced at the HTTP boundary, by convention plus a spec.** `current_account` is
`Current.session.user.account` (`ApplicationController`). `spec/requests/operators_spec.rb`
proves a cross-account id is a 404 byte-identical, in production's reply, to an id that never
existed — and was seen to fail with `User.find` planted. Nothing *mechanically* stops a new
controller using the bare class; each new resource brings its own cross-account spec.

### First-boot setup exists until an Account does, and never in the cloud

With no Account, a self-hosted install redirects every page to `/setup`
(`prepend_before_action`, ahead of authentication — on a fresh install there is nobody to sign
in). It creates the Account and first operator together, then 404s for everyone while any Account
exists — derived from state, not stored, so emptying the accounts table reopens it (R-012). In the
cloud edition it is always a 404. `FirstBoot.complete!` takes a Postgres advisory lock and
re-checks under it, because until used the page is open to anyone who can reach the service; the
re-check depends on READ COMMITTED (see the comment there).

**Constraint on any future account deletion** (accepted with the derived design, 2026-11-09): it
must refuse to delete the last Account, or it must re-gate `/setup` — a stored completion marker —
in the same change. Without one of the two, deleting the last Account hands the install to
whoever reaches `/setup` next.

### Sessions and sign-in

- Sessions last `Authentication::SESSION_LIFETIME` (7 days) from sign-in. The expiry is signed
  into the cookie and checked by Rails on every request — that is the one enforcement; do not add
  an age check on the `sessions` row beside it. A password change destroys every session.
- **Never use `request.remote_ip` for anything security-relevant; use `client_address`.** Rails
  believes `X-Forwarded-For` from any peer — `trusted_proxies` only strips known proxies from the
  chain — so a client reaching Puma directly names its own address. `client_address` believes
  the header only when the peer is a trusted proxy: loopback (the in-image Thruster, verified to
  append the peer it saw) plus whatever `TRUSTED_PROXIES` names. **Fail closed** (decided
  2026-11-09): from any other peer the header is ignored, and the first such request per peer is
  logged so an unlisted proxy is diagnosable (`config/trusted_proxies.rb`). Both ask the one
  predicate, `TrustedProxies.trusted?`. Sign-in is rate-limited both by that address and by email
  address.
- TLS is the fronting proxy's job and is required: `assume_ssl` means the app cannot detect its
  absence (README).

There is **no password reset**: the generator's email flow was removed, because a self-hosted
install usually has no mail and a link that emails into the void is a claim the mechanism does
not deliver (root principle 1). The README documents the console reset.

### An edition changes policy, never data shape

`Airkvm.edition.self_hosted?` / `.cloud?` may decide *who* may do something — whether sign-up
exists, whether the first-boot setup page is reachable, limits. It must never decide *what the
data is*: no edition-only column, no edition-only table, no record that is tenant-scoped in one
edition and global in the other. Both editions run the same migrations against the same schema.
Code that branches on the edition to add or drop a column is the seam being misused.

### Every application table has a UUIDv7 primary key, and every reference is a uuid

**Scope: the primary database — the tables this app's migrations own.** The Solid Cache and Solid
Queue databases (`db/cache_schema.rb`, `db/queue_schema.rb`) are framework-owned, use bigint keys
and references, hold no tenant data, and are never exposed by id; they are left as their gems
ship them, because rewriting a vendor schema would be a fork to maintain. The spec reads the
primary database only, and that is the whole of what it enforces.

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

### Secrets arrive as ENV, and nothing secret is committed — not even encrypted

**The rule, decided 2026-11-09** (it was a proposal in #81): every secret the coordinator needs —
`SECRET_KEY_BASE` today, and whatever comes next — reaches it as an environment variable, set by
the gitops repo that deploys it. There is no `config/credentials.yml.enc`, no `master.key`, no
`RAILS_MASTER_KEY`, and none is to be added. Why: an encrypted file in the repo is only as safe as
a key file sitting in somebody's checkout, and that key becomes the one way to decrypt it — so
the secret's real home is wherever that key happens to be. With ENV the deploying repo is the one
source, and this tree holds no ciphertext to protect. A missing secret is required configuration
and fails at boot, like the rest of this section. Build steps that need a stand-in get one scoped
to the step (`bin/precompile-assets`), never baked into the image (root §8).

### Required configuration fails fast at boot

`DATABASE_URL`, `CACHE_DATABASE_URL`, `QUEUE_DATABASE_URL`, `AIRKVM_EDITION`,
`COORDINATOR_HOSTS` and `SECRET_KEY_BASE` are required in production and raise at boot when missing
(`config/initializers/`, and Rails itself for the secret — there is no credentials file to fall
back on). `spec/smoke/boot_spec.rb` boots production in a child process to pin each refusal. None has a production
default, for reasons worth knowing before adding one:

- **Three database URLs, not one.** Rails applies `DATABASE_URL` to the *primary* database
  only. A deploy setting just that one boots healthy and sends every cache write and background
  job to a local socket.
- **No default edition.** Both editions boot and serve normally, so a wrong guess does not fail
  — a cloud deploy running as `self_hosted` would expose the first-boot setup page instead. The
  local default is an allowlist (development and test), so staging fails closed.
- **`COORDINATOR_HOSTS` is required, not optional-with-a-warning** (#88, decided 2026-11-09). It
  populates `config.hosts`; unset, Rails answers any `Host`, which lets a DNS-rebinding page reach
  the first-boot setup page (R-012). `/up` is exempt so health checks dialling an address work.

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
- **The root `mise.toml` must never set `PORT` globally.** `[env]` reaches every process mise
  starts, Rails reads `PORT` as the TCP port to bind, and the firmware's serial default
  `/dev/ttyACM0` made `co:dev` die with `Integer(): "/dev/ttyACM0"`. It is now per-task on the
  three flash tasks. Inside an agent worktree the main checkout's `mise.toml` is an ancestor
  (root §6.1), so the collision persists there until `main` carries the fix — bypass it with
  `env -u PORT PATH="$(mise where ruby)/bin:$PATH" bin/rails server`, not `mise x`, which
  re-applies it.
- **Rails 8.1.3.1 plus json 3 cannot read a signed cookie.** ActiveSupport passed `JSON.parse` a
  positional options hash that json 3 no longer accepts, so every login raised ArgumentError.
  The Gemfile floor is `~> 8.1.4`; do not relax it.
- **The test environment's 404 is a debug page that echoes the requested id.** A spec comparing
  two 404s there will always see them differ. Compare production's reply
  (`show_detailed_exceptions = false`, i.e. `public/404.html`) — see `operators_spec.rb`.
- **cuprite works here; the Chromium hang in root §6.1 does not apply to it.** Probed with a
  throwaway system spec rendering `/setup/new` in ~5 s, and a planted `have_text` for absent text
  failed with the real page body — so the instrument was seen both ways. It needs only
  `require "capybara/cuprite"` and `driven_by :cuprite` — no sandbox flag (tested without one).
- **`0 examples, 0 failures` is not a pass — it is a suite that never ran.** A probe migration
  dated 2999 was rejected outright (Rails 8.1: `Invalid timestamp … for migration file`, exit 1),
  which is loud — but the probe had redirected that output to `/dev/null`, so the rejection was
  invisible. The pending migration then made rspec abort before any example, and a check looking
  only at the failure count read it as green. The first draft of this very entry blamed Rails for
  "silently skipping" the migration; re-running with the output visible showed the silence was
  the probe's. Read the example count, not just the failures, and never hide a setup step's
  output in a check whose conclusion depends on that step having happened.

## 7. CI

The `coordinator` job in the root `.github/workflows/ci.yml`: specs, `co:lint` and `co:precompile`
(the image's asset step, by the script the Dockerfile runs) against a `postgres:18` service on port
25433, matching the local config. A third job rather than steps in
`checks`, because a `services:` block is per-job — the one case the root §7 billing logic cannot
satisfy.
