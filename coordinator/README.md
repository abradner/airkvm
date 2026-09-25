# coordinator

The fleet coordination service for airkvm — the Rails app devices enrol with and
operators drive. Design: `../docs/design/fleet.md`. Roadmap and decisions:
[issue #68](https://github.com/abradner/airkvm/issues/68).

**Status: scaffold.** It boots, connects to a database, and has nothing else in
it yet. No models, no auth, no device enrolment, no `/metrics`.

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

## Runtime configuration

Required in production. The app **raises at boot** if any is missing rather
than starting and failing later (`config/initializers/require_database_urls.rb`;
AGENTS.md §8).

| Variable | Why |
|---|---|
| `DATABASE_URL` | Primary database |
| `CACHE_DATABASE_URL` | Solid Cache's database |
| `QUEUE_DATABASE_URL` | Solid Queue's database |
| `SECRET_KEY_BASE` | Signs session cookies. Rails refuses to boot production without it. Generate with `openssl rand -hex 64` |
| `AIRKVM_EDITION` | `self_hosted` or `cloud`. No default outside development and test — see below |

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
Local overrides: `COORDINATOR_DB_HOST`, `COORDINATOR_DB_PORT`,
`COORDINATOR_DB_USER`, `COORDINATOR_DB_PASSWORD` (development and test only —
they are deliberately not in the `default:` anchor, so production cannot
inherit a development host).

## Deployment

This repo's responsibility ends at a runnable image. Deploy manifests live in a
separate gitops repo, which is why Kamal is not used here.
