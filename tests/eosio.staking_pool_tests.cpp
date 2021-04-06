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

constexpr auto sys                 = "eosio"_n;
constexpr auto srpool               = "eosio.srpool"_n;
constexpr auto bpspay               = "eosio.bpspay"_n;
constexpr auto reserv              = "eosio.reserv"_n;
constexpr auto rex                 = "eosio.rex"_n;
constexpr auto transferstake_notif = "eosio.tstake"_n;

constexpr auto alice = "alice1111111"_n;
constexpr auto bob   = "bob111111111"_n;
constexpr auto jane  = "jane11111111"_n;
constexpr auto sue   = "sue111111111"_n;
constexpr auto tom   = "tom111111111"_n;
constexpr auto prox  = "proxy1111111"_n;
constexpr auto bpa   = "bpa111111111"_n;
constexpr auto bpb   = "bpb111111111"_n;
constexpr auto bpc   = "bpc111111111"_n;
constexpr auto bpd   = "bpd111111111"_n;

constexpr auto blocks_per_round  = eosiosystem::blocks_per_round;
constexpr auto seconds_per_round = blocks_per_round / 2;

inline constexpr int64_t rentbw_frac    = 1'000'000'000'000'000ll; // 1.0 = 10^15
inline constexpr int64_t rentbw_percent = rentbw_frac / 100;

struct sr_state_obj {
   int num_pools = 0;
};

struct voter_obj {
      name                                owner;
      std::vector<double>                 owned_shares;   // shares in each pool
      std::vector<double>                 proxied_shares; // shares in each pool delegated to this voter as a proxy
      std::vector<double>                 last_votes;     // vote weights cast the last time the vote was updated
      name                                proxy;          // the proxy set by the voter, if any
      std::vector<name>                   producers;      // the producers approved by this voter if no proxy set
      bool                                is_proxy       = false; // whether the voter is a proxy for others
      bool                                xfer_out_notif = false; // opt into outgoing transferstake notifications
      bool                                xfer_in_notif  = false; // opt into incoming transferstake notifications
   };

class pool {
   // duplicate of the staking_pool.hpp pool class because of mismatching asset
   public:
   auto shares() const { return total_shares; }
   auto bal() const { return balance; }

   void adjust(asset delta) { balance += delta; }

   // temp adjust function
   void adjust_bal(asset new_bal) { balance = new_bal; }

   double buy(asset b) {
      auto out = simulate_buy(b);
      balance += b;
      total_shares += out;
      return out;
   }

   asset sell(double s) {
      auto out = simulate_sell(s);
      balance -= out;
      total_shares -= s;
      return out;
   }

   double simulate_buy(asset b) const {
      if (!b.get_amount())
         return 0;
      if (!total_shares)
         return b.get_amount();
      else
         return (b.get_amount() * total_shares) / double(balance.get_amount());
   }

   asset simulate_sell(double s) const {
      if (!s)
         return asset{ 0, balance.get_symbol() };
      if (s >= total_shares)
         return balance;
      BOOST_REQUIRE(total_shares > 0);

      return asset(double(s) * double(balance.get_amount()) / double(total_shares), balance.get_symbol());
   }

   // the number of shares need to get d_out
   double simulate_sell(asset d_out) const {
      if (d_out == balance)
         return total_shares;
      else if (!d_out.get_amount())
         return 0;
      return (d_out.get_amount() * total_shares) / double(balance.get_amount());
   }

   private:
   asset       balance = a("0.0000 TST");
   double      total_shares = 0;
};

struct prod_pool_votes {
   std::vector<double> pool_votes;           // shares in each pool
   double              total_pool_votes = 0; // total shares in all pools, weighted by update time and pool strength
   asset               vote_pay;             // unclaimed vote pay
};

struct producers_obj {
   name                 owner;
   std::vector<double>  votes;      // shares in each pool
};

struct transferstake_notification {
   name        from;
   name        to;
   uint32_t    pool_index;
   asset       requested;
   asset       transferred_amount;
   std::string memo;
};

FC_REFLECT(transferstake_notification, (from)(to)(pool_index)(requested)(transferred_amount)(memo))

struct votepool_tester : eosio_system_tester {
   btime start_transition;
   btime end_transition;

   std::map<name, voter_obj> voter_obj_pool;
   std::vector<pool> token_pools;
   std::map<name, producers_obj> producers_table;
   // TODO mimic SC table better (or at all)
   sr_state_obj state_table;

   votepool_tester() : eosio_system_tester(setup_level::none) {
      create_accounts({ srpool, bpspay });
      basic_setup();
      create_core_token();
      deploy_contract();
      activate_chain();
   }

