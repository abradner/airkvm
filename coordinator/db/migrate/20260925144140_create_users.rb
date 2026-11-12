class CreateUsers < ActiveRecord::Migration[8.1]
  def change
    create_table :users, id: :uuid, default: -> { "uuidv7()" } do |t|
      # The tenant this operator belongs to. One Account per User: a login
      # identity maps to exactly one tenant, which is what makes
      # current_account unambiguous.
      t.references :account, null: false, foreign_key: true, type: :uuid
      t.string :email_address, null: false
      t.string :password_digest, null: false

      t.timestamps
    end
    add_index :users, :email_address, unique: true
  end
end
