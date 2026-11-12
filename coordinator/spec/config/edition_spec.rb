require "rails_helper"

# Tests Airkvm::Edition.resolve directly rather than by rebooting Rails in each
# environment: resolve is the whole decision, and it takes rails_env as an
# argument precisely so every branch is reachable from one test process.
# The one thing this cannot show — that the initializer actually calls resolve
# at boot — is covered by the boot check recorded in the commit message.
RSpec.describe Airkvm::Edition do
  describe ".resolve" do
    it "accepts each named edition in every environment" do
      %w[development test production staging].each do |env|
        expect(described_class.resolve("self_hosted", rails_env: env)).to be_self_hosted
        expect(described_class.resolve("cloud", rails_env: env)).to be_cloud
      end
    end

    it "refuses to start production without the variable" do
      expect { described_class.resolve(nil, rails_env: "production") }
        .to raise_error(Airkvm::Edition::Error, /AIRKVM_EDITION is not set/)
    end

    it "fails closed in environments it has never heard of" do
      # The allowlist is development and test, not "anything but production" —
      # a staging box that forgot the variable must not quietly pick one.
      %w[staging preview review].each do |env|
        expect { described_class.resolve("", rails_env: env) }
          .to raise_error(Airkvm::Edition::Error, /not set/)
      end
    end

    it "defaults to self_hosted only in development and test" do
      %w[development test].each do |env|
        expect(described_class.resolve(nil, rails_env: env).name).to eq("self_hosted")
      end
    end

    it "treats whitespace as unset rather than as a name" do
      expect { described_class.resolve("   ", rails_env: "production") }
        .to raise_error(Airkvm::Edition::Error, /not set/)
    end

    it "rejects anything that is not an edition, in every environment" do
      %w[development production].each do |env|
        expect { described_class.resolve("Cloud", rails_env: env) }
          .to raise_error(Airkvm::Edition::Error, /not an edition/)
        expect { described_class.resolve("selfhosted", rails_env: env) }
          .to raise_error(Airkvm::Edition::Error, /not an edition/)
      end
    end
  end

  it "refuses to construct a non-edition directly, not just through resolve" do
    expect { described_class.new("Cloud") }
      .to raise_error(Airkvm::Edition::Error, /not an edition/)
    expect(described_class.new("cloud")).to be_cloud
  end

  it "names exactly two editions" do
    # Adding one is a product decision with policy consequences everywhere the
    # seam is consulted, so it should arrive as a deliberate change to this
    # line rather than as a new string somewhere.
    expect(Airkvm::Edition::NAMES).to contain_exactly("self_hosted", "cloud")
  end

  it "resolved an edition at boot" do
    expect(Airkvm.edition).to be_a(described_class)
    expect(Airkvm.edition).to be_self_hosted # the test default
  end
end