   // 'bp11activate' votes for self, then unvotes and unregisters
   void activate_chain() {
      create_account_with_resources("bp11activate"_n, sys);
      transfer(sys, "bp11activate"_n, a("150000000.0000 TST"), sys);
      BOOST_REQUIRE_EQUAL(success(), regproducer("bp11activate"_n));
      BOOST_REQUIRE_EQUAL(success(),
                          stake("bp11activate"_n, "bp11activate"_n, a("75000000.0000 TST"), a("75000000.0000 TST")));
      BOOST_REQUIRE_EQUAL(success(), vote("bp11activate"_n, { "bp11activate"_n }));
      BOOST_REQUIRE_EQUAL(success(), vote("bp11activate"_n, {}));
      BOOST_REQUIRE_EQUAL(success(), push_action("bp11activate"_n, "unregprod"_n, mvo()("producer", "bp11activate"_n)));
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

   void init_pool(name user) {
      voter_obj_pool[user].owner = user;
      voter_obj_pool[user].owned_shares.resize(state_table.num_pools);
      voter_obj_pool[user].proxied_shares.resize(state_table.num_pools);
      voter_obj_pool[user].last_votes.resize(state_table.num_pools);
   }

   void init_pools(std::vector<name> users, int num_pools){
      token_pools.resize(num_pools);

      for (name u : users) {
         init_pool(u);
      }
   }

   void display_pools() {
      auto chain_pools = get_poolstate()["pools"];
      for (size_t i = 0; i < chain_pools.size(); ++i) {
         auto& tok_pool = chain_pools[i]["token_pool"];
         ilog("pool: balance: ${bal} total_shares: ${shares}", ("bal",token_pools[i].bal())("shares",token_pools[i].shares()));
         ilog("pool: balance: ${bal} total_shares: ${shares}", ("bal",tok_pool["balance"].as<asset>())("shares",tok_pool["total_shares"].as<double>()));

      }
   }

   void display_voters(std::vector<name> users) {
      for (name u : users) {
         auto voter = pool_voter(u);
         if (!voter.is_null()){
         ilog("voters: ${owner}: shares: ${owned_shares} chain_shares: ${chain_shares}", 
             ("owner",voter_obj_pool[u].owner)
             ("owned_shares", voter_obj_pool[u].owned_shares)
             ("chain_shares", voter["owned_shares"].as<std::vector<double>>()));
         }
      }
   }

   void display_producers() {
      for (auto& prod : producers_table) {
         if (prod.second.votes.size() > 0) {
            for (size_t i = 0; i < prod.second.votes.size(); ++i)
               ilog("${prod} [${i}]: ${votes}", ("prod", prod.first) ("i",i) ("votes",prod.second.votes[i]));
         } else {
            ilog("${prod} has no votes yet.", ("prod", prod.first));
         }
      }
   }  

   fc::variant pool_voter(name owner) {
      vector<char> data = get_row_by_account(sys, sys, "poolvoter"_n, owner);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("pool_voter", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_poolstate() const {
      vector<char> data = get_row_by_account(sys, sys, "poolstate"_n, "poolstate"_n);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("staking_pool_state", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_total_pool_votes(name producer) const {
      vector<char> data = get_row_by_account(sys, sys, "totpoolvotes"_n, producer);
      return data.empty() ? fc::variant()
                          : abi_ser.binary_to_variant("total_pool_votes", data,
                                                      abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void check_pool_totals(const std::vector<name>& users) {
      auto                pools         = get_poolstate()["pools"];
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
      BOOST_TEST(get_balance(srpool) == total_balance);
   }

   void check_pool_votes(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters) {
      check_pool_totals(voters);

      for (auto& [prod, ppv] : pool_votes) {
         ppv.pool_votes.clear();
         ppv.pool_votes.resize(num_pools);
      }
      // TODO remove this
      for (auto voter : voters) {
         auto calc_v = voter_obj_pool[voter];

         auto v = pool_voter(voter);
         if (!v.is_null()) {
            auto prods  = v["producers"].as<vector<name>>();
            auto proxy  = v["proxy"].as<name>();
            if (proxy.to_uint64_t() && pool_voter(proxy)["is_proxy"].as<bool>())
               prods = pool_voter(proxy)["producers"].as<vector<name>>();
            for (auto prod : prods) {
               auto& ppv = pool_votes[prod];
               BOOST_REQUIRE_EQUAL(ppv.pool_votes.size(), calc_v.owned_shares.size());
               for (size_t i = 0; i < calc_v.owned_shares.size(); ++i)
                  ppv.pool_votes[i] += calc_v.owned_shares[i];
            }
         }
      }

      for (auto& [prod_name, prod] : producers_table) {
         for (size_t i = 0; i < prod.votes.size(); ++i) 
            BOOST_TEST(prod.votes[i] == get_producer_info(prod_name)["pool_votes"].as<vector<double>>()[i]);
      }
   }; // check_pool_votes

   void update_bps(int num_pools, std::map<name, prod_pool_votes>& pool_votes, const std::vector<name>& voters,
                   const std::vector<name>& bps) {
      auto state = get_poolstate();
      auto pools = state["pools"];
      BOOST_REQUIRE_EQUAL(pools.size(), num_pools);
      for (auto bp : bps) {
         // TODO remove
         auto& ppv = pool_votes[bp];
         auto& prod = producers_table[bp];
         BOOST_REQUIRE_EQUAL(ppv.pool_votes.size(), num_pools);
         double total = 0;
         for (int i = 0; i < num_pools; ++i) {
            auto token_pool = pools[i]["token_pool"];
            // if (ppv.pool_votes[i]) {
            //    int64_t sim_sell = ppv.pool_votes[i] * token_pool["balance"].as<asset>().get_amount() /
            //                       token_pool["total_shares"].as<double>();
            //    total += sim_sell * pools[i]["vote_weight"].as<double>();
               
            //    elog("${bp}: [${i}] ${x}(${pool_votes}), ${y}, ${z}, sim=${sim} => ${res}    ${calc_votes}", //
            //         ("bp", bp)("i", i)("x", ppv.pool_votes[i])              //
            //         ("y", token_pool["balance"].as<asset>().get_amount())   //
            //         ("z", token_pool["total_shares"].as<double>())          //
            //         ("sim", sim_sell)                                       //
            //         ("pool_votes", get_producer_info(bp)["pool_votes"].as<vector<double>>()[i])
            //         ("res", sim_sell * pools[i]["vote_weight"].as<double>())
            //         ("calc_votes",producers_table[bp].votes));
            // }

            if (prod.votes[i]) {
               // int64_t sim_sell = prod.votes[i] * token_pool["balance"].as<asset>().get_amount() /
               //                    token_pool["total_shares"].as<double>();
               
               total += prod.votes[i] * pools[i]["vote_weight"].as<double>();
               
               elog("${bp}: [${i}] ${calc_votes}(${pool_votes}), ${y}, ${z} => ${res}    ${x}", //
                    ("bp", bp)("i", i)("x", ppv.pool_votes[i])              //
                    ("y", token_pool["balance"].as<asset>().get_amount())   //
                    ("z", token_pool["total_shares"].as<double>())          //
                  //   ("sim", sim_sell)                                       //
                    ("pool_votes", get_producer_info(bp)["pool_votes"].as<vector<double>>()[i])
                    ("res", prod.votes[i] * pools[i]["vote_weight"].as<double>())
                    ("calc_votes",prod.votes[i]));
            }

         }
         elog("   total:         ${t} == ${votes}", ("t", total)("votes",get_total_pool_votes(bp)["votes"].as<double>()));
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

   void update_producer(name producer ) {
      producers_table[producer].owner = producer;
      producers_table[producer].votes.resize(state_table.num_pools);
   }

   action_result regproducer(const account_name& acnt, unsigned location = 0) {
      action_result r = eosio_system_tester::push_action(
            acnt, "regproducer"_n,
            mvo()("producer", acnt)("producer_key", get_public_key(acnt, "active"))("url", "")("location", location));
      update_producer(acnt);
      BOOST_REQUIRE_EQUAL(success(), r);
      return r;
   }

   // doesn't move time forward
   action_result regproducer_0_time(const account_name& prod) {
      action_result r = push_action(
            prod, "regproducer"_n,
            mvo()("producer", prod)("producer_key", get_public_key(prod, "active"))("url", "")("location", 0));   
      update_producer(prod);
      BOOST_REQUIRE_EQUAL(success(), r);
      return r;
   }

   void deposit_pool(pool& pl, double& owned_shares, asset new_amount) {
      auto new_shares = pl.buy(new_amount);
      // implement claim logic?

      owned_shares += new_shares;
   }

   asset withdraw_pool(pool& pl, double& owned_shares, asset max_requested) {
      asset balance = pl.simulate_sell(owned_shares);
      // implement claim logic?
      if (max_requested >= balance) {
         asset sold = pl.sell(owned_shares);
         owned_shares = 0;
         return sold;
      } else {
         asset available = balance;
         asset sell_amount = std::min(available, max_requested);
         double sell_shares = std::min(pl.simulate_sell(sell_amount), owned_shares);
         asset sold = pl.sell(sell_shares);
         owned_shares -= sell_shares;
         return sold;
      }
   }

   action_result cfgsrpool(name                         authorizer, //
                          const std::optional<double>& prod_rate,  //
                          const std::optional<double>& voter_rate) {
      return cfgsrpool(authorizer, nullopt, nullopt, nullopt, nullopt, nullopt, prod_rate, voter_rate);
   }

   action_result cfgsrpool(name                                        authorizer,                    //
                          const std::optional<std::vector<uint32_t>>& durations,                     //
                          const std::optional<std::vector<uint32_t>>& claim_periods,                 //
                          const std::optional<std::vector<double>>&   vote_weights,                  //
                          const std::optional<block_timestamp_type>&  begin_transition,              //
                          const std::optional<block_timestamp_type>&  end_transition,                //
                          const std::optional<double>&                prod_rate           = nullopt, //
                          const std::optional<double>&                voter_rate          = nullopt, //
                          const std::optional<uint8_t>&               max_num_pay         = nullopt, //
                          const std::optional<double>&                max_vote_ratio      = nullopt, //
                          const std::optional<asset>&                 min_transfer_create = nullopt) {
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
      fill("max_num_pay", max_num_pay);
      fill("max_vote_ratio", max_vote_ratio);
      fill("min_transfer_create", min_transfer_create);
      
      if(durations) {
         state_table.num_pools = durations->size();
         token_pools.resize(state_table.num_pools);
      }
      return push_action(authorizer, "cfgsrpool"_n, v);
   }

   action_result cfgsrpool_bp_thresholds(uint8_t max_num_pay, double max_vote_ratio) {
      return cfgsrpool(sys, nullopt, nullopt, nullopt, nullopt, nullopt, nullopt, nullopt, max_num_pay, max_vote_ratio,
                      nullopt);
   }

   action_result stake2pool(name authorizer, name owner, uint32_t pool_index, asset amount) {
      action_result r = push_action(authorizer, "stake2pool"_n, mvo()("owner", owner)("pool_index", pool_index)("amount", amount));
      if(r == success()) {
         // deposit into the pool
         if(voter_obj_pool.find(owner) == voter_obj_pool.end())
            init_pool(owner);
         voter_obj& voter = voter_obj_pool.at(owner);
         deposit_pool(token_pools[pool_index], voter.owned_shares[pool_index], amount);

         update_pool_votes(owner, voter.proxy, voter.producers);
      }
      return r;
   }

   action_result setpoolnotif(name authorizer, name owner, std::optional<bool> xfer_out_notif = nullopt,
                              std::optional<bool> xfer_in_notif = nullopt) {
      mvo  v("owner", owner);
      auto fill = [&](const char* name, auto& opt) {
         if (opt)
            v(name, *opt);
         else
            v(name, nullptr);
      };
      fill("xfer_out_notif", xfer_out_notif);
      fill("xfer_in_notif", xfer_in_notif);
      return push_action(authorizer, "setpoolnotif"_n, v);
   }

   action_result claimstake(name authorizer, name owner, uint32_t pool_index, asset requested) {
      action_result r = push_action(authorizer, "claimstake"_n,
                         mvo()("owner", owner)("pool_index", pool_index)("requested", requested));
      // TODO add logic to track these changes
      return r;
   }

   action_result transferstake(name authorizer, name from, name to, uint32_t pool_index, asset requested,
                               const std::string& memo) {
      action_result r = push_action(authorizer, "transferstake"_n,
                         mvo()("from", from)("to", to)("pool_index", pool_index)("requested", requested)("memo", memo));
      // TODO add logic to track these changes
      return r;
   }

   void transferstake_notify(name from, name to, uint32_t pool_index, asset requested, asset transferred_amount,
                             const std::string& memo, bool notif_from, bool notif_to) {
      mvo v;
      v("from", from)("to", to)("pool_index", pool_index)("requested", requested)("memo", memo);
      auto traces = base_tester::push_action(sys, "transferstake"_n, from, v, 1, 0);
      int  pos    = 1;

      auto check_notif = [&](name receiver) {
         BOOST_REQUIRE_LT(pos, traces->action_traces.size());
         auto& at = traces->action_traces[pos++];
         BOOST_TEST(at.receiver == receiver);
         BOOST_TEST(at.act.account == receiver);
         BOOST_TEST(at.act.name == transferstake_notif);
         auto n = fc::raw::unpack<transferstake_notification>(at.act.data);
         BOOST_TEST(n.from == from);
         BOOST_TEST(n.to == to);
         BOOST_TEST(n.pool_index == pool_index);
         BOOST_TEST(n.requested == requested);
         BOOST_TEST(n.transferred_amount == transferred_amount);
         BOOST_TEST(n.memo == memo);
      };

      if (notif_from)
         check_notif (from);
      if (notif_to)
         check_notif (to);
      BOOST_TEST(pos == traces->action_traces.size());
   }

   action_result upgradestake(name authorizer, name owner, uint32_t from_pool_index, uint32_t to_pool_index,
                              asset requested) {      
      action_result r = push_action(authorizer, "upgradestake"_n,
                         mvo()("owner", owner)("from_pool_index", from_pool_index)("to_pool_index", to_pool_index)(
                               "requested", requested));

      if(r == success()) {
         voter_obj& voter = voter_obj_pool.at(owner);
         // withdraw from_pool_index
         asset transferred_amount = withdraw_pool(token_pools[from_pool_index], voter.owned_shares[from_pool_index], requested);

         // deposit from_pool_index
         deposit_pool(token_pools[to_pool_index], voter.owned_shares[to_pool_index], transferred_amount);
         update_pool_votes(owner, voter.proxy, voter.producers);
      }
      return r;
   }


   void add_pool_votes(producers_obj& prod, const std::vector<double>& deltas) {
      // resize just in case
      prod.votes.resize(deltas.size());
      for (size_t i = 0; i < deltas.size(); ++i)
         prod.votes[i] += deltas[i];
   }

   void sub_pool_votes(producers_obj& prod, const std::vector<double>& deltas) {
      for (size_t i = 0; i < deltas.size(); ++i)
         prod.votes[i] -= deltas[i];
   }

   void add_proxied_shares(voter_obj& proxy, std::vector<double> deltas) {
      for (size_t i = 0; i < deltas.size(); ++i)
         proxy.proxied_shares[i] += deltas[i];
   }

   void sub_proxied_shares(voter_obj& proxy, std::vector<double> deltas) {
      for (size_t i = 0; i < deltas.size(); ++i)
         proxy.proxied_shares[i] -= deltas[i];
   }

   void update_pool_proxy(voter_obj& voter) {
      std::vector<double> new_pool_votes = voter.owned_shares;
      if (voter.is_proxy) 
         for (size_t i = 0; i < new_pool_votes.size(); ++i) 
            new_pool_votes[i] += voter.proxied_shares[i];
      
      if (voter.proxy) {
         auto& proxy = voter_obj_pool.at(voter.proxy);
         sub_proxied_shares(proxy, voter.last_votes);
         add_proxied_shares(proxy, new_pool_votes);
         update_pool_proxy(proxy);
      } else {
         for (auto acnt : voter.producers) {
            auto& prod = producers_table.at(acnt);
            sub_pool_votes(prod, voter.last_votes);
            add_pool_votes(prod, new_pool_votes);
         }
      }
      voter.last_votes = std::move(new_pool_votes);
   }

   void update_pool_votes(const name& voter_name, const name& proxy, const std::vector<name>& producers) {
      auto& voter = voter_obj_pool.at(voter_name);

      std::vector<double> new_pool_votes = voter.owned_shares;
      if (voter.is_proxy)
         for (size_t i = 0; i < new_pool_votes.size(); ++i)
            new_pool_votes[i] += voter.proxied_shares[i];

      struct producer_change {
         bool old_vote = false;
         bool new_vote = false;
      };
      std::map<name, producer_change> producer_changes;

      if (voter.proxy) {
         auto& old_proxy = voter_obj_pool.at(voter.proxy);
         sub_proxied_shares(old_proxy, voter.last_votes);
         update_pool_proxy(old_proxy);
      } else {
         for ( const auto& p : voter.producers) {
            producer_changes[p].old_vote = true;
         }
      }

      if (proxy) {
         auto& new_proxy = voter_obj_pool.at(proxy);
         add_proxied_shares(new_proxy, new_pool_votes);
         update_pool_proxy(new_proxy);
      } else {
         for (const auto& p : producers)
            producer_changes[p].new_vote = true;
      }

      for (const auto& pc : producer_changes) {
         auto& prod = producers_table.at(pc.first);
         if (pc.second.old_vote)
            sub_pool_votes(prod, voter.last_votes);
         if (pc.second.new_vote) 
            add_pool_votes(prod, new_pool_votes);
      }

      // update fields
      voter.proxy = proxy;
      voter.producers = producers;
      voter.last_votes = std::move(new_pool_votes);
   }

   action_result votewithpool(name authorizer, name voter, name proxy, const vector<name>& producers) {
      // push action
      action_result r = push_action(authorizer, "votewithpool"_n, mvo()("voter", voter)("proxy", proxy)("producers", producers));
      // process own state if the action was successful
      if (r == success()) {
         update_pool_votes(voter, proxy, producers);
      }

      return r;
   }

   action_result votewithpool(name voter, name proxy) { return votewithpool(voter, voter, proxy, {}); }

   action_result votewithpool(name voter, const vector<name>& producers) {
      return votewithpool(voter, voter, {}, producers);
   }

   action_result votewithpool(name voter) { return votewithpool(voter, voter, {}, {}); }

   action_result regpoolproxy(name authorizer, name proxy, bool isproxy) {
      // update voter pool
      action_result r = push_action(authorizer, "regpoolproxy"_n, mvo()("proxy", proxy)("isproxy", isproxy));
      if(r == success()) {
         voter_obj_pool[proxy].is_proxy = isproxy;
         update_pool_proxy(voter_obj_pool[proxy]);
      }
      return r;
   }

   action_result regpoolproxy(name proxy, bool isproxy) { 
      return regpoolproxy(proxy, proxy, isproxy); 
   }

   action_result updatevotes(name authorizer, name user, name producer) {
      return push_action(authorizer, "updatevotes"_n, mvo()("user", user)("producer", producer));
   }

   action_result updatepay(name authorizer, name user) {     
      action_result r = push_action(authorizer, "updatepay"_n, mvo()("user", user));
      
      if(r == success()) {
         // adjust balance from state 
         // TODO calculate using own values ??
         auto pools = get_poolstate()["pools"];
         for (size_t i = 0; i < pools.size(); ++i) {
            token_pools[i].adjust_bal(pools[i]["token_pool"]["balance"].as<asset>());
         }
      }
      return r;
   }

   action_result claimvotepay(name authorizer, name producer) {
      return push_action(authorizer, "claimvotepay"_n, mvo()("producer", producer));
   }

   action_result rentbw(const name& payer, const name& receiver, uint32_t days, int64_t net_frac, int64_t cpu_frac,
                        const asset& max_payment) {
      return push_action(payer, "powerup"_n,
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

BOOST_AUTO_TEST_CASE(cfgsrpool) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice }, sys);

   BOOST_REQUIRE_EQUAL("missing authority of eosio",
                       t.cfgsrpool(alice, { { 1, 2, 3, 4 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations is required on first use of cfgsrpool"),
                       t.cfgsrpool(sys, nullopt, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods is required on first use of cfgsrpool"),
                       t.cfgsrpool(sys, { { 1 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights is required on first use of cfgsrpool"),
                       t.cfgsrpool(sys, { { 2 } }, { { 1 } }, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition is required on first use of cfgsrpool"),
                       t.cfgsrpool(sys, { { 2 } }, { { 1 } }, { { 1 } }, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("end_transition is required on first use of cfgsrpool"),
                       t.cfgsrpool(sys, { { 2 } }, { { 1 } }, { { 1 } }, btime(), nullopt));
   BOOST_REQUIRE_EQUAL(
         t.wasm_assert_msg("durations is empty"),
         t.cfgsrpool(sys, std::vector<uint32_t>{}, std::vector<uint32_t>{}, std::vector<double>{}, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgsrpool(sys, { { 1 } }, std::vector<uint32_t>{}, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgsrpool(sys, { { 1, 2 } }, { { 1, 3, 4 } }, { { 1, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("mismatched vector sizes"),
                       t.cfgsrpool(sys, { { 10, 20 } }, { { 1, 2 } }, { { 1, 2, 3 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("duration must be positive"),
                       t.cfgsrpool(sys, { { 0 } }, { { 1 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be positive"),
                       t.cfgsrpool(sys, { { 1 } }, { { 0 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgsrpool(sys, { { 2 } }, { { 1 } }, { { -1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weight must be positive"),
                       t.cfgsrpool(sys, { { 2 } }, { { 1 } }, { { 0 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgsrpool(sys, { { 1 } }, { { 1 } }, { { 1 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_period must be less than duration"),
                       t.cfgsrpool(sys, { { 10, 20 } }, { { 9, 20 } }, { { 1, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgsrpool(sys, { { 2, 3, 4, 3 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations must be increasing"),
                       t.cfgsrpool(sys, { { 2, 3, 4, 4 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods must be non-decreasing"),
                       t.cfgsrpool(sys, { { 3, 4, 5, 6 } }, { { 2, 2, 2, 1 } }, { { 1, 2, 3, 4 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights must be non-decreasing"),
                       t.cfgsrpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 1, 1 } }, { { 1, 2, 3, 2 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition > end_transition"),
                       t.cfgsrpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } },
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.000")), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("begin_transition > end_transition"),
                       t.cfgsrpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } },
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.500")),
                                  btime(time_point::from_iso_string("2020-01-01T00:00:18.000"))));
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 2, 2, 3 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("durations can't change"),
                       t.cfgsrpool(sys, { { 1, 2, 3 } }, nullopt, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim_periods can't change"),
                       t.cfgsrpool(sys, nullopt, { { 1, 2, 3 } }, nullopt, nullopt, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("vote_weights can't change"),
                       t.cfgsrpool(sys, nullopt, nullopt, { { 1, 2, 3 } }, nullopt, nullopt));

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, 0, .999));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, .999, 0));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgsrpool(sys, -.001, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("prod_rate out of range"), t.cfgsrpool(sys, 1, nullopt));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgsrpool(sys, nullopt, -.001));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("voter_rate out of range"), t.cfgsrpool(sys, nullopt, 1));
} // cfgsrpool
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(checks) try {
   votepool_tester t;
   t.create_accounts_with_resources({ alice, bob }, sys);

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.stake2pool(alice, bob, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"), t.stake2pool(alice, alice, 0, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.setpoolnotif(alice, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"), t.setpoolnotif(alice, alice));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.claimstake(alice, bob, 0, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"), t.claimstake(alice, alice, 0, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111",
                       t.transferstake(alice, bob, alice, 0, a("1.0000 TST"), "memo"));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("memo has more than 256 bytes"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 TST"), std::string(257, 'x')));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from = to"),
                       t.transferstake(alice, alice, alice, 0, a("1.0000 TST"), std::string(256, 'x')));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("invalid account"),
                       t.transferstake(alice, alice, "oops"_n, 0, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"),
                       t.transferstake(alice, alice, bob, 0, a("1.0000 TST"), ""));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.upgradestake(alice, bob, 0, 1, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"),
                       t.upgradestake(alice, alice, 0, 1, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL("missing authority of bob111111111", t.updatepay(alice, bob));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("staking pools not configured"), t.updatepay(alice, alice));
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 2, 3, 4, 5 } }, { { 1, 1, 3, 3 } }, { { 1, 1, 1, 1 } }, btime(), btime()));

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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested amount is too small to automatically create pool_voter record"),
                       t.transferstake(alice, alice, bob, 0, a("0.9999 TST"), ""));

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
                       t.votewithpool(alice, { "a"_n, "b"_n, "c"_n, "d"_n,  "e"_n,  "f"_n,  "g"_n,  "h"_n,  "i"_n, "j"_n, "k"_n,
                                               "l"_n, "m"_n, "n"_n, "o"_n,  "p"_n,  "q"_n,  "r"_n,  "s"_n,  "t"_n, "u"_n, "v"_n,
                                               "w"_n, "x"_n, "y"_n, "za"_n, "zb"_n, "zc"_n, "zd"_n, "ze"_n, "zf"_n }));
} // checks
FC_LOG_AND_RETHROW()

// Without inflation, 1.0 share = 0.0001 TST
BOOST_AUTO_TEST_CASE(no_inflation) try {
   votepool_tester   t;
   std::vector<name> users = { alice, bob, jane, sue };
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
   t.create_accounts_with_resources(users, sys);
   t.create_account_with_resources(tom, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, sue, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, tom, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);
   t.transfer(sys, sue, a("1000.0000 TST"), sys);
   t.transfer(sys, tom, a("1000.0000 TST"), sys);
   
   // TODO initialize better
   t.init_pools(users, 2);

   t.check_pool_totals(users);

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("1.0000 TST")));
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.pool_voter(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("2.0000 TST")));
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 2'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 2'0000.0 })),               //
                           t.pool_voter(bob));

   // Increasing stake at the same time as the original; next_claim doesn't move

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, a("0.5000 TST")));
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'5000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'5000.0, 0.0 })),              //
                           t.pool_voter(alice));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("1.0000 TST")));
   t.check_pool_totals(users);
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
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(53), btime() })) //
                           ("owned_shares", vector({ 2'2500.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 2'2500.0, 0.0 })),              //
                           t.pool_voter(alice));

   // stake-weighting next_claim: (240s, 3'0000.0), (256s, 6'0000.0) => (250.5s, 9'0000.0)
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, a("6.0000 TST")));
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                      //
                           ("next_claim", vector({ btime(), t.pending_time(250.5) })) //
                           ("owned_shares", vector({ 0.0, 9'0000.0 }))                //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                   //
                           ("last_votes", vector({ 0.0, 9'0000.0 })),                 //
                           t.pool_voter(bob));

   // Move time forward 52.5s (1 block before alice may claim)
   t.produce_blocks(105);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("claim too soon"), t.claimstake(alice, alice, 0, a("1.0000 TST")));
   t.check_pool_totals(users);

   // 2.2500 * 64/1024 ~= 0.1406
   t.produce_block();
   auto alice_bal = t.get_balance(alice);
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("withdrawing 0"), t.claimstake(alice, alice, 1, a("10000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.claimstake(alice, alice, 0, a("10000.0000 TST")));
   t.check_pool_totals(users);
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
   t.check_pool_totals(users);
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

   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested amount is too small to automatically create pool_voter record"),
                       t.transferstake(bob, bob, jane, 1, a("0.9999 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("from pool_voter record missing"),
                       t.transferstake(jane, jane, bob, 1, a("1.0000 TST"), ""));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("pool_voter record missing"),
                       t.upgradestake(jane, jane, 0, 1, a("1.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("transferred 0"), t.upgradestake(bob, bob, 0, 1, a("1.0000 TST")));

   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ 1'0000.0, 0.0 }))             //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ 1'0000.0, 0.0 })),              //
                           t.pool_voter(jane));

   // transfer bob -> jane. bob's next_claim doesn't change. jane's next_claim is fresh.
   t.transferstake_notify(bob, jane, 1, a("4.0000 TST"), a("4.0000 TST"), "", false, false);
   t.check_pool_totals(users);
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
   BOOST_REQUIRE_EQUAL(t.success(), t.setpoolnotif(bob, bob, nullopt, true));
   t.transferstake_notify(jane, bob, 1, a("2.0000 TST"), a("2.0000 TST"), "", false, true);
   t.check_pool_totals(users);
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
   BOOST_REQUIRE_EQUAL(t.success(), t.setpoolnotif(jane, jane, true, nullopt));
   BOOST_REQUIRE_EQUAL(t.success(), t.setpoolnotif(bob, bob, nullopt, false));
   t.transferstake_notify(jane, bob, 1, a("1.0000 TST"), a("1.0000 TST"), "", true, false);
   t.check_pool_totals(users);
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
   t.check_pool_totals(users);
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

   // transfer sue -> tom; sue pays for tom's new record
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("requested amount is too small to automatically create pool_voter record"),
                       t.transferstake(sue, sue, tom, 1, a("0.9999 TST"), ""));
   users.push_back(tom);
   t.check_pool_totals(users);
   BOOST_REQUIRE_EQUAL(t.success(), t.setpoolnotif(sue, sue, true, nullopt));
   BOOST_REQUIRE_EQUAL(t.success(), t.setpoolnotif(tom, tom, nullopt, true));
   t.transferstake_notify(sue, tom, 1, a("1.0000 TST"), a("1.0000 TST"), "", true, true);
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                                 //
                           ("next_claim", vector({ t.pending_time(16), t.pending_time(217.5) })) //
                           ("owned_shares", vector({ 0'5000.0, 1'5000.0 }))                      //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                              //
                           ("last_votes", vector({ 0'5000.0, 1'5000.0 })),                       //
                           t.pool_voter(sue));
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, 1'0000.0 }))              //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, 1'0000.0 })),               //
                           t.pool_voter(tom));
} // no_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(pool_inflation) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("1000.0000 TST"), a("1000.0000 TST")));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("1000.0000 TST"), a("1000.0000 TST")));
   t.transfer(sys, alice, a("1000.0000 TST"), sys);
   t.transfer(sys, bob, a("1000.0000 TST"), sys);
   t.transfer(sys, jane, a("1000.0000 TST"), sys);

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:18.000"));

   // TODO initialize better
   t.init_pools(users, num_pools);

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_poolstate());
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
                           t.get_poolstate());
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(alice, alice));

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);

   // First interval is partial
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 179),            //
                           t.get_poolstate());

   t.produce_to(interval_start.to_time_point() + fc::seconds(seconds_per_round));
   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 179),            //
                           t.get_poolstate());

   t.produce_block();
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", 0.0)                  //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_poolstate());

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));

   // unpaid_blocks doesn't accumulate
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", 0.0)                  //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_poolstate());

   auto supply = t.get_token_supply();
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("already processed pay for this time interval"), t.updatepay(bob, bob));

   REQUIRE_MATCHING_OBJECT(mvo()                              //
                           ("prod_rate", 0.0)                 //
                           ("voter_rate", 0.0)                //
                           ("interval_start", interval_start) //
                           ("unpaid_blocks", 0),              //
                           t.get_poolstate());

   // inflation is 0
   BOOST_REQUIRE_EQUAL(supply, t.get_token_supply());
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), a("0.0000 TST"));

