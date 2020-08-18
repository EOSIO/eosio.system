#pragma once

#include <cmath>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>

#include <eosio.system/time_constants.hpp>

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

      EOSLIB_SERIALIZE(vote_pool_state, (pools)(prod_rate)(voter_rate)(interval_start)(blocks)(unpaid_blocks))
   };

   typedef eosio::singleton<"vpoolstate"_n, vote_pool_state> vote_pool_state_singleton;

} // namespace eosiosystem
