class SessionsController < ApplicationController
  allow_unauthenticated_access only: %i[ new create ]
  # Two limits, because each alone is bypassable. Per client address — via
  # client_address, not the default remote_ip, which a client can set with its
  # own X-Forwarded-For (Authentication#client_address). Per email address, so
  # guessing one operator's password from many addresses is limited too.
  rate_limit to: 10, within: 3.minutes, only: :create, name: "by-address",
             by: -> { client_address },
             with: -> { redirect_to new_session_path, alert: "Try again later." }
  rate_limit to: 10, within: 3.minutes, only: :create, name: "by-email",
             by: -> { params[:email_address].to_s.strip.downcase },
             with: -> { redirect_to new_session_path, alert: "Try again later." }

  def new
  end

  def create
    if user = User.authenticate_by(params.permit(:email_address, :password))
      start_new_session_for user
      redirect_to after_authentication_url
    else
      redirect_to new_session_path, alert: "Try another email address or password."
    end
  end

  def destroy
    terminate_session
    redirect_to new_session_path, status: :see_other
  end
end
