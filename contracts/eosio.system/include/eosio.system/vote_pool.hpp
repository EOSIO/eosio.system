#pragma once

#include <cmath>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>

#include <eosio.system/constants.hpp>

namespace eosiosystem {

   class pool {
    public:
      void init(eosio::symbol s) { balance = { 0, s }; }

      auto shares() const { return total_shares; }
      auto bal() const { return balance; }

      void adjust(eosio::asset delta) { balance += delta; }

      double buy(eosio::asset b) {
         auto out = simulate_buy(b);
         balance += b;
         total_shares += out;
         return out;
      }

      eosio::asset sell(double s) {
         auto out = simulate_sell(s);
         balance -= out;
         total_shares -= s;
         return out;
      }

      double simulate_buy(eosio::asset b) const {
         if (!b.amount)
            return 0;
         if (!total_shares)
            return b.amount;
         else
            return (b.amount * total_shares) / double(balance.amount);
      }

      eosio::asset simulate_sell(double s) const {
         if (!s)
            return eosio::asset{ 0, balance.symbol };
         if (s >= total_shares)
            return balance;
         eosio::check(total_shares > 0, "no shares in pool");

         return eosio::asset(double(s) * double(balance.amount) / double(total_shares), balance.symbol);
      }

      // the number of shares need to get d_out
      double simulate_sell(eosio::asset d_out) const {
         if (d_out == balance)
            return total_shares;
         else if (!d_out.amount)
            return 0;
         return (d_out.amount * total_shares) / double(balance.amount);
      }

    private:
      eosio::asset balance;
      double       total_shares = 0;
      EOSLIB_SERIALIZE(pool, (balance)(total_shares))
   };

   struct vote_pool {
      uint32_t          duration;     // duration, seconds
      uint32_t          claim_period; // how often owners may claim, seconds
      double            vote_weight;  // voting power. if vote_weight == 1, then 1.0000 SYS in pool has 1.0 votes.
      eosiosystem::pool token_pool;   // token tracking

      EOSLIB_SERIALIZE(vote_pool, (duration)(claim_period)(vote_weight)(token_pool))
   };

   struct [[eosio::table("vpoolstate"), eosio::contract("eosio.system")]] vote_pool_state {
      std::vector<vote_pool> pools;
      double prod_rate  = 0; // Inflation rate (compounded each minute) allocated to producer pay (0.01 = 1%)
      double voter_rate = 0; // Inflation rate (compounded each minute) allocated to voters (0.01 = 1%)
      eosio::block_timestamp interval_start;    // Beginning of current 1-minute block production interval
      uint32_t               blocks        = 0; // Blocks produced in current interval
      uint32_t               unpaid_blocks = 0; // Blocks produced in previous interval
      eosio::block_timestamp begin_transition;  // Beginning of transition
      eosio::block_timestamp end_transition;    // End of transition

      template <typename T>
      T transition(eosio::block_timestamp time, T val) const {
         if (time.slot >= end_transition.slot)
            return val;
         if (time.slot <= begin_transition.slot)
            return 0;
         return val * (time.slot - begin_transition.slot) / (end_transition.slot - begin_transition.slot);
      }

      EOSLIB_SERIALIZE(vote_pool_state, (pools)(prod_rate)(voter_rate)(interval_start)(blocks)(unpaid_blocks)(
                                              begin_transition)(end_transition))
   };

   typedef eosio::singleton<"vpoolstate"_n, vote_pool_state> vote_pool_state_singleton;

   struct [[eosio::table, eosio::contract("eosio.system")]] pool_voter {
      name                                owner;
      std::vector<eosio::block_timestamp> next_claim;     // next time user may claim shares
      std::vector<double>                 owned_shares;   // shares in each pool
      std::vector<double>                 proxied_shares; // shares in each pool delegated to this voter as a proxy
      std::vector<double>                 last_votes;     // vote weights cast the last time the vote was updated
      name                                proxy;          // the proxy set by the voter, if any
      std::vector<name>                   producers;      // the producers approved by this voter if no proxy set
      bool                                is_proxy       = false; // whether the voter is a proxy for others
      bool                                xfer_in_notif  = false; // opt into incoming transferstake notifications
      bool                                xfer_out_notif = false; // opt into outgoing transferstake notifications

      uint64_t primary_key() const { return owner.value; }

      EOSLIB_SERIALIZE(pool_voter, (owner)(next_claim)(owned_shares)(proxied_shares)(last_votes)(proxy)(producers)(
                                         is_proxy)(xfer_in_notif)(xfer_out_notif))
   };

   typedef eosio::multi_index<"poolvoter"_n, pool_voter> pool_voter_table;

   struct [[eosio::table, eosio::contract("eosio.system")]] total_pool_votes {
      name         owner;
      bool         active = true;
      double       votes  = 0; // total shares in all pools, weighted by pool strength
      eosio::asset vote_pay;   // unclaimed vote pay

      EOSLIB_SERIALIZE(total_pool_votes, (owner)(active)(votes)(vote_pay))

      uint64_t primary_key() const { return owner.value; }
      double   by_votes() const { return active ? -votes : votes; }
   };

   typedef eosio::multi_index<
         "totpoolvotes"_n, total_pool_votes,
         eosio::indexed_by<"byvotes"_n, eosio::const_mem_fun<total_pool_votes, double, &total_pool_votes::by_votes>>>
         total_pool_votes_table;

   // transferstake sends this inline action (eosio.tstake) to each account which has opted into receiving the
   // notifications. This notification has no authorizer; to check its authenticity, the receiving contract should
   // verify using: get_sender() == "eosio"_n
   struct transferstake_notification {
      name     from;               // Transfer from this account
      name     to;                 // Transfer to this account
      uint32_t pool_index;         // Which pool
      asset    requested;          // Eequested amount
      asset    transferred_amount; // Actual amount transferred. May differ from requested because of rounding.
                                   // May also be less than requested if user didn't have enough shares.
      std::string memo;

      EOSLIB_SERIALIZE(transferstake_notification, (from)(to)(pool_index)(requested)(transferred_amount)(memo))
   };

} // namespace eosiosystem
