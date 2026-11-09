# coordinator

The fleet coordination service for airkvm — the Rails app devices enrol with and
operators drive. Design: `../docs/design/fleet.md`. Roadmap and decisions:
[issue #68](https://github.com/abradner/airkvm/issues/68).

**Status: scaffold.** There are Accounts, operators who sign in, and a
first-boot setup page; there is no device enrolment yet.

## Running it locally

```sh
mise run co:db       # start the development Postgres (docker compose)
mise run co:setup    # bundle install + db:prepare
mise run co:dev      # rails server
```

Then `mise run co:test` for the specs and `mise run co:lint` for style and
security (rubocop, brakeman, bundler-audit, `importmap audit` — four tools, so a
tree that runs only the first has no security gate).

Two things that will otherwise waste your time:

- **The development database is on port 25433**, not 5432 and not 25432. A
  sibling repo already uses 25432, and a collision there connects you to the
  wrong project's database rather than refusing — which is worse.
- **`co:*` is deliberately not in the `test`/`lint`/`fmt` umbrellas.** Its
  checks need a container and the network, and a mise umbrella kills its
  siblings on first failure, so wiring it in would redden a firmware-only
  change on a machine with no database running. Run the `co:` tasks directly.

## First boot (self-hosted)

On a fresh self-hosted install every page redirects to `/setup`, which creates
the Account and its first operator and signs you in. After that `/setup` is a
404 for everyone, signed in or not, for as long as any Account exists — it is
derived from that, not stored, so emptying the accounts table from a console
would reopen it. It is race-safe — two people submitting at the same moment get
one Account between them, not two. Until it is used, anyone who can reach the
coordinator can use it, so complete it before exposing the service. The cloud
edition has no setup page at all.

There is **no password reset** yet. Rails' generator ships one that sends email,
but nothing here has mail configured, and a "forgot password?" link that emails
into the void would be a promise the app cannot keep. Reset from a console:

```sh
bin/rails runner 'User.find_by!(email_address: "you@example.com").update!(password: "a new long password")'
```

Changing a password signs that operator out everywhere, so this is also how to
cut off a leaked session. Sessions otherwise last seven days from sign-in.

## TLS is required in front of it

Production sets `assume_ssl` and `force_ssl`: the app believes every request
arrived over HTTPS, marks its cookies `Secure`, and sends HSTS. It does **not**
redirect plain HTTP, and cannot tell when nothing terminated TLS. Run it without
a TLS-terminating proxy in front and passwords cross the network in cleartext,
while browsers drop the `Secure` session cookie so sign-in silently fails.

## Runtime configuration

Required in production. The app **raises at boot** if any is missing rather
than starting and failing later (AGENTS.md §8): the database URLs in
`config/initializers/require_database_urls.rb`, the edition in
`config/initializers/edition.rb`, the host list in
`config/initializers/require_hosts.rb`, and `SECRET_KEY_BASE` by Rails itself. Each
refusal is pinned by `spec/smoke/boot_spec.rb`, which boots production in a
child process.

| Variable | Why |
|---|---|
| `DATABASE_URL` | Primary database |
| `CACHE_DATABASE_URL` | Solid Cache's database |
| `QUEUE_DATABASE_URL` | Solid Queue's database |
| `SECRET_KEY_BASE` | Signs session cookies. Rails refuses to boot production without it. Generate with `openssl rand -hex 64` |
| `AIRKVM_EDITION` | `self_hosted` or `cloud`. No default outside development and test — see below |
| `COORDINATOR_HOSTS` | The host name(s) operators reach the service by, comma-separated (`kvm.example.net`; a leading dot, `.example.net`, also matches subdomains). Every other `Host` gets a 403, except `/up` for health checks. Without it a DNS-rebinding page could reach the first-boot setup page |

**The three URLs are not redundant, and this is the trap.** Rails applies
`DATABASE_URL` to the *primary* database only — `url ||= ENV["DATABASE_URL"] if
name == "primary"` in Active Record's `database_configurations.rb`. Named
entries read `<NAME>_DATABASE_URL`. A deploy that set `DATABASE_URL` alone would
boot healthy, pass its health check, serve requests, and send every cache write
and every background job to a *local socket* instead — with the entrypoint's
`db:prepare` creating those databases there. Hence the boot check.

**`AIRKVM_EDITION` has no production default on purpose.** Both editions boot
and serve normally, so a wrong guess would not fail loudly — a cloud deploy
running as `self_hosted` would expose the first-boot setup page to the internet
rather than refuse to start. Development and test default to `self_hosted`; any
other environment, staging included, must say which it is. (That allowlist
keys on `RAILS_ENV`, which Rails defaults to `development` when unset — the
image sets it, but a hand-run `bin/rails server` on a host must too, or it
quietly becomes a self-hosted development server.) The rule the seam
holds: an edition may change policy (who may sign up, limits), never data shape.

There is no `RAILS_MASTER_KEY` and no `config/credentials.yml.enc`: secrets
arrive as ENV from the deploying gitops repo, so no key file sitting in one
checkout is ever the only way to decrypt something committed here.

Optional, with safe defaults: `RAILS_MAX_THREADS`, `RAILS_LOG_LEVEL`, and
`HTTP_PORT` (the port Thruster listens on in the image, 80 by default — not
`PORT`, which Thruster sets for the server behind it).

`TRUSTED_PROXIES` is optional in the sense that it has a safe default, but a
real deployment needs it: the address (or CIDR) of the TLS proxy in front,
comma-separated. Without it every client appears as that proxy and they all
share one sign-in rate-limit bucket, so ten bad attempts from anyone lock every
operator out for three minutes. Name the proxy itself rather than its subnet:
a client inside a trusted range can choose its own address. A forwarded header
from a peer that is not listed is ignored, never believed, and the first one
from each such peer logs a warning naming it — so if the log says
`X-Forwarded-For arrived from <address>, which is not a trusted proxy`, and that
address is your proxy, add it here.
Local overrides: `COORDINATOR_DB_HOST`, `COORDINATOR_DB_PORT`,
`COORDINATOR_DB_USER`, `COORDINATOR_DB_PASSWORD` (development and test only —
they are deliberately not in the `default:` anchor, so production cannot
inherit a development host).

## Metrics

`bin/rails server` (Puma) serves Prometheus metrics on **port 9394**, separate
from the app: request counts and latency by `controller#action` (never by raw
path, so cardinality stays bounded), Puma pool gauges, and Ruby GC/heap stats.
`rails runner`, specs and the console do not start it.

- It is **unauthenticated**, so it binds `127.0.0.1` by default. The container
  sets `PROMETHEUS_EXPORTER_BIND=0.0.0.0` because its scraper is outside it,
  which exposes it to everything that can route to the container — the whole
  Docker network, or the whole cluster without a NetworkPolicy. Restrict it
  there; never publish it or route it through an ingress. A failing collector
  gets a bare 500, never a backtrace.
- **One Puma process, no workers.** `WEB_CONCURRENCY` refuses to boot rather
  than being honoured: with workers, the exporter would describe a master
  process that serves no requests. Scale with `RAILS_MAX_THREADS`.
- `PROMETHEUS_EXPORTER_PORT` moves it, if 9394 is taken on a shared host.
- Puma's control socket (which the pool gauges read) lives in a private `0700`
  directory under `$TMPDIR`, not in `tmp/` — see `config/puma_control_socket.rb`
  for why, and why boot refuses to start if that directory looks wrong.

## Deployment

This repo's responsibility ends at a runnable image. Deploy manifests live in a
separate gitops repo, which is why Kamal is not used here.
