require "rails_helper"

RSpec.describe FirstBoot do
  let(:details) do
    { account_name: "Homelab", email_address: "me@example.com",
      password: "correct horse battery", password_confirmation: "correct horse battery" }
  end

  def as_edition(name)
    allow(Airkvm).to receive(:edition).and_return(Airkvm::Edition.new(name))
  end

  describe ".pending?" do
    it "is true on a fresh self-hosted install" do
      expect(described_class).to be_pending
    end

    it "is false once any Account exists" do
      create(:account)
      expect(described_class).not_to be_pending
    end

    it "is never true in the cloud edition, even with no Account at all" do
      as_edition("cloud")
      expect(Account.count).to eq(0)
      expect(described_class).not_to be_pending
    end
  end

  describe ".complete!" do
    it "creates the Account and its first operator together" do
      user = described_class.complete!(**details)
      expect(user.account.name).to eq("Homelab")
      expect(Account.count).to eq(1)
    end

    it "works exactly once" do
      described_class.complete!(**details)
      expect { described_class.complete!(**details, email_address: "second@example.com") }
        .to raise_error(FirstBoot::Closed)
      expect(Account.count).to eq(1)
    end

    it "creates nothing if the operator is invalid" do
      # One transaction: a bad password must not leave an Account behind with no
      # way to sign in to it, which would also close setup for good.
      expect { described_class.complete!(**details, password: "short", password_confirmation: "short") }
        .to raise_error(ActiveRecord::RecordInvalid)
      expect(Account.count).to eq(0)
      expect(described_class).to be_pending
    end

    it "refuses in the cloud edition" do
      as_edition("cloud")
      expect { described_class.complete!(**details) }.to raise_error(FirstBoot::Closed)
    end
  end
end
