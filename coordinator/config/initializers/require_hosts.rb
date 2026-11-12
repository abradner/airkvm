# Which Host headers production answers (config.hosts), and the refusal to boot
# without them (root AGENTS.md §8: required configuration fails fast). Decided
# 2026-11-09, closing issue #88.
#
# Why required rather than optional-with-a-warning: with config.hosts empty,
# Rails answers ANY Host header, so during the first-boot window (R-012 in
# docs/repo-map.md) a DNS-rebinding page in an operator's browser could reach
# /setup on a fresh self-hosted install. Two things happened to block that —
# the TLS certificate would not match the rebound name, and the Secure session
# cookie — and neither was designed as protection. This is the one that is.
#
# COORDINATOR_HOSTS is comma-separated. Each entry is an exact host name
# ("kvm.example.net"), a leading-dot domain matching it and every subdomain
# (".example.net"), or an IPv4 address — the forms config.hosts takes as
# strings. An IPv6 address must be bracketed ("[::1]"): the Host header carries
# the brackets, and a bare "::1" is compared as an exact string and never
# matches.
#
# Every production process boots this, not only the web server: rails console,
# rake tasks and bin/jobs need the variable too, the same as SECRET_KEY_BASE.
#
# A refused Host is logged by Rails (HostAuthorization) as one error line per
# request, quoting the Host it was sent — so DNS-rebinding probes are visible
# in the log, at the cost of a line each.
#
# /up is excluded so a container or orchestrator health check, which dials an
# address rather than a name, still gets its 200. It reveals nothing.
#
# Defined unconditionally so spec/smoke/boot_spec.rb can clear it in the
# production child process it boots; only the enforcement is environment-gated.
# An initializer, not config/environments/production.rb, for the same reason
# the database URLs are (config/initializers/require_database_urls.rb), and it
# runs before the middleware stack is built, so HostAuthorization sees the list.
module CoordinatorHosts
  VARIABLE = "COORDINATOR_HOSTS"

  def self.parse(value)
    value.to_s.split(",").map(&:strip).reject(&:empty?)
  end
end

if Rails.env.production?
  hosts = CoordinatorHosts.parse(ENV[CoordinatorHosts::VARIABLE])

  if hosts.empty?
    raise <<~MESSAGE
      Missing required configuration: #{CoordinatorHosts::VARIABLE}.

      Set it to the host name(s) operators use to reach this service,
      comma-separated (for example "kvm.example.net"). Without it the
      coordinator would answer any Host header, which on a fresh self-hosted
      install lets a DNS-rebinding page reach the first-boot setup page.
    MESSAGE
  end

  Rails.application.config.hosts.replace(hosts)
  Rails.application.config.host_authorization = { exclude: ->(request) { request.path == "/up" } }
end
