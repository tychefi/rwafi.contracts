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

namespace rwafi {

using namespace eosio;
using std::string;
using namespace wasm::db;
using namespace flon;

static constexpr eosio::name active_perm{"active"_n};

// ===== 时间基础单位 =====
static constexpr uint64_t DAY_SECONDS        = 10; // 24 * 3600;
static constexpr uint64_t seconds_per_month  = 30 * DAY_SECONDS;

// ===== 业务周期定义 =====
static constexpr uint64_t MONTHS_PER_YEAR    = 12;
static constexpr uint64_t seconds_per_year   = MONTHS_PER_YEAR * seconds_per_month;

// ===== 短期过期 / 宽限（⚠️ 非周期）=====
static constexpr uint8_t  EXPIRY_HOURS       = 12;
static constexpr uint32_t MAX_TITLE_SIZE     = 64;


#define TBL struct [[eosio::table, eosio::contract("rwaguarapool")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("rwaguarapool")]]

/**
 * 全局配置：存放关联合约账户
 */
NTBL("global") global_t {
    name            admin;                                      // 管理员
    name            invest_contract     = "rwaverse.io"_n;       // 投资/募资主合约
    name            yield_contract      = "rwayieldpool"_n;        // 收益日志/计算合约
    name            stake_contract      = "rwastakepool"_n;        // 质押/分配合约（担保金转入目标）

    EOSLIB_SERIALIZE( global_t,
        (admin)(invest_contract)(yield_contract)(stake_contract))
};
typedef eosio::singleton< "global"_n, global_t > global_singleton;

/**
 * 担保统计（按计划）
 * scope: self
 */
TBL guaranty_stats_t {
    uint64_t        plan_id;
    asset           total_guarantee_funds;     // 担保池总额
    asset           used_guarantee_funds;      // 担保已使用
    asset           cumulative_yield;          // 担保池累计分红（投资人部分）
    time_point_sec created_at;
    time_point_sec updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_stats_t() {}
    guaranty_stats_t(const uint64_t& pid): plan_id(pid) {}

    typedef eosio::multi_index<"guarantystat"_n, guaranty_stats_t> idx_t;

    EOSLIB_SERIALIZE(guaranty_stats_t,(plan_id)(total_guarantee_funds)(used_guarantee_funds)(cumulative_yield)(created_at)(updated_at))
};

struct redeem_cap_t {
    int64_t total_unlock_cap = 0;   // 个人累计可解封上限（按 shares 分摊）
    int64_t remaining_unlock = 0;   // 个人当前剩余可解封（扣掉 withdrawn）
    int64_t global_unlock    = 0;   // 全局可解封总量（G + investor_yield - H）
    int64_t investor_yield   = 0;   // 累计投资人收益
    int64_t H                = 0;   // 半仓安全线
};

/**
 * 担保人质押记录
 * scope: plan_id
 */
TBL guarantor_stake_t {
    name       guarantor;     // 担保人
    asset      shares;        // 永久份额（加入时mint，取回时按比例burn）
    asset      total_stake;   // 当前担保人持有资产（本金+分红）
    asset      earned_yield;  // 累计分红记录（仅记录，不用于锁仓算法）
    asset      withdrawn;     // 总计已取走金额
    time_point_sec created_at;
    time_point_sec updated_at;

    uint64_t primary_key() const { return guarantor.value; }

    typedef eosio::multi_index<"stakes"_n, guarantor_stake_t> idx_t;
    EOSLIB_SERIALIZE(guarantor_stake_t,
        (guarantor)(shares)(total_stake)(earned_yield)(withdrawn)(created_at)(updated_at)
    )
};

} //namespace rwafi