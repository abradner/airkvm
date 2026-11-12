# The coordinator runs as ONE Puma process. Loaded by config/puma.rb, which
# also pins `workers 0`, and by its spec.
#
# Two things depend on it, and both fail silently rather than loudly:
#
#   * /metrics. The exporter starts in whichever process runs Puma's plugins —
#     the master, in cluster mode — while requests are counted in each worker's
#     own in-process registry. With workers, rails_requests_total stays at zero
#     and the GC series describe a master that serves nothing, and every scrape
#     still succeeds.
#   * The relay (docs/design/fleet.md, issue #68): the self-hosted edition
#     forwards frames between a device socket and an operator socket in-process.
#     Two processes would need a pub/sub hop in front of every keystroke.
#
# Puma reads WEB_CONCURRENCY by itself, so an operator following generic Rails
# advice would get cluster mode without touching this repo. Pinning `workers 0`
# alone would quietly ignore what they asked for, so a set, non-zero value
# refuses to boot instead (AGENTS.md §8, fail fast).
module PumaSingleProcess
  class Error < StandardError; end

  module_function

  def check!(env = ENV)
    value = env["WEB_CONCURRENCY"].to_s.strip
    return if value.empty? || value == "0"

    raise Error, <<~MESSAGE
      WEB_CONCURRENCY=#{value.inspect}, but the coordinator runs as a single Puma
      process. Unset it (or set it to 0) and scale with RAILS_MAX_THREADS.

      Workers would make /metrics report a master process that serves no
      requests, and the device relay forwards between sockets in-process
      (config/puma_single_process.rb).
    MESSAGE
  end
end
