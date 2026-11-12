require "rails_helper"

RSpec.describe "signing in" do
  let!(:user) { create(:user) }

  it "signs in with the right password and out again" do
    sign_in_as(user)
    expect(response).to redirect_to(root_path)
    get root_path
    expect(response).to have_http_status(:ok)

    delete session_path
    get root_path
    expect(response).to redirect_to(new_session_path)
  end

  it "does not sign in with the wrong password" do
    sign_in_as(user, password: "not the password")
    get root_path
    expect(response).to redirect_to(new_session_path)
  end

  describe "session lifetime" do
    include ActiveSupport::Testing::TimeHelpers

    it "refuses a session cookie presented after SESSION_LIFETIME, even re-sent by hand" do
      sign_in_as(user)
      signed_cookie = cookies[:session_id]
      travel(Authentication::SESSION_LIFETIME - 1.minute) do
        cookies[:session_id] = signed_cookie # positive control: the re-send itself works
        get root_path
        expect(response).to have_http_status(:ok)
      end
      travel(Authentication::SESSION_LIFETIME + 1.minute) do
        # The browser would drop the expired cookie by itself; a stolen copy
        # need not. Re-send it, without an expiry, so the server has to decide.
        cookies[:session_id] = signed_cookie
        get root_path
        expect(response).to redirect_to(new_session_path)
      end
    end

    it "ends every session when the password changes" do
      sign_in_as(user)
      user.update!(password: "a different long password", password_confirmation: "a different long password")
      expect(user.sessions.count).to eq(0)
      get root_path
      expect(response).to redirect_to(new_session_path)
    end
  end

  describe "rate limiting" do
    def attempt(email, from:, forwarded_for: nil)
      headers = forwarded_for ? { "X-Forwarded-For" => forwarded_for } : {}
      post session_path, params: { email_address: email, password: "wrong wrong wrong" },
                         headers:, env: { "REMOTE_ADDR" => from }
      flash[:alert]
    end

    it "cannot be dodged from the LAN by inventing X-Forwarded-For addresses" do
      # 192.168.x is exactly where a self-hosted client sits, and Rails'
      # default trusted-proxy list would believe its forwarded header.
      alerts = (1..11).map { |i| attempt("x#{i}@example.com", from: "192.168.1.50", forwarded_for: "203.0.113.#{i}") }
      expect(alerts.last).to eq("Try again later.")
    end

    it "believes X-Forwarded-For from the in-image proxy (loopback), using its rightmost entry" do
      # Thruster appends the real peer; anything to its left came from the
      # client. Two clients behind the same loopback proxy must get separate
      # buckets — that is what proves the header is believed at all. (An earlier
      # version sent all attempts from one client, which passed even with the
      # header ignored: then everyone shares the proxy's bucket, the global
      # lockout this guards against.)
      exhausted = (1..11).map { |i| attempt("y#{i}@example.com", from: "127.0.0.1", forwarded_for: "203.0.113.#{i}, 192.168.1.50") }
      expect(exhausted.last).to eq("Try again later.")

      fresh = attempt("z@example.com", from: "127.0.0.1", forwarded_for: "192.168.1.51")
      expect(fresh).to eq("Try another email address or password.")
    end

    it "believes X-Forwarded-For from a proxy named in TRUSTED_PROXIES, and from no other peer" do
      proxies = Rails.application.config.action_dispatch.trusted_proxies
      original = proxies.dup
      proxies << IPAddr.new("10.0.0.2")

      (1..11).each { |i| attempt("p#{i}@example.com", from: "10.0.0.2", forwarded_for: "192.168.1.60") }
      expect(attempt("q@example.com", from: "10.0.0.2", forwarded_for: "192.168.1.61"))
        .to eq("Try another email address or password.")

      # 10.0.0.3 is not named: its header is ignored, so all of its clients share
      # one bucket and a spoofed entry cannot buy a fresh one.
      (1..10).each { |i| attempt("r#{i}@example.com", from: "10.0.0.3", forwarded_for: "203.0.113.#{i}") }
      expect(attempt("s@example.com", from: "10.0.0.3", forwarded_for: "203.0.113.99")).to eq("Try again later.")
    ensure
      proxies.replace(original)
    end

    it "limits guesses at one operator's password from many addresses" do
      alerts = (1..11).map { |i| attempt(user.email_address, from: "198.51.100.#{i}") }
      expect(alerts.first).to eq("Try another email address or password.")
      expect(alerts.last).to eq("Try again later.")
    end
  end
end
