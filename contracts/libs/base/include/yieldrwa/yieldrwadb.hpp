#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>
#include <eosio/time.hpp>
#include <flon/wasm_db.hpp>

using namespace eosio;
using namespace std;

namespace rwafi {


#define TBL struct [[eosio::table, eosio::contract("yield.rwa")]]

TBL yield_log_t {
    uint64_t        period;              // 主键：YYYYMM
    asset           period_yield;        // 当期（月度）分红总额
    asset           cumulative_yield;    // 累计分红总额
    time_point_sec  created_at;          // 创建时间
    time_point_sec  updated_at;          // 最近更新时间

    uint64_t primary_key() const { return period; }

    yield_log_t() {}
    yield_log_t(const uint64_t& p): period(p) {}

    typedef eosio::multi_index<"yieldlogs"_n, yield_log_t> idx_t;

    EOSLIB_SERIALIZE(yield_log_t, (period)(period_yield)(cumulative_yield)(created_at)(updated_at))
};

} // namespace rwafi