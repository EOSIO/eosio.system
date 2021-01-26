include(ExternalProject)

find_package(Git REQUIRED)

set(EOSIO_TOKEN_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/eosio.token")

# pull down seperate eosio.token contract
ExternalProject_Add(
   eosio.token
   PREFIX ${EOSIO_TOKEN_PREFIX}
   GIT_REPOSITORY git@github.com:EOSIO/eosio.token.git
   TIMEOUT 10
   UPDATE_COMMAND ${GIT_EXECUTABLE} pull
   GIT_TAG develop
   CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${EOSIO_TOKEN_PREFIX}
   LOG_DOWNLOAD ON
   INSTALL_COMMAND ""
)

add_contract(eosio.system eosio.system
   ${CMAKE_CURRENT_SOURCE_DIR}/src/eosio.system.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/delegate_bandwidth.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/exchange_state.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/name_bidding.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/native.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/producer_pay.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/powerup.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/rex.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/staking_pool.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/voting.cpp
)

target_include_directories(eosio.system
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
   ${EOSIO_TOKEN_PREFIX}/src/eosio.token/contracts/eosio.token/include
   ${CMAKE_CURRENT_SOURCE_DIR}/../eosio.token/include)

set_target_properties(eosio.system
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_contract(rex.results rex.results ${CMAKE_CURRENT_SOURCE_DIR}/src/rex.results.cpp)
add_contract(powup.results powup.results ${CMAKE_CURRENT_SOURCE_DIR}/src/powerup.results.cpp)

target_include_directories(rex.results
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(powup.results
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(rex.results
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.rex")

set_target_properties(powup.results
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.powerup")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ricardian/eosio.system.contracts.md.in ${CMAKE_CURRENT_BINARY_DIR}/ricardian/eosio.system.contracts.md @ONLY )

target_compile_options( eosio.system PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )

add_dependencies(eosio.system eosio.token)