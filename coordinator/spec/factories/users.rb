FactoryBot.define do
  factory :account do
    sequence(:name) { |n| "Account #{n}" }
  end

  factory :user do
    account
    sequence(:email_address) { |n| "operator#{n}@example.com" }
    password { "correct horse battery" }
  end
end
