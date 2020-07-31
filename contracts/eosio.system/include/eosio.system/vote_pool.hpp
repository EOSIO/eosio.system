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

   struct per_pool_stake {
      double                 current_shares; // owned shares in pool; includes both vested and unvested
      eosio::asset           initial_unvested;
      eosio::block_timestamp start_vesting; // unvested = initial_unvested at this time
      eosio::block_timestamp end_vesting;   // unvested = 0 at this time. end_vesting - start_vesting may be less than
                                            // pool duration; this happens when initial_unvested is updated because of
                                            // transfer or additional staking.
      eosio::block_timestamp last_claim;    // limits claim frequency

      eosio::asset unvested(eosio::block_timestamp current_time) {
         if (current_time >= end_vesting)
            return { 0, initial_unvested.symbol };
         auto    age       = current_time.slot - start_vesting.slot;
         auto    duration  = end_vesting.slot - start_vesting.slot;
         int64_t spendable = uint128_t(initial_unvested.amount) * age / duration;
         return { initial_unvested.amount - spendable, initial_unvested.symbol };
      }

      EOSLIB_SERIALIZE(per_pool_stake, (current_shares)(initial_unvested)(start_vesting)(end_vesting)(last_claim))
   };

   struct [[eosio::table("vpool.stake"), eosio::contract("eosio.system")]] vote_pool_stake {
      eosio::name                 owner;
      std::vector<per_pool_stake> stakes;

      uint64_t primary_key() const { return owner.value; }

      EOSLIB_SERIALIZE(vote_pool_stake, (owner)(stakes))
   };

   typedef eosio::multi_index<"vpool.stake"_n, vote_pool_stake> vote_pool_stake_table;

} // namespace eosiosystem
