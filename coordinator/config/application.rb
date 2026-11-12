require_relative "boot"

require "rails"
# Pick the frameworks you want:
require "active_model/railtie"
require "active_job/railtie"
require "active_record/railtie"
require "action_controller/railtie"
require "action_mailer/railtie"
# require "action_mailbox/engine"
# require "action_text/engine"
require "action_view/railtie"
# require "action_cable/engine"
# require "rails/test_unit/railtie"

# Require the gems listed in Gemfile, including any gems
# you've limited to :test, :development, or :production.
Bundler.require(*Rails.groups)

module Coordinator
  class Application < Rails::Application
    # Initialize configuration defaults for originally generated Rails version.
    config.load_defaults 8.1

    # Please, add to the `ignore` list any other `lib` subdirectories that do
    # not contain `.rb` files, or that should not be reloaded or eager loaded.
    # Common ones are `templates`, `generators`, or `middleware`, for example.
    config.autoload_lib(ignore: %w[assets tasks])

    # Generated migrations get a UUIDv7 primary key and uuid-typed references.
    # This line (with lib/templates/) only makes the default right; a
    # hand-written migration bypasses it. What enforces the rule — every
    # primary key a uuidv7() uuid, every *_id column a uuid — is
    # spec/db/primary_keys_spec.rb. Why v7, and what it gives away (creation
    # time), is in coordinator/AGENTS.md §4.
    # Trust no proxy but loopback — the Thruster process the image runs in
    # front of Puma. Rails' default trusts every private address, which on a
    # self-hosted LAN is where the clients are, so a client's own
    # X-Forwarded-For entries would be skipped over as "proxies". Behind a
    # further reverse proxy, every client then shares the proxy's address — a
    # stricter rate limit, not a looser one. This alone is not enough: see
    # Authentication#client_address for the half Rails does not do.
    config.action_dispatch.trusted_proxies = [ IPAddr.new("127.0.0.1"), IPAddr.new("::1") ]

    config.generators do |g|
      g.orm :active_record, primary_key_type: :uuid
    end

    # Configuration for the application, engines, and railties goes here.
    #
    # These settings can be overridden in specific environments using the files
    # in config/environments, which are processed later.
    #
    # config.time_zone = "Central Time (US & Canada)"
    # config.eager_load_paths << Rails.root.join("extras")

    # Don't generate system test files.
    config.generators.system_tests = nil
  end
end
