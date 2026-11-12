class CreateAccounts < ActiveRecord::Migration[8.1]
  def change
    create_table :accounts, id: :uuid, default: -> { "uuidv7()" } do |t|
      # The tenant. Every domain table belongs_to :account; see coordinator/AGENTS.md §4.
      t.string :name, null: false

      t.timestamps
    end
  end
end
