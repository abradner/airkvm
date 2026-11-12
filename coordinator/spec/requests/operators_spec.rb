require "rails_helper"

# The tenancy rule from coordinator/AGENTS.md §4, proven where it matters: at
# the HTTP boundary, through a real sign-in.
RSpec.describe "operators" do
  let(:mine) { create(:user) }
  let(:colleague) { create(:user, account: mine.account) }
  let(:stranger) { create(:user, account: create(:account, name: "Someone else")) }

  before { sign_in_as(mine) }

  it "shows an operator in my own account" do
    get operator_path(colleague)
    expect(response).to have_http_status(:ok)
  end

  it "answers another account's operator with a 404, exactly like an id that never existed" do
    # Compare what PRODUCTION sends. The test environment renders Rails' debug
    # page for a 404, which echoes the requested id, so two different ids would
    # differ by that id alone and prove nothing either way. With detailed
    # exceptions off, the response is public/404.html — the real reply.
    Rails.application.env_config["action_dispatch.show_detailed_exceptions"] = false

    get operator_path(stranger)
    expect(response).to have_http_status(:not_found)
    foreign = response.body

    get operator_path(SecureRandom.uuid_v7)
    expect(response).to have_http_status(:not_found)
    # Same body too, so nothing in the reply distinguishes "real but not yours".
    expect(response.body).to eq(foreign)
  ensure
    Rails.application.env_config.delete("action_dispatch.show_detailed_exceptions")
  end

  it "treats a malformed id as not found, not as a server error" do
    get operator_path("not-a-uuid")
    expect(response).to have_http_status(:not_found)
  end

  it "lists only my own account's operators" do
    colleague
    stranger
    get operators_path
    expect(response.body).to include(colleague.email_address)
    expect(response.body).not_to include(stranger.email_address)
  end
end
