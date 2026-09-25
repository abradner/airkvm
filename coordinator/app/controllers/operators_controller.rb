# The operators who can sign in to the current Account.
#
# Every lookup goes through current_account (coordinator/AGENTS.md §4). An id
# belonging to another Account raises RecordNotFound and becomes a 404 — the
# same response as an id that never existed, so the reply cannot be used to
# learn which ids are real in someone else's tenant.
class OperatorsController < ApplicationController
  def index
    @operators = current_account.users.order(:created_at)
  end

  def show
    @operator = current_account.users.find(params[:id])
  end
end
