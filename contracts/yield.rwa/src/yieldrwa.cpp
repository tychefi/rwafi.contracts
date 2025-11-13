#include <flon/flon.token.hpp>
#include "yieldrwa.hpp"
#include <flon/utils.hpp>
#include <flon/consts.hpp>

#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include <invest.rwa/investrwadb.hpp>
#include <guaranty.rwa/guarantyrwadb.hpp>
#include <flon.swap/flon.swap.db.hpp>

using namespace eosio;
using namespace rwafi;
using namespace flon;

static constexpr eosio::name active_perm{"active"_n};

static uint64_t current_period_yyyymm() {
    time_t t = (time_t) current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

double get_amm_price(const asset& in_pool, const asset& out_pool)
{
    int64_t in_amount = in_pool.amount;
    int64_t out_amount = out_pool.amount;

    int64_t in_boost = power10(in_pool.symbol.precision());
    int64_t out_boost = power10(out_pool.symbol.precision());

    return (double)out_amount * in_boost / (in_amount * out_boost);
}

int64_t calc_min_received(double min_price, const asset& input, const symbol& output_symbol)
{
    int64_t in_boost  = power10(input.symbol.precision());
    int64_t out_boost = power10(output_symbol.precision());

    double out = (double)input.amount * min_price * out_boost / in_boost;
    return (int64_t)out;
}

string build_swap_memo(const extended_asset& input,const name& pair_name,double slippage,const name& swap_contract)
{
    CHECKC(slippage >= 0 && slippage <= 1, err::PARAM_ERROR, "invalid slippage");

    flon::market_t::idx_t markets(swap_contract, swap_contract.value);
    auto itr = markets.find(pair_name.value);
    CHECKC(itr != markets.end(), err::RECORD_NOT_FOUND, "swap market not found");

    extended_asset left = itr->left_pool_quant;
    extended_asset right = itr->right_pool_quant;

    bool is_left_input =
        (input.contract == left.contract &&
         input.quantity.symbol == left.quantity.symbol);

    bool is_right_input =
        (input.contract == right.contract &&
         input.quantity.symbol == right.quantity.symbol);

    CHECKC(is_left_input || is_right_input, err::SYMBOL_MISMATCH, "input not in pair");

    extended_asset in_pool  = is_left_input ? left  : right;
    extended_asset out_pool = is_left_input ? right : left;

    double price = get_amm_price(in_pool.quantity, out_pool.quantity);
    double min_price = price * (1.0 - slippage);

    int64_t min_amt = calc_min_received(min_price, input.quantity, out_pool.quantity.symbol);

    return string("swap:") + asset(min_amt, out_pool.quantity.symbol).to_string()
           + ":" + pair_name.to_string();
}

name yieldrwa::find_pair_by_symbols(const symbol& in_sym,const symbol& out_sym,const name& swap_contract)
{
    flon::market_t::idx_t markets(swap_contract, swap_contract.value);

    for (auto itr = markets.begin(); itr != markets.end(); ++itr) {
        symbol left  = itr->left_pool_quant.quantity.symbol;
        symbol right = itr->right_pool_quant.quantity.symbol;

        if ((in_sym == left && out_sym == right) ||
            (in_sym == right && out_sym == left))
        {
            return itr->tpcode;
        }
    }

    CHECKC(false, err::RECORD_NOT_FOUND,
           "swap pair not found for symbols: "
           + in_sym.code().to_string() + " <-> " + out_sym.code().to_string());
    return name{0};
}

void yieldrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin account");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

void yieldrwa::updateconfig(const name& key, const uint8_t& value) {
    require_auth(_gstate.admin);
    CHECKC(_gstate.yield_split_conf.count(key), err::PARAM_ERROR, "invalid yield key");
    _gstate.yield_split_conf[key] = value;
    _global.set(_gstate, get_self());
}

void yieldrwa::on_transfer(const name& from, const name& to,const asset& quantity, const string& memo)
{
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2 && parts[0] == "plan",
           err::INVALID_FORMAT, "memo must be plan:<id>");

    uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(), err::RECORD_NOT_FOUND, "plan not found");

    CHECKC(p->goal_quantity.symbol == quantity.symbol,
           err::SYMBOL_MISMATCH, "symbol mismatch");

    const name bank = get_first_receiver();
    _perform_distribution(bank, quantity, plan_id);
}

