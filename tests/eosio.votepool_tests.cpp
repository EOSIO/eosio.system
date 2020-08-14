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
using btime = block_timestamp_type;

auto a(const char* s) { return asset::from_string(s); }

constexpr auto sys   = N(eosio);
constexpr auto vpool = N(eosio.vpool);
constexpr auto bvpay = N(eosio.bvpay);

constexpr auto alice = N(alice1111111);
constexpr auto bob   = N(bob111111111);
constexpr auto jane  = N(jane11111111);

struct votepool_tester : eosio_system_tester {
   votepool_tester() : eosio_system_tester(setup_level::none) {
      create_accounts({ vpool, bvpay });
      basic_setup();
      create_core_token();
      deploy_contract();
      activate_chain();
   }

   // 'bp11activate' votes for self, then unvotes
   void activate_chain() {
      create_account_with_resources(N(bp11activate), sys);
      transfer(sys, N(bp11activate), a("150000000.0000 TST"), sys);
      BOOST_REQUIRE_EQUAL(success(), regproducer(N(bp11activate)));
      BOOST_REQUIRE_EQUAL(success(),
                          stake(N(bp11activate), N(bp11activate), a("75000000.0000 TST"), a("75000000.0000 TST")));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), { N(bp11activate) }));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), {}));
   }

   btime pending_time(double delta_sec = 0) {
      btime t = control->pending_block_time();
      t.slot += delta_sec * 2;
      return t;
   }

   void produce_to(time_point t) {
      while (control->pending_block_time() < t)
         produce_block();
   }

   fc::variant voter_pool_votes(name owner) {
      auto info = get_voter_info(owner);
      if (!info.is_null() && info.get_object().contains("pool_votes"))
         return info["pool_votes"];
      else
         return {};
   }

   fc::variant get_vpoolstate() const {
      vector<char> data = get_row_by_account(sys, {}, N(vpoolstate), N(vpoolstate));
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant("vote_pool_state", data, abi_serializer_max_time);
   }

   void check_vpool_totals(const std::vector<name>& users) {
      auto                pools         = get_vpoolstate()["pools"];
      auto                total_balance = a("0.0000 TST");
      std::vector<double> total_shares(pools.size());

      for (auto voter : users) {
         auto v = voter_pool_votes(voter);
         if (!v.is_null()) {
            auto s = v["owned_shares"].as<std::vector<double>>();
            BOOST_REQUIRE_EQUAL(s.size(), pools.size());
            for (size_t i = 0; i < pools.size(); ++i)
               total_shares[i] += s[i];
         }
      }

      for (size_t i = 0; i < pools.size(); ++i) {
         auto& pool = pools[i]["token_pool"];
         BOOST_REQUIRE_EQUAL(total_shares[i], pool["total_shares"].as<double>());
         total_balance += pool["balance"].as<asset>();
      }
      BOOST_REQUIRE_EQUAL(get_balance(vpool), total_balance);
   }

   // Like eosio_system_tester::push_action, but doesn't move time forward
   action_result push_action(name authorizer, name act, const variant_object& data) {
      try {
         // Some overloads move time forward, some don't. Use one that doesn't,
         // but translate the exception like one that does.
         base_tester::push_action(sys, act, authorizer, data, 1, 0);
      } catch (const fc::exception& ex) {
         edump((ex.to_detail_string()));
         return error(ex.top_message());
      }
      return success();
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
   t.create_accounts_with_resources({ alice }, sys);

   BOOST_REQUIRE_EQUAL("missing authority of eosio", t.cfgvpool(alice, { { 1, 2, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is required on first use of cfgvpool"),
                       t.cfgvpool(sys, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 1 } }, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is empty"),
                       t.cfgvpool(sys, std::vector<uint32_t>{}, std::vector<uint32_t>{}));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 1 } }, std::vector<uint32_t>{}));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"), t.cfgvpool(sys, { { 1, 2 } }, { { 1, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("duration must be positive"), t.cfgvpool(sys, { { 0 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be positive"), t.cfgvpool(sys, { { 1 } }, { { 0 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 1 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 10, 20 } }, { { 9, 20 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 3 } }, { { 1, 1, 1, 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 4 } }, { { 1, 1, 1, 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods must be non-decreasing"),
                       t.cfgvpool(sys, { { 3, 4, 5, 6 } }, { { 2, 2, 2, 1 } }));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations can't change"), t.cfgvpool(sys, { { 1, 2, 3 } }, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods can't change"), t.cfgvpool(sys, nullopt, { { 1, 2, 3 } }));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, 0, .999));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, .999, 0));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, 1));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, nullopt, 1));
} // cfgvpool
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(checks) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice, bob }, sys);

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.stake2pool(alice, bob, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"), t.stake2pool(alice, alice, 0, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.claimstake(alice, bob, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"), t.claimstake(alice, alice, 0, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111",
                       t.transferstake(alice, bob, alice, 0, a("1.0000 TST"), "memo"));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("memo has more than 256 bytes"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 TST"), std::string(257, 'x')));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from = to"),
                       t.transferstake(alice, alice, alice, 0, a("1.0000 TST"), std::string(256, 'x')));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid account"),
                       t.transferstake(alice, alice, N(oops), 0, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 TST"), ""));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.updatepay(alice, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"), t.updatepay(alice, alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.stake2pool(alice, alice, 4, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount doesn't match core symbol"),
                       t.stake2pool(alice, alice, 3, a("1.0000 FOO")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount doesn't match core symbol"),
                       t.stake2pool(alice, alice, 3, a("1.000 FOO")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount must be positive"), t.stake2pool(alice, alice, 3, a("0.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("amount must be positive"), t.stake2pool(alice, alice, 3, a("-1.0000 TST")));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.claimstake(alice, alice, 4, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.claimstake(alice, alice, 3, a("1.0000 FOO")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"), t.claimstake(alice, alice, 3, a("0.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.claimstake(alice, alice, 3, a("-1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter record missing"), t.claimstake(alice, alice, 3, a("1.0000 TST")));

   t.transfer(sys, alice, a("2.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(alice, alice, a("1.0000 TST"), a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter is not upgraded"), t.claimstake(alice, alice, 0, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.transferstake(alice, alice, bob, 4, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 OOPS"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.transferstake(alice, alice, bob, 0, a("1.000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.transferstake(alice, alice, bob, 0, a("0.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.transferstake(alice, alice, bob, 0, a("-1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from voter record missing"),
                       t.transferstake(bob, bob, alice, 0, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("to voter record missing"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 TST"), ""));
} // checks
FC_LOG_AND_RETHROW()

// Without inflation, 1.0 share = 0.0001 SYS
BOOST_AUTO_TEST_CASE(no_inflation) try {
   votepool_tester   t;
   std::vector<name> users = { alice, bob, jane };
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(jane, jane, a("0.0001 TST"), a("0.0001 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.unstake(jane, jane, a("0.0001 TST"), a("0.0001 TST")));
   t.check_vpool_totals(users);

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.voter_pool_votes(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("2.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 2'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 2'0000.0 })),               //
                           t.voter_pool_votes(bob));

   // Increasing stake at the same time as the original; next_claim doesn't move

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("0.5000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'5000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'5000.0, 0.0 })),              //
                           t.voter_pool_votes(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 3'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 3'0000.0 })),               //
                           t.voter_pool_votes(bob));

   // Move time forward 16s. Increasing stake uses weighting to advance next_claim
   t.produce_blocks(32);

   // stake-weighting next_claim: (48s, 1'5000.0), (64s, 0'7500.0) => (53s, 2'2500)
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("0.7500 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(53), btime() })) //
                           ("owned_shares", vector({ 2'2500.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 2'2500.0, 0.0 })),              //
                           t.voter_pool_votes(alice));

   // stake-weighting next_claim: (240s, 3'0000.0), (256s, 6'0000.0) => (250.5s, 9'0000.0)
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("6.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                      //
                           ("next_claim", vector({ btime(), t.pending_time(250.5) })) //
                           ("owned_shares", vector({ 0.0, 9'0000.0 }))                //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                   //
                           ("last_votes", vector({ 0.0, 9'0000.0 })),                 //
                           t.voter_pool_votes(bob));

   // Move time forward 52.5s (1 block before alice may claim)
   t.produce_blocks(105);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim too soon"), t.claimstake(alice, alice, 0, a("1.0000 TST")));
   t.check_vpool_totals(users);

   // 2.2500 * 64/1024 ~= 0.1406
   t.produce_block();
   auto alice_bal = t.get_balance(alice);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("withdrawing 0"), t.claimstake(alice, alice, 1, a("10000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.claimstake(alice, alice, 0, a("10000.0000 TST")));
   t.check_vpool_totals(users);
   BOOST_REQUIRE_EQUAL(t.get_balance(alice).get_amount(), alice_bal.get_amount() + 1406);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 2'1094.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 2'1094.0, 0.0 })),              //
                           t.voter_pool_votes(alice));

   // Move time far forward
   t.produce_block();
   t.produce_block(fc::days(300));

   // 9.0000 * 256/2048 = 1.1250
   auto bob_bal = t.get_balance(bob);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("withdrawing 0"), t.claimstake(bob, bob, 0, a("10000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.claimstake(bob, bob, 1, a("10000.0000 TST")));
   t.check_vpool_totals(users);
   BOOST_REQUIRE_EQUAL(t.get_balance(bob).get_amount(), bob_bal.get_amount() + 1'1250);
   REQUIRE_MATCHING_OBJECT(mvo()                                                      //
                           ("next_claim", vector({ btime(), t.pending_time(256.0) })) //
                           ("owned_shares", vector({ 0.0, 7'8750.0 }))                //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                   //
                           ("last_votes", vector({ 0.0, 7'8750.0 })),                 //
                           t.voter_pool_votes(bob));

   // Move time forward 192s
   t.produce_blocks(384);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(64) })) //
                           ("owned_shares", vector({ 0.0, 7'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 7'8750.0 })),              //
                           t.voter_pool_votes(bob));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter is not upgraded"),
                       t.transferstake(bob, bob, jane, 1, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter is not upgraded"),
                       t.transferstake(jane, jane, bob, 1, a("1.0000 TST"), ""));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.voter_pool_votes(jane));

   // transfer bob -> jane. bob's next_claim doesn't change. jane's next_claim is fresh.
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(bob, bob, jane, 1, a("4.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(64) })) //
                           ("owned_shares", vector({ 0.0, 3'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 3'8750.0 })),              //
                           t.voter_pool_votes(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(64), t.pending_time(256) })) //
                           ("owned_shares", vector({ 1'0000.0, 4'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 4'0000.0 })),                     //
                           t.voter_pool_votes(jane));

   // transfer jane -> bob. bob's next_claim moves.
   // (3.8750, 64s), (2.0000, 256s) => (5.8750, 129s)
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(jane, jane, bob, 1, a("2.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(129) })) //
                           ("owned_shares", vector({ 0.0, 5'8750.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 5'8750.0 })),               //
                           t.voter_pool_votes(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(64), t.pending_time(256) })) //
                           ("owned_shares", vector({ 1'0000.0, 2'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 2'0000.0 })),                     //
                           t.voter_pool_votes(jane));

   // Move time forward 32s
   t.produce_blocks(64);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(97) })) //
                           ("owned_shares", vector({ 0.0, 5'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 5'8750.0 })),              //
                           t.voter_pool_votes(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(32), t.pending_time(224) })) //
                           ("owned_shares", vector({ 1'0000.0, 2'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 2'0000.0 })),                     //
                           t.voter_pool_votes(jane));

   // transfer jane -> bob. Even though jane's next_claim is 224, the transfer counts as 256 at the receiver.
   // (5.8750, 97s), (1.0000, 256s) => (6.8750, 120s)
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(jane, jane, bob, 1, a("1.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(120) })) //
                           ("owned_shares", vector({ 0.0, 6'8750.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 6'8750.0 })),               //
                           t.voter_pool_votes(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(32), t.pending_time(224) })) //
                           ("owned_shares", vector({ 1'0000.0, 1'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 1'0000.0 })),                     //
                           t.voter_pool_votes(jane));
} // no_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(pool_inflation) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:00.000"));

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(alice, alice));

   // Bring the pending block to the beginning of the next time interval.
   // Note: on_block sees the previous block produced, not the pending block, so it won't
   //       trigger the rollover until 1 block later.

   t.produce_to(interval_start.to_time_point() + fc::seconds(60));

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(alice, alice));

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(60);

   // First interval is partial
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 11),             //
                           t.get_vpoolstate());

   t.produce_to(interval_start.to_time_point() + fc::seconds(60));
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 11),             //
                           t.get_vpoolstate());

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(60);

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 120),            //
                           t.get_vpoolstate());

   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 120),            //
                           t.get_vpoolstate());

   auto supply = t.get_token_supply();
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(bob, bob));

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_vpoolstate());

   // inflation is 0
   BOOST_REQUIRE_EQUAL(supply, t.get_token_supply());
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), a("0.0000 TST"));

   // enable voter pool inflation
   double rate = 0.5;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, rate));

   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", rate)               //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 120),            //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));

   // pools can't receive inflation since users haven't bought into them yet
   BOOST_REQUIRE_EQUAL(supply, t.get_token_supply());
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), a("0.0000 TST"));

   // alice buys into pool 0
   auto   alice_bought = a("1.0000 TST");
   double alice_shares = 1'0000.0;
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, alice_bought));
   auto pool_0_balance = alice_bought;
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ alice_shares, 0.0 }))         //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ alice_shares, 0.0 })),          //
                           t.voter_pool_votes(alice));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), a("0.0000 TST"));

   // produce inflation
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", rate)               //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 120),            //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   auto per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::minutes_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   supply += per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), a("0.0000 TST"));

   // bob buys into pool 1
   auto   bob_bought = a("2.0000 TST");
   double bob_shares = 2'0000.0;
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, bob_bought));
   auto pool_1_balance = bob_bought;
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, bob_shares }))            //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, bob_shares })),             //
                           t.voter_pool_votes(bob));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);

   // produce inflation
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", rate)               //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 120),            //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::minutes_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   pool_1_balance += per_pool_inflation;
   supply = supply + per_pool_inflation + per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), pool_0_balance + pool_1_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);

   // alice made a profit
   auto alice_approx_sell_shares = alice_shares * 64 / 1024; // easier formula, but rounds different from actual
   auto alice_sell_shares        = 624.9998690972549;        // calculated by contract
   BOOST_REQUIRE(abs(alice_sell_shares - alice_approx_sell_shares) < 0.001);
   auto alice_returned_funds =
         asset(pool_0_balance.get_amount() * alice_sell_shares / alice_shares, symbol{ CORE_SYM });
   auto alice_bal = t.get_balance(alice);
   BOOST_REQUIRE_EQUAL(t.success(), t.claimstake(alice, alice, 0, a("10000.0000 TST")));
   t.check_vpool_totals(users);
   alice_shares -= alice_sell_shares;
   alice_bal += alice_returned_funds;
   pool_0_balance -= alice_returned_funds;
   BOOST_REQUIRE_EQUAL(t.get_balance(alice), alice_bal);
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), pool_0_balance + pool_1_balance);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ alice_shares, 0.0 }))         //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ alice_shares, 0.0 })),          //
                           t.voter_pool_votes(alice));

   // BPs miss 30 blocks
   t.produce_block();
   t.produce_block(fc::milliseconds(15'500));
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.5)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 90),             //
                           t.get_vpoolstate());

   // check inflation with missed blocks
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   auto pay_scale = pow((double)90 / 120, 10);
   per_pool_inflation =
         asset(supply.get_amount() * rate * pay_scale / eosiosystem::minutes_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   pool_1_balance += per_pool_inflation;
   supply = supply + per_pool_inflation + per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(vpool), pool_0_balance + pool_1_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);
} // pool_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
