require "rails_helper"

# The one enforcement point for the key rules in coordinator/AGENTS.md §4:
# every primary key is a uuid defaulting to uuidv7(), and every *_id column is a
# uuid — in the PRIMARY database, the one this app's migrations own. The Solid
# Cache and Solid Queue schemas are framework-owned bigint tables and out of
# scope by decision (AGENTS.md §4 says why); in test they are not even loaded.
#
# The generator config in config/application.rb and the migration template in
# lib/templates/ make generated migrations right by default, but nothing stops a
# hand-written `create_table :things do |t|` from getting a bigint id. This spec
# reads the real schema, so it catches that regardless of how the table was made.
#
# The foreign-key half is not tidiness. A hand-written `t.references :account`
# is bigint, and Postgres only refuses a uuid there if the reference also has
# `foreign_key: true`. Without it, Active Model casts the uuid String with
# `to_i`, which keeps the leading decimal digits and stops at the first hex
# letter — "01998a3c-…" becomes 1998 — and v7 ids created close together share
# those digits. `current_account.devices` would then match other tenants' rows.
RSpec.describe "primary and foreign keys" do
  # Rails' own bookkeeping, keyed by version strings rather than ids.
  RAILS_INTERNAL = %w[schema_migrations ar_internal_metadata].freeze

  let(:connection) { ActiveRecord::Base.connection }
  let(:tables) { connection.tables - RAILS_INTERNAL }

  it "covers at least one table, so it cannot pass by checking nothing" do
    expect(tables).to include("accounts")
  end

  it "is a UUIDv7 on every application table" do
    offenders = tables.filter_map do |table|
      pk = connection.primary_key(table)
      column = connection.columns(table).find { |c| c.name == pk }
      default = column&.default_function.to_s

      if column.nil? then "#{table}: no primary key"
      elsif column.sql_type != "uuid" then "#{table}: #{pk} is #{column.sql_type}, not uuid"
      # Exactly the native function: pg_uuidv7's uuid_generate_v7() or any shim
      # would also contain "uuidv7".
      elsif default != "uuidv7()" then "#{table}: #{pk} defaults to #{default.inspect}, not uuidv7()"
      end
    end

    expect(offenders).to be_empty, "Tables without a UUIDv7 primary key:\n  #{offenders.join("\n  ")}"
  end

  it "types every *_id column, and every foreign key column, as uuid" do
    offenders = tables.flat_map do |table|
      fk_columns = connection.foreign_keys(table).map(&:column)
      connection.columns(table).filter_map do |column|
        next unless column.name.end_with?("_id") || fk_columns.include?(column.name)
        "#{table}.#{column.name} is #{column.sql_type}, not uuid" unless column.sql_type == "uuid"
      end
    end

    expect(offenders).to be_empty, "Reference columns that are not uuid:\n  #{offenders.join("\n  ")}"
  end
end
