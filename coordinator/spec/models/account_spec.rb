require "rails_helper"

RSpec.describe Account do
  it "requires a name, in the model and in the database" do
    expect(described_class.new(name: "")).not_to be_valid
    # The NOT NULL constraint is the enforcement; the validation above is only
    # the friendly error (AGENTS.md §8, deletion test). Bypass the model to
    # prove the database refuses on its own.
    expect { described_class.new.save!(validate: false) }
      .to raise_error(ActiveRecord::NotNullViolation)
  end

  it "gets a UUIDv7 primary key from the database" do
    id = described_class.create!(name: "homelab").id
    # The version is the first hex digit of the third group.
    expect(id).to match(/\A\h{8}-\h{4}-7\h{3}-[89ab]\h{3}-\h{12}\z/)
  end

  it "issues ids that sort in creation order" do
    # The reason v7 was chosen over v4: time-ordered ids keep inserts at the
    # right-hand edge of the index. If the default ever silently became v4 this
    # would fail roughly always, rather than by bad luck.
    ids = 5.times.map { |i| described_class.create!(name: "a#{i}").id }
    expect(ids).to eq(ids.sort)
  end
end
