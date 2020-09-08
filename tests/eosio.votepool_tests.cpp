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

namespace std {
   std::ostream& operator<<(std::ostream& os, const std::vector<name>& v) {
      os << "[";
      for (auto n : v)
         os << n << ",";
      return os << "]";
   }
} // namespace std

using namespace eosio_system;
using std::nullopt;
using btime = block_timestamp_type;

auto a(const char* s) { return asset::from_string(s); }

constexpr auto sys    = N(eosio);
constexpr auto vpool  = N(eosio.vpool);
constexpr auto bvpay  = N(eosio.bvpay);
constexpr auto reserv = N(eosio.reserv);
constexpr auto rex    = N(eosio.rex);

constexpr auto alice = N(alice1111111);
constexpr auto bob   = N(bob111111111);
constexpr auto jane  = N(jane11111111);
constexpr auto sue   = N(sue111111111);
constexpr auto prox  = N(proxy1111111);
constexpr auto bpa   = N(bpa111111111);
constexpr auto bpb   = N(bpb111111111);
constexpr auto bpc   = N(bpc111111111);
constexpr auto bpd   = N(bpd111111111);

constexpr auto blocks_per_round  = eosiosystem::blocks_per_round;
constexpr auto seconds_per_round = blocks_per_round / 2;

inline constexpr int64_t rentbw_frac    = 1'000'000'000'000'000ll; // 1.0 = 10^15
inline constexpr int64_t rentbw_percent = rentbw_frac / 100;

struct prod_pool_votes {
   std::vector<double> pool_votes;           // shares in each pool
   double              total_pool_votes = 0; // total shares in all pools, weighted by update time and pool strength
   asset               vote_pay;             // unclaimed vote pay
};

struct votepool_tester : eosio_system_tester {
   btime start_transition;
   btime end_transition;

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

   void skip_to(time_point t) { produce_block(t - control->pending_block_time()); }

   template <typename T>
   T transition(const btime& t, T val) {
      if (t.slot >= end_transition.slot)
         return val;
      if (t.slot <= start_transition.slot)
         return 0;
      return val * (t.slot - start_transition.slot) / (end_transition.slot - start_transition.slot);
   };

