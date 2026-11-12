# Which peers may speak for a client through X-Forwarded-For, and the warning
# for a deploy that has not named its proxy.
#
# Lives in config/ and is required from application.rb, not autoloaded from
# lib/: the list is read while the app is being configured and the middleware
# is inserted into the stack at boot, and a reloadable constant captured then
# would point at a class the next reload replaced (the same reason as
# config/initializers/edition.rb).
require "ipaddr"
require "concurrent/map"

module TrustedProxies
  LOOPBACK = [ IPAddr.new("127.0.0.1"), IPAddr.new("::1") ].freeze

  module_function

  # Loopback (the in-image Thruster) plus every address or CIDR in
  # TRUSTED_PROXIES, comma-separated.
  def from_env(value)
    LOOPBACK + value.to_s.split(",").map(&:strip).reject(&:empty?).map { |cidr| IPAddr.new(cidr) }
  end

  # The one predicate both the enforcement (Authentication#client_address) and
  # the warning below ask, so the two cannot disagree about what "trusted"
  # means.
  def trusted?(peer, proxies = Rails.application.config.action_dispatch.trusted_proxies)
    proxies.any? { |proxy| proxy.include?(peer) }
  rescue IPAddr::InvalidAddressError
    false
  end

  # Logs a warning when a request carries X-Forwarded-For from a peer that is
  # not a trusted proxy.
  #
  # Fail-closed is the behaviour and stays it: client_address ignores that
  # header, so the request is attributed to the peer. That is safe, and it is
  # also invisible. A TLS proxy in front of the container that nobody listed in
  # TRUSTED_PROXIES makes every client look like the proxy, so they all share
  # ONE sign-in rate-limit bucket and anyone can lock every operator out — and
  # nothing says why. This says why.
  #
  # Politeness only, by the deletion test (root AGENTS.md §8): removing this
  # changes what an operator is told, never which address a request gets.
  #
  # Once per peer per process, so a misconfigured deploy logs one line per
  # proxy rather than one per request, and at most MAX_PEERS distinct peers, so
  # a client spraying the header from many addresses cannot grow the memo
  # without bound; reaching the cap logs one line saying so, so the silence
  # after it is visible. The line names the connecting peer — the address that
  # would need listing, normally the proxy's own infrastructure address — and
  # never the header's contents. That does add the peer's IP to the log, once
  # per peer: Rails' own request line logs remote_ip, which for this very case
  # is the header's CLAIMED address, not the peer.
  class UntrustedForwardingWarning
    MAX_PEERS = 32

    def initialize(app, logger: nil)
      @app = app
      @logger = logger
      @warned = Concurrent::Map.new
    end

    def call(env)
      warn_if_untrusted(env) if env.key?("HTTP_X_FORWARDED_FOR")
      @app.call(env)
    end

    private
      def warn_if_untrusted(env)
        peer = env["REMOTE_ADDR"].to_s
        return if peer.empty? || TrustedProxies.trusted?(peer)
        return if @warned.size > MAX_PEERS
        return unless @warned.put_if_absent(peer, true).nil?

        if @warned.size > MAX_PEERS
          logger.warn("X-Forwarded-For from untrusted peers: #{MAX_PEERS} distinct peers already " \
                      "logged; further untrusted peers will not be logged by this process.")
          return
        end

        logger.warn(
          "X-Forwarded-For arrived from #{peer}, which is not a trusted proxy, so it was " \
          "ignored and the request is attributed to #{peer}. If #{peer} is the proxy in " \
          "front of this service, add it to TRUSTED_PROXIES; until then every client " \
          "behind it shares one sign-in rate-limit bucket. Logged once per peer."
        )
      end

      def logger = @logger || Rails.logger
  end
end
