#pragma once

#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/permission.hpp>
#include <eosio/action.hpp>


#include "stakerwadb.hpp"

namespace rwafi {

using namespace eosio;
using namespace std;
using namespace flon;
using namespace wasm::db;

/**
 * 合约：stakerwa
 * 功能：RWA 质押奖励系统
 * 说明：
 *   - 用户通过 rwafi.token 转账质押（on_transfer_rwafi）
 *   - 管理员通过 sing.token 转账注入奖励（on_transfer_reward）
 *   - 用户通过 claim 领取 SING 奖励
 */
class [[eosio::contract("stakerwa")]] stakerwa : public contract {
public:
    using contract::contract;

    /**
     * 初始化（仅一次）
     * @param admin 管理员账户
     * @param investrwa_contract RWA 主投资合约
     */
    ACTION init(const name& admin, const name& investrwa_contract);

    /**
     * 添加质押计划（由管理员调用）
     * @param plan_id 对应 invest.rwa 的 fundplan.id
     * @param receipt_sym 质押凭证币符号（如 RWA1）
     */
    ACTION addplan(const uint64_t& plan_id, const symbol& receipt_sym);

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


    ACTION unstake(const name& owner, const uint64_t& plan_id, const asset& quantity) ;


    using claim_action      = eosio::action_wrapper<"claim"_n, &stakerwa::claim>;
    using addplan_action    = eosio::action_wrapper<"addplan"_n, &stakerwa::addplan>;

};

} // namespace rwafi