module SignInHelpers
  # Signs in through the real login form rather than by planting a session, so
  # a request spec that passes has exercised the same path a browser takes.
  def sign_in_as(user, password: "correct horse battery")
    post session_path, params: { email_address: user.email_address, password: }
  end
end

RSpec.configure do |config|
  config.include SignInHelpers, type: :request
  config.include FactoryBot::Syntax::Methods
end
