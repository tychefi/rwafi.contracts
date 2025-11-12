#include "guarantyrwa.hpp"
#include "yield.rwa/yieldrwadb.hpp"

#include <flon/flon.token.hpp>
#include <flon/utils.hpp>

using namespace rwafi;
using namespace eosio;
using namespace flon;

// ====================== 工具函数 ======================

uint64_t guarantyrwa::_current_period_yyyymm() {
    time_t t = (time_t) current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

asset guarantyrwa::_yearly_guarantee_principal(const fundplan_t& plan) {
    uint16_t years = (plan.return_months + 11) / 12;
    if (years == 0) years = 1;

    __int128 num = (__int128)plan.goal_quantity.amount;
    int64_t yearly = (int64_t)(num / years / 2);
    return asset(yearly, plan.goal_quantity.symbol);
}

static void record_yield_log_monthly(const name& yield_contract,
                                     const name& payer,
                                     uint64_t plan_id,
                                     const asset& pay) {
    yield_log_t::idx_t logs(yield_contract, plan_id);

    uint64_t period = []() {
        time_t t = (time_t) current_time_point().sec_since_epoch();
        tm* g = gmtime(&t);
        return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
    }();

    asset cumulative_prev(0, pay.symbol);
    if (logs.begin() != logs.end()) {
        auto last = logs.end();
        --last;
        cumulative_prev = last->cumulative_yield;
    }

    auto itr = logs.find(period);
    if (itr == logs.end()) {
        logs.emplace(payer, [&](auto& y) {
            y.period           = period;
            y.period_yield     = pay;
            y.cumulative_yield = cumulative_prev + pay;
            y.created_at       = time_point_sec(current_time_point());
            y.updated_at       = y.created_at;
        });
    } else {
        logs.modify(itr, payer, [&](auto& y) {
            y.period_yield     += pay;
            y.cumulative_yield += pay;
            y.updated_at        = time_point_sec(current_time_point());
        });
    }
}

// ====================== 主流程 ======================

void guarantyrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin account");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

/**
 * @brief 担保金与收益接收入口
 *
 * 支持三种 memo 格式：
 *  - guaranty:<plan_id>   → 担保本金充值
 *  - reward:<plan_id>     → 担保收益分红
 */
void guarantyrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "transfer amount must be positive");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2, err::INVALID_FORMAT, "memo format must be <type>:<plan_id>");

    string action = parts[0];
    uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(get_first_receiver() == plan.goal_asset_contract,
           err::CONTRACT_MISMATCH,
           "invalid token contract");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol,
           err::SYMBOL_MISMATCH,
           "symbol mismatch");

    if (action == "guaranty") {
        _handle_guaranty_transfer(from, plan, quantity);
    } else if (action == "reward") {
        _handle_reward_transfer(plan, quantity);
    } else {
        CHECKC(false, err::PARAM_ERROR, "unknown transfer type");
    }
}

void guarantyrwa::_handle_guaranty_transfer(const name& from,
                                            const fundplan_t& plan,
                                            const asset& quantity) {
    time_point_sec now = time_point_sec(current_time_point());
    uint64_t plan_id = plan.id;

    // === 1️⃣ 更新担保池汇总 ===
    guaranty_stats_t stats(plan_id);
    if (!_db.get(stats)) {
        stats.plan_id               = plan_id;
        stats.total_guarantee_funds = quantity;          // 新担保池初始化
        stats.used_guarantee_funds  = asset(0, quantity.symbol);
        stats.created_at = stats.updated_at = now;
    } else {
        stats.total_guarantee_funds += quantity;
        stats.updated_at = now;
    }
    _db.set(stats);

    // === 2️⃣ 更新担保人个人记录 ===
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto itr = stakes.find(from.value);

    if (itr == stakes.end()) {
        // 🆕 首次担保，初始化结构
        stakes.emplace(get_self(), [&](auto& s) {
            s.guarantor       = from;
            s.total_stake     = quantity;
            s.available_stake = asset(0, quantity.symbol);
            s.locked_stake    = quantity;                // 初始全部锁定
            s.earned_yield    = asset(0, quantity.symbol);
            s.withdrawn       = asset(0, quantity.symbol);
            s.created_at = s.updated_at = now;
        });
    } else {
        // ♻️ 增加本金到锁仓
        stakes.modify(itr, same_payer, [&](auto& s) {
            s.total_stake     += quantity;
            s.locked_stake    += quantity;
            s.updated_at = now;
        });
    }
}

