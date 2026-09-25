require "rails_helper"

# FirstBoot claims to be race-safe: two first-boot requests arriving together
# must not both create an Account. A timing-based test of that would be flaky,
# so this one is deterministic. The test holds the advisory lock itself, proves
# a worker calling complete! BLOCKS on it rather than racing ahead, completes a
# first boot underneath it, and then proves the worker re-checks after getting
# the lock and refuses.
#
# Not transactional: Rails' transactional tests make every thread share one
# connection, which would hand the worker the test's own transaction and make
# the whole scenario meaningless. Cleans up after itself instead.
RSpec.describe FirstBoot, "under concurrency" do
  self.use_transactional_tests = false

  after do
    Session.delete_all
    User.delete_all
    Account.delete_all
  end

  let(:details) do
    { account_name: "Homelab", email_address: "me@example.com",
      password: "correct horse battery", password_confirmation: "correct horse battery" }
  end

  it "waits for the lock, then re-checks, so a concurrent first boot cannot slip through" do
    pool = ActiveRecord::Base.connection_pool
    holder = pool.checkout
    holder.execute("BEGIN")
    holder.execute("SELECT pg_advisory_xact_lock(#{FirstBoot::LOCK_KEY})")

    outcome = nil
    worker = Thread.new do
      pool.with_connection do
        outcome = begin
          described_class.complete!(**details)
          :created
        rescue FirstBoot::Closed
          :closed
        end
      end
    end

    begin
      # Wait until Postgres itself reports the worker blocked on the advisory
      # lock. Asserting only that the thread is still alive after a pause (an
      # earlier version) could pass on a slow runner with the lock removed.
      waiting = 0
      50.times do
        waiting = holder.select_value(
          "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"
        ).to_i
        break if waiting.positive? || !worker.alive?

        sleep 0.1
      end
      expect(waiting).to eq(1), "complete! did not wait for the lock — it raced ahead"

      # Another first boot finishes while the worker is waiting on the lock.
      holder.execute("INSERT INTO accounts (id, name, created_at, updated_at) VALUES (uuidv7(), 'first', now(), now())")
      holder.execute("COMMIT") # releases the transaction-scoped lock
      committed = true
    ensure
      # A failed expectation above must not leak an open transaction and a held
      # lock into every later example (this file is not transactional).
      holder.execute("ROLLBACK") unless committed
      pool.checkin(holder)
    end

    expect(worker.join(10)).to be_truthy
    expect(outcome).to eq(:closed)
    expect(Account.count).to eq(1)
  end
end
