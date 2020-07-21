#include <eosio.system/eosio.system.hpp>

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

} // namespace eosiosystem
