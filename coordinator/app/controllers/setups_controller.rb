# Self-hosted first boot: create the Account and its first operator, then close.
# Everything interesting is in FirstBoot; this only maps it onto HTTP.
class SetupsController < ApplicationController
  allow_unauthenticated_access
  skip_before_action :send_to_first_boot_setup
  before_action :require_first_boot

  def new
  end

  def create
    # Named, not splatted: a missing field must be a validation error (422),
    # not an ArgumentError (500). A missing confirmation becomes "" so it fails
    # the match rather than skipping it, which is what nil would do.
    p = setup_params
    user = FirstBoot.complete!(account_name: p[:account_name], email_address: p[:email_address],
                               password: p[:password], password_confirmation: p[:password_confirmation].to_s)
    start_new_session_for(user)
    redirect_to root_path, notice: "Setup complete."
  rescue FirstBoot::Closed
    # Someone completed setup between this request's check and its lock.
    head :not_found
  rescue ActiveRecord::RecordInvalid => e
    @errors = e.record.errors.full_messages
    render :new, status: :unprocessable_content
  end

  private

  # 404 rather than a redirect or a 403: once setup has happened — or on the
  # cloud edition, where it never happens — this page does not exist, and
  # nothing about the response should say it once did.
  def require_first_boot
    head :not_found unless FirstBoot.pending?
  end

  def setup_params
    params.expect(setup: %i[account_name email_address password password_confirmation])
  end
end
