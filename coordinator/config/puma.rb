# This configuration file will be evaluated by Puma. The top-level methods that
# are invoked here are part of Puma's configuration DSL. For more information
# about methods provided by the DSL, see https://puma.io/puma/Puma/DSL.html.
#
# ONE process, many threads: no workers. WEB_CONCURRENCY — which Puma reads by
# itself — is refused rather than honoured, for reasons in
# config/puma_single_process.rb. Scale with RAILS_MAX_THREADS.
#
# The ideal number of threads per worker depends both on how much time the
# application spends waiting for IO operations and on how much you wish to
# prioritize throughput over latency.
#
# As a rule of thumb, increasing the number of threads will increase how much
# traffic a given process can handle (throughput), but due to CRuby's
# Global VM Lock (GVL) it has diminishing returns and will degrade the
# response time (latency) of the application.
#
# The default is set to 3 threads as it's deemed a decent compromise between
# throughput and latency for the average Rails application.
#
# Any libraries that use a connection pool or another resource pool should
# be configured to provide at least as many connections as the number of
# threads. This includes Active Record's `pool` parameter in `database.yml`.
require_relative "puma_single_process"
PumaSingleProcess.check!
workers 0

threads_count = ENV.fetch("RAILS_MAX_THREADS", 3)
threads threads_count, threads_count

# Specifies the `port` that Puma will listen on to receive requests; default is 3000.
# Not where it listens: `rails server` passes its own host (BINDING, else
# localhost in development and 0.0.0.0 otherwise) and Puma's rack handler then
# clears any bind set here. The image sets BINDING — see the Dockerfile.
port ENV.fetch("PORT", 3000)

# Allow puma to be restarted by `bin/rails restart` command.
plugin :tmp_restart

# Prometheus metrics, on a SEPARATE port from the app — 9394 by default,
# PROMETHEUS_EXPORTER_PORT to override. `yabeda_prometheus` starts that listener
# inside this process; `yabeda` reads Puma's pool stats through the control app.
# Only `puma` starts them: `rails runner`, rspec and the console never load this
# file, so none of them bind 9394.
#
# The metrics port is unauthenticated, so it binds LOOPBACK by default. The
# exporter's own default is 0.0.0.0, which on a dev machine publishes it to the
# LAN; request counts per controller#action are low-sensitivity, but "which
# endpoints exist and how busy they are" is still reconnaissance. The container
# opts in with PROMETHEUS_EXPORTER_BIND=0.0.0.0 in the Dockerfile so a scraper
# can reach it — which makes it reachable from everything that can route to the
# container: every container on the same Docker network, every pod in the
# cluster unless a NetworkPolicy says otherwise. Restrict it there, and never
# route it through an ingress. A failing collector returns a bare 500, never a
# backtrace (config/initializers/yabeda_exporter.rb).
#
# The control app is a unix socket — no TCP port — in a private directory
# checked at boot; config/puma_control_socket.rb says why each check exists.
# It uses Puma's generated token rather than `no_token: true`, so reaching the
# socket is not enough on its own; yabeda-puma-plugin reads the same token from
# the launcher options, so the gauges keep working.
require_relative "puma_control_socket"
activate_control_app "unix://#{PumaControlSocket.prepare!(File.expand_path("..", __dir__))}"
plugin :yabeda
plugin :yabeda_prometheus
prometheus_exporter_url "tcp://127.0.0.1:9394/metrics"

# Run the Solid Queue supervisor inside of Puma for single-server deployments.
plugin :solid_queue if ENV["SOLID_QUEUE_IN_PUMA"]

# Specify the PID file. Defaults to tmp/pids/server.pid in development.
# In other environments, only set the PID file if requested.
pidfile ENV["PIDFILE"] if ENV["PIDFILE"]
