#pragma once

#include <stdint.h>

namespace eosiosystem {
   static constexpr uint32_t seconds_per_year  = 52 * 7 * 24 * 3600;
   static constexpr uint32_t seconds_per_day   = 24 * 3600;
   static constexpr uint32_t seconds_per_hour  = 3600;
   static constexpr int64_t  useconds_per_year = int64_t(seconds_per_year) * 1000'000ll;
   static constexpr int64_t  useconds_per_day  = int64_t(seconds_per_day) * 1000'000ll;
   static constexpr int64_t  useconds_per_hour = int64_t(seconds_per_hour) * 1000'000ll;
   static constexpr uint32_t blocks_per_minute = 2 * 60;              // half seconds per minute
   static constexpr uint32_t blocks_per_day    = 2 * seconds_per_day; // half seconds per day
   static constexpr uint32_t blocks_per_week   = 7 * blocks_per_day;  // half seconds per week
} // namespace eosiosystem