   // enable voter pool inflation
   double rate = 0.5;
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, nullopt, rate));

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_poolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));

   // pools can't receive inflation since users haven't bought into them yet
   BOOST_REQUIRE_EQUAL(supply, t.get_token_supply());
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), a("0.0000 TST"));

   // alice buys into pool 0
   auto   alice_bought = a("1.0000 TST");
   double alice_shares = 1'0000.0;
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(alice, alice, 0, alice_bought));
   auto pool_0_balance = alice_bought;
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                   //
                           ("next_claim", vector({ t.pending_time(64), btime() })) //
                           ("owned_shares", vector({ alice_shares, 0.0 }))         //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                //
                           ("last_votes", vector({ alice_shares, 0.0 })),          //
                           t.pool_voter(alice));
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), a("0.0000 TST"));

   // produce inflation

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_poolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   auto per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   supply += per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), a("0.0000 TST"));

   // bob buys into pool 1
   auto   bob_bought = a("2.0000 TST");
   double bob_shares = 2'0000.0;
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bob, bob, 1, bob_bought));
   auto pool_1_balance = bob_bought;
   t.check_pool_totals(users);
   REQUIRE_MATCHING_OBJECT(mvo()                                                    //
                           ("next_claim", vector({ btime(), t.pending_time(256) })) //
                           ("owned_shares", vector({ 0.0, bob_shares }))            //
                           ("proxied_shares", vector({ 0.0, 0.0 }))                 //
                           ("last_votes", vector({ 0.0, bob_shares })),             //
                           t.pool_voter(bob));
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);

   // produce inflation
   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   REQUIRE_MATCHING_OBJECT(mvo()                                //
                           ("prod_rate", 0.0)                   //
                           ("voter_rate", rate)                 //
                           ("interval_start", interval_start)   //
                           ("unpaid_blocks", blocks_per_round), //
                           t.get_poolstate());
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));

   // check inflation
   per_pool_inflation =
         asset(supply.get_amount() * rate / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   pool_1_balance += per_pool_inflation;
   supply = supply + per_pool_inflation + per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), pool_0_balance + pool_1_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);

   // alice made a profit
   auto alice_approx_sell_shares = alice_shares * 64 / 1024; // easier formula, but rounds different from actual
   auto alice_sell_shares        = 624.9996257869700;        // calculated by contract
   BOOST_REQUIRE(abs(alice_sell_shares - alice_approx_sell_shares) < 0.001);
   auto alice_returned_funds =
         asset(pool_0_balance.get_amount() * alice_sell_shares / alice_shares, symbol{ CORE_SYM });
   auto alice_bal = t.get_balance(alice);
   BOOST_REQUIRE_EQUAL(t.success(), t.claimstake(alice, alice, 0, a("10000.0000 TST")));
   t.check_pool_totals(users);
   alice_shares -= alice_sell_shares;
   alice_bal += alice_returned_funds;
   pool_0_balance -= alice_returned_funds;
   BOOST_REQUIRE_EQUAL(t.get_balance(alice), alice_bal);
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), pool_0_balance + pool_1_balance);
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
                           t.get_poolstate());

   // check inflation with missed blocks
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   auto pay_scale = pow((double)(blocks_per_round - 30) / blocks_per_round, 10);
   per_pool_inflation =
         asset(supply.get_amount() * rate * pay_scale / eosiosystem::rounds_per_year / num_pools, symbol{ CORE_SYM });
   pool_0_balance += per_pool_inflation;
   pool_1_balance += per_pool_inflation;
   supply = supply + per_pool_inflation + per_pool_inflation;
   BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   BOOST_REQUIRE_EQUAL(t.get_balance(srpool), pool_0_balance + pool_1_balance);
   BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), a("0.0000 TST"));
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), pool_0_balance);
   BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(1)]["token_pool"]["balance"].as<asset>(), pool_1_balance);
} // pool_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(prod_inflation) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bpa, bpb, bpc };
   int               num_pools = 2;
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.0 } }, btime(), btime()));
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
   auto   bpspay_bal    = a("0.0000 TST");
   auto   bpa_vote_pay = a("0.0000 TST");
   auto   bpb_vote_pay = a("0.0000 TST");
   auto   bpc_vote_pay = a("0.0000 TST");
   double bpa_factor   = 0.0;
   double bpb_factor   = 0.0;
   double bpc_factor   = 0.0;

   auto check_poolstate = [&](uint32_t unpaid_blocks) {
      REQUIRE_MATCHING_OBJECT(mvo()                              //
                              ("prod_rate", prod_rate)           //
                              ("voter_rate", 0.0)                //
                              ("interval_start", interval_start) //
                              ("unpaid_blocks", unpaid_blocks),  //
                              t.get_poolstate());
   };

   auto check_vote_pay = [&](uint32_t unpaid_blocks = blocks_per_round) {
      check_poolstate(unpaid_blocks);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(alice, alice));
      check_poolstate(0);
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
         // ilog("${bp} pay: ${x} actual: ${actual}", ("bp", bp)("x", pay)("actual",actual));
         supply += pay;
         bpspay_bal += pay;
         bp_vote_pay += pay;
         BOOST_REQUIRE_EQUAL(actual, bp_vote_pay);
      };
      check_pay(bpa, bpa_vote_pay, bpa_factor);
      check_pay(bpb, bpb_vote_pay, bpb_factor);
      check_pay(bpc, bpc_vote_pay, bpc_factor);

      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
      BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), bpspay_bal);
   };

   auto claimvotepay = [&](auto bp, auto& vote_pay) {
      auto bal = t.get_balance(bp);
      BOOST_REQUIRE_EQUAL(t.get_total_pool_votes(bp)["vote_pay"].template as<asset>(), vote_pay);
      BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), bpspay_bal);
      BOOST_REQUIRE_EQUAL(t.success(), t.claimvotepay(bp, bp));
      BOOST_REQUIRE_EQUAL(t.get_total_pool_votes(bp)["vote_pay"].template as<asset>(), a("0.0000 TST"));
      bpspay_bal -= vote_pay;
      BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), bpspay_bal);
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
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, prod_rate, nullopt));
   next_interval();
   check_vote_pay();

   // manually update bpa, bpb votes; they'll be automatically counted from now on
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("unknown producer"), t.updatevotes(alice, alice, alice));
   BOOST_REQUIRE_EQUAL("missing authority of bpa111111111", t.updatevotes(alice, bpa, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(alice, alice, bpa));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpb, bpb, bpb));
   bpa_factor = 0.333333333;
   bpb_factor = 0.333333333;
   next_interval();
   check_vote_pay();

   // bpc joins
   BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bpc, bpc, bpc));
   bpc_factor = 0.333333333;
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