   fc::variant pool_voter(name owner) {
      vector<char> data = get_row_by_account(sys, sys, N(poolvoter), owner);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("pool_voter", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_vpoolstate() const {
      vector<char> data = get_row_by_account(sys, sys, N(vpoolstate), N(vpoolstate));
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("vote_pool_state", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_total_pool_votes(name producer) const {
      vector<char> data = get_row_by_account(sys, sys, N(totpoolvotes), producer);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("total_pool_votes", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void check_vpool_totals(const std::vector<name>& users) {
      auto                pools         = get_vpoolstate()["pools"];
      auto                total_balance = a("0.0000 TST");
      std::vector<double> total_shares(pools.size());

      for (auto voter : users) {
         auto v = pool_voter(voter);
         if (!v.is_null()) {
            auto s = v["owned_shares"].as<std::vector<double>>();
            BOOST_REQUIRE_EQUAL(s.size(), pools.size());
            for (size_t i = 0; i < pools.size(); ++i)
               total_shares[i] += s[i];
         }
      }

      for (size_t i = 0; i < pools.size(); ++i) {
         auto& pool = pools[i]["token_pool"];
         BOOST_TEST(total_shares[i] == pool["total_shares"].as<double>());
         total_balance += pool["balance"].as<asset>();
      }
      BOOST_TEST(get_balance(vpool) == total_balance);
   }

   void check_pool_votes(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters) {
      check_vpool_totals(voters);

      for (auto& [prod, ppv] : pool_votes) {
         ppv.pool_votes.clear();
         ppv.pool_votes.resize(num_pools);
      }

      for (auto voter : voters) {
         auto v = pool_voter(voter);
         if (!v.is_null()) {
            auto shares = v["owned_shares"].as<vector<double>>();
            auto prods  = v["producers"].as<vector<name>>();
            auto proxy  = v["proxy"].as<name>();
            if (proxy.to_uint64_t() && pool_voter(proxy)["is_proxy"].as<bool>())
               prods = pool_voter(proxy)["producers"].as<vector<name>>();
            for (auto prod : prods) {
               auto& ppv = pool_votes[prod];
               BOOST_REQUIRE_EQUAL(ppv.pool_votes.size(), shares.size());
               for (size_t i = 0; i < shares.size(); ++i)
                  ppv.pool_votes[i] += shares[i] / prods.size();
            }
         }
      }

      for (auto& [prod, ppv] : pool_votes) {
         for (size_t i = 0; i < ppv.pool_votes.size(); ++i)
            BOOST_TEST(ppv.pool_votes[i] == get_producer_info(prod)["pool_votes"].as<vector<double>>()[i]);
      }
   }; // check_pool_votes

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
            if (ppv.pool_votes[i]) {
               int64_t sim_sell = ppv.pool_votes[i] * token_pool["balance"].as<asset>().get_amount() /
                                  token_pool["total_shares"].as<double>();
               total += sim_sell * pools[i]["vote_weight"].as<double>();

               // elog("${bp}: [${i}] ${x}, ${y}, ${z}, sim=${sim} => ${res}", //
               //      ("bp", bp)("i", i)("x", ppv.pool_votes[i])              //
               //      ("y", token_pool["balance"].as<asset>().get_amount())   //
               //      ("z", token_pool["total_shares"].as<double>())          //
               //      ("sim", sim_sell)                                       //
               //      ("res", sim_sell * pools[i]["vote_weight"].as<double>()));
            }
         }
         // elog("   total:         ${t}", ("t", total));
         ppv.total_pool_votes = total;
      }
   }

   void check_total_pool_votes(std::map<name, prod_pool_votes>& pool_votes) {
      for (auto& [prod, ppv] : pool_votes) {
         BOOST_REQUIRE_EQUAL(ppv.total_pool_votes, get_total_pool_votes(prod)["votes"].as<double>());
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

   // doesn't move time forward
   action_result regproducer_0_time(const account_name& prod) {
      action_result r = push_action(
            prod, N(regproducer),
            mvo()("producer", prod)("producer_key", get_public_key(prod, "active"))("url", "")("location", 0));
      BOOST_REQUIRE_EQUAL(success(), r);
      return r;
   }

   action_result cfgvpool(name                         authorizer, //
                          const std::optional<double>& prod_rate,  //
                          const std::optional<double>& voter_rate) {
      return cfgvpool(authorizer, nullopt, nullopt, nullopt, nullopt, nullopt, prod_rate, voter_rate);
   }

   action_result cfgvpool(name                                        authorizer,           //
                          const std::optional<std::vector<uint32_t>>& durations,            //
                          const std::optional<std::vector<uint32_t>>& claim_periods,        //
                          const std::optional<std::vector<double>>&   vote_weights,         //
                          const std::optional<block_timestamp_type>&  begin_transition,     //
                          const std::optional<block_timestamp_type>&  end_transition,       //
                          const std::optional<double>&                prod_rate  = nullopt, //
                          const std::optional<double>&                voter_rate = nullopt) {
      mvo  v;
      auto fill = [&](const char* name, auto& opt) {
         if (opt)
            v(name, *opt);
         else
            v(name, nullptr);
      };
      fill("durations", durations);
      fill("claim_periods", claim_periods);
      fill("vote_weights", vote_weights);
      fill("begin_transition", begin_transition);
      fill("end_transition", end_transition);
      fill("prod_rate", prod_rate);
      fill("voter_rate", voter_rate);
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

   action_result votewithpool(name authorizer, name voter, name proxy, const vector<name>& producers) {
      return push_action(authorizer, N(votewithpool), mvo()("voter", voter)("proxy", proxy)("producers", producers));
   }

   action_result votewithpool(name voter, name proxy) { return votewithpool(voter, voter, proxy, {}); }

   action_result votewithpool(name voter, const vector<name>& producers) {
      return votewithpool(voter, voter, {}, producers);
   }

   action_result votewithpool(name voter) { return votewithpool(voter, voter, {}, {}); }

   action_result regpoolproxy(name authorizer, name proxy, bool isproxy) {
      return push_action(authorizer, N(regpoolproxy), mvo()("proxy", proxy)("isproxy", isproxy));
   }

   action_result regpoolproxy(name proxy, bool isproxy) { return regpoolproxy(proxy, proxy, isproxy); }

   action_result updatevotes(name authorizer, name user, name producer) {
      return push_action(authorizer, N(updatevotes), mvo()("user", user)("producer", producer));
   }

   action_result updatepay(name authorizer, name user) {
      return push_action(authorizer, N(updatepay), mvo()("user", user));
   }

   action_result claimvotepay(name authorizer, name producer) {
      return push_action(authorizer, N(claimvotepay), mvo()("producer", producer));
   }

   action_result rentbw(const name& payer, const name& receiver, uint32_t days, int64_t net_frac, int64_t cpu_frac,
                        const asset& max_payment) {
      return push_action(payer, N(rentbw),
                         mvo()("payer", payer)("receiver", receiver)("days", days)("net_frac", net_frac)(
                               "cpu_frac", cpu_frac)("max_payment", max_payment));
   }

   std::vector<name> active_producers() {
      std::vector<name> result;
      auto&             prods = control->active_producers().producers;
      for (auto& prod : prods)
         result.push_back(prod.producer_name);
      return result;
   }
}; // votepool_tester

BOOST_AUTO_TEST_SUITE(eosio_system_votepool_tests)

BOOST_AUTO_TEST_CASE(cfgvpool) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice }, sys);

   BOOST_REQUIRE_EQUAL("missing authority of eosio",
                       t.cfgvpool(alice, { { 1, 2, 3, 4 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is required on first use of cfgvpool"),
                       t.cfgvpool(sys, nullopt, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 1 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { 1 } }, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("end_transition is required on first use of cfgvpool"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { 1 } }, btime(), nullopt));
   BOOST_REQUIRE_EQUAL(
         t.wasm_assert_msg("durations is empty"),
         t.cfgvpool(sys, std::vector<uint32_t>{}, std::vector<uint32_t>{}, std::vector<double>{}, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 1 } }, std::vector<uint32_t>{}, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 1, 2 } }, { { 1, 3, 4 } }, { { 1, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgvpool(sys, { { 10, 20 } }, { { 1, 2 } }, { { 1, 2, 3 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("duration must be positive"),
                       t.cfgvpool(sys, { { 0 } }, { { 1 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be positive"),
                       t.cfgvpool(sys, { { 1 } }, { { 0 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { -1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgvpool(sys, { { 2 } }, { { 1 } }, { { 0 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 1 } }, { { 1 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgvpool(sys, { { 10, 20 } }, { { 9, 20 } }, { { 1, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 3 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 4 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods must be non-decreasing"),
                       t.cfgvpool(sys, { { 3, 4, 5, 6 } }, { { 2, 2, 2, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights must be non-decreasing"),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition > end_transition"),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } },
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.000")), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition > end_transition"),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } },
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.500")),
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.000"))));
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations can't change"),
                       t.cfgvpool(sys, { { 1, 2, 3 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods can't change"),
                       t.cfgvpool(sys, nullopt, { { 1, 2, 3 } }, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights can't change"),
                       t.cfgvpool(sys, nullopt, nullopt, { { 1, 2, 3 } }, nullopt, nullopt));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, 0, .999));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, .999, 0));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, -.001, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgvpool(sys, 1, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgvpool(sys, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgvpool(sys, nullopt, 1));
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

   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 1, 1, 1 } }, btime(), btime()));

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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("withdrawing 0"), t.claimstake(alice, alice, 0, a("1.0000 TST")));

   t.transfer(sys, alice, a("2.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.produce_block();
   t.produce_block(fc::days(1));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid pool"), t.transferstake(alice, alice, bob, 4, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 OOPS"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested doesn't match core symbol"),
                       t.transferstake(alice, alice, bob, 0, a("1.000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.transferstake(alice, alice, bob, 0, a("0.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested must be positive"),
                       t.transferstake(alice, alice, bob, 0, a("-1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from pool_voter record missing"),
                       t.transferstake(bob, bob, alice, 0, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("to pool_voter record missing"),
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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("pool_voter record missing"), t.upgradestake(bob, bob, 0, 1, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of alice1111111", t.votewithpool(bob, alice, {}, { bpa, bpb }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("producer votes must be unique and sorted"),
                       t.votewithpool(alice, { bpb, bpa }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("producer votes must be unique and sorted"),
                       t.votewithpool(alice, { bpb, bpb }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("attempt to vote for too many producers"),
                       t.votewithpool(alice, { N(a), N(b), N(c), N(d),  N(e),  N(f),  N(g),  N(h),  N(i), N(j), N(k),
                                               N(l), N(m), N(n), N(o),  N(p),  N(q),  N(r),  N(s),  N(t), N(u), N(v),
                                               N(w), N(x), N(y), N(za), N(zb), N(zc), N(zd), N(ze), N(zf) }));
} // checks
FC_LOG_AND_RETHROW()

// Without inflation, 1.0 share = 0.0001 TST
BOOST_AUTO_TEST_CASE(no_inflation) try {
   votepool_tester   t;
   std::vector<name> users = { alice, bob, jane, sue };
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, sue, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);
   t.transfer(sys, sue, a("1000.0000 TST"), sys);
   t.check_vpool_totals(users);

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.pool_voter(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("2.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 2'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 2'0000.0 })),               //
                           t.pool_voter(bob));

   // Increasing stake at the same time as the original; next_claim doesn't move

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("0.5000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'5000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'5000.0, 0.0 })),              //
                           t.pool_voter(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 3'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 3'0000.0 })),               //
                           t.pool_voter(bob));

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
                           t.pool_voter(alice));

   // stake-weighting next_claim: (240s, 3'0000.0), (256s, 6'0000.0) => (250.5s, 9'0000.0)
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("6.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                      //
                           ("next_claim", vector({ btime(), t.pending_time(250.5) })) //
                           ("owned_shares", vector({ 0.0, 9'0000.0 }))                //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                   //
                           ("last_votes", vector({ 0.0, 9'0000.0 })),                 //
                           t.pool_voter(bob));

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
                           t.pool_voter(alice));

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
                           t.pool_voter(bob));

   // Move time forward 192s
   t.produce_blocks(384);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(64) })) //
                           ("owned_shares", vector({ 0.0, 7'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 7'8750.0 })),              //
                           t.pool_voter(bob));

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("to pool_voter record missing"),
                       t.transferstake(bob, bob, jane, 1, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from pool_voter record missing"),
                       t.transferstake(jane, jane, bob, 1, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("pool_voter record missing"),
                       t.upgradestake(jane, jane, 0, 1, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("transferred 0"), t.upgradestake(bob, bob, 0, 1, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.pool_voter(jane));

   // transfer bob -> jane. bob's next_claim doesn't change. jane's next_claim is fresh.
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(bob, bob, jane, 1, a("4.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(64) })) //
                           ("owned_shares", vector({ 0.0, 3'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 3'8750.0 })),              //
                           t.pool_voter(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(64), t.pending_time(256) })) //
                           ("owned_shares", vector({ 1'0000.0, 4'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 4'0000.0 })),                     //
                           t.pool_voter(jane));

   // transfer jane -> bob. bob's next_claim moves.
   // (3.8750, 64s), (2.0000, 256s) => (5.8750, 129s)
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(jane, jane, bob, 1, a("2.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(129) })) //
                           ("owned_shares", vector({ 0.0, 5'8750.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 5'8750.0 })),               //
                           t.pool_voter(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(64), t.pending_time(256) })) //
                           ("owned_shares", vector({ 1'0000.0, 2'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 2'0000.0 })),                     //
                           t.pool_voter(jane));

   // Move time forward 32s
   t.produce_blocks(64);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ btime(), t.pending_time(97) })) //
                           ("owned_shares", vector({ 0.0, 5'8750.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 0.0, 5'8750.0 })),              //
                           t.pool_voter(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(32), t.pending_time(224) })) //
                           ("owned_shares", vector({ 1'0000.0, 2'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 2'0000.0 })),                     //
                           t.pool_voter(jane));

   // transfer jane -> bob. Even though jane's next_claim is 224, the transfer counts as 256 at the receiver.
   // (5.8750, 97s), (1.0000, 256s) => (6.8750, 120s)
   BOOST_REQUIRE_EQUAL(t.success(), t.transferstake(jane, jane, bob, 1, a("1.0000 TST"), ""));
   t.check_vpool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(120) })) //
                           ("owned_shares", vector({ 0.0, 6'8750.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 6'8750.0 })),               //
                           t.pool_voter(bob));
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(32), t.pending_time(224) })) //
                           ("owned_shares", vector({ 1'0000.0, 1'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 1'0000.0 })),                     //
                           t.pool_voter(jane));

   // setup for upgradestake
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(sue, sue, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(sue, sue, 1, a("2.0000 TST")));
   t.check_vpool_totals(users);
   t.produce_blocks(48 * 2);
   REQUIRE_MATCHING_OBJECT(mvo()                                                               //
                           ("next_claim", vector({ t.pending_time(16), t.pending_time(208) })) //
                           ("owned_shares", vector({ 1'0000.0, 2'0000.0 }))                    //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                            //
                           ("last_votes", vector({ 1'0000.0, 2'0000.0 })),                     //
                           t.pool_voter(sue));

   // Upgraded amount counts as fresh (256 s)
   // (2.0000, 208s), (0.50000, 256s) => (2.5000, 217.5)
   BOOST_REQUIRE_EQUAL(t.success(), t.upgradestake(sue, sue, 0, 1, a("0.5000 TST")));
   REQUIRE_MATCHING_OBJECT(mvo()                                                                 //
                           ("next_claim", vector({ t.pending_time(16), t.pending_time(217.5) })) //
                           ("owned_shares", vector({ 0'5000.0, 2'5000.0 }))                      //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                              //
                           ("last_votes", vector({ 0'5000.0, 2'5000.0 })),                       //
                           t.pool_voter(sue));
} // no_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(pool_inflation) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:18.000"));

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

   t.produce_to(interval_start.to_time_point() + fc::seconds(seconds_per_round));

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(alice, alice));

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);

   // First interval is partial
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 179),            //
                           t.get_vpoolstate());

   t.produce_to(interval_start.to_time_point() + fc::seconds(seconds_per_round));
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 179),            //
                           t.get_vpoolstate());

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", 0.0)                  //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_vpoolstate());

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", 0.0)                  //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
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
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, rate));

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
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
                           t.pool_voter(alice));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), a("0.0000 TST"));

   // produce inflation

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   auto per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
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
                           t.pool_voter(bob));
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);

   // produce inflation
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_vpoolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
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
   auto alice_sell_shares        = 624.9996257869700;        // calculated by contract
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
                           t.pool_voter(alice));

   // BPs miss 30 blocks
   t.produce_block();
   t.produce_block(fc::milliseconds(15'500));
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                     //
                           ("prod_rate", 0.0)                        //
                           ("voter_rate", 0.5)                       //
                           ("interval_start", interval_start)        //
                           ("unpaid_blocks", blocks_per_round - 30), //
                           t.get_vpoolstate());

   // check inflation with missed blocks
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   auto pay_scale = pow((double)(blocks_per_round - 30) / blocks_per_round, 10);
   per_pool_inflation =
         asset(supply.get_amount() * rate * pay_scale / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
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
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpc));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, { bpa, bpb, bpc }));

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:18.000"));

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

