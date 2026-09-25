# Fail fast when a production database URL is missing (AGENTS.md §8:
# required configuration raises at startup rather than silently degrading).
#
# This is an initializer rather than an `ENV.fetch` in config/database.yml
# because ERB renders that file in every environment — a bare fetch for a
# production-only variable raises during development boot as well.
#
# All three are required, and the third one is the reason this file exists:
# Rails applies DATABASE_URL to the PRIMARY database only
# (activerecord's database_configurations.rb: `url ||= ENV["DATABASE_URL"]
# if name == "primary"`). Named entries read <NAME>_DATABASE_URL. So a deploy
# that set DATABASE_URL alone booted healthy, served requests, and sent every
# cache write and background job to a local socket instead — with
# bin/docker-entrypoint's db:prepare creating those databases there.
# Defined unconditionally so spec/smoke/boot_spec.rb can clear exactly these
# variables in the production child process it boots; only the enforcement is
# environment-gated.
REQUIRED_DATABASE_URLS = %w[DATABASE_URL CACHE_DATABASE_URL QUEUE_DATABASE_URL].freeze

if Rails.env.production?
  missing = REQUIRED_DATABASE_URLS.reject { |key| ENV[key].present? }

  if missing.any?
    raise <<~MESSAGE
      Missing required database configuration: #{missing.join(", ")}.

      The coordinator needs one URL per database. DATABASE_URL does not cover
      the cache and queue databases — Rails only applies it to the primary —
      so without CACHE_DATABASE_URL and QUEUE_DATABASE_URL this process would
      boot healthy and then write cache entries and background jobs to a
      local socket.
    MESSAGE
  end
end
