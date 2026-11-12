require "rails_helper"

# The warning for X-Forwarded-For from a peer that is not a trusted proxy
# (config/trusted_proxies.rb). Fail-closed is unchanged — sessions_spec.rb
# pins that the header is ignored — so all this asserts is that the deploy is
# TOLD, once per peer, through the real middleware stack.
#
# The middleware is one instance for the whole process, so its once-per-peer
# memo outlives an example: every example uses peers no other example uses.
RSpec.describe "X-Forwarded-For from an untrusted peer" do
  def visit(from:, forwarded_for: nil)
    headers = forwarded_for ? { "X-Forwarded-For" => forwarded_for } : {}
    get new_session_path, headers:, env: { "REMOTE_ADDR" => from }
  end

  def untrusted_warnings
    calls = []
    allow(Rails.logger).to receive(:warn) { |message| calls << message.to_s }
    calls
  end

  it "warns, naming the peer and TRUSTED_PROXIES, and never the header's claimed address" do
    calls = untrusted_warnings
    visit(from: "10.20.0.1", forwarded_for: "203.0.113.77")

    warning = calls.find { |message| message.include?("10.20.0.1") }
    expect(warning).to include("not a trusted proxy")
    expect(warning).to include("TRUSTED_PROXIES")
    expect(warning).not_to include("203.0.113.77")
  end

  it "warns once per peer, not once per request" do
    calls = untrusted_warnings
    3.times { visit(from: "10.20.0.2", forwarded_for: "203.0.113.1") }
    visit(from: "10.20.0.3", forwarded_for: "203.0.113.1")

    expect(calls.count { |message| message.include?("10.20.0.2") }).to eq(1)
    expect(calls.count { |message| message.include?("10.20.0.3") }).to eq(1)
  end

  it "stays quiet for a trusted peer, and for a request with no forwarded header" do
    calls = untrusted_warnings
    visit(from: "127.0.0.1", forwarded_for: "203.0.113.2")
    visit(from: "10.20.0.4")

    expect(calls.grep(/not a trusted proxy/)).to be_empty
  end

  it "stays quiet for a proxy named in TRUSTED_PROXIES" do
    proxies = Rails.application.config.action_dispatch.trusted_proxies
    original = proxies.dup
    proxies << IPAddr.new("10.20.0.5")
    calls = untrusted_warnings

    visit(from: "10.20.0.5", forwarded_for: "203.0.113.3")
    expect(calls.grep(/not a trusted proxy/)).to be_empty
  ensure
    proxies.replace(original)
  end

  it "stops remembering new peers past its cap, so the memo cannot grow without bound" do
    warning = TrustedProxies::UntrustedForwardingWarning
    logger = instance_double(Logger, warn: nil)
    middleware = warning.new(->(_env) { [ 200, {}, [] ] }, logger:)

    (warning::MAX_PEERS + 5).times do |i|
      middleware.call("REMOTE_ADDR" => "10.30.#{i / 250}.#{i % 250 + 1}", "HTTP_X_FORWARDED_FOR" => "203.0.113.4")
    end
    expect(logger).to have_received(:warn).with(/not a trusted proxy/).exactly(warning::MAX_PEERS).times
    expect(logger).to have_received(:warn).with(/further untrusted peers will not be logged/).once
  end

  it "says nothing, and remembers nothing, for a request with no peer address" do
    logger = instance_double(Logger, warn: nil)
    middleware = TrustedProxies::UntrustedForwardingWarning.new(->(_env) { [ 200, {}, [] ] }, logger:)

    middleware.call("HTTP_X_FORWARDED_FOR" => "203.0.113.5")
    expect(logger).not_to have_received(:warn)
  end
end
