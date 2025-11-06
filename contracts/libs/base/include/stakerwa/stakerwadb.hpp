#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>
#include <map>
#include <memory>
#include <string>
#include <flon/consts.hpp>
#include "flon/utils.hpp"
#include "flon/wasm_db.hpp"

namespace rwafi {

using namespace eosio;
using namespace std;
using namespace flon;

#define TBL struct [[eosio::table, eosio::contract("stakerwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("stakerwa")]]

struct stake_reward_st {
    uint64_t        reward_id = 0;                                  // 发奖自增 ID
    asset           total_rewards;                                  // 总奖励 = unalloted + unclaimed + claimed
    asset           last_rewards;                                   // 最近一次新增奖励额
    asset           unalloted_rewards;                              // 未分配（admin 刚打入的）
    asset           unclaimed_rewards;                              // 已分配未领取
    asset           claimed_rewards;                                // 已领取总额
    int128_t        reward_per_share        = 0;                    // 每单位质押的累计奖励积分
    int128_t        last_reward_per_share   = 0;                    // 上次发奖时的奖励积分
    time_point_sec  reward_added_at;                                // 最近奖励发放时间
    time_point_sec  prev_reward_added_at;                           // 上一次奖励时间
    name            reward_token_contract = "sing.token"_n;         // 发奖代币合约
    symbol          reward_symbol         = SING_SYM;               // 奖励代币符号
};

//Scope: _self
TBL stake_plan_t {
    uint64_t            plan_id;                                    // 主键: 对应 invest.rwa 的 fundplan.id
    symbol              receipt_symbol;                             // 质押凭证币符号（receipt token）
    asset               cum_staked;                                 // 累计历史质押总额
    asset               total_staked;                               // 当前质押总额
    stake_reward_st     reward_state;                               // 奖励进度记录
    time_point_sec      created_at;                                 // 创建时间

    stake_plan_t() {}
    stake_plan_t(const uint64_t& i): plan_id(i) {}

    uint64_t primary_key() const { return plan_id; }

    typedef eosio::multi_index<"stakeplans"_n, stake_plan_t> tbl_t;

    EOSLIB_SERIALIZE(stake_plan_t,
        (plan_id)(receipt_symbol)(cum_staked)(total_staked)
        (reward_state)(created_at))
};

//Scope: code
//Note: record will be deleted upon full unstake
TBL staker_t {
    name                owner;                                      // PK: 用户账户
    uint64_t            plan_id;                                    // 质押计划ID（关联 stake_plan_t.plan_id）
    asset               cum_staked;                                 // 累计质押数量（历史统计）
    asset               avl_staked;                                 // 当前质押数量（可赎回部分）
    stake_reward_st     stake_reward;                               // 奖励信息（仅 SING）
    time_point_sec      stake_started_at;                           // 开始质押时间（首次入池时间）
    time_point_sec      last_stake_at;                              // 最近质押时间
    time_point_sec      last_claim_at;                              // 最近领取时间
    time_point_sec      created_at;                                 // 记录创建时间

    staker_t() {}
    staker_t(const name& a, const uint64_t& pid): owner(a), plan_id(pid) {}

    uint64_t primary_key() const { return owner.value; }
    uint128_t by_plan_user() const { return ((uint128_t)plan_id << 64) | owner.value; }

    typedef eosio::multi_index<
        "stakers"_n,
        staker_t,
        indexed_by<"byplanuser"_n, const_mem_fun<staker_t, uint128_t, &staker_t::by_plan_user>>
    > tbl_t;

    EOSLIB_SERIALIZE(staker_t,
        (owner)(plan_id)
        (cum_staked)(avl_staked)
        (stake_reward)
        (stake_started_at)(last_stake_at)(last_claim_at)(created_at))
};





} // namespace rwafi