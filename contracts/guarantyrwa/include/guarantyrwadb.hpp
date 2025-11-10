#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/privileged.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>

#include <flon/wasm_db.hpp>
#include <flon/flon.token.hpp>
#include <flon/utils.hpp>
#include <flon/consts.hpp>

namespace rwafi {

using namespace eosio;
using std::string;
using namespace wasm::db;
using namespace flon;

static constexpr eosio::name active_perm{"active"_n};

#define TBL struct [[eosio::table, eosio::contract("guaranty.rwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("guaranty.rwa")]]

/**
 * 全局配置：存放关联合约账户
 */
NTBL("global") global_t {
    name            admin;                               // 管理员
    name            invest_contract     = INVEST_POOL;   // 投资/募资主合约
    name            yield_contract      = YIELD_POOL;    // 收益日志/计算合约
    name            stake_contract      = STAKE_POOL;    // 质押/分配合约（担保金转入目标）

    EOSLIB_SERIALIZE( global_t,
        (admin)(invest_contract)(yield_contract)(stake_contract))
};
typedef eosio::singleton< "global"_n, global_t > global_singleton;

/**
 * 担保统计（按计划）
 * scope: self
 */
TBL guaranty_stats_t {
    uint64_t        plan_id;                        // PK
    asset           total_guarantee_funds;          // 充值累计
    asset           available_guarantee_funds;      // 可用
    asset           used_guarantee_funds;           // 已用（已支付）
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_stats_t() {}
    guaranty_stats_t(const uint64_t& pid): plan_id(pid) {}

    typedef eosio::multi_index<"guarantystat"_n, guaranty_stats_t> idx_t;

    EOSLIB_SERIALIZE(guaranty_stats_t,
        (plan_id)(total_guarantee_funds)(available_guarantee_funds)(used_guarantee_funds)
        (created_at)(updated_at))
};

/**
 * 担保人质押记录
 * scope: plan_id
 */
TBL guarantor_stake_t {
    name            guarantor;       // ✅ 主键：担保人
    asset           total_funds;     // ✅ 当前担保金额（实时更新）
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return guarantor.value; }

    guarantor_stake_t() {}
    guarantor_stake_t(const name& g): guarantor(g) {}

    typedef eosio::multi_index<"stakes"_n, guarantor_stake_t> idx_t;

    EOSLIB_SERIALIZE(guarantor_stake_t,
        (guarantor)(total_funds)(created_at)(updated_at))
};

/**
 * 每月支付记录（period=YYYYMM）
 * scope: plan_id
 */
TBL plan_payment_t {
    uint64_t        period;                         // PK: YYYYMM（例：202511 表示 2025-11）
    asset           total_paid;                     // 当月担保累计支付
    time_point_sec  created_at;

    uint64_t primary_key() const { return period; }

    plan_payment_t() {}
    explicit plan_payment_t(const uint64_t yyyymm): period(yyyymm) {}

    typedef eosio::multi_index<"payments"_n, plan_payment_t> idx_t;

    EOSLIB_SERIALIZE(plan_payment_t,
        (period)(total_paid)(created_at))
};

/**
 * 每计划担保配置（覆盖比例等）
 * scope: self
 */
TBL guaranty_conf_t {
    uint64_t        plan_id;                        // PK
    uint16_t        coverage_ratio_bp = 10000;      // 部分担保覆盖比例（基点：10000=100%）
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_conf_t() {}
    explicit guaranty_conf_t(const uint64_t& pid): plan_id(pid) {}

    typedef eosio::multi_index<"gconfs"_n, guaranty_conf_t> idx_t;

    EOSLIB_SERIALIZE(guaranty_conf_t,
        (plan_id)(coverage_ratio_bp)(created_at)(updated_at))
};

} //namespace rwafi