require "rails_helper"

RSpec.describe User do
  it "must belong to an Account, in the model and in the database" do
    expect(build(:user, account: nil)).not_to be_valid
    # Clear the association, not just account_id: build(:user) caches an unsaved
    # Account on it, and save! would autosave that and refill the id — which is
    # how the first version of this example passed without the database ever
    # being asked.
    user = build(:user)
    user.account = nil
    expect { user.save!(validate: false) }.to raise_error(ActiveRecord::NotNullViolation)
  end

  it "normalizes the email address so the same person cannot register twice by case" do
    user = create(:user, email_address: "  Op@Example.COM ")
    expect(user.email_address).to eq("op@example.com")
  end

  it "keeps email addresses unique across ALL accounts, not per account" do
    # The address alone must identify which tenant is signing in.
    create(:user, email_address: "shared@example.com")
    other = build(:user, email_address: "shared@example.com", account: create(:account))
    expect(other).not_to be_valid
    expect { other.save!(validate: false) }.to raise_error(ActiveRecord::RecordNotUnique)
  end

  it "refuses a password shorter than 12 characters" do
    expect(build(:user, password: "short-pw")).not_to be_valid
    expect(build(:user, password: "twelve chars")).to be_valid
  end
end
