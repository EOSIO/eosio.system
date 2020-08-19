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
constexpr auto sue   = N(sue111111111);
constexpr auto bpa   = N(bpa111111111);
constexpr auto bpb   = N(bpb111111111);
constexpr auto bpc   = N(bpc111111111);

struct prod_pool_votes {
   std::vector<double> pool_votes;           // shares in each pool
   double              total_pool_votes = 0; // total shares in all pools, weighted by update time and pool strength
   asset               vote_pay;             // unclaimed vote pay
};

struct votepool_tester : eosio_system_tester {
   votepool_tester() : eosio_system_tester(setup_level::none) {
      create_accounts({ vpool, bvpay });
      basic_setup();
      create_core_token();
      deploy_contract();
      activate_chain();
   }

   // 'bp11activate' votes for self, then unvotes and unregisters
   void activate_chain() {
      create_account_with_resources(N(bp11activate), sys);
      transfer(sys, N(bp11activate), a("150000000.0000 TST"), sys);
      BOOST_REQUIRE_EQUAL(success(), regproducer(N(bp11activate)));
      BOOST_REQUIRE_EQUAL(success(),
                          stake(N(bp11activate), N(bp11activate), a("75000000.0000 TST"), a("75000000.0000 TST")));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), { N(bp11activate) }));
      BOOST_REQUIRE_EQUAL(success(), vote(N(bp11activate), {}));
      BOOST_REQUIRE_EQUAL(success(), push_action(N(bp11activate), N(unregprod), mvo()("producer", N(bp11activate))));
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

   void check_pool_votes(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters) {
      check_vpool_totals(voters);

      for (auto& [prod, ppv] : pool_votes) {
         ppv.pool_votes.clear();
         ppv.pool_votes.resize(num_pools);
      }

      for (auto voter : voters) {
         auto info = get_voter_info(voter);
         auto v    = voter_pool_votes(voter);
         if (!v.is_null()) {
            auto shares = v["owned_shares"].as<vector<double>>();
            auto prods  = info["producers"].as<vector<name>>();
            for (auto prod : prods) {
               auto& ppv = pool_votes[prod];
               BOOST_REQUIRE_EQUAL(ppv.pool_votes.size(), shares.size());
               for (size_t i = 0; i < shares.size(); ++i)
                  ppv.pool_votes[i] += shares[i] / prods.size();
            }
         }
      }

      for (auto& [prod, ppv] : pool_votes) {
         BOOST_REQUIRE(ppv.pool_votes == get_producer_info(prod)["pool_votes"]["pool_votes"].as<vector<double>>());
      }
   }; // check_pool_votes

   double time_to_vote_weight(const time_point& time) {
      double weight = int64_t((time.sec_since_epoch() - (eosio::chain::config::block_timestamp_epoch / 1000)) /
                              (eosiosystem::seconds_per_day * 7)) /
                      double(52);
      return std::pow(2, weight);
   }

   void update_bps(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters,
                   const std::vector<name>& bps) {
      auto state = get_vpoolstate();
      auto pools = state["pools"];
      BOOST_REQUIRE_EQUAL(pools.size(), num_pools);
      for (auto bp : bps) {
         auto& ppv = pool_votes[bp];
         BOOST_REQUIRE_EQUAL(ppv.pool_votes.size(), num_pools);
         double total = 0;
         for (int i = 0; i < num_pools; ++i) {
            auto token_pool = pools[i]["token_pool"];
            // elog("${x}, ${y}, ${z}", ("x", ppv.pool_votes[i])("y", token_pool["balance"].as<asset>().get_amount())(
            //                                "z", token_pool["total_shares"].as<double>()));
            if (ppv.pool_votes[i])
               total += ppv.pool_votes[i] * token_pool["balance"].as<asset>().get_amount() /
                        token_pool["total_shares"].as<double>() * pools[i]["vote_weight"].as<double>();
         }
         ppv.total_pool_votes = total * time_to_vote_weight(state["interval_start"].as<block_timestamp_type>());
      }
   }

   void check_total_pool_votes(std::map<name, prod_pool_votes>& pool_votes) {
      for (auto& [prod, ppv] : pool_votes) {
         BOOST_REQUIRE_EQUAL(ppv.total_pool_votes,
                             get_producer_info(prod)["pool_votes"]["total_pool_votes"].as<double>());
      }
   }

   void check_votes(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters,
                    const std::vector<name>& updating_bps = {}) {
      check_pool_votes(num_pools, pool_votes, voters);
      update_bps(num_pools, pool_votes, voters, updating_bps);
      check_total_pool_votes(pool_votes);
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
                          const std::optional<std::vector<double>>&   vote_weights  = nullopt,
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
      if (vote_weights)
         v("vote_weights", *vote_weights);
      else
         v("vote_weights", nullptr);
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

   action_result upgradestake(name authorizer, name owner, uint32_t from_pool_index, uint32_t to_pool_index,
                              asset requested) {
      return push_action(authorizer, N(upgradestake),
                         mvo()("owner", owner)("from_pool_index", from_pool_index)("to_pool_index", to_pool_index)(
                               "requested", requested));
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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is empty"),
                       t.cfgvpool(sys, std::vector<uint32_t>{}, std::vector<uint32_t>{}, std::vector<double>{}));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 1 } }, std::vector<uint32_t>{}, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 1, 2 } }, { { 1, 3, 4 } }, { { 1, 2 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 10, 20 } }, { { 1, 2 } }, { { 1, 2, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("duration must be positive"),
                       t.cfgvpool(sys, { { 0 } }, { { 1 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be positive"),
                       t.cfgvpool(sys, { { 1 } }, { { 0 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { -1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { 0 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 1 } }, { { 1 } }, { { 1 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 10, 20 } }, { { 9, 20 } }, { { 1, 2 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 3 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 4 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods must be non-decreasing"),
                       t.cfgvpool(sys, { { 3, 4, 5, 6 } }, { { 2, 2, 2, 1 } }, { { 1, 2, 3, 4 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights must be non-decreasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 2 } }));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations can't change"), t.cfgvpool(sys, { { 1, 2, 3 } }, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods can't change"), t.cfgvpool(sys, nullopt, { { 1, 2, 3 } }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights can't change"),
                       t.cfgvpool(sys, nullopt, nullopt, { { 1, 2, 3 } }));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, 0, .999));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, .999, 0));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, nullopt, nullopt, nullopt, 1));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"),
                       t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"),
                       t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, 1));
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

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.upgradestake(alice, bob, 0, 1, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"),
                       t.upgradestake(alice, alice, 0, 1, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.updatepay(alice, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote pools not initialized"), t.updatepay(alice, alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 1, 1, 1 } }));

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

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.upgradestake(alice, alice, 0, 4, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.upgradestake(alice, alice, 4, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("may only move from a shorter-term pool to a longer-term one"),
                       t.upgradestake(alice, alice, 3, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.upgradestake(alice, alice, 0, 1, a("1.0000 OOPS")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.upgradestake(alice, alice, 0, 1, a("1.000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.upgradestake(alice, alice, 0, 1, a("0.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.upgradestake(alice, alice, 0, 1, a("-1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter record missing"), t.upgradestake(bob, bob, 0, 1, a("1.0000 TST")));
} // checks
FC_LOG_AND_RETHROW()

// Without inflation, 1.0 share = 0.0001 TST
BOOST_AUTO_TEST_CASE(no_inflation) try {
   votepool_tester   t;
   std::vector<name> users = { alice, bob, jane };
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }));
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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter is not upgraded"), t.upgradestake(jane, jane, 0, 1, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("transferred 0"), t.upgradestake(bob, bob, 0, 1, a("1.0000 TST")));

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
   std::vector<name> users     = { alice, bob, jane, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }));
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
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, rate));

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

BOOST_AUTO_TEST_CASE(prod_inflation) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpc));
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(alice, { bpa, bpb, bpc }));

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:00.000"));

   double prod_rate    = 0.0;
   auto   supply       = t.get_token_supply();
   auto   bvpay_bal    = a("0.0000 TST");
   auto   bpa_vote_pay = a("0.0000 TST");
   auto   bpb_vote_pay = a("0.0000 TST");
   auto   bpc_vote_pay = a("0.0000 TST");
   double bpa_factor   = 0.0;
   double bpb_factor   = 0.0;
   double bpc_factor   = 0.0;

   auto check_vpoolstate = [&](uint32_t unpaid_blocks) {
      REQUIRE_MATCHING_OBJECT(mvo()                              //
                              ("prod_rate", prod_rate)           //
                              ("voter_rate", 0.0)                //
                              ("interval_start", interval_start) //
                              ("unpaid_blocks", unpaid_blocks),  //
                              t.get_vpoolstate());
   };

   auto check_vote_pay = [&](uint32_t unpaid_blocks = 120) {
      check_vpoolstate(unpaid_blocks);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));
      check_vpoolstate(0);

      auto pay_scale = pow(double(unpaid_blocks) / 120, 10);
      auto target_pay =
            asset(pay_scale * prod_rate * supply.get_amount() / eosiosystem::minutes_per_year, symbol{ CORE_SYM });
      // ilog("target_pay: ${x}", ("x", target_pay));

      auto check_pay = [&](auto bp, auto& bp_vote_pay, double ratio) {
         auto pay = asset(target_pay.get_amount() * ratio, symbol{ CORE_SYM });
         auto adj = t.get_producer_info(bp)["pool_votes"]["vote_pay"].template as<asset>() - bp_vote_pay - pay;
         if (abs(adj.get_amount()) <= 2)
            pay += adj; // allow slight rounding difference
         // ilog("${bp} pay: ${x}", ("bp", bp)("x", pay));
         supply += pay;
         bvpay_bal += pay;
         bp_vote_pay += pay;
         BOOST_REQUIRE_EQUAL(t.get_producer_info(bp)["pool_votes"]["vote_pay"].template as<asset>(), bp_vote_pay);
      };
      check_pay(bpa, bpa_vote_pay, bpa_factor);
      check_pay(bpb, bpb_vote_pay, bpb_factor);
      check_pay(bpc, bpc_vote_pay, bpc_factor);

      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
   };

   auto claimvotepay = [&](auto bp, auto& vote_pay) {
      auto bal = t.get_balance(bp);
      BOOST_REQUIRE_EQUAL(t.get_producer_info(bp)["pool_votes"]["vote_pay"].template as<asset>(), vote_pay);
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
      BOOST_REQUIRE_EQUAL(t.success(), t.claimvotepay(bp, bp));
      BOOST_REQUIRE_EQUAL(t.get_producer_info(bp)["pool_votes"]["vote_pay"].template as<asset>(), a("0.0000 TST"));
      bvpay_bal -= vote_pay;
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
      BOOST_REQUIRE_EQUAL(t.get_balance(bp), bal + vote_pay);
      vote_pay = a("0.0000 TST");
   };

   auto next_interval = [&]() {
      t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
      interval_start = interval_start.to_time_point() + fc::seconds(60);
   };

   // Go to first whole interval
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(120'500));
   interval_start = interval_start.to_time_point() + fc::seconds(120);
   check_vote_pay();

   // enable producer inflation; no bps are automatically counted yet
   prod_rate = 0.5;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, prod_rate));
   next_interval();
   check_vote_pay();

   // manually update bpa, bpb votes; they'll be automatically counted from now on
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("unknown producer"), t.updatevotes(alice, alice, alice));
   BOOST_REQUIRE_EQUAL("missing authority of bpa111111111", t.updatevotes(alice, bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(alice, alice, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   bpa_factor = 0.5;
   bpb_factor = 0.5;
   next_interval();
   check_vote_pay();

   // bpc joins
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   bpa_factor = 1.0 / 3;
   bpb_factor = 1.0 / 3;
   bpc_factor = 1.0 / 3;
   next_interval();
   check_vote_pay();

   // bpb claims
   BOOST_REQUIRE_EQUAL("missing authority of bpb111111111", t.claimvotepay(alice, bpb));
   claimvotepay(bpb, bpb_vote_pay);
   t.produce_block();
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("no pay available"), t.claimvotepay(bpb, bpb));

   // more claims
   next_interval();
   check_vote_pay();
   claimvotepay(bpa, bpa_vote_pay);
   claimvotepay(bpb, bpb_vote_pay);
   t.produce_block();
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("no pay available"), t.claimvotepay(bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("no pay available"), t.claimvotepay(bpb, bpb));

   // BPs miss 30 blocks
   t.produce_block();
   t.produce_block(fc::milliseconds(15'500));
   next_interval();
   check_vote_pay(90);
} // prod_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(voting) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane, sue, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.5 } }));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, sue, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bpa, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bpb, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bpc, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);
   t.transfer(sys, sue, a("1000.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpc));

   std::map<name, prod_pool_votes> pool_votes;
   pool_votes[bpa];
   pool_votes[bpb];
   pool_votes[bpc];

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:00.000"));

   // Go to first whole interval
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(120'500));
   interval_start = interval_start.to_time_point() + fc::seconds(120);

   // inflate pool 1
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, 0.5));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 1, a("1.0000 TST")));
   // ilog("pool 1: ${x}", ("x", t.get_vpoolstate()["pools"][1]));
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   // ilog("pool 1: ${x}", ("x", t.get_vpoolstate()["pools"][1]));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, 0.0));

   t.produce_to(interval_start.to_time_point() + fc::milliseconds(60'500));
   interval_start = interval_start.to_time_point() + fc::seconds(60);

   // alice buys pool 0 and votes
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(alice, { bpa, bpb }));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
   t.check_votes(num_pools, pool_votes, users, { bpa });
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   t.check_votes(num_pools, pool_votes, users, { bpb });

   // bob buys pool 0 and votes
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 0, a("1.0000 TST")));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(bob, { bpb, bpc }));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   t.check_votes(num_pools, pool_votes, users, { bpb, bpc });

   // sue buys pool 1 and votes
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(sue, sue, 1, a("1.0000 TST")));
   t.produce_block();
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   t.check_votes(num_pools, pool_votes, users, { bpa, bpc });

   // check balance between pool 0 (not inflated) and pool 1 (inflated)
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(alice, {}));
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(bob, { bpb })); // 1.0000 TST in pool 0; 100000.0 shares
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(sue, { bpc })); // 1.0000 TST in pool 1; small shares
   t.produce_block();
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   t.check_votes(num_pools, pool_votes, users, { bpa, bpb, bpc });
   // ilog("bpa: ${x}", ("x", t.get_producer_info(bpa)["pool_votes"]));
   // ilog("bpb: ${x}", ("x", t.get_producer_info(bpb)["pool_votes"]));
   // ilog("bpc: ${x}", ("x", t.get_producer_info(bpc)["pool_votes"]));

   // bob is now in both pools
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("1.0000 TST")));
   t.produce_block();
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   t.check_votes(num_pools, pool_votes, users, { bpa, bpb, bpc });
} // voting
FC_LOG_AND_RETHROW()

// TODO: proxy
// TODO: producer pay: 50, 80/20 rule

BOOST_AUTO_TEST_SUITE_END()
