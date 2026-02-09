#pragma once

#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/permission.hpp>
#include <eosio/action.hpp>


#include "rwastakepooldb.hpp"

namespace rwafi {

using namespace eosio;
using namespace std;
using namespace flon;
using namespace wasm::db;

/**
 * 合约：rwastakepool
 * 功能：RWA 质押奖励系统
 * 说明：
 *   - 用户通过 rwa.vtoken 转账质押（on_transfer_rwafi）
 *   - 管理员通过 sing.token 转账注入奖励（on_transfer_reward）
 *   - 用户通过 claim 领取 SING 奖励
 */
class [[eosio::contract("rwastakepool")]] rwastakepool : public contract {
public:
    using contract::contract;

    /**
     * 添加质押计划（由管理员调用）
     * @param plan_id 对应 rwaverse.io 的 fundplan.id
     * @param receipt_sym 质押凭证币符号（如 RWA1）
     */
    ACTION addplan(const uint64_t& plan_id,const symbol& receipt_symbol,const name& reward_token_contract,const symbol& reward_symbol);

    /**
     * 删除质押计划（需池为空）
     * @param plan_id 要删除的计划 ID
     */
    ACTION delplan(const uint64_t& plan_id);

    /**
     * 用户领取奖励（SING）
     * @param owner 用户账户
     * @param plan_id 质押池ID
     */
    ACTION claim(const name& owner, const uint64_t& plan_id);

    ACTION batchunstake(const uint64_t& plan_id);

    using claim_action      = eosio::action_wrapper<"claim"_n, &rwastakepool::claim>;
    using addplan_action    = eosio::action_wrapper<"addplan"_n, &rwastakepool::addplan>;
    using batchunstake_action    = eosio::action_wrapper<"batchunstake"_n, &rwastakepool::batchunstake>;
};

} // namespace rwafi