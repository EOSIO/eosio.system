#pragma once

#include <eosio/binary_extension.hpp>

namespace eosiosystem {

   inline bool something_has_value() { return false; }

   template <typename T, typename... Ts>
   bool something_has_value(const T& obj, const Ts&... objs) {
      if (obj.has_value())
         return true;
      return something_has_value(objs...);
   }

   template <typename DataStream>
   DataStream& write_extensions(DataStream& ds) {
      return ds;
   }

   template <typename DataStream, typename T, typename... Ts>
   DataStream& write_extensions(DataStream& ds, const T& obj, const Ts&... objs) {
      if (something_has_value(obj, objs...)) {
         ds << obj;
         return write_extensions(ds, objs...);
      }
      return ds;
   }

} // namespace eosiosystem
