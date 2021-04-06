#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   staking_pool_state_singleton& system_contract::get_staking_pool_state_singleton() {
      static std::optional<staking_pool_state_singleton> sing;
      if (!sing)
         sing.emplace(get_self(), get_self().value);
      return *sing;
   }

   staking_pool_state& system_contract::get_staking_pool_state_mutable(bool init_if_not_exist) {
      static std::optional<staking_pool_state> state;
      if (!state) {
         if (init_if_not_exist && !get_staking_pool_state_singleton().exists()) {
            state.emplace();
            state->interval_start.slot = (eosio::current_block_time().slot / blocks_per_round) * blocks_per_round;
         } else {
            eosio::check(get_staking_pool_state_singleton().exists(), "staking pools not configured");
            state = get_staking_pool_state_singleton().get();
         }
      }
      return *state;
   }

   const staking_pool_state& system_contract::get_staking_pool_state() { return get_staking_pool_state_mutable(); }

   void system_contract::save_staking_pool_state() {
      get_staking_pool_state_singleton().set(get_staking_pool_state_mutable(), get_self());
   }

   double system_contract::claimrewards_transition(block_timestamp time) {
      if (!get_staking_pool_state_singleton().exists())
         return 1.0;
      return 1.0 - get_staking_pool_state().transition(time, 1.0);
   }

   total_pool_votes_table& system_contract::get_total_pool_votes_table() {
      static std::optional<total_pool_votes_table> table;
      if (!table)
         table.emplace(get_self(), get_self().value);
      return *table;
   }

   void system_contract::cfgsrpool(const std::optional<std::vector<uint32_t>>&  durations,        //
                                  const std::optional<std::vector<uint32_t>>&  claim_periods,    //
                                  const std::optional<std::vector<double>>&    vote_weights,     //
                                  const std::optional<eosio::block_timestamp>& begin_transition, //
                                  const std::optional<eosio::block_timestamp>& end_transition,   //
                                  const std::optional<double>&                 prod_rate,        //
                                  const std::optional<double>&                 voter_rate,       //
                                  const std::optional<uint8_t>&                max_num_pay,      //
                                  const std::optional<double>&                 max_vote_ratio,   //
                                  const std::optional<asset>&                  min_transfer_create) {
      require_auth(get_self());
      bool                     is_first_time = !get_staking_pool_state_singleton().exists();
      staking_pool_state_autosave state{ *this, true };

      if (is_first_time) {
         eosio::check(durations.has_value(), "durations is required on first use of cfgsrpool");
         eosio::check(claim_periods.has_value(), "claim_periods is required on first use of cfgsrpool");
         eosio::check(vote_weights.has_value(), "vote_weights is required on first use of cfgsrpool");
         eosio::check(begin_transition.has_value(), "begin_transition is required on first use of cfgsrpool");
         eosio::check(end_transition.has_value(), "end_transition is required on first use of cfgsrpool");
         eosio::check(!durations->empty(), "durations is empty");
         eosio::check(claim_periods->size() == durations->size(), "mismatched vector sizes");
         eosio::check(vote_weights->size() == durations->size(), "mismatched vector sizes");

         if (_gstate.thresh_activated_stake_time == time_point())
            _gstate.thresh_activated_stake_time = eosio::current_time_point();

         for (size_t i = 0; i < durations->size(); ++i) {
            auto d = durations.value()[i];
            auto c = claim_periods.value()[i];
            auto w = vote_weights.value()[i];
            eosio::check(d > 0, "duration must be positive");
            eosio::check(c > 0, "claim_period must be positive");
            eosio::check(w > 0, "vote_weight must be positive");
            eosio::check(c < d, "claim_period must be less than duration");
         }

         for (size_t i = 1; i < durations->size(); ++i) {
            eosio::check(durations.value()[i - 1] < durations.value()[i], "durations must be increasing");
            eosio::check(claim_periods.value()[i - 1] <= claim_periods.value()[i],
                         "claim_periods must be non-decreasing");
            eosio::check(vote_weights.value()[i - 1] <= vote_weights.value()[i], "vote_weights must be non-decreasing");
         }

         auto sym = get_core_symbol();
         state->pools.resize(durations->size());
         for (size_t i = 0; i < durations->size(); ++i) {
            auto& pool        = state->pools[i];
            pool.duration     = durations.value()[i];
            pool.claim_period = claim_periods.value()[i];
            pool.vote_weight  = vote_weights.value()[i];
            pool.token_pool.init(sym);
         }
         state->min_transfer_create = asset{ 1'0000, sym };
         state->total_votes.resize(durations->size());
         state->namebid_proceeds = asset{ 0, core_symbol() };
      } else {
         eosio::check(!durations.has_value(), "durations can't change");
         eosio::check(!claim_periods.has_value(), "claim_periods can't change");
         eosio::check(!vote_weights.has_value(), "vote_weights can't change");
      }

      if (begin_transition)
         state->begin_transition = *begin_transition;
      if (end_transition)
         state->end_transition = *end_transition;
      eosio::check(state->begin_transition <= state->end_transition, "begin_transition > end_transition");

      if (prod_rate) {
         eosio::check(*prod_rate >= 0 && *prod_rate < 1, "prod_rate out of range");
         state->prod_rate = *prod_rate;
      }

      if (voter_rate) {
         eosio::check(*voter_rate >= 0 && *voter_rate < 1, "voter_rate out of range");
         state->voter_rate = *voter_rate;
      }

      if (max_num_pay)
         state->max_num_pay = *max_num_pay;

      if (max_vote_ratio) {
         eosio::check(*max_vote_ratio >= 0 && *max_vote_ratio <= 1, "max_vote_ratio out of range");
         state->max_vote_ratio = *max_vote_ratio;
      }

      if (min_transfer_create) {
         eosio::check(min_transfer_create->symbol == get_core_symbol() && min_transfer_create->amount >= 0,
                      "min_transfer_create out of range");
         state->min_transfer_create = *min_transfer_create;
      }
   } // system_contract::cfgsrpool

   const std::vector<double>* system_contract::get_prod_pool_votes(const producer_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   std::vector<double>* system_contract::get_prod_pool_votes(producer_info& info) {
      if (info.pool_votes.has_value() && info.pool_votes.value().has_value())
         return &info.pool_votes.value().value();
      return nullptr;
   }

   void system_contract::enable_prod_pool_votes(producer_info& info) {
      if (!get_staking_pool_state_singleton().exists())
         return;

      auto& total_table = get_total_pool_votes_table();
      if (get_prod_pool_votes(info)) {
         total_table.modify(total_table.get(info.owner.value), same_payer, [](auto& tot) { tot.active = true; });
         return;
      }

      info.pool_votes.emplace();
      info.pool_votes.value().emplace();
      auto& v = info.pool_votes.value().value();
      v.resize(get_staking_pool_state().pools.size());

      total_table.emplace(info.owner, [&](auto& tot) {
         tot.owner    = info.owner;
         tot.active   = true;
         tot.votes    = 0;
         tot.vote_pay = { 0, get_core_symbol() };
      });
   }

   void system_contract::deactivate_producer(name producer) {
      auto& prod = _producers.get(producer.value, "producer not found");
      _producers.modify(prod, same_payer, [](auto& info) { info.deactivate(); });

      auto& total_table = get_total_pool_votes_table();
      auto  it          = total_table.find(producer.value);
      if (it != total_table.end())
         total_table.modify(*it, same_payer, [](auto& tot) { tot.active = false; });
   }

   pool_voter_table& system_contract::get_pool_voter_table() {
      static std::optional<pool_voter_table> table;
      if (!table)
         table.emplace(get_self(), get_self().value);
      return *table;
   }

   const pool_voter& system_contract::create_pool_voter(name voter_name) {
      auto& pool_voter_table = get_pool_voter_table();
      return *pool_voter_table.emplace(get_self(), [&](auto& voter) {
         auto size   = get_staking_pool_state().pools.size();
         voter.owner = voter_name;
         voter.next_claim.resize(size);
         voter.owned_shares.resize(size);
         voter.proxied_shares.resize(size);
         voter.last_votes.resize(size);
      });
   }

   const pool_voter& system_contract::get_or_create_pool_voter(name voter_name, bool* created) {
      auto& pool_voter_table = get_pool_voter_table();
      auto  it               = pool_voter_table.find(voter_name.value);
      if (it != pool_voter_table.end())
         return *it;
      if (created)
         *created = true;
      return create_pool_voter(voter_name);
   }

   void system_contract::add_proxied_shares(pool_voter& proxy, const std::vector<double>& deltas, const char* error) {
      eosio::check(proxy.proxied_shares.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         proxy.proxied_shares[i] += deltas[i];
   }

   void system_contract::sub_proxied_shares(pool_voter& proxy, const std::vector<double>& deltas, const char* error) {
      eosio::check(proxy.proxied_shares.size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i)
         proxy.proxied_shares[i] -= deltas[i];
   }

   void system_contract::add_pool_votes(staking_pool_state_autosave& state, producer_info& prod,
                                        const std::vector<double>& deltas) {
      auto* votes = get_prod_pool_votes(prod);
      if (!votes || votes->size() != deltas.size())
         eosio::check(false, "producer " + prod.owner.to_string() + " has not upgraded to support pool votes");
      for (size_t i = 0; i < deltas.size(); ++i) {
         (*votes)[i] += deltas[i];
         state->total_votes[i] += deltas[i];
      }
   }

   void system_contract::sub_pool_votes(staking_pool_state_autosave& state, producer_info& prod,
                                        const std::vector<double>& deltas, const char* error) {
      auto* votes = get_prod_pool_votes(prod);
      eosio::check(votes && votes->size() == deltas.size(), error);
      for (size_t i = 0; i < deltas.size(); ++i) {
         (*votes)[i] -= deltas[i];
         state->total_votes[i] -= deltas[i];
      }
   }

   void system_contract::update_pool_votes(staking_pool_state_autosave& state, const name& voter_name, const name& proxy,
                                           const std::vector<name>& producers, bool voting) {
      if (proxy) {
         eosio::check(producers.size() == 0, "cannot vote for producers and proxy at same time");
         eosio::check(voter_name != proxy, "cannot proxy to self");
      } else {
         eosio::check(producers.size() <= 30, "attempt to vote for too many producers");
         for (size_t i = 1; i < producers.size(); ++i)
            eosio::check(producers[i - 1] < producers[i], "producer votes must be unique and sorted");
      }

      auto& pool_voter_table = get_pool_voter_table();
      auto& voter            = get_or_create_pool_voter(voter_name);
      eosio::check(!proxy || !voter.is_proxy, "account registered as a proxy is not allowed to use a proxy");

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
         auto& old_proxy = pool_voter_table.get(voter.proxy.value, "bug: old proxy not found");
         pool_voter_table.modify(old_proxy, same_payer,
                                 [&](auto& vp) { //
                                    sub_proxied_shares(vp, voter.last_votes, "bug: proxy lost its pool");
                                 });
         update_pool_proxy(state, old_proxy);
      } else {
         for (const auto& p : voter.producers)
            producer_changes[p].old_vote = true;
      }

      if (proxy) {
         auto& new_proxy = pool_voter_table.get(proxy.value, "proxy not found");
         eosio::check(!voting || new_proxy.is_proxy, "proxy not found");
         pool_voter_table.modify(new_proxy, same_payer,
                                 [&](auto& vp) { add_proxied_shares(vp, new_pool_votes, "bug: proxy lost its pool"); });
         update_pool_proxy(state, new_proxy);
      } else {
         for (const auto& p : producers)
            producer_changes[p].new_vote = true;
      }

      for (const auto& pc : producer_changes) {
         auto prod = _producers.find(pc.first.value);
         if (prod != _producers.end()) {
            if (voting && !prod->active() && pc.second.new_vote)
               eosio::check(false, ("producer " + prod->owner.to_string() + " is not currently registered").data());
            _producers.modify(prod, same_payer, [&](auto& p) {
               if (pc.second.old_vote)
                  sub_pool_votes(state, p, voter.last_votes, "bug: producer lost its pool");
               if (pc.second.new_vote)
                  add_pool_votes(state, p, new_pool_votes);
            });
         } else {
            if (pc.second.new_vote) {
               eosio::check(false, ("producer " + pc.first.to_string() + " is not registered").data());
            }
         }
      }

      pool_voter_table.modify(voter, same_payer, [&](auto& pv) {
         pv.producers  = producers;
         pv.proxy      = proxy;
         pv.last_votes = std::move(new_pool_votes);
      });
   } // system_contract::update_pool_votes

   void system_contract::update_pool_proxy(staking_pool_state_autosave& state, const pool_voter& voter) {
      check(!voter.proxy || !voter.is_proxy, "account registered as a proxy is not allowed to use a proxy");

      auto&               pool_voter_table = get_pool_voter_table();
      std::vector<double> new_pool_votes   = voter.owned_shares;
      if (voter.is_proxy)
         for (size_t i = 0; i < new_pool_votes.size(); ++i)
            new_pool_votes[i] += voter.proxied_shares[i];

      if (voter.proxy) {
         auto& proxy = pool_voter_table.get(voter.proxy.value, "bug: proxy not found");
         pool_voter_table.modify(proxy, same_payer, [&](auto& p) {
            sub_proxied_shares(p, voter.last_votes, "bug: proxy lost its pool");
            add_proxied_shares(p, new_pool_votes, "bug: proxy lost its pool");
         });
         update_pool_proxy(state, proxy);
      } else {
         for (auto acnt : voter.producers) {
            auto& prod = _producers.get(acnt.value, "bug: producer not found");
            _producers.modify(prod, same_payer, [&](auto& p) {
               sub_pool_votes(state, p, voter.last_votes, "bug: producer lost its pool");
               add_pool_votes(state, p, new_pool_votes);
            });
         }
      }
      pool_voter_table.modify(voter, same_payer, [&](auto& v) { //
         v.last_votes = std::move(new_pool_votes);
      });
   } // system_contract::update_pool_proxy

   std::vector<const total_pool_votes*> system_contract::top_active_producers(size_t n) {
      std::vector<const total_pool_votes*> prods;
      prods.reserve(n);
      auto idx = get_total_pool_votes_table().get_index<"byvotes"_n>();
      for (auto it = idx.begin(); it != idx.end() && prods.size() < n && it->votes > 0 && it->active; ++it)
         prods.push_back(&*it);
      return prods;
   }

   double system_contract::calc_votes(const std::vector<double>& pool_votes) {
      auto&  pools  = get_staking_pool_state().pools;
      double result = 0;
      eosio::check(pool_votes.size() == pools.size(), "staking pool corruption");
      for (size_t i = 0; i < pools.size(); ++i)
         // result += pools[i].token_pool.simulate_sell(pool_votes[i]).amount * pools[i].vote_weight;
         result += pool_votes[i] * pools[i].vote_weight;
      return result;
   }

   void system_contract::update_total_pool_votes(size_t n) {
      auto& total_table = get_total_pool_votes_table();
      auto  prods       = top_active_producers(n);
      for (auto* prod : prods)
         total_table.modify(*prod, same_payer, [&](auto& prod) {
            prod.votes = calc_votes(*get_prod_pool_votes(_producers.get(prod.owner.value)));
         });
   }

   void system_contract::deposit_pool(staking_pool& pool, double& owned_shares, block_timestamp& next_claim,
                                      asset new_amount) {
      auto                   current_time = eosio::current_block_time();
      eosio::block_timestamp max_next_claim{ current_time.slot + pool.claim_period * 2 };
      auto                   current_balance = pool.token_pool.simulate_sell(owned_shares);
      auto                   new_shares      = pool.token_pool.buy(new_amount);

      eosio::check(new_shares > 0, "new_shares must be positive");

      next_claim = std::max(next_claim, current_time);
      eosio::block_timestamp new_next_claim(
            (next_claim.slot * int128_t(current_balance.amount) + max_next_claim.slot * int128_t(new_amount.amount)) /
            (current_balance.amount + new_amount.amount));

      owned_shares += new_shares;
      next_claim = new_next_claim;
   }

   asset system_contract::withdraw_pool(staking_pool& pool, double& owned_shares, asset max_requested, bool claiming) {
      auto balance = pool.token_pool.simulate_sell(owned_shares);

      if (!claiming && max_requested >= balance) {
         // withdraw all shares; sold might be 0 when stake only contains dust
         auto sold    = pool.token_pool.sell(owned_shares);
         owned_shares = 0;
         return sold;
      } else {
         auto available = balance;
         if (claiming)
            available.amount = (int128_t(balance.amount) * pool.claim_period) / pool.duration;
         auto sell_amount = std::min(available, max_requested);
         eosio::check(sell_amount.amount > 0, "withdrawing 0");

         auto sell_shares = std::min(pool.token_pool.simulate_sell(sell_amount), owned_shares);
         auto sold        = pool.token_pool.sell(sell_shares);
         owned_shares -= sell_shares;
         eosio::check(sell_shares > 0 && sold.amount > 0, "withdrawing 0");
         return sold;
      }
   }

   void system_contract::stake2pool(name owner, uint32_t pool_index, asset amount) {
      require_auth(owner);

      staking_pool_state_autosave state{ *this };
      auto                     core_symbol = get_core_symbol();

      eosio::check(pool_index < state->pools.size(), "invalid pool");
      eosio::check(amount.symbol == core_symbol, "amount doesn't match core symbol");
      eosio::check(amount.amount > 0, "amount must be positive");

      auto& pool_voter_table = get_pool_voter_table();
      auto& voter            = get_or_create_pool_voter(owner);
      auto& pool             = state->pools[pool_index];
      pool_voter_table.modify(voter, same_payer, [&](auto& voter) {
         deposit_pool(pool, voter.owned_shares[pool_index], voter.next_claim[pool_index], amount);
      });

      eosio::token::transfer_action transfer_act{ token_account, { owner, active_permission } };
      transfer_act.send(owner, srpool_account, amount,
                        std::string("transfer from ") + owner.to_string() + " to eosio.vpool");

      update_pool_votes(state, owner, voter.proxy, voter.producers, false);
   }

   void system_contract::setpoolnotif(name owner, std::optional<Bool> xfer_out_notif,
                                      std::optional<Bool> xfer_in_notif) {
      require_auth(owner);
      get_staking_pool_state();
      auto& pool_voter_table = get_pool_voter_table();
      auto& voter            = get_or_create_pool_voter(owner);
      pool_voter_table.modify(voter, same_payer, [&](auto& voter) {
         if (xfer_out_notif)
            voter.xfer_out_notif = *xfer_out_notif;
         if (xfer_in_notif)
            voter.xfer_in_notif = *xfer_in_notif;
      });
   }

   void system_contract::claimstake(name owner, uint32_t pool_index, asset requested) {
      require_auth(owner);

      staking_pool_state_autosave state{ *this };
      auto                     core_symbol      = get_core_symbol();
      auto                     current_time     = eosio::current_block_time();
      auto&                    pool_voter_table = get_pool_voter_table();
      auto&                    voter            = get_or_create_pool_voter(owner);

      eosio::check(pool_index < state->pools.size(), "invalid pool");
      eosio::check(requested.symbol == core_symbol, "requested doesn't match core symbol");
      eosio::check(requested.amount > 0, "requested must be positive");
      distribute_namebid_to_pools(state);

      auto& pool = state->pools[pool_index];
      asset claimed_amount;

      pool_voter_table.modify(voter, same_payer, [&](auto& voter) {
         eosio::check(current_time >= voter.next_claim[pool_index], "claim too soon");
         claimed_amount               = withdraw_pool(pool, voter.owned_shares[pool_index], requested, true);
         voter.next_claim[pool_index] = eosio::block_timestamp(current_time.slot + pool.claim_period * 2);
      });

      eosio::check(pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(pool.token_pool.bal().amount >= 0, "pool amount is negative");

      if (claimed_amount.amount) {
         eosio::token::transfer_action transfer_act{ token_account, { srpool_account, active_permission } };
         transfer_act.send(srpool_account, owner, claimed_amount,
                           std::string("transfer from eosio.vpool to ") + owner.to_string());
      }

      update_pool_votes(state, owner, voter.proxy, voter.producers, false);
   }

   void system_contract::transferstake(name from, name to, uint32_t pool_index, asset requested,
                                       const std::string& memo) {
      require_auth(from);
      eosio::check(memo.size() <= 256, "memo has more than 256 bytes");
      eosio::check(from != to, "from = to");
      eosio::check(eosio::is_account(to), "invalid account");

      staking_pool_state_autosave state{ *this };
      auto                     core_symbol = get_core_symbol();

      eosio::check(pool_index < state->pools.size(), "invalid pool");
      eosio::check(requested.symbol == core_symbol, "requested doesn't match core symbol");
      eosio::check(requested.amount > 0, "requested must be positive");

      auto& pool             = state->pools[pool_index];
      auto& pool_voter_table = get_pool_voter_table();
      auto& from_voter       = pool_voter_table.get(from.value, "from pool_voter record missing");
      bool  created_to_voter = false;
      auto& to_voter         = get_or_create_pool_voter(to, &created_to_voter);
      asset transferred_amount;

      eosio::check(!created_to_voter || requested >= state->min_transfer_create,
                   "requested amount is too small to automatically create pool_voter record");

      pool_voter_table.modify(from_voter, same_payer, [&](auto& from_voter) {
         transferred_amount = withdraw_pool(pool, from_voter.owned_shares[pool_index], requested, false);
         eosio::check(transferred_amount.amount > 0, "transferred 0");
      });

      pool_voter_table.modify(to_voter, same_payer, [&](auto& to_voter) {
         deposit_pool(pool, to_voter.owned_shares[pool_index], to_voter.next_claim[pool_index], transferred_amount);
      });

      eosio::check(pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(pool.token_pool.bal().amount >= 0, "pool amount is negative");

      update_pool_votes(state, from, from_voter.proxy, from_voter.producers, false);
      update_pool_votes(state, to, to_voter.proxy, to_voter.producers, false);

      if (from_voter.xfer_out_notif || to_voter.xfer_in_notif) {
         eosio::action act{ std::vector<eosio::permission_level>{}, from, transferstake_notif,
                            transferstake_notification{
                                  .from               = from,
                                  .to                 = to,
                                  .pool_index         = pool_index,
                                  .requested          = requested,
                                  .transferred_amount = transferred_amount,
                                  .memo               = memo,
                            } };
         if (from_voter.xfer_out_notif) {
            act.account = from;
            act.send();
         }
         if (to_voter.xfer_in_notif) {
            act.account = to;
            act.send();
         }
      }
   } // system_contract::transferstake

   void system_contract::upgradestake(name owner, uint32_t from_pool_index, uint32_t to_pool_index, asset requested) {
      require_auth(owner);

      staking_pool_state_autosave state{ *this };
      auto                     core_symbol = get_core_symbol();

      eosio::check(from_pool_index < state->pools.size(), "invalid pool");
      eosio::check(to_pool_index < state->pools.size(), "invalid pool");
      eosio::check(from_pool_index < to_pool_index, "may only move from a shorter-term pool to a longer-term one");
      eosio::check(requested.symbol == core_symbol, "requested doesn't match core symbol");
      eosio::check(requested.amount > 0, "requested must be positive");

      auto& from_pool        = state->pools[from_pool_index];
      auto& to_pool          = state->pools[to_pool_index];
      auto& pool_voter_table = get_pool_voter_table();
      auto& voter            = pool_voter_table.get(owner.value, "pool_voter record missing");

      pool_voter_table.modify(voter, same_payer, [&](auto& voter) {
         auto transferred_amount = withdraw_pool(from_pool, voter.owned_shares[from_pool_index], requested, false);
         eosio::check(transferred_amount.amount > 0, "transferred 0");
         deposit_pool(to_pool, voter.owned_shares[to_pool_index], voter.next_claim[to_pool_index], transferred_amount);
      });

      eosio::check(from_pool.token_pool.shares() >= 0, "pool shares is negative");
      eosio::check(from_pool.token_pool.bal().amount >= 0, "pool amount is negative");

      update_pool_votes(state, owner, voter.proxy, voter.producers, false);
   } // system_contract::upgradestake

   void system_contract::votewithpool(const name& voter, const name& proxy, const std::vector<name>& producers) {
      require_auth(voter);
      staking_pool_state_autosave state{ *this };
      update_pool_votes(state, voter, proxy, producers, true);
   }

   void system_contract::regpoolproxy(const name& proxy, bool isproxy) {
      require_auth(proxy);
      staking_pool_state_autosave state{ *this };
      auto&                    pool_voter_table = get_pool_voter_table();
      auto&                    voter            = get_or_create_pool_voter(proxy);
      check(!isproxy || !voter.proxy, "account that uses a proxy is not allowed to become a proxy");
      pool_voter_table.modify(voter, same_payer, [&](auto& p) { p.is_proxy = isproxy; });
      update_pool_proxy(state, voter);
   }

   void system_contract::onblock_update_pool(block_timestamp production_time) {
      if (!get_staking_pool_state_singleton().exists())
         return;
      staking_pool_state_autosave state{ *this };
      if (production_time.slot >= state->interval_start.slot + blocks_per_round) {
         state->unpaid_blocks       = state->blocks;
         state->blocks              = 0;
         state->interval_start.slot = (production_time.slot / blocks_per_round) * blocks_per_round;
      }
      ++state->blocks;
   }

   asset system_contract::transition_channel_to_pools(const name& from, const asset& amount, bool partial) {
      eosio::check(amount.symbol == core_symbol(), "incorrect symbol");
      if (!get_staking_pool_state_singleton().exists())
         return amount;
      staking_pool_state_autosave state{ *this };
      std::vector<staking_pool*>  active_pools;
      active_pools.reserve(state->pools.size());
      for (auto& pool : state->pools)
         if (pool.token_pool.shares())
            active_pools.push_back(&pool);
      if (active_pools.empty())
         return amount;

      asset to_pools;
      if (partial)
         to_pools = asset(state->transition(eosio::current_block_time(), int128_t(amount.amount)), amount.symbol);
      else
         to_pools = amount;
      if (to_pools.amount) {
         eosio::token::transfer_action transfer_act{ token_account, { from, active_permission } };
         transfer_act.send(from, srpool_account, to_pools,
                           std::string("transfer from ") + from.to_string() + " to eosio.vpool");
         uint64_t distributed = 0;
         for (size_t i = 0; i < active_pools.size(); ++i) {
            int64_t amt = int128_t(to_pools.amount) * (i + 1) / active_pools.size() - distributed;
            if (amt)
               active_pools[i]->token_pool.adjust({ amt, core_symbol() });
            distributed += amt;
         }
      }

      return amount - to_pools;
   }

   void system_contract::channel_to_rex_or_pools(const name& from, const asset& amount,
                                                 bool require_all_funds_transferred) {
      auto remaining = transition_channel_to_pools(from, amount, !require_all_funds_transferred || rex_available());
      if (remaining.amount) {
         eosio::check(!require_all_funds_transferred || rex_available(), "can't channel fees to pools or to rex");
         channel_to_rex(from, remaining);
      }
   }

   void system_contract::channel_namebid_to_rex_or_pools(int64_t highest_bid) {
      if (!get_staking_pool_state_singleton().exists())
         return channel_namebid_to_rex(highest_bid);

      staking_pool_state_autosave state{ *this };
      bool                     found = false;
      for (auto& pool : state->pools)
         if (pool.token_pool.shares())
            found = true;
      if (!found)
         return channel_namebid_to_rex(highest_bid);

      int64_t to_pools = state->transition(eosio::current_block_time(), int128_t(highest_bid));
      int64_t to_rex   = highest_bid - to_pools;
      state->namebid_proceeds.amount += to_pools;
      if (to_rex)
         channel_namebid_to_rex(to_rex);
   }

   void system_contract::distribute_namebid_to_pools(staking_pool_state_autosave& state) {
      if (state->namebid_proceeds.amount > 0)
         state->namebid_proceeds = transition_channel_to_pools(names_account, state->namebid_proceeds, false);
   }

   void system_contract::updatevotes(name user, name producer) {
      require_auth(user);
      auto& prod = _producers.get(producer.value, "unknown producer");
      eosio::check(prod.is_active, "producer is not active");
      auto* pool_votes = get_prod_pool_votes(_producers.get(prod.owner.value));
      eosio::check(pool_votes, "producer is not upgraded to support pool votes");
      auto& total_table = get_total_pool_votes_table();
      total_table.modify(total_table.get(prod.owner.value), same_payer,
                         [&](auto& prod) { prod.votes = calc_votes(*pool_votes); });
   }

   void system_contract::updatepay(name user) {
      require_auth(user);
      staking_pool_state_autosave state{ *this };
      auto&                    total_table = get_total_pool_votes_table();
      eosio::check(state->unpaid_blocks > 0, "already processed pay for this time interval");

      update_total_pool_votes(state->max_num_pay);
      auto   prods            = top_active_producers(state->max_num_pay);
      double total_votes      = calc_votes(state->total_votes);
      double total_votes_paid = 0;

      const asset token_supply = eosio::token::get_supply(token_account, core_symbol().code());
      auto        pay_scale =
            pow((double)state->unpaid_blocks / blocks_per_round, 10) * state->transition(state->interval_start, 1.0);
      int64_t target_prod_pay = pay_scale * state->prod_rate / rounds_per_year * token_supply.amount;
      int64_t total_prod_pay  = 0;

      if (target_prod_pay > 0 && total_votes > 0) {
         for (auto* prod : prods) {
            if (total_votes_paid >= total_votes * state->max_vote_ratio)
               break;
            total_table.modify(*prod, same_payer, [&](auto& prod) {
               total_votes_paid += prod.votes;
               int64_t pay = (target_prod_pay * prod.votes) / total_votes;
               prod.vote_pay.amount += pay;
               total_prod_pay += pay;
            });
         }
      }

      int64_t per_pool_pay =
            pay_scale * state->voter_rate / rounds_per_year * token_supply.amount / state->pools.size();
      int64_t total_voter_pay = 0;
      for (auto& pool : state->pools) {
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
            transfer_act.send(get_self(), bpspay_account, eosio::asset{ total_prod_pay, core_symbol() },
                              "fund producer pay");
         }
         if (total_voter_pay > 0) {
            eosio::token::transfer_action transfer_act{ token_account, { get_self(), active_permission } };
            transfer_act.send(get_self(), srpool_account, eosio::asset{ total_voter_pay, core_symbol() },
                              "fund voter pay");
         }
      }

      state->unpaid_blocks = 0;
   }

   void system_contract::claimvotepay(name producer) {
      require_auth(producer);
      auto& total_table = get_total_pool_votes_table();
      auto& prod        = total_table.get(producer.value, "unknown producer");
      total_table.modify(prod, same_payer, [&](auto& prod) {
         eosio::check(prod.vote_pay.amount > 0, "no pay available");
         eosio::token::transfer_action transfer_act{ token_account, { bpspay_account, active_permission } };
         transfer_act.send(bpspay_account, producer, prod.vote_pay, "producer pay");
         prod.vote_pay.amount = 0;
      });
   }

} // namespace eosiosystem
