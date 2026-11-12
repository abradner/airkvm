require "rails_helper"

# Drives the same method the Puma plugin calls (Exporter.rack_app), so this is
# the app the metrics port actually serves.
RSpec.describe "the /metrics listener" do
  let(:app) { Yabeda::Prometheus::Exporter.rack_app(Yabeda::Prometheus::Exporter, path: "/metrics", use_deflater: false) }

  it "serves metrics" do
    response = Rack::MockRequest.new(app).get("/metrics")
    expect(response.status).to eq(200)
    expect(response.body).to include("# TYPE")
  end

  it "answers a failing collector with a bare 500, not a backtrace" do
    allow(Yabeda).to receive(:collect!).and_raise(Errno::ENOENT, "/private/path/pumactl.sock")

    response = Rack::MockRequest.new(app).get("/metrics")

    expect(response.status).to eq(500)
    expect(response.body).to eq("metrics collection failed\n")
    expect(response.body).not_to include("pumactl.sock")
  end
end
