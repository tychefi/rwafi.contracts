#pragma once

#include "guarantyrwadb.hpp"
#include <investrwa/investrwadb.hpp>
namespace rwafi {

using namespace eosio;
using namespace wasm::db;
using namespace flon;
using std::string;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT       = 0,
   TYPE_INVALID         = 1,
   FEE_NOT_FOUND        = 2,
   QUANTITY_INSUFFICIENT  = 3,
   NOT_POSITIVE         = 4,
   SYMBOL_MISMATCH      = 5,
   EXPIRED              = 6,
   PWHASH_INVALID       = 7,
   RECORD_NOT_FOUND     = 8,
   RECORD_EXISTS        = 9,
   NOT_EXPIRED          = 10,
   ACCOUNT_INVALID      = 11,
   FEE_NOT_POSITIVE     = 12,
   VAILD_TIME_INVALID   = 13,
   MIN_UNIT_INVALID     = 14,
   RED_PACK_EXIST       = 15,
   NO_AUTH              = 16,
   UNDER_MAINTENANCE    = 17,
   NONE_DELETED         = 19,
   IN_THE_WHITELIST     = 20,
   NON_RENEWAL          = 21,
   INVALID_STATUS       = 31,
   CONTRACT_MISMATCH    = 32,
   PARAM_ERROR          = 33
};

/**
 * @contract guarantyrwa
 * @brief RWA 收益担保合约
 *
 * 功能说明：
 *  - 接收担保人质押资金（监听 sing.token::transfer）
 *  - 管理计划担保资金（guaranty_stats_t）
 *  - 定期计算应付担保收益（guarantpay）
 *  - 支持部分担保比例 coverage_ratio_bp（最低 5000 = 50%）
 *  - 允许担保人提取质押资金（withdraw）
 */
class [[eosio::contract("guaranty.rwa")]] guarantyrwa : public contract {
public:
    using contract::contract;

    guarantyrwa(name receiver, name code, datastream<const char*> ds)
    : contract(receiver, code, ds),
      _db(get_self()),
      _db_invest(get_self()),
      _global(get_self(), get_self().value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
        _db_invest = dbc(_gstate.invest_contract);
    }

    ~guarantyrwa() {
        _global.set(_gstate, get_self());
    }

    /**
     * @notice 接收担保金充值
     * @dev memo 格式: "guaranty:<plan_id>"
     * @dev 资金来源：sing.token
     * @dev 资金符号必须与计划 fundplan_t.goal_quantity.symbol 一致
     */
    [[eosio::on_notify("sing.token::transfer")]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    /**
     * @notice 初始化合约
     * @param admin 管理员账户
     */
    ACTION init(const name& admin);

    /**
     * @notice 设置计划担保覆盖配置
     * @param plan_id RWA 计划ID
     * @param coverage_ratio_bp 担保覆盖比例（基点制：10000=100%，最低5000=50%）
     *
     * @details
     * - 用于配置某个计划的部分担保比例；
     * - 系统强制要求 `coverage_ratio_bp ∈ [5000, 10000]`
     */
    ACTION setgconf(const uint64_t& plan_id, const uint16_t& coverage_ratio_bp);

    /**
     * @notice 担保收益支付（按月触发）
     * @param submitter 发起者（管理员）
     * @param plan_id RWA 计划ID
     * @param months 支付月数（>=1）
     *
     * @details
     * - 每月担保额 = goal_quantity × (APR / 10000) / 12
     * - 实际支付 = 每月担保额 × months × (coverage_ratio_bp / 10000)
     * - 从合约余额发放到 _gstate.stake_contract
     */
    ACTION guarantpay(const name& submitter, const uint64_t& plan_id, const uint64_t& months);

    /**
     * @notice 担保人赎回质押资金
     * @param guarantor 担保人账户
     * @param plan_id RWA 计划ID
     * @param quantity 要赎回的资金数量
     */
    ACTION withdraw(const name& guarantor, const uint64_t& plan_id, const asset& quantity);

private:

    /**
     * @brief 获取当前时间对应的年月（YYYYMM）
     */
    static uint64_t _current_period_yyyymm();

    /**
     * @brief 计算计划每月最低担保收益
     * @details min_monthly = goal_quantity × (APR / 10000) / 12
     */
    static asset _monthly_guarantee_min(const fundplan_t& plan);


    void _withdraw_failed_project(const name& guarantor,
                                            const fundplan_t& plan,
                                            guaranty_stats_t& stats,
                                            const asset& quantity);

    void _withdraw_in_progress(const name& guarantor,
                                        const fundplan_t& plan,
                                        guaranty_stats_t& stats,
                                        const guaranty_conf_t& conf,
                                        const asset& quantity);

    void _withdraw_project_end(const name& guarantor,
                                        const fundplan_t& plan,
                                        guaranty_stats_t& stats,
                                        const guaranty_conf_t& conf,
                                        const asset& quantity) ;

    void _do_withdraw(const name& guarantor,
                                const fundplan_t& plan,
                                guaranty_stats_t& stats,
                                const asset& quantity,
                                const string& memo) ;
    void _deduct_from_guarantors(uint64_t plan_id, const asset& pay);

private:
    dbc              _db;           ///< 本合约数据库
    dbc              _db_invest;    ///< 投资计划数据库 (investrwa)
    global_singleton _global;       ///< 全局配置
    global_t         _gstate;       ///< 全局状态
};

} // namespace rwafi