BOOST_AUTO_TEST_CASE(prod_pay_cutoff) try {
   votepool_tester t;

   struct bp_votes {
      name  bp;
      asset pool_votes;
      asset vote_pay{};
   };

   vector<bp_votes> bps{
      { "bp111111111a"_n, a("59.0000 TST") }, //
      { "bp111111111b"_n, a("58.0000 TST") }, //
      { "bp111111111c"_n, a("57.0000 TST") }, //
      { "bp111111111d"_n, a("56.0000 TST") }, //
      { "bp111111111e"_n, a("55.0000 TST") }, //
      { "bp111111111f"_n, a("54.0000 TST") }, //
      { "bp111111111g"_n, a("53.0000 TST") }, //
      { "bp111111111h"_n, a("52.0000 TST") }, //
      { "bp111111111i"_n, a("51.0000 TST") }, //
      { "bp111111111j"_n, a("50.0000 TST") }, //
      { "bp111111111k"_n, a("49.0000 TST") }, //
      { "bp111111111l"_n, a("48.0000 TST") }, //
      { "bp111111111m"_n, a("47.0000 TST") }, //
      { "bp111111111n"_n, a("46.0000 TST") }, //
      { "bp111111111o"_n, a("45.0000 TST") }, //
      { "bp111111111p"_n, a("44.0000 TST") }, //
      { "bp111111111q"_n, a("43.0000 TST") }, //
      { "bp111111111r"_n, a("42.0000 TST") }, //
      { "bp111111111s"_n, a("41.0000 TST") }, //
      { "bp111111111t"_n, a("40.0000 TST") }, //
      { "bp111111111u"_n, a("39.0000 TST") }, //
      { "bp111111111v"_n, a("38.0000 TST") }, //
      { "bp111111111w"_n, a("37.0000 TST") }, //
      { "bp111111111x"_n, a("36.0000 TST") }, //
      { "bp111111111y"_n, a("35.0000 TST") }, //
      { "bp111111111z"_n, a("34.0000 TST") }, //
      { "bp111111112a"_n, a("33.0000 TST") }, //
      { "bp111111112b"_n, a("32.0000 TST") }, //
      { "bp111111112c"_n, a("31.0000 TST") }, //
      { "bp111111112d"_n, a("30.0000 TST") }, //
      { "bp111111112e"_n, a("29.0000 TST") }, //
      { "bp111111112f"_n, a("28.0000 TST") }, //
      { "bp111111112g"_n, a("27.0000 TST") }, //
      { "bp111111112h"_n, a("26.0000 TST") }, //
      { "bp111111112i"_n, a("25.0000 TST") }, //
      { "bp111111112j"_n, a("24.0000 TST") }, //
      { "bp111111112k"_n, a("23.0000 TST") }, //
      { "bp111111112l"_n, a("22.0000 TST") }, //
      { "bp111111112m"_n, a("21.0000 TST") }, //
      { "bp111111112n"_n, a("20.0000 TST") }, //
      { "bp111111112o"_n, a("19.0000 TST") }, //
      { "bp111111112p"_n, a("18.0000 TST") }, //
      { "bp111111112q"_n, a("17.0000 TST") }, //
      { "bp111111112r"_n, a("16.0000 TST") }, //
      { "bp111111112s"_n, a("15.0000 TST") }, //
      { "bp111111112t"_n, a("14.0000 TST") }, //
      { "bp111111112u"_n, a("13.0000 TST") }, //
      { "bp111111112v"_n, a("12.0000 TST") }, //
      { "bp111111112w"_n, a("11.0000 TST") }, //
      { "bp111111112x"_n, a("10.0000 TST") }, //
      { "bp111111112y"_n, a("09.0000 TST") }, //
      { "bp111111112z"_n, a("08.0000 TST") }, //
   };

   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, btime(), btime()));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, 0.5, nullopt));

   for (auto& bp : bps) {
      t.create_account_with_resources(bp.bp, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bp.bp, a("1000.0000 TST"), a("1000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bp.bp));
      t.transfer(sys, bp.bp, bp.pool_votes, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(bp.bp, bp.bp, 0, bp.pool_votes));
      BOOST_REQUIRE_EQUAL(t.success(), t.votewithpool(bp.bp, vector{ bp.bp }));
      BOOST_REQUIRE_EQUAL(t.success(), t.updatevotes(bp.bp, bp.bp, bp.bp));
   }

   int64_t total_votes = 0;
   for (auto& bp : bps)
      total_votes += bp.pool_votes.get_amount();

   t.produce_blocks(blocks_per_round);
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(sys, sys));

   auto check_thresholds = [&](uint8_t max_num_pay, double max_vote_ratio) {
      t.cfgsrpool_bp_thresholds(max_num_pay, max_vote_ratio);
      for (auto& bp : bps)
         bp.vote_pay = t.get_total_pool_votes(bp.bp)["vote_pay"].template as<asset>();

      t.produce_blocks(blocks_per_round);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(sys, sys));

      int64_t  votes              = 0;
      unsigned expected_num_payed = 0;
      for (expected_num_payed = 0; expected_num_payed < max_num_pay && votes < total_votes * max_vote_ratio;
           ++expected_num_payed)
         votes += bps[expected_num_payed].pool_votes.get_amount();

      for (unsigned i = 0; i < bps.size(); ++i) {
         bool is_payed = t.get_total_pool_votes(bps[i].bp)["vote_pay"].template as<asset>() != bps[i].vote_pay;
         BOOST_REQUIRE_EQUAL(is_payed, i < expected_num_payed);
      }
   };

   check_thresholds(10, 1.0);
   check_thresholds(10, 0.1);
   check_thresholds(50, 0.1);
   check_thresholds(50, 0.8);
   check_thresholds(50, 1.0);
} // prod_pay_cutoff
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(voting, *boost::unit_test::tolerance(1e-8)) try {
   votepool_tester   t;
   std::vector<name> users     = { alice, bob, jane, sue, bpa, bpb, bpc, bpd };
   int               num_pools = 2;
   t.create_accounts_with_resources(users, sys);
   BOOST_REQUIRE_EQUAL(t.success(), t.regproducer_0_time(bpd));
   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 1024, 2048 } }, { { 64, 256 } }, { { 1.0, 1.5 } }, btime(), btime()));
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

   t.init_pools(users, num_pools);

   std::map<name, prod_pool_votes> pool_votes;
   pool_votes[bpb];
   pool_votes[bpc];

   btime interval_start(time_point::from_iso_string("2020-01-01T00:00:18.000"));
   // Go to first whole interval
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(120'500));
   interval_start = interval_start.to_time_point() + fc::seconds(120);

   // inflate pool 1
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, nullopt, 0.5));
   BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 1, a("1.0000 TST")));
   // t.display_voters({ alice, bob, jane, sue});

   interval_start = interval_start.to_time_point() + fc::seconds(seconds_per_round);
   t.produce_to(interval_start.to_time_point() + fc::milliseconds(500));
   BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(jane, jane));
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, nullopt, 0.0));

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
   BOOST_REQUIRE_EQUAL(t.success(), t.push_action(bpa, "unregprod"_n, mvo()("producer", bpa)));
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
   BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("proxy not found"), t.votewithpool(alice, "unknownaccnt"_n));
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
   // t.display_voters({ alice, bob, jane, sue});
   // t.display_pools();
   // t.display_producers();
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
      { "bp111111111a"_n, a("1000.0000 TST"), a("120.0000 TST") },
      { "bp111111111b"_n, a("1001.0000 TST"), a("170.0000 TST") },
      { "bp111111111c"_n, a("1002.0000 TST"), a("190.0000 TST") },
      { "bp111111111d"_n, a("1003.0000 TST"), a("110.0000 TST") },
      { "bp111111111e"_n, a("1004.0000 TST"), a("180.0000 TST") },
      { "bp111111111f"_n, a("1005.0000 TST"), a("009.0000 TST") },
      { "bp111111111g"_n, a("1006.0000 TST"), a("130.0000 TST") },
      { "bp111111111h"_n, a("1007.0000 TST"), a("100.0000 TST") },
      { "bp111111111i"_n, a("1008.0000 TST"), a("010.0000 TST") },
      { "bp111111111j"_n, a("1009.0000 TST"), a("008.0000 TST") },
      { "bp111111111k"_n, a("1010.0000 TST"), a("030.0000 TST") },
      { "bp111111111l"_n, a("1011.0000 TST"), a("006.0000 TST") },
      { "bp111111111m"_n, a("1012.0000 TST"), a("160.0000 TST") },
      { "bp111111111n"_n, a("1013.0000 TST"), a("000.0000 TST") },
      { "bp111111111o"_n, a("1014.0000 TST"), a("040.0000 TST") },
      { "bp111111111p"_n, a("1015.0000 TST"), a("140.0000 TST") },
      { "bp111111111q"_n, a("1016.0000 TST"), a("020.0000 TST") },
      { "bp111111111r"_n, a("1017.0000 TST"), a("007.0000 TST") },
      { "bp111111111s"_n, a("1018.0000 TST"), a("150.0000 TST") },
      { "bp111111111t"_n, a("1019.0000 TST"), a("000.0000 TST") },
      { "bp111111111u"_n, a("1020.0000 TST"), a("080.0000 TST") },
      { "bp111111111v"_n, a("1021.0000 TST"), a("060.0000 TST") },
      { "bp111111111w"_n, a("1022.0000 TST"), a("090.0000 TST") },
      { "bp111111111x"_n, a("1023.0000 TST"), a("000.0000 TST") },
      { "bp111111111y"_n, a("1024.0000 TST"), a("050.0000 TST") },
      { "bp111111111z"_n, a("1025.0000 TST"), a("070.0000 TST") },
   };

   BOOST_REQUIRE_EQUAL(t.success(),
                       t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition, t.end_transition));
   unsigned loc = 0;
   for (auto& bp : bps) {
      t.create_account_with_resources(bp.bp, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(bp.bp, loc++));
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

   vector<name> p0  = { "bp111111111f"_n, "bp111111111g"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111j"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p1  = { "bp111111111c"_n, "bp111111111g"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111j"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p2  = { "bp111111111c"_n, "bp111111111e"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111j"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p3  = { "bp111111111b"_n, "bp111111111c"_n, "bp111111111e"_n, "bp111111111i"_n, "bp111111111j"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p7  = { "bp111111111b"_n, "bp111111111c"_n, "bp111111111e"_n, "bp111111111g"_n, "bp111111111j"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p8  = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111e"_n, "bp111111111g"_n,
                       "bp111111111k"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p9  = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                       "bp111111111g"_n, "bp111111111l"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                       "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                       "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                       "bp111111111z"_n };
   vector<name> p10 = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                        "bp111111111g"_n, "bp111111111h"_n, "bp111111111m"_n, "bp111111111n"_n, "bp111111111o"_n,
                        "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                        "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                        "bp111111111z"_n };
   vector<name> p17 = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                        "bp111111111g"_n, "bp111111111h"_n, "bp111111111k"_n, "bp111111111m"_n, "bp111111111o"_n,
                        "bp111111111p"_n, "bp111111111q"_n, "bp111111111r"_n, "bp111111111s"_n, "bp111111111t"_n,
                        "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                        "bp111111111z"_n };
   vector<name> p19 = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                        "bp111111111g"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111k"_n, "bp111111111m"_n,
                        "bp111111111o"_n, "bp111111111p"_n, "bp111111111q"_n, "bp111111111s"_n, "bp111111111t"_n,
                        "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                        "bp111111111z"_n };
   vector<name> p20 = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                        "bp111111111f"_n, "bp111111111g"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111k"_n,
                        "bp111111111m"_n, "bp111111111o"_n, "bp111111111p"_n, "bp111111111q"_n, "bp111111111s"_n,
                        "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111x"_n, "bp111111111y"_n,
                        "bp111111111z"_n };
   vector<name> p21 = { "bp111111111a"_n, "bp111111111b"_n, "bp111111111c"_n, "bp111111111d"_n, "bp111111111e"_n,
                        "bp111111111f"_n, "bp111111111g"_n, "bp111111111h"_n, "bp111111111i"_n, "bp111111111j"_n,
                        "bp111111111k"_n, "bp111111111m"_n, "bp111111111o"_n, "bp111111111p"_n, "bp111111111q"_n,
                        "bp111111111s"_n, "bp111111111u"_n, "bp111111111v"_n, "bp111111111w"_n, "bp111111111y"_n,
                        "bp111111111z"_n };

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
   BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
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

   for (name claimer : { "claimer1111a"_n, "claimer1111b"_n, "claimer1111c"_n, "claimer1111d"_n, "claimer1111e"_n,
                         "claimer1111f"_n, "claimer1111g"_n }) {
      t.create_account_with_resources(claimer, sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, claimer, a("1000.0000 TST"), a("1000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.regproducer(claimer));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, "unregprod"_n, mvo()("producer", claimer)));
   }

   auto supply    = t.get_token_supply();
   auto srpool_bal = a("1.0000 TST");
   auto bpspay_bal = a("0.0000 TST");

   auto transition_to = [&](double r, name claimer) {
      t.produce_block();
      t.skip_to(
            btime(uint32_t((uint64_t(r * (t.end_transition.slot - t.start_transition.slot) + t.start_transition.slot) +
                            blocks_per_round - 1) /
                           blocks_per_round * blocks_per_round)));
      t.produce_blocks(blocks_per_round + 1);

      auto pool_transition = t.transition(t.get_poolstate()["interval_start"].as<btime>(), 1.0);
      BOOST_REQUIRE_EQUAL(t.success(), t.updatepay(bpa, bpa));
      auto bp_pay =
            asset(pool_transition * prod_rate * supply.get_amount() / eosiosystem::rounds_per_year, symbol{ CORE_SYM });
      auto pool_pay = asset(supply.get_amount() * voter_rate * pool_transition / eosiosystem::rounds_per_year,
                            symbol{ CORE_SYM });
      bpspay_bal += bp_pay;
      srpool_bal += pool_pay;
      supply += bp_pay + pool_pay;
      BOOST_REQUIRE_EQUAL(t.get_balance(bpspay), bpspay_bal);
      BOOST_REQUIRE_EQUAL(t.get_balance(srpool), srpool_bal);
      BOOST_REQUIRE_EQUAL(t.get_poolstate()["pools"][int(0)]["token_pool"]["balance"].as<asset>(), srpool_bal);
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
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, "claimrewards"_n, mvo()("owner", claimer)));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(claimer, "unregprod"_n, mvo()("producer", claimer)));
      BOOST_REQUIRE_EQUAL(t.get_token_supply(), supply);
   };

   transition_to(-0.2, "claimer1111a"_n);
   transition_to(0.0, "claimer1111b"_n);
   transition_to(0.2, "claimer1111c"_n);
   transition_to(0.8, "claimer1111d"_n);
   transition_to(1.0, "claimer1111e"_n);
   transition_to(1.2, "claimer1111f"_n);
   transition_to(1.4, "claimer1111g"_n);
} // transition_inflation
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(route_fees) try {
   auto init = [](auto& t) {
      t.create_accounts_with_resources({ reserv, prox, alice, bob, jane }, sys);
      t.transfer(sys, alice, a("1000.0000 TST"), sys);
      t.transfer(sys, bob, a("1000.0000 TST"), sys);
      t.transfer(sys, jane, a("1000.0000 TST"), sys);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, alice, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, bob, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(sys, jane, a("100000.0000 TST"), a("100000.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.stake(bob, bob, a("1.0000 TST"), a("0.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(prox, "regproxy"_n, mvo()("proxy", prox)("isproxy", true)));
      BOOST_REQUIRE_EQUAL(t.success(), t.vote(bob, {}, prox));
      t.produce_block();
      t.produce_block(fc::days(14));
      t.produce_block();

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
            ("powerup_days", 30) //
            ("min_powerup_fee", a("0.0001 TST"));
      BOOST_REQUIRE_EQUAL(t.success(), t.push_action(sys, "cfgpowerup"_n, mvo()("args", std::move(conf))));
      t.produce_block();
   };

   auto check_fee = [](auto& t, bool rent, name newname, asset rex_rentbw_fee, asset pool_rentbw_fee, asset rex_ram_fee,
                       asset pool_ram_fee, asset rex_namebid_fee, asset pool_namebid_fee) {
      auto ram_payment   = a("1.0000 TST");
      auto jane_balance  = t.get_balance(jane);
      auto rex_balance   = t.get_balance(rex);
      auto pool_balance = t.get_balance(srpool);
      if (rent)
         BOOST_REQUIRE_EQUAL(t.success(), t.rentbw(jane, jane, 30, rentbw_percent, 0, a("1.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.buyram(jane, jane, ram_payment));
      BOOST_REQUIRE_EQUAL(t.get_balance(jane), jane_balance - rex_rentbw_fee - pool_rentbw_fee - ram_payment);
      BOOST_REQUIRE_EQUAL(t.get_balance(rex), rex_balance + rex_rentbw_fee + rex_ram_fee);
      BOOST_REQUIRE_EQUAL(t.get_balance(srpool), pool_balance + pool_rentbw_fee + pool_ram_fee);

      auto rex_proceeds_before = a("0.0000 TST");
      if (!t.get_rex_pool().is_null())
         rex_proceeds_before = t.get_rex_pool()["namebid_proceeds"].template as<asset>();
      auto pool_proceeds_before = a("0.0000 TST");
      if (!t.get_poolstate().is_null())
         pool_proceeds_before = t.get_poolstate()["namebid_proceeds"].template as<asset>();

      BOOST_REQUIRE_EQUAL(t.success(), t.bidname(jane, newname, a("1.0000 TST")));
      t.produce_block();
      t.produce_block(fc::days(1));
      t.produce_blocks(120);

      auto rex_proceeds_after = a("0.0000 TST");
      if (!t.get_rex_pool().is_null())
         rex_proceeds_after = t.get_rex_pool()["namebid_proceeds"].template as<asset>();
      auto pool_proceeds_after = a("0.0000 TST");
      if (!t.get_poolstate().is_null())
         pool_proceeds_after = t.get_poolstate()["namebid_proceeds"].template as<asset>();

      BOOST_REQUIRE_EQUAL(rex_proceeds_after - rex_proceeds_before, rex_namebid_fee);
      BOOST_REQUIRE_EQUAL(pool_proceeds_after - pool_proceeds_before, pool_namebid_fee);
   };

   // rex not enabled, no pools exist or no pools active
   {
      votepool_tester t;
      init(t);

      // no pools exist
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      check_fee(t, false, "1a"_n, a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));

      BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      t.produce_block();

      // no pools active
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      check_fee(t, false, "1b"_n, a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));

      t.skip_to(t.start_transition);
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      check_fee(t, false, "1c"_n, a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));

      t.skip_to(time_point::from_iso_string("2020-06-10T10:00:00.000"));
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      check_fee(t, false, "1d"_n, a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));

      t.skip_to(t.end_transition);
      BOOST_REQUIRE_EQUAL(t.wasm_assert_msg("can't channel fees to pools or to rex"),
                          t.rentbw(alice, alice, 30, rentbw_percent, 0, a("1.0000 TST")));
      check_fee(t, false, "1e"_n, a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));
   }

   // rex not enabled, pools active
   {
      votepool_tester t;
      init(t);
      BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      // TODO initialize better
      t.init_pools({jane}, 1);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
      check_fee(t, true, "2a"_n, a("0.0000 TST"), a("1.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0000 TST"));
      t.produce_block();
      t.skip_to(t.start_transition);
      check_fee(t, true, "2b"_n, a("0.0000 TST"), a("1.0000 TST"), a("0.0000 TST"), a("0.0000 TST"), a("0.0000 TST"),
                a("0.0081 TST"));
      t.produce_block();
      t.skip_to(t.end_transition);
      check_fee(t, true, "2c"_n, a("0.0000 TST"), a("1.0000 TST"), a("0.0000 TST"), a("0.0050 TST"), a("0.0000 TST"),
                a("1.0000 TST"));
   }

   // transition between rex and pools
   {
      votepool_tester t;
      init(t);
      BOOST_REQUIRE_EQUAL(t.success(), t.cfgsrpool(sys, { { 1024 } }, { { 64 } }, { { 1.0 } }, t.start_transition,
                                                  t.end_transition, 0.0, 0.0));
      // TODO initialize better
      t.init_pools({jane}, 1);
      BOOST_REQUIRE_EQUAL(t.success(), t.stake2pool(jane, jane, 0, a("1.0000 TST")));
      BOOST_REQUIRE_EQUAL(t.success(), t.unstaketorex(bob, bob, a("1.0000 TST"), a("0.0000 TST")));
      check_fee(t, true, "3a"_n, a("1.0000 TST"), a("0.0000 TST"), a("0.0050 TST"), a("0.0000 TST"), a("1.0000 TST"),
                a("0.0000 TST"));
      t.produce_block();

      t.skip_to(t.start_transition);
      check_fee(t, true, "3b"_n, a("1.0000 TST"), a("0.0000 TST"), a("0.0050 TST"), a("0.0000 TST"), a("0.9919 TST"),
                a("0.0081 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-05-10T10:00:00.000"));
      check_fee(t, true, "3c"_n, a("0.7541 TST"), a("0.2459 TST"), a("0.0038 TST"), a("0.0012 TST"), a("0.7460 TST"),
                a("0.2540 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-06-10T10:00:00.000"));
      check_fee(t, true, "3d"_n, a("0.5000 TST"), a("0.5000 TST"), a("0.0025 TST"), a("0.0025 TST"), a("0.4919 TST"),
                a("0.5081 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2020-07-10T10:00:00.000"));
      check_fee(t, true, "3e"_n, a("0.2541 TST"), a("0.7459 TST"), a("0.0013 TST"), a("0.0037 TST"), a("0.2460 TST"),
                a("0.7540 TST"));
      t.produce_block();

      t.skip_to(t.end_transition);
      check_fee(t, true, "3f"_n, a("0.0000 TST"), a("1.0000 TST"), a("0.0000 TST"), a("0.0050 TST"), a("0.0000 TST"),
                a("1.0000 TST"));
      t.produce_block();

      t.skip_to(time_point::from_iso_string("2021-07-10T10:00:00.000"));
      check_fee(t, true, "3g"_n, a("0.0000 TST"), a("1.0000 TST"), a("0.0000 TST"), a("0.0050 TST"), a("0.0000 TST"),
                a("1.0000 TST"));
   }
} // route_fees
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
