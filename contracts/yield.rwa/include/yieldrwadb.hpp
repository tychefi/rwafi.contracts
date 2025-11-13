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
// 全局配置表（收益分配比例）
// ----------------------------------------------------
NTBL("global") global_t {
    name admin;

    map<name, uint8_t> yield_split_conf = {
        { STAKE_POOL,     80 },
        { SWAP_POOL,      10 },
        { GUARANTY_POOL,  10 }
    };

    EOSLIB_SERIALIZE(global_t, (admin)(yield_split_conf))
};
typedef eosio::singleton<"global"_n, global_t> global_singleton;

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
//self: self
TBL plan_buyback_t {
    uint64_t    plan_id;               // PK

    asset       total_buyback;         // 累计 buyback_yield（从收益分配累积）
    asset       used_buyback;          // 已用于回购的 SING 数量

    asset       total_voucher;         // 累计回购到的凭证资产

    uint16_t    max_slippage = 100;    // 1% = 100 bp

    time_point_sec updated_at;

    uint64_t primary_key() const { return plan_id; }

    // 动态计算剩余 SING
    asset remaining() const {
        return asset(
            total_buyback.amount - used_buyback.amount,
            total_buyback.symbol
        );
    }
    typedef eosio::multi_index<"planbuyback"_n, plan_buyback_t> pl_tbl;

    EOSLIB_SERIALIZE(plan_buyback_t,(plan_id)(total_buyback)(used_buyback)(total_voucher)(max_slippage)(updated_at))
};



} // namespace rwafi