void guarantyrwa::_handle_reward_transfer(const fundplan_t& plan,
                                          const asset& quantity) {
    uint64_t plan_id = plan.id;
    time_point_sec now = time_point_sec(current_time_point());

    // === 1️⃣ 获取担保人列表 ===
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors found");

    // === 2️⃣ 计算担保池总本金 ===
    asset total_stake(0, quantity.symbol);
    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        total_stake += it->total_stake;
    }
    CHECKC(total_stake.amount > 0, err::PARAM_ERROR, "total stake is zero");

    // === 3️⃣ 分红分配（按比例） ===
    int64_t total_distributed = 0;
    auto end_itr = stakes.end(); --end_itr;

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t share_amt = 0;

        if (it != end_itr) {
            __int128 part = (__int128)quantity.amount * it->total_stake.amount / total_stake.amount;
            share_amt = (int64_t)part;
            total_distributed += share_amt;
        } else {
            // 补尾差
            share_amt = quantity.amount - total_distributed;
        }

        if (share_amt <= 0) continue;

        asset share(share_amt, quantity.symbol);

        stakes.modify(it, get_self(), [&](auto& s) {
            s.earned_yield += share;  // 增加累计收益
            s.updated_at = now;
        });
    }
}

/**
 * 担保收益补足（年度本金保障）
 */
void guarantyrwa::guarantpay(const name& submitter, const uint64_t& plan_id, const uint64_t& year) {
    require_auth(submitter);

    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no guaranty stats");
    CHECKC(stats.total_guarantee_funds.amount > 0, err::QUANTITY_INSUFFICIENT, "no funds");

    uint16_t total_years = (plan.return_months + 11) / 12;
    if (total_years == 0) total_years = 1;
    CHECKC(year > 0 && year <= total_years, err::PARAM_ERROR, "invalid year");

    asset yearly_due = _yearly_guarantee_principal(plan);
    CHECKC(yearly_due.amount > 0, err::PARAM_ERROR, "invalid yearly due");

    yield_log_t::idx_t logs(_gstate.yield_contract, plan_id);
    asset distributed(0, yearly_due.symbol);
    if (logs.begin() != logs.end()) {
        auto last = logs.end();
        --last;
        distributed = last->period_yield;
    }

    int64_t diff = yearly_due.amount - distributed.amount;
    if (diff <= 0) return;

    asset pay(diff, yearly_due.symbol);
    if (stats.total_guarantee_funds < pay) pay = stats.total_guarantee_funds;
    CHECKC(pay.amount > 0, err::QUANTITY_INSUFFICIENT, "insufficient guarantee pool");

    stats.total_guarantee_funds -= pay;
    stats.used_guarantee_funds  += pay;
    stats.updated_at = time_point_sec(current_time_point());
    _db.set(stats);

    _deduct_from_guarantors(plan_id, pay);
    TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay,
             "principal guarantee payout for plan:" + std::to_string(plan_id));
    record_yield_log_monthly(_gstate.yield_contract, get_self(), plan_id, pay);
}

/**
 * 担保资金解押
 */
void guarantyrwa::redeem(const name& guarantor, const uint64_t& plan_id, const asset& quantity) {
    require_auth(guarantor);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "redeem amount must be positive");

    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no guaranty stats");

    bool failed = (plan.status == PlanStatus::FAILED || plan.status == PlanStatus::CANCELLED);
    bool ended  = (time_point_sec(current_time_point()) >= plan.return_end_time);

    if (failed)
        _redeem_failed_project(guarantor, plan, stats, quantity);
    else if (ended)
        _redeem_project_end(guarantor, plan, stats, quantity);
    else
        _redeem_in_progress(guarantor, plan, stats, quantity);
}

void guarantyrwa::_redeem_failed_project(const name& guarantor,
                                         const fundplan_t& plan,
                                         guaranty_stats_t& stats,
                                         const asset& quantity) {
    CHECKC(quantity <= stats.total_guarantee_funds, err::QUANTITY_INSUFFICIENT, "exceeds funds");
    _do_redeem(guarantor, plan, stats, quantity, "redeem failed project");
}

