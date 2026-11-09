require "rails_helper"
require "open3"

# What this file may and may not assert.
#
# An earlier version opened with three examples and a comment claiming none of
# them was a placeholder. Two of them were, structurally, and a review caught
# it:
#
#   * `expect(Rails.application).to be_initialized` — rails_helper requires
#     config/environment, so if boot failed RSpec aborts at load and no example
#     runs at all. It passes if and only if the suite ran. It cannot fail.
#   * `ActiveRecord::Migration.check_all_pending!` — rails_helper already calls
#     maintain_test_schema!, which LOADS the schema rather than raising. The
#     helper establishes the exact condition the example then asserts.
#
# Both are gone. What is left can fail for a real reason, which is the only
# thing that makes a suite worth running (AGENTS.md §8).
RSpec.describe "the coordinator scaffold" do
  it "round-trips a query against its database" do
    # active? issues a real query, unlike anything rails_helper has already
    # guaranteed — this fails if Postgres is down or the credentials are wrong.
    expect(ActiveRecord::Base.connection).to be_active
    expect(ActiveRecord::Base.connection.select_value("SELECT 1")).to eq(1)
  end

  it "is connected to the test database, not development" do
    # The real foot-gun: a misread database.yml pointing specs at development
    # would pass every other example here while destroying dev data. Asked of
    # the server rather than read from config, so it is the database actually
    # connected to. (The port is not asserted: inside the compose container
    # Postgres listens on 5432 whatever the host publishes, and a developer may
    # legitimately set COORDINATOR_DB_PORT.)
    expect(Rails.env).to eq("test")
    expect(ActiveRecord::Base.connection.select_value("SELECT current_database()")).to match(/_test\z/)
  end

  # Boot-time enforcement, exercised by booting production in a child process.
  # Pinning the REQUIRED_DATABASE_URLS list alone (an earlier version) left the
  # enforcement itself deletable with every spec green; this fails if the check
  # stops raising, stops naming what is missing, or starts refusing a complete
  # configuration.
  describe "production boot" do
    def boot_production(edition: "self_hosted", secret: :dummy, hosts: "kvm.example.net",
                        script: "puts :booted", **urls)
      env = REQUIRED_DATABASE_URLS.to_h { |key| [ key, nil ] }.merge(urls.transform_keys(&:to_s))
      env.merge!("RAILS_ENV" => "production", "AIRKVM_EDITION" => edition,
                 CoordinatorHosts::VARIABLE => hosts,
                 "SECRET_KEY_BASE" => nil, "SECRET_KEY_BASE_DUMMY" => (secret == :dummy ? "1" : nil))
      out, status = Open3.capture2e(env, "bin/rails", "runner", script,
                                    chdir: Rails.root.to_s)
      [ out, status ]
    end

    def all_urls = { DATABASE_URL: url, CACHE_DATABASE_URL: url, QUEUE_DATABASE_URL: url }

    let(:url) { "postgres://boot-spec-stand-in/unused" }

    it "refuses to start with no database URLs, naming all three" do
      out, status = boot_production
      expect(status).not_to be_success
      expect(out).to include("Missing required database configuration: DATABASE_URL, CACHE_DATABASE_URL, QUEUE_DATABASE_URL")
    end

    it "refuses DATABASE_URL alone — the trap: Rails applies it to the primary only" do
      out, status = boot_production(DATABASE_URL: url)
      expect(status).not_to be_success
      expect(out).to include("Missing required database configuration: CACHE_DATABASE_URL, QUEUE_DATABASE_URL")
    end

    it "refuses to start without an edition, even with every URL set" do
      out, status = boot_production(edition: nil, DATABASE_URL: url, CACHE_DATABASE_URL: url, QUEUE_DATABASE_URL: url)
      expect(status).not_to be_success
      expect(out).to include("AIRKVM_EDITION is not set")
    end

    it "refuses to start without SECRET_KEY_BASE — there is no credentials file to fall back on" do
      out, status = boot_production(secret: nil, DATABASE_URL: url, CACHE_DATABASE_URL: url, QUEUE_DATABASE_URL: url)
      expect(status).not_to be_success
      expect(out).to include("Missing `secret_key_base`")
    end

    it "refuses to start without COORDINATOR_HOSTS, even with everything else set (#88)" do
      out, status = boot_production(hosts: nil, **all_urls)
      expect(status).not_to be_success
      expect(out).to include("Missing required configuration: COORDINATOR_HOSTS")
    end

    it "refuses a COORDINATOR_HOSTS that names no host (blank entries do not count)" do
      out, status = boot_production(hosts: " , ", **all_urls)
      expect(status).not_to be_success
      expect(out).to include("Missing required configuration: COORDINATOR_HOSTS")
    end

    # The boot refusal is only worth having if the list is then enforced. Rack
    # requests straight into the booted production app: HostAuthorization sits
    # ahead of everything that would touch the (stand-in) database.
    it "answers only the hosts it was given, and /up to anyone" do
      script = <<~RUBY
        %w[https://evil.example/setup/new https://kvm.example.net/up https://evil.example/up].each do |url|
          puts "\#{url} \#{Rails.application.call(Rack::MockRequest.env_for(url)).first}"
        end
      RUBY
      out, status = boot_production(script:, **all_urls)
      expect(status).to be_success, out
      expect(out).to include("https://evil.example/setup/new 403")
      expect(out).to include("https://kvm.example.net/up 200")
      expect(out).to include("https://evil.example/up 200")
    end

    it "starts with all three (the positive control: the check is not simply always failing)" do
      out, status = boot_production(DATABASE_URL: url, CACHE_DATABASE_URL: url, QUEUE_DATABASE_URL: url)
      expect(out).to include("booted")
      expect(status).to be_success
    end
  end
end
