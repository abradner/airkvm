require "rails_helper"
require Rails.root.join("config/puma_single_process")

RSpec.describe PumaSingleProcess do
  it "allows WEB_CONCURRENCY unset, empty or 0" do
    [ {}, { "WEB_CONCURRENCY" => "" }, { "WEB_CONCURRENCY" => "0" } ].each do |env|
      expect { described_class.check!(env) }.not_to raise_error
    end
  end

  it "refuses any worker count, including 1 and auto" do
    # 1 is not single-process: Puma still forks a worker under a master, which
    # is the split that breaks the metrics.
    %w[1 2 auto].each do |value|
      expect { described_class.check!("WEB_CONCURRENCY" => value) }
        .to raise_error(PumaSingleProcess::Error, /single Puma/)
    end
  end
end
