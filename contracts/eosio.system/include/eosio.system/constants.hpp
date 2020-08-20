#pragma once

#include <stdint.h>

namespace eosiosystem {
   static constexpr uint32_t seconds_per_year  = 52 * 7 * 24 * 3600;
   static constexpr uint32_t seconds_per_day   = 24 * 3600;
   static constexpr uint32_t seconds_per_hour  = 3600;
   static constexpr int64_t  useconds_per_year = int64_t(seconds_per_year) * 1000'000ll;
   static constexpr int64_t  useconds_per_day  = int64_t(seconds_per_day) * 1000'000ll;
   static constexpr int64_t  useconds_per_hour = int64_t(seconds_per_hour) * 1000'000ll;
   static constexpr uint32_t blocks_per_day    = 2 * seconds_per_day; // half seconds per day
   static constexpr uint32_t blocks_per_round  = 21 * 12;             // 21 producers * 12 blocks ea turn
   static constexpr double   rounds_per_year   = double(seconds_per_year) * 2 / blocks_per_round;
} // namespace eosiosystem
