#pragma once

#include <eosio/binary_extension.hpp>

namespace eosiosystem {

   inline bool something_has_value() { return false; }

   template <typename T, typename... Ts>
   bool something_has_value(const eosio::binary_extension<T>& obj, const Ts&... objs) {
      if (obj.has_value())
         return true;
      return something_has_value(objs...);
   }

   template <typename DataStream>
   DataStream& write_extensions(DataStream& ds) {
      return ds;
   }

   // Similar to `ds << ...`, but only supports binary_extensions and leaves off unfilled ones at the end.
   // Also avoids a CDT 1.7 issue that copies inner vectors before serializing them.
   template <typename DataStream, typename T, typename... Ts>
   DataStream& write_extensions(DataStream& ds, const eosio::binary_extension<T>& obj, const Ts&... objs) {
      if (something_has_value(obj, objs...)) {
         if (obj.has_value())
            ds << obj.value();
         else
            ds << T{};
         return write_extensions(ds, objs...);
      }
      return ds;
   }

} // namespace eosiosystem
