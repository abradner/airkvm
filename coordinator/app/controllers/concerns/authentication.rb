module Authentication
  extend ActiveSupport::Concern

  # Absolute, from sign-in. The generator's default was a permanent (20-year)
  # cookie, so a leaked cookie was a login for good. Enforced on the server, not
  # by trusting the browser to drop it: Rails signs the expiry INTO the cookie
  # and refuses a copy presented after it, so a stolen cookie re-sent later
  # fails verification before any lookup (spec/requests/sessions_spec.rb). That
  # is the one enforcement; there is deliberately no second age check on the
  # sessions row. Seven days, accepted 2026-11-09: long enough not to nag on
  # a homelab, short enough that a stolen cookie dies on its own. Changing a
  # password ends every session regardless (User#end_all_sessions).
  SESSION_LIFETIME = 7.days

  included do
    before_action :require_authentication
    helper_method :authenticated?
  end

  class_methods do
    def allow_unauthenticated_access(**options)
      skip_before_action :require_authentication, **options
    end
  end

  private
    def authenticated?
      resume_session
    end

    def require_authentication
      resume_session || request_authentication
    end

    def resume_session
      Current.session ||= find_session_by_cookie
    end

    def find_session_by_cookie
      Session.find_by(id: cookies.signed[:session_id]) if cookies.signed[:session_id]
    end

    def request_authentication
      session[:return_to_after_authenticating] = request.url
      redirect_to new_session_path
    end

    def after_authentication_url
      session.delete(:return_to_after_authenticating) || root_url
    end

    # The client's address, for rate limiting and for the record on a session.
    #
    # Not request.remote_ip on its own: Rails takes X-Forwarded-For at face
    # value whoever sent it — trusted_proxies only strips known proxies out of
    # the chain, it never asks whether the connecting peer IS one — so a client
    # reaching Puma directly (a dev server, or a deployment that runs Puma
    # without the image's BINDING=127.0.0.1) could name any address it liked.
    # So the header is believed only when the peer is a trusted proxy
    # (TrustedProxies.trusted?, config/trusted_proxies.rb: loopback, i.e. the
    # in-image Thruster, plus TRUSTED_PROXIES), and then remote_ip's
    # rightmost-untrusted walk applies. From any other peer it is ignored —
    # fail closed — and the middleware there logs that it happened.
    def client_address
      peer = request.remote_addr
      TrustedProxies.trusted?(peer) ? request.remote_ip : peer
    end

    def start_new_session_for(user)
      # A fresh Rails session on every sign-in, keeping only where to go next,
      # so nothing a previous visitor left in it carries over.
      return_to = session[:return_to_after_authenticating]
      reset_session
      session[:return_to_after_authenticating] = return_to if return_to

      user.sessions.create!(user_agent: request.user_agent, ip_address: client_address).tap do |session|
        Current.session = session
        cookies.signed[:session_id] = {
          value: session.id, httponly: true, same_site: :lax, expires: SESSION_LIFETIME.from_now
        }
      end
    end

    def terminate_session
      Current.session.destroy
      cookies.delete(:session_id)
      reset_session
    end
end