void yieldrwa::buyback(const name& submitter,const uint64_t& plan_id)
{
    require_auth(submitter);

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(p->status == "success"_n, err::STATUS_ERROR, "plan not in success state");

    plan_buyback_t::pl_tbl tbl(get_self(), get_self().value);
    auto it = tbl.find(plan_id);
    CHECKC(it != tbl.end(), err::RECORD_NOT_FOUND,
           "planbuyback record missing");

    asset remaining = it->remaining();
    CHECKC(remaining.amount > 0, err::INCORRECT_AMOUNT, "no buyback balance");

    symbol sing = p->goal_quantity.symbol;
    symbol voucher = p->receipt_symbol;
    name   bank = p->goal_asset_contract;

    name pair = find_pair_by_symbols(sing, voucher, SWAP_POOL);

    double slip_rate = (double)it->max_slippage / 10000.0;

    string memo = build_swap_memo(
        extended_asset(remaining, bank),
        pair,
        slip_rate,
        SWAP_POOL
    );

    TRANSFER(bank, SWAP_POOL, remaining, memo);

    tbl.modify(it, same_payer, [&](auto& row){
        row.used_buyback += remaining;
        row.updated_at = time_point_sec(current_time_point());
    });
}

void yieldrwa::setslippage(const name& submitter,const uint64_t& plan_id,const uint16_t& max_slippage)
{
    require_auth(submitter);
    CHECKC(submitter == _gstate.admin, err::NO_AUTH,
           "only admin can update slippage");

    CHECKC(max_slippage <= 2000, err::PARAM_ERROR,
           "slippage too large");

    plan_buyback_t::pl_tbl tbl(get_self(), get_self().value);
    auto it = tbl.find(plan_id);

    CHECKC(it != tbl.end(), err::RECORD_NOT_FOUND,
           "planbuyback not found");

    tbl.modify(it, submitter, [&](auto& row){
        row.max_slippage = max_slippage;
        row.updated_at = time_point_sec(current_time_point());
    });
}

double yieldrwa::_get_coverage_ratio(const uint64_t& plan_id)
{
    guaranty_stats_t::idx_t stats(GUARANTY_POOL, GUARANTY_POOL.value);
    auto gs = stats.find(plan_id);
    if (gs == stats.end()) return 0.0;

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    if (p == plans.end()) return 0.0;

    double required_half = (double)p->goal_quantity.amount / 2.0;
    if (required_half <= 0) return 0.0;

    double cover = (double)gs->total_guarantee_funds.amount / required_half;
    return std::max(0.0, std::min(1.0, cover));
}

