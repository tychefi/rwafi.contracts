#include <flon/flon.token.hpp>
#include "rwayieldpool.hpp"
#include <flon/utils.hpp>
#include <flon/consts.hpp>

#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include <rwaverse.io/rwaversedb.hpp>
#include <rwaguarapool/rwaguarapooldb.hpp>
#include <flon.swap/flon.swap.db.hpp>
#include <rwastakepool/rwastakepooldb.hpp>

using namespace eosio;
using namespace rwafi;
using namespace flon;

static constexpr eosio::name active_perm{"active"_n};

static uint64_t current_period_yyyymm() {
    time_t t = (time_t) current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

static void refresh_plan_status(const name& submitter, uint64_t plan_id) {
    rwaverse::refreshstat_action act{ get_self(), { {get_self(), "active"_n} } };
    act.send(submitter,plan_id);


}

int64_t calc_min_received(const asset& input, const asset& in_pool, const asset& out_pool, uint16_t slippage_bp)
{
    CHECKC(in_pool.amount > 0 && out_pool.amount > 0, err::PARAM_ERROR, "invalid pool amounts");
    CHECKC(slippage_bp <= 10000, err::PARAM_ERROR, "invalid slippage");

    uint128_t numerator = (uint128_t)input.amount * (uint128_t)out_pool.amount;
    uint128_t denom = (uint128_t)in_pool.amount;
    uint128_t out = numerator / denom;
    out = out * (uint128_t)(10000 - slippage_bp) / 10000;

    CHECKC(out <= std::numeric_limits<int64_t>::max(), err::PARAM_ERROR, "min amount overflow");
    return (int64_t)out;
}

string build_swap_memo(const extended_asset& input,const name& pair_name,uint16_t slippage_bp,const name& swap_contract)
{
    CHECKC(slippage_bp <= 10000, err::PARAM_ERROR, "invalid slippage");

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

    int64_t min_amt = calc_min_received(input.quantity, in_pool.quantity, out_pool.quantity, slippage_bp);

    return string("swap:") + asset(min_amt, out_pool.quantity.symbol).to_string()
           + ":" + pair_name.to_string();
}

name rwayieldpool::find_pair_by_symbols(const symbol& in_sym,const symbol& out_sym,const name& swap_contract)
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

void rwayieldpool::notify(const name& contract,
                            const name& from,
                            const name& to,
                            const asset& quantity,
                            const string& memo,
                            const string& type) {
    require_auth(get_self());
}

void rwayieldpool::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin account");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

void rwayieldpool::updateconfig(const name& key, const uint8_t& value) {
    require_auth(_gstate.admin);
    CHECKC(_gstate.yield_split_conf.count(key), err::PARAM_ERROR, "invalid yield key");
    _gstate.yield_split_conf[key] = value;
    _global.set(_gstate, get_self());
}

void rwayieldpool::on_transfer(const name& from, const name& to,const asset& quantity, const string& memo)
{
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive");
    CHECKC(quantity.symbol.precision() == SING_SYM.precision(),
           err::INVALID_FORMAT, "precision mismatch");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2 && parts[0] == "plan",
           err::INVALID_FORMAT, "memo must be plan:<id>");

    uint64_t plan_id = std::stoull(parts[1]);
    refresh_plan_status( get_self(), plan_id);

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(), err::RECORD_NOT_FOUND, "plan not found");

    CHECKC(p->goal_quantity.symbol == quantity.symbol,
           err::SYMBOL_MISMATCH, "symbol mismatch");

    const name bank = get_first_receiver();
    _perform_distribution(bank, quantity, plan_id);
}
//未分配的那一部分收益，进行回购
void rwayieldpool::buyback(const name& submitter,const uint64_t& plan_id)
{
    require_auth(submitter);
    refresh_plan_status( get_self(), plan_id);

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(p->status == "success"_n || p->status == "completed"_n, err::STATUS_ERROR, "plan not in success/completed state");

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

    string memo = build_swap_memo(
        extended_asset(remaining, bank),
        pair,
        it->max_slippage,
        SWAP_POOL
    );

    TRANSFER(bank, SWAP_POOL, remaining, memo);

    tbl.modify(it, same_payer, [&](auto& row){
        row.used_buyback += remaining;
        row.updated_at = time_point_sec(current_time_point());
    });
}

void rwayieldpool::setslippage(const name& submitter,const uint64_t& plan_id,const uint16_t& max_slippage)
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

double rwayieldpool::_get_coverage_ratio(const uint64_t& plan_id){
    // 1) 读计划：H = total_raised / 2
    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    if (p == plans.end()) return 0.0;

    const double H = (double)p->total_raised_funds.amount / 2.0;
    if (H <= 0.0) return 0.0;

    // 2) 统计担保“初始本金” G0 = sum(shares)
    guarantor_stake_t::idx_t stakes(GUARANTY_POOL, plan_id);

    __int128 sum_shares = 0;
    for (const auto& s : stakes) {
        // shares 才是“担保本金”口径
        sum_shares += s.shares.amount;
    }
    if (sum_shares <= 0) return 0.0;

    double G0 = (double)sum_shares;

    // 3) coverage progress
    double ratio = G0 / H;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return ratio;
}

