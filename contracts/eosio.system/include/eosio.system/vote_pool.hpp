#pragma once

#include <cmath>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>

#include <eosio.system/time_constants.hpp>

namespace eosiosystem {

   class pool {
    public:
      void init(eosio::symbol s) { _balance = { 0, s }; }

      auto shares() const { return _total_shares; }
      auto balance() const { return _balance; }

      void adjust(eosio::asset delta) { _balance += delta; }

      double buy(eosio::asset b) {
         auto out = simulate_buy(b);
         _balance += b;
         _total_shares += out;
         return out;
      }

      eosio::asset sell(double s) {
         auto out = simulate_sell(s);
         _balance -= out;
         _total_shares -= s;
         return out;
      }

      double simulate_buy(eosio::asset b) const {
         if (!b.amount)
            return 0;
         if (!_total_shares)
            return b.amount;
         else
            return (b.amount * _total_shares) / double(_balance.amount);
      }

      eosio::asset simulate_sell(double s) const {
         if (!s)
            return eosio::asset{ 0, _balance.symbol };
         if (s >= _total_shares)
            return _balance;
         eosio::check(_total_shares > 0, "no shares in pool");

         return eosio::asset(double(s) * double(_balance.amount) / double(_total_shares), _balance.symbol);
      }

      // the number of shares need to get d_out
      double simulate_sell(eosio::asset d_out) const {
         if (d_out == _balance)
            return _total_shares;
         else if (!d_out.amount)
            return 0;
         return (d_out.amount * _total_shares) / double(_balance.amount);
      }

    private:
      eosio::asset _balance;
      double       _total_shares = 0;
      EOSLIB_SERIALIZE(pool, (_total_shares)(_balance))
   };

   struct vote_pool {
      uint32_t          duration;     // duration, seconds
      uint32_t          claim_period; // how often owners may claim, seconds
      eosiosystem::pool token_pool;   // token tracking

      EOSLIB_SERIALIZE(vote_pool, (duration)(claim_period)(token_pool))
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