void yieldrwa::_perform_distribution(const name& bank,const asset& total,const uint64_t& plan_id)
{
    CHECKC(total.amount > 0,    err::NOT_POSITIVE, "zero total");

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(),                                            err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(p->status == "success"_n,                                    err::INVALID_FORMAT,"plan not in yield stage");
    CHECKC(time_point_sec(current_time_point()) < p->return_end_time,   err::EXPIRED,"plan already ended, no further yield accepted");
    CHECKC(total.symbol == p->goal_quantity.symbol,                     err::SYMBOL_MISMATCH, "symbol mismatch");

    auto& cfg = _gstate.yield_split_conf;
    CHECKC(cfg.count(STAKE_POOL) &&cfg.count(GUARANTY_POOL) &&cfg.count(SWAP_POOL),err::PARAM_ERROR, "yield config missing keys");

    double stake_pct    = cfg[STAKE_POOL];
    double guaranty_pct = cfg[GUARANTY_POOL] * _get_coverage_ratio(plan_id);
    double swap_pct     = 100.0 - stake_pct - guaranty_pct;
    if (swap_pct < 0) swap_pct = 0;

    const __int128 T = total.amount;

    asset stake{(int64_t)(T * stake_pct / 100.0), total.symbol};
    asset guar {(int64_t)(T * guaranty_pct / 100.0), total.symbol};
    asset swap {total.amount - stake.amount - guar.amount, total.symbol};

    if (stake.amount > 0)
        TRANSFER(bank, STAKE_POOL, stake, "reward:" + std::to_string(plan_id));

    if (guar.amount > 0)
        TRANSFER(bank, GUARANTY_POOL, guar, "reward:" + std::to_string(plan_id));

    // accumulate buyback
    if (swap.amount > 0) {
        plan_buyback_t::pl_tbl tbl(get_self(), get_self().value);
        auto it = tbl.find(plan_id);

        asset zero(0, total.symbol);

        if (it == tbl.end()) {
            tbl.emplace(get_self(), [&](auto& row){
                row.plan_id       = plan_id;
                row.total_buyback = swap;
                row.used_buyback  = zero;
                row.total_voucher = zero;
                row.max_slippage  = 100; // 1%
                row.updated_at    = time_point_sec(current_time_point());
            });
        } else {
            tbl.modify(it, get_self(), [&](auto& row){
                row.total_buyback += swap;
                row.updated_at     = time_point_sec(current_time_point());
            });
        }
    }

    _log_yield(plan_id, total, stake, guar, swap);
}

void yieldrwa::_log_yield(const uint64_t& plan_id,const asset& total,const asset& stake,const asset& guar,const asset& swap)
{
    if (total.amount <= 0) return;

    uint64_t period = current_period_yyyymm();
    time_point_sec now = time_point_sec(current_time_point());

    yield_log_t::idx_t logs(get_self(), plan_id);

    asset cumulative_prev(0, total.symbol);
    if (logs.begin() != logs.end()) {
        auto last = logs.end(); --last;
        cumulative_prev = last->cumulative_yield;
    }

    auto it = logs.find(period);
    if (it == logs.end()) {
        logs.emplace(get_self(), [&](auto& y){
            y.period                = period;
            y.period_yield          = total;
            y.investor_yield        = stake;
            y.guarantor_yield       = guar;
            y.buyback_yield         = swap;
            y.cumulative_yield      = cumulative_prev + total;
            y.created_at            = now;
            y.updated_at            = now;
        });
    } else {
        logs.modify(it, get_self(), [&](auto& y){
            y.period_yield          += total;
            y.investor_yield        += stake;
            y.guarantor_yield       += guar;
            y.buyback_yield         += swap;
            y.cumulative_yield      += total;
            y.updated_at             = now;
        });
    }
}

asset yieldrwa::_calc_yearly_yield_core(const uint64_t& plan_id,const uint64_t& year,const string& type) const
{
    yield_log_t::idx_t logs(get_self(), plan_id);
    CHECKC(logs.begin() != logs.end(), err::RECORD_NOT_FOUND,
           "no yield logs");

    int64_t total = 0;
    symbol sym = logs.begin()->period_yield.symbol;

    uint64_t start = year * 100 + 1;
    uint64_t end   = (year + 1) * 100;

    for (auto& row : logs) {
        if (row.period < start || row.period >= end) continue;

        if (type == "total")            total += row.period_yield.amount;
        else if (type == "investor")    total += row.investor_yield.amount;
        else if (type == "guarantor")   total += row.guarantor_yield.amount;
        else if (type == "buyback")     total += row.buyback_yield.amount;
    }

    return asset(total, sym);
}

asset yieldrwa::get_yearly_yield(const uint64_t& plan_id,const uint64_t& year,const string& type) const
{
    return _calc_yearly_yield_core(plan_id, year, type);
}