void guarantyrwa::_redeem_in_progress(const name& guarantor,
                                      const fundplan_t& plan,
                                      guaranty_stats_t& stats,
                                      const asset& quantity) {
    time_point_sec now = time_point_sec(current_time_point());

    uint64_t elapsed_months =
        (now.sec_since_epoch() - plan.start_time.sec_since_epoch()) / (30ull * 86400ull);
    uint64_t total_months = plan.return_months;

    uint16_t elapsed_years = (elapsed_months + 11) / 12;
    uint16_t total_years   = (total_months + 11) / 12;
    if (total_years == 0) total_years = 1;
    if (elapsed_years > total_years) elapsed_years = total_years;

    __int128 required_pool = (__int128)plan.goal_quantity.amount / 2 * elapsed_years / total_years;

    yield_log_t::idx_t logs(_gstate.yield_contract, plan.id);
    asset distributed(0, plan.goal_quantity.symbol);
    if (logs.begin() != logs.end()) {
        auto last = logs.end();
        --last;
        distributed = last->cumulative_yield;
    }

    __int128 total_cover = (__int128)stats.total_guarantee_funds.amount + distributed.amount;
    CHECKC(total_cover >= required_pool, err::INVALID_STATUS, "not enough coverage yet");

    __int128 unlocked_amt = total_cover - required_pool;
    if (unlocked_amt < 0) unlocked_amt = 0;
    int64_t unlocked = (int64_t)std::min<__int128>(unlocked_amt, stats.total_guarantee_funds.amount);

    CHECKC(quantity.amount <= unlocked, err::QUANTITY_INSUFFICIENT, "redeem exceeds unlocked funds");
    _do_redeem(guarantor, plan, stats, quantity, "redeem unlocked guarantee (in progress)");
}

void guarantyrwa::_redeem_project_end(const name& guarantor,
                                      const fundplan_t& plan,
                                      guaranty_stats_t& stats,
                                      const asset& quantity) {
    uint16_t total_years = (plan.return_months + 11) / 12;
    if (total_years == 0) total_years = 1;

    __int128 theoretical_total = (__int128)plan.goal_quantity.amount / 2;
    yield_log_t::idx_t logs(_gstate.yield_contract, plan.id);
    asset distributed(0, plan.goal_quantity.symbol);
    if (logs.begin() != logs.end()) {
        auto last = logs.end();
        --last;
        distributed = last->cumulative_yield;
    }

    int64_t diff = theoretical_total - distributed.amount;
    if (diff > 0) {
        asset pay(diff, plan.goal_quantity.symbol);
        if (stats.total_guarantee_funds < pay) pay = stats.total_guarantee_funds;
        if (pay.amount > 0) {
            TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay,
                     "final guarantee payout: " + std::to_string(plan.id));
            stats.total_guarantee_funds -= pay;
            stats.used_guarantee_funds  += pay;
            stats.updated_at = time_point_sec(current_time_point());
            _db.set(stats);
            _deduct_from_guarantors(plan.id, pay);
            record_yield_log_monthly(_gstate.yield_contract, get_self(), plan.id, pay);
        }
    }

    CHECKC(quantity <= stats.total_guarantee_funds,
           err::QUANTITY_INSUFFICIENT, "redeem exceeds remaining funds");
    _do_redeem(guarantor, plan, stats, quantity, "redeem after project end");
}

void guarantyrwa::_do_redeem(const name& guarantor,
                             const fundplan_t& plan,
                             guaranty_stats_t& stats,
                             const asset& quantity,
                             const string& memo) {
    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto itr = stakes.find(guarantor.value);
    CHECKC(itr != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");
    CHECKC(itr->total_stake >= quantity, err::QUANTITY_INSUFFICIENT, "insufficient stake");

    stats.total_guarantee_funds -= quantity;
    stats.updated_at = time_point_sec(current_time_point());
    _db.set(stats);

    stakes.modify(itr, get_self(), [&](auto& s) {
        s.total_stake -= quantity;
        s.updated_at = time_point_sec(current_time_point());
    });

    TRANSFER(plan.goal_asset_contract, guarantor, quantity, memo);
}

void guarantyrwa::_deduct_from_guarantors(uint64_t plan_id, const asset& pay) {
    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no stats");
    asset total = stats.total_guarantee_funds;
    CHECKC(total.amount > 0, err::PARAM_ERROR, "empty pool");

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors");

    int64_t deducted_sum = 0;
    auto last = stakes.end(); --last;

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t part = 0;
        if (it != last) {
            __int128 p = (__int128)pay.amount * it->total_stake.amount / total.amount;
            part = (int64_t)p;
            deducted_sum += part;
        } else {
            part = pay.amount - deducted_sum;
        }

        if (part <= 0) continue;
        stakes.modify(it, get_self(), [&](auto& s) {
            s.total_stake.amount -= part;
            if (s.total_stake.amount < 0) s.total_stake.amount = 0;
            s.updated_at = time_point_sec(current_time_point());
        });
    }
}