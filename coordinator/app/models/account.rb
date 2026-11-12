# The tenant. In the self-hosted edition there is normally exactly one; in the
# cloud edition there is one per customer. The data shape is identical in both —
# the edition may change who can create an Account, never what an Account is
# (coordinator/AGENTS.md §4).
#
# Every domain record belongs to an Account, and is only ever reached through
# one: `current_account.devices.find(id)`, never `Device.find(id)`. That rule is
# what will keep one tenant's devices unreachable from another's session —
# stated, not yet enforced: `current_account` arrives with operator
# authentication, together with the spec that proves it.
class Account < ApplicationRecord
  validates :name, presence: true
end
