# An operator: someone who signs in to the coordinator and drives devices.
#
# Belongs to exactly one Account, which is what makes `current_account`
# unambiguous — a login identity maps to one tenant. Email addresses are unique
# across the whole service, not per Account, for the same reason: the address
# alone must identify which tenant is signing in. (Multi-account membership is
# a later decision, and would replace this belongs_to rather than bolt onto it.)
#
# No password reset flow yet. Rails' generator includes one that sends email,
# but nothing here has mail configured and many self-hosted installs have no
# SMTP at all — a "forgot password?" link that emails into the void is a claim
# the mechanism does not deliver. Reset from a console until that is designed.
class User < ApplicationRecord
  belongs_to :account
  has_secure_password
  has_many :sessions, dependent: :destroy

  normalizes :email_address, with: ->(e) { e.strip.downcase }

  validates :email_address, presence: true, uniqueness: true,
                            format: { with: URI::MailTo::EMAIL_REGEXP }
  # has_secure_password only enforces presence on create; this is the floor.
  validates :password, length: { minimum: 12 }, allow_nil: true

  # A new password ends every session, including ones on stolen cookies —
  # otherwise the console reset in the README would look like recovery and
  # leave an attacker signed in.
  after_update :end_all_sessions, if: :saved_change_to_password_digest?

  private

  def end_all_sessions
    sessions.destroy_all
  end
end
