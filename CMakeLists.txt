cmake_minimum_required(VERSION 3.5)

project(eosio_system)

set(VERSION_MAJOR 2)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX rc1)

if (VERSION_SUFFIX)
   set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
   set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()

include(ExternalProject)

find_package(eosio.cdt)

find_package(Git REQUIRED)

set(EOSIO_TOKEN_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/eosio.token")

# pull down seperate eosio.token contract
ExternalProject_Add(
   eosio.token
   PREFIX ${EOSIO_TOKEN_PREFIX}
   GIT_REPOSITORY https://github.com/EOSIO/eosio.token.git
   TIMEOUT 10
   UPDATE_COMMAND ${GIT_EXECUTABLE} checkout
   GIT_TAG "refs/tags/v2.0.0-rc1"
   # CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${EOSIO_TOKEN_PREFIX}
   INSTALL_COMMAND ""
   LOG_DOWNLOAD ON
   BUILD_BYPRODUCTS ${EOSIO_TOKEN_PREFIX}/src/eosio.token-build/contracts/eosio.token/eosio.token.wasm
)

message(STATUS "Building eosio.system v${VERSION_FULL}")

set(EOSIO_CDT_VERSION_MIN "1.8")
set(EOSIO_CDT_VERSION_SOFT_MAX "1.8")
#set(EOSIO_CDT_VERSION_HARD_MAX "")

### Check the version of eosio.cdt
set(VERSION_MATCH_ERROR_MSG "")
EOSIO_CHECK_VERSION(VERSION_OUTPUT "${EOSIO_CDT_VERSION}"
                                   "${EOSIO_CDT_VERSION_MIN}"
                                   "${EOSIO_CDT_VERSION_SOFT_MAX}"
                                   "${EOSIO_CDT_VERSION_HARD_MAX}"
                                   VERSION_MATCH_ERROR_MSG)
if(VERSION_OUTPUT STREQUAL "MATCH")
   message(STATUS "Using eosio.cdt version ${EOSIO_CDT_VERSION}")
elseif(VERSION_OUTPUT STREQUAL "WARN")
   message(WARNING "Using eosio.cdt version ${EOSIO_CDT_VERSION} even though it exceeds the maximum supported version of ${EOSIO_CDT_VERSION_SOFT_MAX}; continuing with configuration, however build may fail.\nIt is recommended to use eosio.cdt version ${EOSIO_CDT_VERSION_SOFT_MAX}.x")
else() # INVALID OR MISMATCH
   message(FATAL_ERROR "Found eosio.cdt version ${EOSIO_CDT_VERSION} but it does not satisfy version requirements: ${VERSION_MATCH_ERROR_MSG}\nPlease use eosio.cdt version ${EOSIO_CDT_VERSION_SOFT_MAX}.x")
endif(VERSION_OUTPUT STREQUAL "MATCH")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(TEST_BUILD_TYPE "Debug")
   set(CMAKE_BUILD_TYPE "Release")
else()
   set(TEST_BUILD_TYPE ${CMAKE_BUILD_TYPE})
endif()

option(SETKVPARAMS "enables setkvparams" OFF)

if(SETKVPARAMS)
   set(SETKVPARAMS_OPT "-DSETKVPARAMS=1")
endif()

ExternalProject_Add(
   contracts_project
   SOURCE_DIR ${CMAKE_SOURCE_DIR}/contracts
   BINARY_DIR ${CMAKE_BINARY_DIR}/contracts
   CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${EOSIO_CDT_ROOT}/lib/cmake/eosio.cdt/EosioWasmToolchain.cmake ${SETKVPARAMS_OPT}
   UPDATE_COMMAND ""
   PATCH_COMMAND ""
   TEST_COMMAND ""
   INSTALL_COMMAND ""
   BUILD_ALWAYS 1
   DEPENDS eosio.token
)

if (APPLE)
   set(OPENSSL_ROOT "/usr/local/opt/openssl")
elseif (UNIX)
   set(OPENSSL_ROOT "/usr/include/openssl")
endif()
set(SECP256K1_ROOT "/usr/local")

if (APPLE)
   set(OPENSSL_ROOT "/usr/local/opt/openssl")
elseif (UNIX)
   set(OPENSSL_ROOT "/usr/include/openssl")
endif()
set(SECP256K1_ROOT "/usr/local")

string(REPLACE ";" "|" TEST_PREFIX_PATH "${CMAKE_PREFIX_PATH}")
string(REPLACE ";" "|" TEST_FRAMEWORK_PATH "${CMAKE_FRAMEWORK_PATH}")
string(REPLACE ";" "|" TEST_MODULE_PATH "${CMAKE_MODULE_PATH}")

set(BUILD_TESTS FALSE CACHE BOOL "Build unit tests")

if(BUILD_TESTS)
   message(STATUS "Building unit tests.")
   ExternalProject_Add(
     contracts_unit_tests
     LIST_SEPARATOR | # Use the alternate list separator
     CMAKE_ARGS -DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE} -DCMAKE_PREFIX_PATH=${TEST_PREFIX_PATH} -DCMAKE_FRAMEWORK_PATH=${TEST_FRAMEWORK_PATH} -DCMAKE_MODULE_PATH=${TEST_MODULE_PATH} -DEOSIO_ROOT=${EOSIO_ROOT} -DLLVM_DIR=${LLVM_DIR} -DBOOST_ROOT=${BOOST_ROOT} -DBUILD_TESTS_PINNED=${BUILD_TESTS_PINNED}  -DEOSIO_DIR_PROMPT=${EOSIO_DIR_PROMPT} ${SETKVPARAMS_OPT}
     SOURCE_DIR ${CMAKE_SOURCE_DIR}/tests
     BINARY_DIR ${CMAKE_BINARY_DIR}/tests
     BUILD_ALWAYS 1
     TEST_COMMAND   ""
     INSTALL_COMMAND ""
   )
else()
   message(STATUS "Unit tests will not be built. To build unit tests, set BUILD_TESTS to true.")
endif()
