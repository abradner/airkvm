# Which edition this process is: `self_hosted` (a homelab or a single operator's
# own infrastructure) or `cloud` (app.airkvm.io). One codebase serves both —
# issue #68 records the decision.
#
# The rule the seam exists to hold: **an edition may change policy — who may
# create an account, whether sign-up exists, limits — and never data shape.**
# Every table is tenant-scoped in both editions. Code that branches on
# `Airkvm.edition` to add or drop a column is the seam being misused.
#
# Required in production, and it fails fast (AGENTS.md §8). Defaulting it there
# would be worse than most missing config, because both editions boot and serve
# healthily — a cloud deploy that fell back to self_hosted would expose the
# first-boot setup page to the internet rather than refuse to start.
#
# Development and test default to self_hosted, the edition this repo is built
# for first. The allowlist is explicit — development and test only — rather
# than "anything but production", so a staging or preview environment that
# forgot the variable fails closed rather than quietly picking one.
#
# Defined here rather than in lib/ because config.autoload_lib makes lib/
# reloadable, and a reloadable constant referenced during initialization is the
# case Rails warns against: the value captured at boot would point at a class
# the next reload replaced.
module Airkvm
  class Edition
    NAMES = %w[self_hosted cloud].freeze
    LOCAL_ENVIRONMENTS = %w[development test].freeze
    LOCAL_DEFAULT = "self_hosted"

    class Error < StandardError; end

    attr_reader :name

    def self.resolve(value, rails_env:)
      value = value.to_s.strip

      if value.empty?
        unless LOCAL_ENVIRONMENTS.include?(rails_env.to_s)
          raise Error, <<~MESSAGE
            AIRKVM_EDITION is not set. It is required outside development and
            test: set it to one of #{NAMES.join(", ")}.

            There is no default here on purpose. Both editions boot and serve
            normally, so a wrong guess would not fail — a cloud deploy running
            as self_hosted would expose the first-boot setup page instead.
          MESSAGE
        end
        value = LOCAL_DEFAULT
      end

      new(value)
    end

    # Validates here as well as in resolve, because specs (and anything else)
    # construct editions directly. An unvalidated `Edition.new("Cloud")` would
    # answer false to both predicates, and `if cloud? … else <self-hosted
    # policy>` would hand it self-hosted policy — the wrong default the seam
    # exists to prevent.
    def initialize(name)
      unless NAMES.include?(name)
        raise Error, "AIRKVM_EDITION=#{name.inspect} is not an edition. " \
                     "Expected one of: #{NAMES.join(", ")}."
      end

      @name = name
      freeze
    end

    def self_hosted? = name == "self_hosted"
    def cloud? = name == "cloud"
    def to_s = name
  end

  class << self
    attr_reader :edition
  end

  @edition = Edition.resolve(ENV["AIRKVM_EDITION"], rails_env: Rails.env)
end
