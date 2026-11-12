class ApplicationController < ActionController::Base
  include Authentication
  # Only allow modern browsers supporting webp images, web push, badges, import maps, CSS nesting, and CSS :has.
  allow_browser versions: :modern

  # Changes to the importmap will invalidate the etag for HTML responses
  stale_when_importmap_changes

  # Prepended so it runs BEFORE require_authentication: on a fresh self-hosted
  # install there are no users, so sending the visitor to a login page would be
  # a dead end nobody could pass.
  prepend_before_action :send_to_first_boot_setup

  helper_method :current_account

  private

  # Every lookup in a signed-in request goes through this
  # (coordinator/AGENTS.md §4): current_account.users.find(id), never User.find(id).
  def current_account = Current.account

  def send_to_first_boot_setup
    redirect_to new_setup_path if FirstBoot.pending?
  end
end
