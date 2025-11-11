#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>
#include <eosio/time.hpp>
#include <flon/wasm_db.hpp>
#include <flon/consts.hpp>
using namespace eosio;
using namespace std;
using namespace wasm::db;
using namespace flon;
namespace rwafi {

// 收益日志表（按月）
struct [[eosio::table, eosio::contract("yield.rwa")]] yield_log_t {
    uint64_t        period;              // 主键：YYYYMM
    asset           period_yield;        // 当月收益
    asset           cumulative_yield;    // 累计收益
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return period; }

    yield_log_t() {}
    yield_log_t(const uint64_t& p): period(p) {}

    typedef eosio::multi_index<"yieldlogs"_n, yield_log_t> idx_t;

    EOSLIB_SERIALIZE(yield_log_t, (period)(period_yield)(cumulative_yield)(created_at)(updated_at))
};

} // namespace rwafi