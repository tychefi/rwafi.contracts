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
 * 担保统计（按计划）
 * scope: self
 */
TBL guaranty_stats_t {
    uint64_t        plan_id;
    asset           total_guarantee_funds;     // 担保池总额
    asset           total_locked_funds;        // 当前应锁定的总担保额
    asset           total_unlocked_funds;      // 已可解押但未取走总额
    asset           used_guarantee_funds;      // 担保已使用
    asset           cumulative_yield;          // 担保池累计分红（投资人部分）
    time_point_sec created_at;
    time_point_sec updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_stats_t() {}
    guaranty_stats_t(const uint64_t& pid): plan_id(pid) {}

    typedef eosio::multi_index<"guarantystat"_n, guaranty_stats_t> idx_t;

    EOSLIB_SERIALIZE(guaranty_stats_t,
        (plan_id)(total_guarantee_funds)(total_locked_funds)(total_unlocked_funds)(used_guarantee_funds)(cumulative_yield)
        (created_at)(updated_at))
};

/**
 * 担保人质押记录
 * scope: plan_id
 */
TBL guarantor_stake_t {
    name       guarantor;            // 担保人账户
    asset      total_stake;          // 总质押本金
    asset      available_stake;      // 可赎回部分（动态更新）
    asset      locked_stake;         // 已锁定（未可解押）
    asset      earned_yield;         // 累计担保分红收益
    asset      withdrawn;            // 已取走金额（总计）
    time_point_sec created_at;
    time_point_sec updated_at;

    uint64_t primary_key() const { return guarantor.value; }

    guarantor_stake_t() {}
    guarantor_stake_t(const name& g): guarantor(g) {}

    typedef eosio::multi_index<"stakes"_n, guarantor_stake_t> idx_t;

    EOSLIB_SERIALIZE(guarantor_stake_t,
        (guarantor)(total_stake)(available_stake)(locked_stake)(earned_yield)(withdrawn)(created_at)(updated_at))
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

} //namespace rwafi