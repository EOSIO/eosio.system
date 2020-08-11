#include <Runtime/Runtime.h>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <fc/log/logger.hpp>
#include <iostream>
#include <sstream>

#include "eosio.system_tester.hpp"

using namespace eosio_system;
using std::nullopt;

auto a(const char* s) { return asset::from_string(s); }

constexpr auto alice = N(alice1111111);
constexpr auto bob   = N(bob111111111);

struct votepool_tester : eosio_system_tester {
   votepool_tester() : eosio_system_tester(setup_level::none) {
      create_accounts({ N(eosio.vpool), N(eosio.bvpay) });
      basic_setup();
      create_core_token();
      deploy_contract();
      activate_chain();
   }

   // 'bp11activate' votes for self, then unvotes
   void activate_chain() {
      create_account_with_resources(N(bp11activate), N(eosio));
      transfer(N(eosio), N(bp11activate), a("150000000.0000 TST"), N(eosio));
      BOOST_REQUIRE_EQUAL(success(), regproducer(N(bp11activate)));
      BOOST_REQUIRE_EQUAL(success(),
                          stake(N(bp11activate), N(bp11activate), a("75000000.0000 TST"), a("75000000.0000 TST")));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), { N(bp11activate) }));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), {}));
   }

   action_result cfgvpool(name authorizer, const std::optional<std::vector<uint32_t>>& durations = nullopt,
                          const std::optional<std::vector<uint32_t>>& claim_periods = nullopt,
                          const std::optional<double>&                prod_rate     = nullopt,
                          const std::optional<double>&                voter_rate    = nullopt) {
      mvo v;
      if (durations)
         v("durations", *durations);
      else
         v("durations", nullptr);
      if (claim_periods)
         v("claim_periods", *claim_periods);
      else
         v("claim_periods", nullptr);
      if (prod_rate)
         v("prod_rate", *prod_rate);
      else
         v("prod_rate", nullptr);
      if (voter_rate)
         v("voter_rate", *voter_rate);
      else
         v("voter_rate", nullptr);
      return push_action(authorizer, N(cfgvpool), v);
   }

   action_result stake2pool(name authorizer, name owner, uint32_t pool_index, asset amount) {
      return push_action(authorizer, N(stake2pool), mvo()("owner", owner)("pool_index", pool_index)("amount", amount));
   }

   action_result claimstake(name authorizer, name owner, uint32_t pool_index, asset requested) {
      return push_action(authorizer, N(claimstake),
                         mvo()("owner", owner)("pool_index", pool_index)("requested", requested));
   }

   action_result transferstake(name authorizer, name from, name to, uint32_t pool_index, asset requested,
                               const std::string& memo) {
      return push_action(authorizer, N(transferstake),
                         mvo()("from", from)("to", to)("pool_index", pool_index)("requested", requested)("memo", memo));
   }

   action_result updatevotes(name authorizer, name user, name producer) {
      return push_action(authorizer, N(updatevotes), mvo()("user", user)("producer", producer));
   }

   action_result updatepay(name authorizer, name user) {
      return push_action(authorizer, N(updatepay), mvo()("user", user));
   }

   action_result claimvotepay(name authorizer, name producer) {
      return push_action(authorizer, N(claimvotepay), mvo()("producer", producer));
   }
};

BOOST_AUTO_TEST_SUITE(eosio_system_votepool_tests)

BOOST_AUTO_TEST_CASE(cfgvpool) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice }, N(eosio));

   BOOST_REQUIRE_EQUAL("missing authority of eosio", t.cfgvpool(alice, { { 1, 2, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is required on first use of cfgvpool"),
                       t.cfgvpool(N(eosio), nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods is required on first use of cfgvpool"),
                       t.cfgvpool(N(eosio), { { 1 } }, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is empty"),
                       t.cfgvpool(N(eosio), std::vector<uint32_t>{}, std::vector<uint32_t>{}));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(N(eosio), { { 1 } }, std::vector<uint32_t>{}));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(N(eosio), { { 1, 2 } }, { { 1, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("duration must be positive"), t.cfgvpool(N(eosio), { { 0 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be positive"), t.cfgvpool(N(eosio), { { 1 } }, { { 0 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(N(eosio), { { 1 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(N(eosio), { { 10, 20 } }, { { 9, 20 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(N(eosio), { { 2, 3, 4, 3 } }, { { 1, 1, 1, 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(N(eosio), { { 2, 3, 4, 4 } }, { { 1, 1, 1, 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods must be non-decreasing"),
                       t.cfgvpool(N(eosio), { { 3, 4, 5, 6 } }, { { 2, 2, 2, 1 } }));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(N(eosio), { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations can't change"), t.cfgvpool(N(eosio), { { 1, 2, 3 } }, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods can't change"), t.cfgvpool(N(eosio), nullopt, { { 1, 2, 3 } }));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(N(eosio), nullopt, nullopt, 0, .999));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(N(eosio), nullopt, nullopt, .999, 0));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(N(eosio), nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(N(eosio), nullopt, nullopt, 1));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"),
                       t.cfgvpool(N(eosio), nullopt, nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"),
                       t.cfgvpool(N(eosio), nullopt, nullopt, nullopt, 1));
} // cfgvpool
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(stake2pool_checks) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice }, N(eosio));

   BOOST_REQUIRE_EQUAL("missing authority of bob", t.stake2pool(alice, N(bob), 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(N(eosio), { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.stake2pool(alice, alice, 4, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount doesn't match core symbol"),
                       t.stake2pool(alice, alice, 3, a("1.0000 FOO")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount doesn't match core symbol"),
                       t.stake2pool(alice, alice, 3, a("1.000 FOO")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount must be positive"), t.stake2pool(alice, alice, 3, a("0.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount must be positive"), t.stake2pool(alice, alice, 3, a("-1.0000 TST")));
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
