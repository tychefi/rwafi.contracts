#include <flon/flon.token.hpp>
#include "yieldrwa.hpp"
#include <flon/utils.hpp>
#include <flon/consts.hpp>

#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include <invest.rwa/investrwadb.hpp>

using namespace eosio;
using namespace rwafi;
using namespace flon;
static constexpr eosio::name active_perm{"active"_n};

//-----------------------------------------
// 获取当前周期 YYYYMM
//-----------------------------------------
static uint64_t current_period_yyyymm() {
    time_t t = (time_t) current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

// 初始化管理员
void yieldrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin account");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

// 更新分配配置
void yieldrwa::updateconfig(const name& key, const uint8_t& value) {
    require_auth(_gstate.admin);
    CHECKC(_gstate.yield_split_conf.find(key) != _gstate.yield_split_conf.end(),
           err::PARAM_ERROR, "invalid yield key");
    _gstate.yield_split_conf[key] = value;
    _global.set(_gstate, get_self());
}

// 接收转账，分配收益
void yieldrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2 && parts[0] == "plan", err::INVALID_FORMAT, "invalid memo format, expect plan:<id>");
    const uint64_t plan_id = std::stoull(parts[1]);

    // 查找 plan
    fundplan_t::idx_t plan_tbl("invest.rwa"_n, "invest.rwa"_n.value);
    auto plan_itr = plan_tbl.find(plan_id);
    CHECKC(plan_itr != plan_tbl.end(), err::RECORD_NOT_FOUND, "plan not found: " + std::to_string(plan_id));

    // 校验符号匹配
    CHECKC(quantity.symbol == plan_itr->goal_quantity.symbol,
                err::SYMBOL_MISMATCH,
                "symbol mismatch, expected " + plan_itr->goal_quantity.symbol.code().to_string() +
                ", got " + quantity.symbol.code().to_string());

    // 执行分配
    const name bank = get_first_receiver();
    _perform_distribution(bank, quantity, plan_id);
    _log_yield(plan_id, quantity);
}

// 分配收益（stake / swap / guaranty）
void yieldrwa::_perform_distribution(const name& bank, const asset& total, const uint64_t& plan_id) {
    auto cfg = _gstate.yield_split_conf;

    CHECKC(cfg.count(STAKE_POOL) && cfg.count(GUARANTY_POOL) && cfg.count(SWAP_POOL),
                        err::PARAM_ERROR, "yield_split_conf missing one or more required keys");
    // 校验比例总和
    uint16_t total_ratio = cfg[STAKE_POOL] + cfg[GUARANTY_POOL] + cfg[SWAP_POOL];
    CHECKC(total_ratio == 100, err::PARAM_ERROR,
           "yield_split_conf ratio sum must equal 100 (current=" + std::to_string(total_ratio) + ")");

    auto amt_stake     = asset{(total.amount * cfg[STAKE_POOL]) / 100, total.symbol};
    auto amt_guaranty  = asset{(total.amount * cfg[GUARANTY_POOL]) / 100, total.symbol};
    auto amt_swap      = total - amt_stake - amt_guaranty;

    // 仅当金额非零才转账
    if (amt_stake.amount > 0)
        TRANSFER(bank, STAKE_POOL, amt_stake, "reward:" + std::to_string(plan_id));
    // if (amt_swap.amount > 0)
    //     TRANSFER(bank, SWAP_POOL, amt_swap, "AMM Swap buyback & burn");
    if (amt_guaranty.amount > 0)
        TRANSFER(bank, GUARANTY_POOL, amt_guaranty, "guaranty:" + std::to_string(plan_id));
}

// 写入月度收益记录
void yieldrwa::_log_yield(const uint64_t& plan_id, const asset& quantity) {

    if (quantity.amount <= 0) return;

    uint64_t period = current_period_yyyymm();
    time_point_sec now = time_point_sec(current_time_point());


    yield_log_t::idx_t yieldlogs(get_self(), plan_id);

    auto itr = yieldlogs.find(period);

    if (yieldlogs.begin() != yieldlogs.end()) {
        CHECKC(yieldlogs.begin()->period_yield.symbol == quantity.symbol,
                            err::SYMBOL_MISMATCH, "symbol mismatch in yield log");
    }
    if (itr == yieldlogs.end()) {
        asset cumulative_prev(0, quantity.symbol);
        if (yieldlogs.begin() != yieldlogs.end()) {
            auto last = yieldlogs.end();
            --last;
            cumulative_prev = last->cumulative_yield;
        }

        yieldlogs.emplace(get_self(), [&](auto& y) {
            y.period           = period;
            y.period_yield     = quantity;
            y.cumulative_yield = cumulative_prev + quantity;
            y.created_at = y.updated_at = now;
        });
    } else {
        yieldlogs.modify(itr, get_self(), [&](auto& y) {
            y.period_yield     += quantity;
            y.cumulative_yield += quantity;
            y.updated_at        = now;
        });
    }
}

// 计算年度收益核心逻辑
asset yieldrwa::_calc_yearly_yield_core(const uint64_t& plan_id, const uint64_t& year) const {
    yield_log_t::idx_t yieldlogs(get_self(), plan_id);
    CHECKC(yieldlogs.begin() != yieldlogs.end(), err::RECORD_NOT_FOUND, "no yield records found for plan");

    int64_t total = 0;
    symbol sym;
    bool has_symbol = false;

    uint64_t min_period = year * 100 + 1;
    uint64_t max_period = (year + 1) * 100;

    for (auto itr = yieldlogs.begin(); itr != yieldlogs.end(); ++itr) {
        if (!has_symbol) {
            sym = itr->period_yield.symbol;
            has_symbol = true;
        }

        CHECKC(itr->period_yield.symbol == sym, err::SYMBOL_MISMATCH,
               "inconsistent symbol in yield records");

        if (itr->period < min_period) continue;
        if (itr->period >= max_period) break;

        total += itr->period_yield.amount;
    }

    if (!has_symbol)
        sym = yieldlogs.begin()->period_yield.symbol;

    return asset(total, sym);
}

asset yieldrwa::get_yearly_yield(const uint64_t& plan_id, const uint64_t& year) const {
    return _calc_yearly_yield_core(plan_id, year);
}