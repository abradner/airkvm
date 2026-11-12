# syntax=docker/dockerfile:1
# check=error=true

# Production image. Kamal is deliberately NOT used here — this repo's
# responsibility ends at a runnable image and deploy manifests live in a
# separate gitops repo (issue #68). Build and run by hand:
#
#   docker build -t coordinator .
#   docker run -d -p 80:80 \
#     -e SECRET_KEY_BASE=<openssl rand -hex 64> \
#     -e DATABASE_URL=... -e CACHE_DATABASE_URL=... -e QUEUE_DATABASE_URL=... \
#     --name coordinator coordinator
#
# Runtime ENV contract. Required — the app raises at boot without these rather
# than degrading (AGENTS.md §8; config/initializers/):
#
#   DATABASE_URL         primary database
#   CACHE_DATABASE_URL   Solid Cache's database
#   QUEUE_DATABASE_URL   Solid Queue's database
#   SECRET_KEY_BASE      signs session cookies; Rails refuses to boot without it
#   AIRKVM_EDITION       self_hosted | cloud — no default outside dev/test, because
#                        both editions boot fine, so a wrong guess fails silently
#                        (config/initializers/edition.rb)
#
# No RAILS_MASTER_KEY: there is no config/credentials.yml.enc. Secrets arrive
# as ENV from the gitops repo that deploys this, so a key file that exists in
# one checkout is never the only way to decrypt a committed file.
#
# All three URLs are needed, and this is the trap: Rails applies DATABASE_URL
# to the PRIMARY database only, so a container given just that one boots
# healthy, passes its health check, and then writes every cache entry and
# background job to a local socket. See README.md.
#
# Optional tuning: RAILS_MAX_THREADS, RAILS_LOG_LEVEL, and HTTP_PORT — the port
# Thruster (the CMD below) listens on, 80 by default. Not PORT: Thruster sets
# PORT itself for the Rails server it fronts, so a PORT given here is ignored.

# For a containerized dev environment, see Dev Containers: https://guides.rubyonrails.org/getting_started_with_devcontainer.html

# Make sure RUBY_VERSION matches the Ruby version in .ruby-version
ARG RUBY_VERSION=4.0.6
FROM docker.io/library/ruby:$RUBY_VERSION-slim AS base

# Rails app lives here
WORKDIR /rails

# Install base packages
RUN apt-get update -qq && \
    apt-get install --no-install-recommends -y curl libjemalloc2 postgresql-client && \
    ln -s /usr/lib/$(uname -m)-linux-gnu/libjemalloc.so.2 /usr/local/lib/libjemalloc.so && \
    rm -rf /var/lib/apt/lists /var/cache/apt/archives

# Set production environment variables and enable jemalloc for reduced memory usage and latency.
ENV RAILS_ENV="production" \
    BUNDLE_DEPLOYMENT="1" \
    BUNDLE_PATH="/usr/local/bundle" \
    BUNDLE_WITHOUT="development:test" \
    LD_PRELOAD="/usr/local/lib/libjemalloc.so"

# Throw-away build stage to reduce size of final image
FROM base AS build

# Install packages needed to build gems
RUN apt-get update -qq && \
    apt-get install --no-install-recommends -y build-essential git libpq-dev libyaml-dev pkg-config && \
    rm -rf /var/lib/apt/lists /var/cache/apt/archives

# Install application gems
COPY vendor/* ./vendor/
COPY Gemfile Gemfile.lock ./

RUN bundle install && \
    rm -rf ~/.bundle/ "${BUNDLE_PATH}"/ruby/*/cache "${BUNDLE_PATH}"/ruby/*/bundler/gems/*/.git && \
    # -j 1 disable parallel compilation to avoid a QEMU bug: https://github.com/rails/bootsnap/issues/495
    bundle exec bootsnap precompile -j 1 --gemfile

# Copy application code
COPY . .

# Precompile bootsnap code for faster boot times.
# -j 1 disable parallel compilation to avoid a QEMU bug: https://github.com/rails/bootsnap/issues/495
RUN bundle exec bootsnap precompile -j 1 app/ lib/

# Stand-ins for this step only — see the script, which CI also runs.
RUN ./bin/precompile-assets




# Final stage for app image
FROM base

# Run and own only the runtime files as a non-root user for security
RUN groupadd --system --gid 1000 rails && \
    useradd rails --uid 1000 --gid 1000 --create-home --shell /bin/bash
USER 1000:1000

# Copy built artifacts: gems, application
COPY --chown=rails:rails --from=build "${BUNDLE_PATH}" "${BUNDLE_PATH}"
COPY --chown=rails:rails --from=build /rails /rails

# Entrypoint prepares the database.
ENTRYPOINT ["/rails/bin/docker-entrypoint"]

# Start server via Thruster by default, this can be overwritten at runtime
EXPOSE 80
CMD ["./bin/thrust", "./bin/rails", "server"]
