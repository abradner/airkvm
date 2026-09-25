# The /metrics listener answers a failing collector with a bare 500, not a
# backtrace.
#
# yabeda-prometheus builds its rack app with Rack::ShowExceptions baked in
# (Exporter.rack_app), and yabeda-puma-plugin serves exactly that app. So any
# collector that raises — a stale control socket, a stats parse error, both of
# which this app has hit — returned a full debug page with source excerpts and
# absolute paths, on an unauthenticated port that the container binds on all
# interfaces. The scraper learns nothing from a backtrace that the logs do not
# already hold; everyone else who can reach the port learns the filesystem.
#
# Replaces rack_app rather than wrapping it, because the plugin calls it by
# name. Kept to the same middleware minus that one, so nothing else changes.
module YabedaExporterWithoutBacktraces
  def rack_app(exporter = self, logger: Logger.new(IO::NULL), use_deflater: true, **exporter_options)
    ::Rack::Builder.new do
      use ::Rack::Deflater if use_deflater
      use ::Rack::CommonLogger, logger
      use YabedaExporterWithoutBacktraces::BareErrors
      use exporter, **exporter_options
      run Yabeda::Prometheus::Exporter::NOT_FOUND_HANDLER
    end
  end

  class BareErrors
    def initialize(app) = @app = app

    def call(env)
      @app.call(env)
    rescue StandardError => e
      Rails.logger.error("[metrics] #{e.class}: #{e.message}")
      [ 500, { "content-type" => "text/plain" }, [ "metrics collection failed\n" ] ]
    end
  end
end

Yabeda::Prometheus::Exporter.singleton_class.prepend(YabedaExporterWithoutBacktraces)
