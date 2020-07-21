#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   void system_contract::initvpool(const std::vector<uint32_t>& durations) {
      require_auth(get_self());

      vote_pool_state_singleton state_sing{ get_self(), 0 };
      eosio::check(!state_sing.exists(), "vote pools already initialized");
      eosio::check(!durations.empty(), "durations is empty");
      for (auto d : durations)
         eosio::check(d > 0, "duration must be positive");

      vote_pool_state state;
      auto            sym = get_core_symbol();
      state.pools.resize(durations.size());
      for (size_t i = 0; i < durations.size(); ++i) {
         auto& pool    = state.pools[i];
         pool.duration = durations[i];
         pool.token_pool.init(sym);
      }

      state_sing.set(state, get_self());
   }

   void system_contract::stake2pool(name owner, uint32_t pool_index, asset amount) {
      // TODO: require upgraded account
      // TODO: update vote weights
      require_auth(owner);

      vote_pool_state_singleton state_sing{ get_self(), 0 };
      vote_pool_stake_table     stake_table{ get_self(), 0 };

      eosio::check(state_sing.exists(), "vote pools not initialized");
      auto state       = state_sing.get();
      auto core_symbol = get_core_symbol();

      eosio::check(pool_index <= state.pools.size(), "invalid pool");
      eosio::check(amount.symbol == core_symbol, "amount doesn't match core symbol");
      eosio::check(amount.amount > 0, "amount must be positive"); // TODO: higher minimum amount?

      auto& pool = state.pools[pool_index];

      stake_table.emplace(owner, [&](auto& row) {
         row.id             = stake_table.available_primary_key();
         row.owner          = owner;
         row.pool_index     = pool_index;
         row.initial_amount = amount;
         row.current_shares = pool.token_pool.buy(amount);
         row.created        = eosio::current_block_time();
         row.matures        = eosio::block_timestamp{ row.created.slot + pool.duration * blocks_per_week };
         row.last_claim     = row.created;
      });
      state_sing.set(state, get_self());

      eosio::token::transfer_action transfer_act{ token_account, { owner, active_permission } };
      transfer_act.send(owner, vpool_account, amount,
                        std::string("transfer from ") + owner.to_string() + " to eosio.vpool");
   }

   void system_contract::claimstake(name owner, uint64_t id, asset max_amount) {
      // TODO: update vote weights
      require_auth(owner);

      vote_pool_state_singleton state_sing{ get_self(), 0 };
      vote_pool_stake_table     stake_table{ get_self(), 0 };
      auto                      state        = state_sing.get();
      auto                      it           = stake_table.find(id);
      auto                      current_time = eosio::current_block_time();
      eosio::check(it != stake_table.end(), "stake not found");
      eosio::check(it->owner == owner, "stake has different owner");
      eosio::check(current_time.slot >= it->last_claim.slot + blocks_per_week, "claim too soon");

      auto         row          = *it;
      auto&        pool         = state.pools[row.pool_index];
      auto         curr_balance = pool.token_pool.simulate_sell(row.current_shares);
      auto         min_balance  = row.min_balance(pool, current_time);
      eosio::asset claimed_amount;

      if (min_balance.amount == 0 && max_amount >= curr_balance) {
         claimed_amount = pool.token_pool.sell(row.current_shares);
         stake_table.erase(it);
      } else {
         auto available   = curr_balance - min_balance;
         auto sell_amount = std::min(available, max_amount);

         eosio::check(min_balance < curr_balance, "no funds are claimable yet");
         eosio::check(sell_amount.amount > 0, "claiming 0");

         auto sell_shares = std::min(pool.token_pool.simulate_sell(sell_amount), row.current_shares);
         claimed_amount   = pool.token_pool.sell(sell_shares);
         row.current_shares -= sell_shares;
         eosio::check(row.current_shares > 0, "row.current_shares reached 0");
         row.last_claim = current_time;
         stake_table.modify(it, owner, [&](auto& x) { x = row; });
      }

      eosio::check(pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(pool.token_pool.balance().amount >= 0, "pool amount is negative");
      state_sing.set(state, get_self());

      if (claimed_amount.amount) {
         eosio::token::transfer_action transfer_act{ token_account, { vpool_account, active_permission } };
         transfer_act.send(vpool_account, owner, claimed_amount,
                           std::string("transfer from eosio.vpool to ") + owner.to_string());
      }
   }

   void system_contract::transferstake(name from, name to, uint64_t id, const std::string& memo) {
      // TODO: require "to" has upgraded account
      // TODO: update vote weights (from)
      // TODO: update vote weights (to)
      // TODO: switch notifications to inline actions?
      require_auth(from);
      require_recipient(from);
      require_recipient(to);
      check(memo.size() <= 256, "memo has more than 256 bytes");
      // TODO: transfer
   }

} // namespace eosiosystem
