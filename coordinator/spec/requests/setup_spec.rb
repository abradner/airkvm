require "rails_helper"

RSpec.describe "first-boot setup" do
  let(:form) do
    { setup: { account_name: "Homelab", email_address: "me@example.com",
               password: "correct horse battery", password_confirmation: "correct horse battery" } }
  end

  context "on a fresh self-hosted install" do
    it "sends every page to setup, not to a login nobody can pass" do
      get root_path
      expect(response).to redirect_to(new_setup_path)
    end

    it "creates the account, signs the operator in, and then the page is gone" do
      get new_setup_path
      expect(response).to have_http_status(:ok)

      post setup_path, params: form
      expect(response).to redirect_to(root_path)
      follow_redirect!
      expect(response).to have_http_status(:ok)
      expect(response.body).to include("Homelab")

      # The page no longer exists — for a signed-in operator and for a stranger.
      get new_setup_path
      expect(response).to have_http_status(:not_found)
      delete session_path
      get new_setup_path
      expect(response).to have_http_status(:not_found)
      post setup_path, params: form
      expect(response).to have_http_status(:not_found)
      expect(Account.count).to eq(1)
    end

    it "rejects bad input without creating anything, and stays open" do
      post setup_path, params: { setup: form[:setup].merge(password: "short", password_confirmation: "short") }
      expect(response).to have_http_status(:unprocessable_content)
      expect(Account.count).to eq(0)
      get new_setup_path
      expect(response).to have_http_status(:ok)
    end
  end

  context "with missing or forged input" do
    it "answers a missing field with 422, not a 500" do
      form[:setup].except(:password_confirmation).then do |partial|
        post setup_path, params: { setup: partial }
      end
      expect(response).to have_http_status(:unprocessable_content)
      post setup_path, params: { setup: form[:setup].except(:account_name) }
      expect(response).to have_http_status(:unprocessable_content)
      expect(Account.count).to eq(0)
    end

    it "refuses a POST without the form's CSRF token" do
      # The test environment turns forgery protection off; production has it
      # on by load_defaults. Turn it on here so this page's reliance on it is
      # checked rather than assumed.
      ActionController::Base.allow_forgery_protection = true
      post setup_path, params: form
      expect(response).to have_http_status(:unprocessable_content)
      expect(Account.count).to eq(0)
    ensure
      ActionController::Base.allow_forgery_protection = false
    end

    it "reopens if every Account is removed — setup is derived from state (R-012)" do
      post setup_path, params: form
      delete session_path
      Account.first.users.destroy_all
      Account.destroy_all
      get new_setup_path
      expect(response).to have_http_status(:ok)
    end
  end

  context "in the cloud edition" do
    before { allow(Airkvm).to receive(:edition).and_return(Airkvm::Edition.new("cloud")) }

    it "has no setup page at all, even with no Account" do
      get new_setup_path
      expect(response).to have_http_status(:not_found)
      post setup_path, params: form
      expect(response).to have_http_status(:not_found)
      expect(Account.count).to eq(0)
    end

    it "sends visitors to sign in rather than to setup" do
      get root_path
      expect(response).to redirect_to(new_session_path)
    end
  end
end
