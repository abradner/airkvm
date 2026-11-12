class Current < ActiveSupport::CurrentAttributes
  attribute :session
  delegate :user, to: :session, allow_nil: true

  # The tenant every lookup in a signed-in request goes through
  # (coordinator/AGENTS.md §4). Derived from the user rather than stored, so
  # there is one source for it and it cannot disagree with the session.
  def account = user&.account
end
