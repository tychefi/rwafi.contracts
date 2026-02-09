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

#define TBL struct [[eosio::table, eosio::contract("rwaguarapool")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("rwaguarapool")]]

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