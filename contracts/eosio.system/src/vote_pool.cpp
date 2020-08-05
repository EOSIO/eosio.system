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
         state->interval_start.slot = (eosio::current_block_time().slot / blocks_per_minute) * blocks_per_minute;
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
      for (size_t i = 1; i < durations.size(); ++i)
         eosio::check(durations[i - 1] < durations[i], "durations out of order");

      auto sym = get_core_symbol();
      state.pools.resize(durations.size());
      for (size_t i = 0; i < durations.size(); ++i) {
         auto& pool    = state.pools[i];
         pool.duration = durations[i];
         pool.token_pool.init(sym);
      }

      save_vote_pool_state();
   }

   void system_contract::cfgvpool(double prod_rate, double voter_rate) {
      // TODO: convert rates from yearly compound to per-minute compound? Keep args per-minute compound?
      require_auth(get_self());
      eosio::check(prod_rate >= 0 && prod_rate < 1, "prod_rate out of range");
      eosio::check(voter_rate >= 0 && voter_rate < 1, "voter_rate out of range");

      auto& state      = get_vote_pool_state();
      state.prod_rate  = prod_rate;
      state.voter_rate = voter_rate;
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
      v.vote_pay = { 0, get_core_symbol() };
   }

   const voter_pool_votes* system_contract::get_voter_pool_votes(const voter_info& info, bool required) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      eosio::check(!required, "voter is not upgraded");
      return nullptr;
   }

   voter_pool_votes* system_contract::get_voter_pool_votes(voter_info& info, bool required) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      eosio::check(!required, "voter is not upgraded");
      return nullptr;
   }

   void system_contract::enable_voter_pool_votes(voter_info& info) {
      if (get_voter_pool_votes(info) || !get_vote_pool_state_singleton().exists())
         return;

      info.pool_votes.emplace();
      info.pool_votes.value().emplace();
      auto& v    = info.pool_votes.value().value();
      auto  size = get_vote_pool_state().pools.size();
      v.owned_shares.resize(size);
      v.proxied_shares.resize(size);
      v.last_votes.resize(size);

      auto core_symbol = get_core_symbol();
      get_vote_pool_stake_table().emplace(info.owner, [&](auto& s) {
         s.owner = info.owner;
         s.stakes.resize(size);
         for (auto& stake : s.stakes)
            stake.initial_unvested = { 0, core_symbol };
      });

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

   std::vector<const producer_info*> system_contract::top_active_producers(size_t n) {
      std::vector<const producer_info*> prods;
      prods.reserve(n);
      auto idx = _producers.get_index<"prototalvote"_n>();
      for (auto it = idx.begin(); it != idx.end() && prods.size() < n && it->total_votes > 0 && it->active(); ++it)
         prods.push_back(&*it);
      return prods;
   }

   void system_contract::update_total_pool_votes(producer_info& prod, double pool_vote_weight) {
      // TODO: reconsider pool weights
      // TODO: transition weighting between old and new systems
      auto* prod_pool_votes = get_prod_pool_votes(prod);
      if (!prod_pool_votes)
         return;
      auto& vote_pool_state = get_vote_pool_state();
      prod.total_votes -= prod_pool_votes->total_pool_votes;
      prod_pool_votes->total_pool_votes = 0;
      eosio::check(prod_pool_votes->pool_votes.size() == vote_pool_state.pools.size(), "vote pool corruption");
      for (size_t i = 0; i < vote_pool_state.pools.size(); ++i)
         prod_pool_votes->total_pool_votes +=
               vote_pool_state.pools[i].token_pool.simulate_sell(prod_pool_votes->pool_votes[i]).amount;
      prod_pool_votes->total_pool_votes *= pool_vote_weight;
      prod.total_votes += prod_pool_votes->total_pool_votes;
   }

   void system_contract::update_total_pool_votes(size_t n) {
      auto pool_vote_weight = time_to_vote_weight(get_vote_pool_state().interval_start);
      auto prods            = top_active_producers(n);
      for (auto* prod : prods)
         _producers.modify(*prod, same_payer, [&](auto& prod) { update_total_pool_votes(prod, pool_vote_weight); });
   }

   void system_contract::deposit_unvested(vote_pool& pool, double& owned_shares, per_pool_stake& stake,
                                          asset new_unvested) {
      eosio::check(new_unvested.amount > 0, "new_unvested must be positive");

      auto                   current_time = eosio::current_block_time();
      eosio::block_timestamp max_end_time{ current_time.slot + pool.duration * blocks_per_week };
      auto                   new_shares       = pool.token_pool.buy(new_unvested);
      auto                   current_unvested = stake.unvested(current_time);

      eosio::check(new_shares > 0, "new_shares must be positive");

      eosio::block_timestamp new_end_time((stake.end_vesting.slot * int128_t(current_unvested.amount) +
                                           max_end_time.slot * int128_t(new_unvested.amount)) /
                                          (current_unvested.amount + new_unvested.amount));

      owned_shares += new_shares;
      stake.initial_unvested = current_unvested + new_unvested;
      stake.start_vesting    = current_time;
      stake.end_vesting      = new_end_time;
   }

   asset system_contract::withdraw_vested(vote_pool& pool, double& owned_shares, per_pool_stake& stake,
                                          asset max_requested) {
      eosio::check(max_requested.amount > 0, "max_requested must be positive");
      auto current_time = eosio::current_block_time();
      auto balance      = pool.token_pool.simulate_sell(owned_shares);
      auto unvested     = stake.unvested(current_time);

      if (unvested.amount == 0 && max_requested >= balance) {
         // withdraw all shares; sold might be 0 when stake only contains dust
         auto sold    = pool.token_pool.sell(owned_shares);
         owned_shares = 0;
         return sold;
      } else {
         auto sell_amount = std::min(balance - unvested, max_requested);
         eosio::check(sell_amount.amount > 0, "withdrawing 0");

         auto sell_shares = std::min(pool.token_pool.simulate_sell(sell_amount), owned_shares);
         auto sold        = pool.token_pool.sell(sell_shares);
         owned_shares -= sell_shares;
         eosio::check(sell_shares > 0 && sold.amount > 0, "withdrawing 0");
         eosio::check(owned_shares > 0, "owned_shares reached 0");
         return sold;
      }
   }

   void system_contract::stake2pool(name owner, uint32_t pool_index, asset amount) {
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

      auto& pool   = state.pools[pool_index];
      auto& stakes = stake_table.get(owner.value);
      _voters.modify(voter_itr, same_payer, [&](auto& voter) {
         stake_table.modify(stakes, same_payer, [&](auto& stakes) {
            auto& owned_shares = get_voter_pool_votes(voter, true)->owned_shares[pool_index];
            deposit_unvested(pool, owned_shares, stakes.stakes[pool_index], amount);
         });
      });

      eosio::token::transfer_action transfer_act{ token_account, { owner, active_permission } };
      transfer_act.send(owner, vpool_account, amount,
                        std::string("transfer from ") + owner.to_string() + " to eosio.vpool");

      save_vote_pool_state();
      update_votes(owner, voter_itr->proxy, voter_itr->producers, false);
   }

   void system_contract::claimstake(name owner, uint32_t pool_index, asset requested) {
      require_auth(owner);

      auto& state        = get_vote_pool_state();
      auto& stake_table  = get_vote_pool_stake_table();
      auto  core_symbol  = get_core_symbol();
      auto  current_time = eosio::current_block_time();

      eosio::check(pool_index <= state.pools.size(), "invalid pool");
      eosio::check(requested.symbol == core_symbol, "requested doesn't match core symbol");
      eosio::check(requested.amount > 0, "requested must be positive"); // TODO: higher minimum amount?

      auto& pool   = state.pools[pool_index];
      auto& stakes = stake_table.get(owner.value, "stake not found");
      auto& voter  = _voters.get(owner.value, "voter record missing");
      asset claimed_amount;

      _voters.modify(voter, same_payer, [&](auto& voter) {
         stake_table.modify(stakes, same_payer, [&](auto& stakes) {
            auto& owned_shares = get_voter_pool_votes(voter, true)->owned_shares[pool_index];
            auto& stake        = stakes.stakes[pool_index];
            eosio::check(current_time.slot >= stake.last_claim.slot + blocks_per_week, "claim too soon");
            claimed_amount   = withdraw_vested(pool, owned_shares, stake, requested);
            stake.last_claim = current_time;
         });
      });

      eosio::check(pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(pool.token_pool.balance().amount >= 0, "pool amount is negative");
      save_vote_pool_state();

      if (claimed_amount.amount) {
         eosio::token::transfer_action transfer_act{ token_account, { vpool_account, active_permission } };
         transfer_act.send(vpool_account, owner, claimed_amount,
                           std::string("transfer from eosio.vpool to ") + owner.to_string());
      }

      update_votes(owner, voter.proxy, voter.producers, false);
   }

   void system_contract::transferstake(name from, name to, uint32_t pool_index, asset requested,
                                       const std::string& memo) {
      // TODO: notifications. require_recipient doesn't work because requested and actual amount may differ
      // TODO: assert if requested is too far below actual?
      // TODO: enfource last_claim?
      // TODO: need way to upgrade receiver's account
      require_auth(from);
      eosio::check(memo.size() <= 256, "memo has more than 256 bytes");
      eosio::check(from != to, "from = to");
      eosio::check(eosio::is_account(to), "invalid account");

      auto& state        = get_vote_pool_state();
      auto& stake_table  = get_vote_pool_stake_table();
      auto  core_symbol  = get_core_symbol();
      auto  current_time = eosio::current_block_time();

      eosio::check(pool_index <= state.pools.size(), "invalid pool");
      eosio::check(requested.symbol == core_symbol, "requested doesn't match core symbol");
      eosio::check(requested.amount > 0, "requested must be positive"); // TODO: higher minimum amount?

      auto& pool        = state.pools[pool_index];
      auto& from_voter  = _voters.get(from.value, "from voter record missing");
      auto& to_voter    = _voters.get(to.value, "to voter record missing");
      auto& from_stakes = stake_table.get(from.value, "stake not found");
      auto& to_stakes   = stake_table.get(to.value, "receiver can't reveive stake");
      asset transferred_amount;

      _voters.modify(from_voter, same_payer, [&](auto& from_voter) {
         stake_table.modify(from_stakes, same_payer, [&](auto& from_stakes) {
            auto& from_owned_shares = get_voter_pool_votes(from_voter, true)->owned_shares[pool_index];
            auto& from_stake        = from_stakes.stakes[pool_index];
            transferred_amount      = withdraw_vested(pool, from_owned_shares, from_stake, requested);
            eosio::check(transferred_amount.amount > 0, "transferred 0");
         });
      });

      _voters.modify(to_voter, same_payer, [&](auto& to_voter) {
         stake_table.modify(to_stakes, same_payer, [&](auto& to_stakes) {
            auto& to_owned_shares = get_voter_pool_votes(to_voter, true)->owned_shares[pool_index];
            auto& to_stake        = to_stakes.stakes[pool_index];
            deposit_unvested(pool, to_owned_shares, to_stake, transferred_amount);
         });
      });

      eosio::check(pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(pool.token_pool.balance().amount >= 0, "pool amount is negative");
      save_vote_pool_state();

      update_votes(from, from_voter.proxy, from_voter.producers, false);
      update_votes(to, to_voter.proxy, to_voter.producers, false);
   }

   void system_contract::onblock_update_vpool(block_timestamp production_time) {
      if (!get_vote_pool_state_singleton().exists())
         return;
      auto& state = get_vote_pool_state();
      if (production_time.slot >= state.interval_start.slot + blocks_per_minute) {
         state.unpaid_blocks       = state.blocks;
         state.blocks              = 0;
         state.interval_start.slot = (production_time.slot / blocks_per_minute) * blocks_per_minute;
      }
      ++state.blocks;
      save_vote_pool_state();
   }

   void system_contract::updatevotes(name user, name producer) {
      require_auth(user);
      auto  pool_vote_weight = time_to_vote_weight(get_vote_pool_state().interval_start);
      auto& prod             = _producers.get(producer.value, "unknown producer");
      eosio::check(prod.is_active, "producer is not active");
      _producers.modify(prod, same_payer, [&](auto& prod) { update_total_pool_votes(prod, pool_vote_weight); });
      save_vote_pool_state();
   }

   void system_contract::updatepay(name user) {
      require_auth(user);
      auto& state = get_vote_pool_state();
      eosio::check(state.unpaid_blocks > 0, "already processed pay for this time interval");

      update_total_pool_votes(50);
      auto prods = top_active_producers(50);

      double total_votes = 0;
      for (auto* prod : prods) {
         if (get_prod_pool_votes(*prod))
            total_votes += prod->total_votes;
      }

      const asset token_supply    = eosio::token::get_supply(token_account, core_symbol().code());
      auto        pay_scale       = pow(10, (double)state.unpaid_blocks / blocks_per_minute);
      int64_t     target_prod_pay = pay_scale * state.prod_rate / minutes_per_year * token_supply.amount;
      int64_t     total_prod_pay  = 0;

      if (target_prod_pay > 0 && total_votes > 0) {
         for (auto* prod : prods) {
            _producers.modify(*prod, same_payer, [&](auto& prod) {
               if (auto* pv = get_prod_pool_votes(prod)) {
                  int64_t pay = (target_prod_pay * total_votes) / prod.total_votes;
                  pv->vote_pay.amount += pay;
                  total_prod_pay += pay;
               }
            });
         }
      }

      int64_t per_pool_pay = pay_scale * state.voter_rate / minutes_per_year * token_supply.amount / state.pools.size();
      int64_t total_voter_pay = 0;
      for (auto& pool : state.pools) {
         if (pool.token_pool.shares()) {
            pool.token_pool.adjust({ per_pool_pay, core_symbol() });
            total_voter_pay += per_pool_pay;
         }
      }

      int64_t new_tokens = total_prod_pay + total_voter_pay;
      if (new_tokens > 0) {
         {
            eosio::token::issue_action issue_act{ token_account, { { get_self(), active_permission } } };
            issue_act.send(get_self(), asset(new_tokens, core_symbol()), "issue tokens for producer pay and voter pay");
         }
         if (total_prod_pay > 0) {
            eosio::token::transfer_action transfer_act{ token_account, { get_self(), active_permission } };
            transfer_act.send(get_self(), bvpay_account, eosio::asset{ total_prod_pay, core_symbol() },
                              "fund producer pay");
         }
         if (total_voter_pay > 0) {
            eosio::token::transfer_action transfer_act{ token_account, { get_self(), active_permission } };
            transfer_act.send(get_self(), vpool_account, eosio::asset{ total_voter_pay, core_symbol() },
                              "fund voter pay");
         }
      }

      state.unpaid_blocks = 0;
      save_vote_pool_state();
   }

   void system_contract::claimvotepay(name producer) {
      require_auth(producer);
      auto& prod = _producers.get(producer.value, "unknown producer");
      _producers.modify(prod, same_payer, [&](auto& prod) {
         auto* pv = get_prod_pool_votes(prod);
         eosio::check(pv && pv->vote_pay.amount > 0, "no pay available");
         eosio::token::transfer_action transfer_act{ token_account, { bvpay_account, active_permission } };
         transfer_act.send(bvpay_account, producer, pv->vote_pay, "producer pay");
         pv->vote_pay.amount = 0;
      });
   }

} // namespace eosiosystem
