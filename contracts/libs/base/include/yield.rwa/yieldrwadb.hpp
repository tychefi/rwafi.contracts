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

// ----------------------------------------------------
// 表宏定义
// ----------------------------------------------------
#define TBL struct [[eosio::table, eosio::contract("yield.rwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("yield.rwa")]]

// ----------------------------------------------------
// 收益日志表（按月）
// ----------------------------------------------------
//self:plan_id
TBL yield_log_t {
    uint64_t        period;               // 主键：YYYYMM
    asset           period_yield;         // 当月总收益
    asset           guarantor_yield;      // 担保人收益 (≈10%×覆盖率)
    asset           investor_yield;       // 投资人收益 (≈80%)
    asset           buyback_yield;        // 回购凭证收益 (剩余)
    asset           cumulative_yield;     // 累计总收益
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return period; }

    yield_log_t() {}
    yield_log_t(const uint64_t& p): period(p) {}

    typedef eosio::multi_index<"yieldlogs"_n, yield_log_t> idx_t;

    EOSLIB_SERIALIZE(yield_log_t,(period)(period_yield)(guarantor_yield)(investor_yield)
                    (buyback_yield)(cumulative_yield)(created_at)(updated_at))
};

} // namespace rwafi