   auto check_vote_pay = [&](uint32_t unpaid_blocks = blocks_per_round) {
      check_vpoolstate(unpaid_blocks);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));
      check_vpoolstate(0);

      auto pay_scale = pow(double(unpaid_blocks) / blocks_per_round, 10);
      auto target_pay =
            asset(pay_scale * prod_rate * supply.get_amount() / eosiosystem::rounds_per_year, symbol{ CORE_SYM });
      // ilog("target_pay: ${x}", ("x", target_pay));

      auto check_pay = [&](auto bp, auto& bp_vote_pay, double ratio) {
         auto pay    = asset(target_pay.get_amount() * ratio, symbol{ CORE_SYM });
         auto actual = t.get_total_pool_votes(bp)["vote_pay"].template as<asset>();
         auto adj    = actual - bp_vote_pay - pay;
         if (abs(adj.get_amount()) <= 2)
            pay += adj; // allow slight rounding difference
         // ilog("${bp} pay: ${x}", ("bp", bp)("x", pay));
         supply += pay;
         bvpay_bal += pay;
         bp_vote_pay += pay;
         BOOST_REQUIRE_EQUAL(actual, bp_vote_pay);
      };
      check_pay(bpa, bpa_vote_pay, bpa_factor);
      check_pay(bpb, bpb_vote_pay, bpb_factor);
      check_pay(bpc, bpc_vote_pay, bpc_factor);

      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
   };

   auto claimvotepay = [&](auto bp, auto& vote_pay) {
      auto bal = t.get_balance(bp);
      BOOST_REQUIRE_EQUAL(t.get_total_pool_votes(bp)["vote_pay"].template as<asset>(), vote_pay);
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
      BOOST_REQUIRE_EQUAL(t.success(), t.claimvotepay(bp, bp));
      BOOST_REQUIRE_EQUAL(t.get_total_pool_votes(bp)["vote_pay"].template as<asset>(), a("0.0000 TST"));
      bvpay_bal -= vote_pay;
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
      BOOST_REQUIRE_EQUAL(t.get_balance(bp), bal + vote_pay);
      vote_pay = a("0.0000 TST");
   };

   auto next_interval = [&]() {
      interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
      t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   };

   // Go to first whole interval
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round * 2);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   check_vote_pay();

   // enable producer inflation; no bps are automatically counted yet
   prod_rate = 0.5;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, prod_rate, nullopt));
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
   check_vote_pay(blocks_per_round - 30);
} // prod_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(voting, *boost::unit_test::tolerance(1e-8)) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane, sue, bpa, bpb, bpc, bpd };
   int               num_pools = 2;
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer_0_time(bpd));
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.5 } }, btime(), btime()));
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
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpb));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpc));

   std::map<name, prod_pool_votes> pool_votes;
   pool_votes[bpb];
   pool_votes[bpc];

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:18.000"));

   // Go to first whole interval
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(120'500));
   interval_start = interval_start.to_time_point() + fc::seconds(120);

   // inflate pool 1
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, 0.5));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 1, a("1.0000 TST")));
   // ilog("pool 1: ${x}", ("x", t.get_vpoolstate()["pools"][1]));
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   // ilog("pool 1: ${x}", ("x", t.get_vpoolstate()["pools"][1]));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, nullopt, 0.0));

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));

   // alice buys pool 0 and votes
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("producer bpd111111111 has not upgraded to support pool votes"),
                       t.votewithpool(alice, vector{ bpd }));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("producer bpa111111111 is not registered"),
                       t.votewithpool(alice, { bpa, bpb }));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   pool_votes[bpa];
   BOOST_REQUIRE_EQUAL(t.success(), t.push_action(bpa, N(unregprod), mvo()("producer", bpa)));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("producer bpa111111111 is not currently registered"),
                       t.votewithpool(alice, { bpa, bpb }));
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, { bpa, bpb }));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
   t.check_votes(num_pools, pool_votes, users, { bpa });
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   t.check_votes(num_pools, pool_votes, users, { bpb });

   // bob buys pool 0 and votes
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 0, a("1.0000 TST")));
   t.check_votes(num_pools, pool_votes, users);
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bob, { bpb, bpc }));
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
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bob, vector{ bpb })); // 1.0000 TST in pool 0; 100000.0 shares
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(sue, vector{ bpc })); // 1.0000 TST in pool 1; small shares
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

   auto update_and_check = [&] {
      t.produce_block();
      BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));
      BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
      BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
      t.check_votes(num_pools, pool_votes, users, { bpa, bpb, bpc });
   };

   // bob: {b, c}; alice: {a, b}
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, { bpa, bpb }));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bob, { bpb, bpc }));
   update_and_check();

   // bob becomes proxy and alice switches to proxy
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("proxy not found"), t.votewithpool(alice, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("proxy not found"), t.votewithpool(alice, N(unknownaccnt)));
   BOOST_REQUIRE_EQUAL(t.success(), t.regpoolproxy(bob, true));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("cannot proxy to self"), t.votewithpool(bob, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("account registered as a proxy is not allowed to use a proxy"),
                       t.votewithpool(bob, alice));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("cannot vote for producers and proxy at same time"),
                       t.votewithpool(alice, alice, bob, { bpa, bpb }));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, bob));
   update_and_check();

   // alice stakes more
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("4.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 1, a("5.0000 TST")));
   update_and_check();

   // bob stakes more
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 0, a("4.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("5.0000 TST")));
   update_and_check();

   // alice upgrades some stake
   BOOST_REQUIRE_EQUAL(t.success(), t.upgradestake(alice, alice, 0, 1, a("0.5000 TST")));
   update_and_check();

   // bob upgrades some stake
   BOOST_REQUIRE_EQUAL(t.success(), t.upgradestake(bob, bob, 0, 1, a("0.5000 TST")));
   update_and_check();

   // alice switches back to manual voting
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, { bpa, bpb }));
   update_and_check();

   // alice uses bob as a proxy again
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, bob));
   update_and_check();

   // bob unregisters
   BOOST_REQUIRE_EQUAL(t.success(), t.regpoolproxy(bob, false));
   update_and_check();

   // alice switches back to manual voting
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(alice, { bpa, bpb }));
   update_and_check();

   // alice becomes a proxy
   BOOST_REQUIRE_EQUAL(t.success(), t.regpoolproxy(alice, true));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bob, alice));
   update_and_check();
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("account that uses a proxy is not allowed to become a proxy"),
                       t.regpoolproxy(bob, true));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bob, { bpb, bpc }));
   BOOST_REQUIRE_EQUAL(t.success(), t.regpoolproxy(bob, true));
   update_and_check();
} // voting
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(transition_voting) try {
   votepool_tester t;

   t.start_transition = time_point::from_iso_string("2020-04-10T10:00:00.000");
   t.end_transition   = time_point::from_iso_string("2020-08-10T10:00:00.000");

   struct bp_votes {
      name  bp;
      asset cpu_votes;
      asset pool_votes;
   };

   vector<bp_votes> bps{
      { N(bp111111111a), a("1000.0000 TST"), a("120.0000 TST") },
      { N(bp111111111b), a("1001.0000 TST"), a("170.0000 TST") },
      { N(bp111111111c), a("1002.0000 TST"), a("190.0000 TST") },
      { N(bp111111111d), a("1003.0000 TST"), a("110.0000 TST") },
      { N(bp111111111e), a("1004.0000 TST"), a("180.0000 TST") },
      { N(bp111111111f), a("1005.0000 TST"), a("009.0000 TST") },
      { N(bp111111111g), a("1006.0000 TST"), a("130.0000 TST") },
      { N(bp111111111h), a("1007.0000 TST"), a("100.0000 TST") },
      { N(bp111111111i), a("1008.0000 TST"), a("010.0000 TST") },
      { N(bp111111111j), a("1009.0000 TST"), a("008.0000 TST") },
      { N(bp111111111k), a("1010.0000 TST"), a("030.0000 TST") },
      { N(bp111111111l), a("1011.0000 TST"), a("006.0000 TST") },
      { N(bp111111111m), a("1012.0000 TST"), a("160.0000 TST") },
      { N(bp111111111n), a("1013.0000 TST"), a("000.0000 TST") },
      { N(bp111111111o), a("1014.0000 TST"), a("040.0000 TST") },
      { N(bp111111111p), a("1015.0000 TST"), a("140.0000 TST") },
      { N(bp111111111q), a("1016.0000 TST"), a("020.0000 TST") },
      { N(bp111111111r), a("1017.0000 TST"), a("007.0000 TST") },
      { N(bp111111111s), a("1018.0000 TST"), a("150.0000 TST") },
      { N(bp111111111t), a("1019.0000 TST"), a("000.0000 TST") },
      { N(bp111111111u), a("1020.0000 TST"), a("080.0000 TST") },
      { N(bp111111111v), a("1021.0000 TST"), a("060.0000 TST") },
      { N(bp111111111w), a("1022.0000 TST"), a("090.0000 TST") },
      { N(bp111111111x), a("1023.0000 TST"), a("000.0000 TST") },
      { N(bp111111111y), a("1024.0000 TST"), a("050.0000 TST") },
      { N(bp111111111z), a("1025.0000 TST"), a("070.0000 TST") },
   };

   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgvpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition, t.end_transition));
   for (auto& bp : bps) {
      t.create_account_with_resources(bp.bp, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bp.bp));
      if (bp.cpu_votes.get_amount()) {
         t.transfer(sys, bp.bp, bp.cpu_votes, sys);
         BOOST_REQUIRE_EQUAL(t.success(), t.stake(bp.bp, bp.bp, a("0.0000 TST"), bp.cpu_votes));
         BOOST_REQUIRE_EQUAL(t.success(), t.vote(bp.bp, { bp.bp }));
      }
      if (bp.pool_votes.get_amount()) {
         t.transfer(sys, bp.bp, bp.pool_votes, sys);
         BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bp.bp, bp.bp, 0, bp.pool_votes));
         BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bp.bp, vector{ bp.bp }));
         BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bp.bp, bp.bp, bp.bp));
      }
   }

   auto transition_to = [&](int i, auto& prods) {
      t.skip_to(btime(t.start_transition.slot + i * (t.end_transition.slot - t.start_transition.slot) / 21));
      t.produce_blocks(blocks_per_round);
      t.produce_blocks(blocks_per_round);
      t.produce_blocks(blocks_per_round);
      t.produce_blocks(blocks_per_round);
      BOOST_REQUIRE_EQUAL(t.active_producers(), prods);
   };

   vector<name> p0  = { N(bp111111111f), N(bp111111111g), N(bp111111111h), N(bp111111111i), N(bp111111111j),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p1  = { N(bp111111111c), N(bp111111111g), N(bp111111111h), N(bp111111111i), N(bp111111111j),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p2  = { N(bp111111111c), N(bp111111111e), N(bp111111111h), N(bp111111111i), N(bp111111111j),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p3  = { N(bp111111111b), N(bp111111111c), N(bp111111111e), N(bp111111111i), N(bp111111111j),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p7  = { N(bp111111111b), N(bp111111111c), N(bp111111111e), N(bp111111111g), N(bp111111111j),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p8  = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111e), N(bp111111111g),
                       N(bp111111111k), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p9  = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                       N(bp111111111g), N(bp111111111l), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                       N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                       N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                       N(bp111111111z) };
   vector<name> p10 = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                        N(bp111111111g), N(bp111111111h), N(bp111111111m), N(bp111111111n), N(bp111111111o),
                        N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                        N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                        N(bp111111111z) };
   vector<name> p17 = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                        N(bp111111111g), N(bp111111111h), N(bp111111111k), N(bp111111111m), N(bp111111111o),
                        N(bp111111111p), N(bp111111111q), N(bp111111111r), N(bp111111111s), N(bp111111111t),
                        N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                        N(bp111111111z) };
   vector<name> p19 = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                        N(bp111111111g), N(bp111111111h), N(bp111111111i), N(bp111111111k), N(bp111111111m),
                        N(bp111111111o), N(bp111111111p), N(bp111111111q), N(bp111111111s), N(bp111111111t),
                        N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                        N(bp111111111z) };
   vector<name> p20 = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                        N(bp111111111f), N(bp111111111g), N(bp111111111h), N(bp111111111i), N(bp111111111k),
                        N(bp111111111m), N(bp111111111o), N(bp111111111p), N(bp111111111q), N(bp111111111s),
                        N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111x), N(bp111111111y),
                        N(bp111111111z) };
   vector<name> p21 = { N(bp111111111a), N(bp111111111b), N(bp111111111c), N(bp111111111d), N(bp111111111e),
                        N(bp111111111f), N(bp111111111g), N(bp111111111h), N(bp111111111i), N(bp111111111j),
                        N(bp111111111k), N(bp111111111m), N(bp111111111o), N(bp111111111p), N(bp111111111q),
                        N(bp111111111s), N(bp111111111u), N(bp111111111v), N(bp111111111w), N(bp111111111y),
                        N(bp111111111z) };

   t.produce_blocks(100);
   BOOST_REQUIRE_EQUAL(t.active_producers(), p0);

   transition_to(0, p0);
   transition_to(1, p1);
   transition_to(2, p2);
   transition_to(3, p3);
   transition_to(4, p3); // bp111111111m switches to being selected via pool
   transition_to(5, p3); // bp111111111s switches to being selected via pool
   transition_to(6, p3); // bp111111111p switches to being selected via pool
   transition_to(7, p7);
   transition_to(8, p8);
   transition_to(9, p9);
   transition_to(10, p10);
   transition_to(11, p10); // bp111111111w switches to being selected via pool
   transition_to(12, p10); // bp111111111u switches to being selected via pool
   transition_to(13, p10); // bp111111111z switches to being selected via pool
   transition_to(14, p10); // bp111111111v switches to being selected via pool
   transition_to(15, p10); // bp111111111y switches to being selected via pool
   transition_to(16, p10); // bp111111111o switches to being selected via pool
   transition_to(17, p17);
   transition_to(18, p17); // bp111111111q switches to being selected via pool
   transition_to(19, p19);
   transition_to(20, p20);
   transition_to(21, p21);
   transition_to(22, p21);
   transition_to(23, p21);
   transition_to(100, p21);
} // transition_voting
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(transition_inflation) try {
   votepool_tester t;
   t.start_transition = time_point::from_iso_string("2020-04-10T10:00:00.000");
   t.end_transition   = time_point::from_iso_string("2020-08-10T10:00:00.000");

   double prod_rate  = .4;
   double voter_rate = .5;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                               t.end_transition, prod_rate, voter_rate));
   t.create_account_with_resources(bpa, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bpa, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, bpa, a("2.0000 TST"), sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(bpa, bpa, a("0.0000 TST"), a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.vote(bpa, { bpa }));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bpa, bpa, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bpa, vector{ bpa }));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpa, bpa, bpa));

   for (name claimer : { N(claimer1111a), N(claimer1111b), N(claimer1111c), N(claimer1111d), N(claimer1111e),
                         N(claimer1111f), N(claimer1111g) }) {
      t.create_account_with_resources(claimer, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, claimer, a("1000.0000 TST"), a("1000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(claimer));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, N(unregprod), mvo()("producer", claimer)));
   }

   auto supply    = t.get_token_supply();
   auto vpool_bal = a("1.0000 TST");
   auto bvpay_bal = a("0.0000 TST");

   auto transition_to = [&](double r, name claimer) {
      t.produce_block();
      t.skip_to(
            btime(uint32_t((uint64_t(r * (t.end_transition.slot - t.start_transition.slot) + t.start_transition.slot) +
                            blocks_per_round - 1) /
                           blocks_per_round * blocks_per_round)));
      t.produce_blocks(blocks_per_round + 1);

      auto pool_transition = t.transition(t.get_vpoolstate()["interval_start"].as<btime>(), 1.0);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(bpa, bpa));
      auto bp_pay =
            asset(pool_transition * prod_rate * supply.get_amount() / eosiosystem::rounds_per_year, symbol{ CORE_SYM });
      auto pool_pay = asset(supply.get_amount() * voter_rate * pool_transition / eosiosystem::rounds_per_year,
                            symbol{ CORE_SYM });
      bvpay_bal += bp_pay;
      vpool_bal += pool_pay;
      supply += bp_pay + pool_pay;
      BOOST_REQUIRE_EQUAL(t.get_balance(bvpay), bvpay_bal);
      BOOST_REQUIRE_EQUAL(t.get_balance(vpool), vpool_bal);
      BOOST_REQUIRE_EQUAL(t.get_vpoolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), vpool_bal);
      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);

      auto    ct                    = t.pending_time(0.5).to_time_point();
      auto    gstate                = t.get_global_state();
      auto    gstate4               = t.get_global_state4();
      auto    claim_transition      = 1.0 - t.transition(ct, 1.0);
      auto    usecs_since_last_fill = (ct - gstate["last_pervote_bucket_fill"].as<time_point>()).count();
      int64_t claim_inflation       = (claim_transition * gstate4["continuous_rate"].as<double>() *
                                 double(supply.get_amount()) * double(usecs_since_last_fill)) /
                                double(eosiosystem::useconds_per_year);
      supply += asset(claim_inflation, symbol{ CORE_SYM });
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(claimer));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, N(claimrewards), mvo()("owner", claimer)));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, N(unregprod), mvo()("producer", claimer)));
      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   };

   transition_to(-0.2, N(claimer1111a));
   transition_to(0.0, N(claimer1111b));
   transition_to(0.2, N(claimer1111c));
   transition_to(0.8, N(claimer1111d));
   transition_to(1.0, N(claimer1111e));
   transition_to(1.2, N(claimer1111f));
   transition_to(1.4, N(claimer1111g));
} // transition_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(rentbw_route_fees) try {
   auto init = [](auto& t) {
      t.create_accounts_with_resources({ reserv, prox, alice, bob, jane }, sys);
      t.transfer(sys, alice, a("1000.0000 TST"), sys);
      t.transfer(sys, bob, a("1000.0000 TST"), sys);
      t.transfer(sys, jane, a("1000.0000 TST"), sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(bob, bob, a("1.0000 TST"), a("0.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(prox, N(regproxy), mvo()("proxy", prox)("isproxy", true)));
      BOOST_REQUIRE_EQUAL(t.success(), t.vote(bob, {}, prox));

      t.start_transition = time_point::from_iso_string("2020-04-10T10:00:00.000");
      t.end_transition   = time_point::from_iso_string("2020-08-10T10:00:00.000");
      auto res           = mvo()                              //
            ("current_weight_ratio", int64_t(10000000000000)) //
            ("target_weight_ratio", int64_t(10000000000000))  //
            ("assumed_stake_weight", int64_t(1000000000000))  //
            ("target_timestamp", nullptr)                     //
            ("exponent", 1.0)                                 //
            ("decay_secs", nullptr)                           //
            ("min_price", a("100.0000 TST"))                  //
            ("max_price", a("100.0000 TST"));
      auto conf = mvo()       //
            ("net", res)      //
            ("cpu", res)      //
            ("rent_days", 30) //
            ("min_rent_fee", a("0.0001 TST"));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(sys, N(configrentbw), mvo()("args", std::move(conf))));
      t.produce_block();
   };

   auto check_fee = [](auto& t, asset rex_fee, asset pool_fee) {
      auto jane_balance  = t.get_balance(jane);
      auto rex_balance   = t.get_balance(rex);
      auto vpool_balance = t.get_balance(vpool);
      // ilog("before ${a} ${b} ${c}", ("a",t.get_balance(jane))("b",t.get_balance(rex))("c",t.get_balance(vpool)));
      BOOST_REQUIRE_EQUAL(t.success(), t.rentbw(jane, jane, 30, rentbw_percent, 0, a("1.0000 TST")));
      // ilog("after  ${a} ${b} ${c}", ("a",t.get_balance(jane))("b",t.get_balance(rex))("c",t.get_balance(vpool)));
      BOOST_REQUIRE_EQUAL(t.get_balance(jane), jane_balance - rex_fee - pool_fee);
      BOOST_REQUIRE_EQUAL(t.get_balance(rex), rex_balance + rex_fee);
      BOOST_REQUIRE_EQUAL(t.get_balance(vpool), vpool_balance + pool_fee);
   };

   // rex not enabled, no pools exist or no pools active
   {
      votepool_tester t;
      init(t);

      // no pools exist
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));

      BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      t.produce_block();

      // no pools active
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      t.skip_to(t.start_transition);
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      t.skip_to(t.end_transition);
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
   }

   // rex not enabled, pools active
   {
      votepool_tester t;
      init(t);
      BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
      check_fee(t, a("0.0000 TST"), a("1.0000 TST"));
      t.produce_block();
      t.skip_to(t.start_transition);
      check_fee(t, a("0.0000 TST"), a("1.0000 TST"));
      t.produce_block();
      t.skip_to(t.end_transition);
      check_fee(t, a("0.0000 TST"), a("1.0000 TST"));
   }

   // transition between rex and pools
   {
      votepool_tester t;
      init(t);
      BOOST_REQUIRE_EQUAL(t.success(), t.cfgvpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.unstaketorex(bob, bob, a("1.0000 TST"), a("0.0000 TST")));
      check_fee(t, a("1.0000 TST"), a("0.0000 TST"));
      t.produce_block();

      t.skip_to(t.start_transition);
      check_fee(t, a("1.0000 TST"), a("0.0000 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-05-10T10:00:00.000"));
      check_fee(t, a("0.7541 TST"), a("0.2459 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-06-10T10:00:00.000"));
      check_fee(t, a("0.5000 TST"), a("0.5000 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-07-10T10:00:00.000"));
      check_fee(t, a("0.2541 TST"), a("0.7459 TST"));
      t.produce_block();

      t.skip_to(t.end_transition);
      check_fee(t, a("0.0000 TST"), a("1.0000 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2021-07-10T10:00:00.000"));
      check_fee(t, a("0.0000 TST"), a("1.0000 TST"));
   }
} // rentbw_route_fees
FC_LOG_AND_RETHROW()

// TODO: producer pay: 50, 80/20 rule

BOOST_AUTO_TEST_SUITE_END()
