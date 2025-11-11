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
class [[eosio::contract("stake.rwa")]] stakerwa : public contract {
public:
    using contract::contract;

    stakerwa(name receiver, name code, datastream<const char*> ds)
    : contract(receiver, code, ds),
      _global(get_self(), get_self().value),
      _db(get_self())
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
    }

    ~stakerwa() {
        _global.set(_gstate, get_self());
    }

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

    ACTION batchunstake(const uint64_t& plan_id);
    // ========== 监听转账 ==========
    /**
     * 用户质押（监听 rwafi.token 转账）
     * memo 格式： "stake:<plan_id>"
     */
    [[eosio::on_notify("rwafi.token::transfer")]]
    void on_transfer_rwafi(const name& from, const name& to, const asset& quantity, const std::string& memo);

    /**
     * 管理员充值奖励（监听 sing.token 转账）
     * memo 格式： "reward:<plan_id>"
     */
    [[eosio::on_notify("sing.token::transfer")]]
    void on_transfer_reward(const name& from, const name& to, const asset& quantity, const std::string& memo);


    using claim_action      = eosio::action_wrapper<"claim"_n, &stakerwa::claim>;
    using addplan_action    = eosio::action_wrapper<"addplan"_n, &stakerwa::addplan>;
    using batchunstake_action    = eosio::action_wrapper<"batchunstake"_n, &stakerwa::batchunstake>;

private:
    // ========== 内部逻辑函数 ==========

    /**
     * 处理用户质押
     */
    void _on_stake(const name& from, const asset& quantity, const uint64_t& plan_id);

    /**
     * 处理奖励注入
     */
    void _on_reward_in(const name& from, const asset& quantity, const uint64_t& plan_id);

    /**
     * 计算 reward_per_share 增量
     */
    static int128_t calc_reward_per_share_delta(const asset& rewards, const asset& total_staked);

    /**
     * 计算单个用户应得奖励
     */
    static asset calc_user_reward(const asset& staked, const int128_t& reward_per_share_delta, const symbol& reward_symbol);


private:
    global_singleton       _global;
    global_t               _gstate;
    dbc                    _db;
};

} // namespace rwafi