//如果stake中计划总量是0的话，分红划给swap
void rwayieldpool::_perform_distribution(const name& bank,const asset& total,const uint64_t& plan_id)
{
    CHECKC(total.amount > 0,    err::NOT_POSITIVE, "zero total");

    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);

    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(),  err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(p->status == PlanStatus::SUCCESS || p->status == PlanStatus::COMPLETED, err::INVALID_FORMAT, "plan not in yield stage");
    CHECKC(time_point_sec(current_time_point()) >= p->end_time, err::INVALID_STATUS,"yield not started yet");
    CHECKC(total.symbol == p->goal_quantity.symbol,   err::SYMBOL_MISMATCH, "symbol mismatch");

    auto& cfg = _gstate.yield_split_conf;
    CHECKC(cfg.count(STAKE_POOL) &&cfg.count(GUARANTY_POOL) &&cfg.count(SWAP_POOL),err::PARAM_ERROR, "yield config missing keys");

    double stake_pct    = cfg[STAKE_POOL];
    double guaranty_pct = cfg[GUARANTY_POOL] * _get_coverage_ratio(plan_id);
    double swap_pct     = 100.0 - stake_pct - guaranty_pct;
    if (swap_pct < 0) swap_pct = 0;

    const uint128_t T = total.amount;

    auto stake_calc = (T * stake_pct) / 100;
    auto guar_calc  = (T * guaranty_pct) / 100;

    int64_t stake_amt = static_cast<int64_t>(stake_calc);
    int64_t guar_amt  = static_cast<int64_t>(guar_calc);

    asset stake{ stake_amt, total.symbol };
    asset guar { guar_amt,  total.symbol };
    asset swap { total.amount - stake_amt - guar_amt, total.symbol };

    // === 没人 stake 时，stake 份额并入 swap ===
    stake_plan_t::tbl_t stakeplans(STAKE_POOL, STAKE_POOL.value);
    auto sitr = stakeplans.find(plan_id);
    CHECKC(sitr != stakeplans.end(),err::RECORD_NOT_FOUND, "stake plan not found");
    if (sitr->total_staked.amount == 0 && stake.amount > 0) {
        swap += stake;
        stake.amount = 0;
    }

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

void rwayieldpool::_log_yield(const uint64_t& plan_id,const asset& total,const asset& stake,const asset& guar,const asset& swap)
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

asset rwayieldpool::_calc_yearly_yield_core(const uint64_t& plan_id,const uint64_t& year,const string& type) const
{
    yield_log_t::idx_t logs(get_self(), plan_id);
    CHECKC(logs.begin() != logs.end(), err::RECORD_NOT_FOUND,
           "no yield logs");

    uint64_t total = 0;
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

asset rwayieldpool::get_yearly_yield(const uint64_t& plan_id,const uint64_t& year,const string& type) const
{
    return _calc_yearly_yield_core(plan_id, year, type);
}



void rwayieldpool::recordyield(const uint64_t& plan_id,const asset&    total_yield) {
    if (has_auth(get_self())) {
        // ok
    } else if (has_auth(_gstate.admin)) {
        require_auth(_gstate.admin);
    } else {
        require_auth(GUARANTY_POOL);
    }
    CHECKC(total_yield.amount > 0, err::NOT_POSITIVE, "yield must be positive");
    CHECKC(total_yield.symbol.precision() == SING_SYM.precision(),
           err::INVALID_FORMAT, "precision mismatch");
    refresh_plan_status( get_self(), plan_id);

    // 1. 读取 plan（只读 rwaverse.io）
    fundplan_t::idx_t plans(INVEST_POOL, INVEST_POOL.value);
    auto p = plans.find(plan_id);
    CHECKC(p != plans.end(), err::RECORD_NOT_FOUND, "plan not found");

    CHECKC(p->status == PlanStatus::SUCCESS || p->status == PlanStatus::COMPLETED,
           err::INVALID_STATUS, "plan not in yield stage");

    CHECKC(total_yield.symbol == p->goal_quantity.symbol,
           err::SYMBOL_MISMATCH, "symbol mismatch");

    // 2. 计算 period（UTC YYYYMM）
    const uint64_t period = []() {
        const time_t t = (time_t) current_time_point().sec_since_epoch();
        const tm* g = gmtime(&t);
        return ((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
    }();

    // 3. 打开 yield_log 表（⚠️ code = rwayieldpool）
    yield_log_t::idx_t logs(get_self(), plan_id);

    asset cumulative_prev(0, total_yield.symbol);
    if (auto it = logs.rbegin(); it != logs.rend()) {
        cumulative_prev = it->cumulative_yield;
    }

    const time_point_sec now = time_point_sec(current_time_point());

    auto itr = logs.find(period);
    if (itr == logs.end()) {
        logs.emplace(get_self(), [&](auto& y) {
            y.period           = period;
            y.period_yield     = total_yield;
            y.cumulative_yield = cumulative_prev + total_yield;
            y.investor_yield   = asset(0, total_yield.symbol); // 拆分由 rwayieldpool 内部逻辑做
            y.created_at       = now;
            y.updated_at       = now;
        });
    } else {
        logs.modify(itr, get_self(), [&](auto& y) {
            y.period_yield     += total_yield;
            y.cumulative_yield += total_yield;
            y.updated_at        = now;
        });
    }
}
