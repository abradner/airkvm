# The self-hosted edition's first-boot setup: creating the first Account and its
# first operator.
#
# "Pending" is derived from state — no Account exists — not stored. So setup is
# closed for as long as any Account exists, and would REOPEN, to anyone who can
# reach the page, if the accounts table were ever emptied. Nothing in the app
# deletes an Account today (the users foreign key refuses it while operators
# exist); a console or a future deletion feature could. R-012 records this, and
# the constraint accepted with it (2026-11-09): any account-deletion feature
# must refuse to delete the last Account, or re-gate this page with a stored
# completion marker, in the same change.
#
# Self-hosted only. In the cloud edition accounts are created some other way
# (not built yet), and this page must not exist — an internet-facing "create the
# first admin" form is the exact failure the edition seam's fail-fast was
# written to prevent (config/initializers/edition.rb).
#
# Race-safe on purpose. Until it is used, this page is reachable by anyone who
# can reach the coordinator. Two requests arriving together must not both see
# "no Account yet" and both succeed, or a second party gets a tenant of its own.
# So the check happens again inside a transaction holding a Postgres advisory
# lock, and only the first request through creates anything.
module FirstBoot
  class Closed < StandardError; end

  # Arbitrary, fixed, and only ever used here.
  LOCK_KEY = 0x6169726b766d # "airkvm"

  module_function

  def pending?
    Airkvm.edition.self_hosted? && !Account.exists?
  end

  # Returns the new operator. Raises Closed if setup already happened (or this
  # is the cloud edition), and ActiveRecord::RecordInvalid for bad input.
  def complete!(account_name:, email_address:, password:, password_confirmation:)
    Account.transaction do
      Account.connection.execute("SELECT pg_advisory_xact_lock(#{LOCK_KEY})")
      # This re-check sees a first boot committed while we waited on the lock
      # only because Postgres' default READ COMMITTED gives each statement a
      # fresh snapshot. Under REPEATABLE READ the snapshot would be taken at
      # the lock SELECT, before the wait, and both requests would pass it.
      raise Closed unless pending?

      account = Account.create!(name: account_name)
      account.users.create!(email_address:, password:, password_confirmation:)
    end
  end
end
