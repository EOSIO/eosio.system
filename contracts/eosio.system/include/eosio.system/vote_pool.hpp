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
      uint32_t          duration;   // duration, in weeks
      eosiosystem::pool token_pool; // token tracking

      EOSLIB_SERIALIZE(vote_pool, (duration)(token_pool))
   };

   struct [[eosio::table("vpoolstate"), eosio::contract("eosio.system")]] vote_pool_state {
      std::vector<vote_pool> pools;

      EOSLIB_SERIALIZE(vote_pool_state, (pools))
   };

   typedef eosio::singleton<"vpoolstate"_n, vote_pool_state> vote_pool_state_singleton;

   struct [[eosio::table("vpool.stake"), eosio::contract("eosio.system")]] vote_pool_stake {
      uint64_t               id;
      eosio::name            owner;
      uint32_t               pool_index;
      eosio::asset           initial_amount;
      double                 current_shares;
      eosio::block_timestamp created;
      eosio::block_timestamp matures;
      eosio::block_timestamp last_claim;

      uint64_t primary_key() const { return id; }

      eosio::asset min_balance(const vote_pool& pool, eosio::block_timestamp current_time) {
         uint32_t age = (current_time.slot - created.slot) / blocks_per_week;
         if (age >= pool.duration)
            return { 0, initial_amount.symbol };
         int64_t spendable = uint128_t(initial_amount.amount) * age / pool.duration;
         return { initial_amount.amount - spendable, initial_amount.symbol };
      }

      EOSLIB_SERIALIZE(vote_pool_stake,
                       (id)(owner)(pool_index)(initial_amount)(current_shares)(created)(matures)(last_claim))
   };

   typedef eosio::multi_index<"vpool.stake"_n, vote_pool_stake> vote_pool_stake_table;

} // namespace eosiosystem
