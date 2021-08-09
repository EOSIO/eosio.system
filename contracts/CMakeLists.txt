cmake_minimum_required( VERSION 3.5 )

project(contracts)

set(EOSIO_WASM_OLD_BEHAVIOR "Off")
find_package(eosio.cdt)

set(ICON_BASE_URL "http://127.0.0.1/ricardian_assets/eosio.contracts/icons")

set(ACCOUNT_ICON_URI  "account.png#3d55a2fc3a5c20b456f5657faf666bc25ffd06f4836c5e8256f741149b0b294f")
set(ADMIN_ICON_URI    "admin.png#9bf1cec664863bd6aaac0f814b235f8799fb02c850e9aa5da34e8a004bd6518e")
set(RESOURCE_ICON_URI "resource.png#3830f1ce8cb07f7757dbcf383b1ec1b11914ac34a1f9d8b065f07600fa9dac19")
set(REX_ICON_URI      "rex.png#d229837fa62a464b9c71e06060aa86179adf0b3f4e3b8c4f9702f4f4b0c340a8")
set(VOTING_ICON_URI   "voting.png#db28cd3db6e62d4509af3644ce7d377329482a14bb4bfaca2aa5f1400d8e8a84")

add_subdirectory(eosio.system)
add_subdirectory(eosio.distribute)