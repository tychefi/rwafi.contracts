#pragma once

#include "guarantyrwadb.hpp"
#include <flon/wasm_db.hpp>
#include <flon/flon.token.hpp>
#include <flon/utils.hpp>
namespace rwafi {

using namespace eosio;
using namespace wasm::db;
using namespace flon;
using std::string;


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
class [[eosio::contract("guarantyrwa")]] guarantyrwa : public contract {
public:
    using contract::contract;

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

};

} // namespace rwafi