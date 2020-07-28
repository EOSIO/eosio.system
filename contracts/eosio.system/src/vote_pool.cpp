#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   vote_pool_state_singleton& system_contract::get_vote_pool_state_singleton() {
      static std::optional<vote_pool_state_singleton> sing;
      if (!sing)
         sing.emplace(get_self(), 0);
      return *sing;
   }

   vote_pool_state& system_contract::get_vote_pool_state(bool init) {
      static std::optional<vote_pool_state> state;
      if (init) {
         eosio::check(!state && !get_vote_pool_state_singleton().exists(), "vote pools already initialized");
         state.emplace();
      } else if (!state) {
         eosio::check(get_vote_pool_state_singleton().exists(), "vote pools not initialized");
         state = get_vote_pool_state_singleton().get();
      }
      return *state;
   }

   void system_contract::save_vote_pool_state() {
      get_vote_pool_state_singleton().set(get_vote_pool_state(), get_self());
   }

   vote_pool_stake_table& system_contract::get_vote_pool_stake_table() {
      static std::optional<vote_pool_stake_table> table;
      if (!table)
         table.emplace(get_self(), 0);
      return *table;
   }

   void system_contract::initvpool(const std::vector<uint32_t>& durations) {
      require_auth(get_self());

      auto& state = get_vote_pool_state(true);
      eosio::check(!durations.empty(), "durations is empty");
      for (auto d : durations)
         eosio::check(d > 0, "duration must be positive");

      auto sym = get_core_symbol();
      state.pools.resize(durations.size());
      for (size_t i = 0; i < durations.size(); ++i) {
         auto& pool    = state.pools[i];
         pool.duration = durations[i];
         pool.token_pool.init(sym);
      }

      save_vote_pool_state();
   }

   const prod_pool_votes* system_contract::get_prod_pool_votes(const producer_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   prod_pool_votes* system_contract::get_prod_pool_votes(producer_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   void system_contract::enable_prod_pool_votes(producer_info& info) {
      if (get_prod_pool_votes(info) || !get_vote_pool_state_singleton().exists())
         return;

      info.pool_votes.emplace();
      info.pool_votes.value().emplace();
      auto& v = info.pool_votes.value().value();
      v.pool_votes.resize(get_vote_pool_state().pools.size());
   }

   const voter_pool_votes* system_contract::get_voter_pool_votes(const voter_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   voter_pool_votes* system_contract::get_voter_pool_votes(voter_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   void system_contract::enable_voter_pool_votes(voter_info& info) {
      if (get_voter_pool_votes(info) || !get_vote_pool_state_singleton().exists())
         return;

      info.pool_votes.emplace();
      info.pool_votes.value().emplace();
      auto& v    = info.pool_votes.value().value();
      auto  size = get_vote_pool_state().pools.size();
      v.shares.resize(size);
      v.proxied_shares.resize(size);
      v.last_votes.resize(size);

      if (info.proxy.value) {
         auto it = _voters.find(info.proxy.value);
         if (it == _voters.end() || !it->is_proxy)
            eosio::check(false, info.owner.to_string() + " is using proxy " + info.proxy.to_string() +
                                      ", which is no longer active");
         if (!get_voter_pool_votes(*it))
            eosio::check(false, info.owner.to_string() + " is using proxy " + info.proxy.to_string() +
                                      ", which is not upgraded to support pool votes");
      }

      for (auto prod : info.producers) {
         auto it = _producers.find(prod.value);
         if (it == _producers.end() || !it->is_active)
            eosio::check(false,
                         info.owner.to_string() + " is voting for " + prod.to_string() + ", which is no longer active");
         if (!get_prod_pool_votes(*it))
            eosio::check(false, info.owner.to_string() + " is voting for " + prod.to_string() +
                                      ", which is not upgraded to support pool votes");
      }
   }

   void system_contract::add_proxied_shares(voter_info& proxy, const std::vector<double>& deltas, const char* error) {

      auto* votes = get_voter_pool_votes(proxy);
      eosio::check(votes && votes->proxied_shares.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         votes->proxied_shares[i] += deltas[i];
   }

   void system_contract::sub_proxied_shares(voter_info& proxy, const std::vector<double>& deltas, const char* error) {

      auto* votes = get_voter_pool_votes(proxy);
      eosio::check(votes && votes->proxied_shares.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         votes->proxied_shares[i] -= deltas[i];
   }

   void system_contract::add_pool_votes(producer_info& prod, const std::vector<double>& deltas, const char* error) {

      auto* votes = get_prod_pool_votes(prod);
      eosio::check(votes && votes->pool_votes.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         votes->pool_votes[i] += deltas[i];
   }

   void system_contract::sub_pool_votes(producer_info& prod, const std::vector<double>& deltas, const char* error) {

      auto* votes = get_prod_pool_votes(prod);
      eosio::check(votes && votes->pool_votes.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         votes->pool_votes[i] -= deltas[i];
   }

   void system_contract::stake2pool(name owner, uint32_t pool_index, asset amount) {
      // TODO: update vote weights
      require_auth(owner);

      auto& state       = get_vote_pool_state();
      auto& stake_table = get_vote_pool_stake_table();
      auto  core_symbol = get_core_symbol();

      eosio::check(pool_index <= state.pools.size(), "invalid pool");
      eosio::check(amount.symbol == core_symbol, "amount doesn't match core symbol");
      eosio::check(amount.amount > 0, "amount must be positive"); // TODO: higher minimum amount?

      auto voter_itr = _voters.find(owner.value);
      if (voter_itr != _voters.end()) {
         if (!get_voter_pool_votes(*voter_itr)) {
            _voters.modify(voter_itr, owner, [&](auto& v) { enable_voter_pool_votes(v); });
         }
      } else {
         voter_itr = _voters.emplace(owner, [&](auto& v) {
            v.owner = owner;
            enable_voter_pool_votes(v);
         });
      }

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
      save_vote_pool_state();

      eosio::token::transfer_action transfer_act{ token_account, { owner, active_permission } };
      transfer_act.send(owner, vpool_account, amount,
                        std::string("transfer from ") + owner.to_string() + " to eosio.vpool");
   }

   void system_contract::claimstake(name owner, uint64_t id, asset max_amount) {
      // TODO: update vote weights
      require_auth(owner);

      auto& state        = get_vote_pool_state();
      auto& stake_table  = get_vote_pool_stake_table();
      auto  it           = stake_table.find(id);
      auto  current_time = eosio::current_block_time();
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
      save_vote_pool_state();

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
      eosio::check(memo.size() <= 256, "memo has more than 256 bytes");
      eosio::check(from != to, "from = to");
      eosio::check(eosio::is_account(to), "invalid account");

      auto& stake_table = get_vote_pool_stake_table();
      auto  it          = stake_table.find(id);
      eosio::check(it != stake_table.end(), "stake not found");
      eosio::check(it->owner == from, "stake has different owner");
      stake_table.modify(it, from, [&](auto& row) { row.owner = to; });
   }

} // namespace eosiosystem
