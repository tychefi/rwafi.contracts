#include "guarantyrwa.hpp"
#include "yieldrwa/yieldrwadb.hpp"
#include <flon/utils.hpp>
#include <flon/flon.token.hpp>

using namespace rwafi;
using namespace flon;

static constexpr eosio::name active_perm{"active"_n};

/**
 *
 * 公式：
 *   可解押金额 = 总担保资金 - (应锁定金额 + 已支付金额)
 *
 */
static asset calc_unlocked_amount_now(
    const fundplan_t&       plan,
    const guaranty_stats_t& stats,
    const guaranty_conf_t&  conf,
    const asset&            monthly_min
) {
    uint64_t total_months = plan.return_months;
    uint64_t elapsed_months =
        (eosio::current_time_point().sec_since_epoch() - plan.start_time.sec_since_epoch())
        / (30ull * 86400ull);
    if (elapsed_months > total_months) elapsed_months = total_months;

    int128_t expected_cov =
        (int128_t)monthly_min.amount * elapsed_months * conf.coverage_ratio_bp / 10000;

    int128_t required_locked = expected_cov + stats.used_guarantee_funds.amount;
    if (required_locked < 0) required_locked = 0;

    int128_t unlocked = stats.total_guarantee_funds.amount - required_locked;
    if (unlocked < 0) unlocked = 0;

    return asset((int64_t)unlocked, monthly_min.symbol);
}

/**
 *
 * 写入收益日志到 yield.rwa（按月）
 *
 */
static void record_yield_log_monthly(
    const name&  yield_contract,
    const name&  payer,
    uint64_t     plan_id,
    const asset& pay
) {
    time_t t = (time_t)eosio::current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    uint64_t period = (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
    eosio::time_point_sec now = eosio::time_point_sec(eosio::current_time_point());

    yield_log_t::idx_t yieldlogs(yield_contract, plan_id);

    auto itr = yieldlogs.find(period);
    if (itr == yieldlogs.end()) {
        asset cumulative_prev(0, pay.symbol);
        if (yieldlogs.begin() != yieldlogs.end()) {
            auto last = yieldlogs.end();
            --last;
            cumulative_prev = last->cumulative_yield;
        }

        yieldlogs.emplace(payer, [&](auto& y) {
            y.period           = period;
            y.period_yield     = pay;
            y.cumulative_yield = cumulative_prev + pay;
            y.created_at = y.updated_at = now;
        });
    } else {
        yieldlogs.modify(itr, payer, [&](auto& y) {
            y.period_yield     += pay;
            y.cumulative_yield += pay;
            y.updated_at        = now;
        });
    }
}

uint64_t guarantyrwa::_current_period_yyyymm() {
    time_t t = (time_t) current_time_point().sec_since_epoch();
    tm* g = gmtime(&t);
    return (uint64_t)((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

asset guarantyrwa::_monthly_guarantee_min(const fundplan_t& plan) {
    int128_t num = (int128_t)plan.goal_quantity.amount * plan.guaranteed_yield_apr;
    int64_t monthly = (int64_t)(num / 10000 / 12);
    return asset(monthly, plan.goal_quantity.symbol);
}

void guarantyrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin account");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

/**
 *
 * 设置担保覆盖比例（最低 50%）
 *
 */
void guarantyrwa::setgconf(const uint64_t& plan_id, const uint16_t& coverage_ratio_bp) {
    require_auth(_gstate.admin);
    CHECKC(coverage_ratio_bp >= 5000, err::PARAM_ERROR, "coverage_ratio_bp must be >= 5000 (50%)");

    time_point_sec now = time_point_sec(current_time_point());

    guaranty_conf_t conf(plan_id);
    if (!_db.get(conf)) {
        conf.plan_id           = plan_id;
        conf.coverage_ratio_bp = coverage_ratio_bp;
        conf.created_at = conf.updated_at = now;
    } else {
        conf.coverage_ratio_bp = coverage_ratio_bp;
        conf.updated_at        = now;
    }
    _db.set(conf);
}

/**
 *
 * 💰 担保人充值（on_notify）
 *  memo 格式: "guaranty:<plan_id>"
 */
void guarantyrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "transfer amount must be positive");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2 && parts[0] == "guaranty", err::INVALID_FORMAT, "expect guaranty:<plan_id>");
    uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    time_point_sec now = time_point_sec(current_time_point());

    // 更新担保池
    guaranty_stats_t  stats(plan_id);
    if (!_db.get(stats)) {
        stats.plan_id = plan_id;
        stats.total_guarantee_funds = quantity;
        stats.available_guarantee_funds = quantity;
        stats.used_guarantee_funds = asset(0, quantity.symbol);
        stats.created_at = stats.updated_at = now;
    } else {
        stats.total_guarantee_funds += quantity;
        stats.available_guarantee_funds += quantity;
        stats.updated_at = now;
    }
    _db.set(stats);

    // 更新担保人质押记录（scope: plan_id）
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto itr = stakes.find(from.value);
    if (itr == stakes.end()) {
        stakes.emplace(get_self(), [&](auto& s) {
            s.guarantor = from;
            s.total_funds = quantity;
            s.created_at = s.updated_at = now;
        });
    } else {
        stakes.modify(itr, same_payer, [&](auto& s) {
            s.total_funds += quantity;
            s.updated_at = now;
        });
    }
}

/**
 *
 * guarantpay — 担保收益支付（按月/按到期）
 *
 */
void guarantyrwa::guarantpay(const name& submitter, const uint64_t& plan_id, const uint64_t& months) {
    require_auth(submitter);
    CHECKC(months > 0, err::PARAM_ERROR, "months must be positive");

    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(plan.guaranteed_yield_apr > 0, err::PARAM_ERROR, "no guaranteed APR");

    // 状态校验：项目未结束不得提前支付
    time_point_sec now = time_point_sec(current_time_point());
    bool project_ended = now >= plan.return_end_time;
    if (!project_ended && plan.return_months > 12) {
        CHECKC(months <= plan.return_months, err::PARAM_ERROR, "invalid payment months");
    }

    guaranty_conf_t conf(plan_id);
    if (!_db.get(conf)) conf.coverage_ratio_bp = 5000;

    asset monthly_due = _monthly_guarantee_min(plan);
    CHECKC(monthly_due.amount > 0, err::PARAM_ERROR, "invalid monthly guarantee");

    // 从 yieldlog 中读取历史已分红累计
    yield_log_t::idx_t yieldlogs(_gstate.yield_contract, plan_id);
    asset distributed(0, monthly_due.symbol);
    if (yieldlogs.begin() != yieldlogs.end()) {
        auto last = yieldlogs.end(); --last;
        distributed = last->cumulative_yield;
    }

    // 应付 = 理论应付 - 已分红
    int128_t theoretical = (int128_t)monthly_due.amount * months * conf.coverage_ratio_bp / 10000;
    int128_t adjusted = theoretical - distributed.amount;
    if (adjusted < 0) adjusted = 0;
    asset pay((int64_t)adjusted, monthly_due.symbol);

    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no guaranty stats");
    CHECKC(stats.available_guarantee_funds.amount > 0, err::QUANTITY_INSUFFICIENT, "no available funds");

    if (stats.available_guarantee_funds < pay) pay = stats.available_guarantee_funds;
    CHECKC(pay.amount > 0, err::QUANTITY_INSUFFICIENT, "insufficient funds to pay");

    stats.available_guarantee_funds -= pay;
    stats.used_guarantee_funds      += pay;
    stats.updated_at = time_point_sec(current_time_point());
    _db.set(stats);

    TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay, "reward:" + std::to_string(plan_id));

    record_yield_log_monthly(_gstate.yield_contract, get_self(), plan_id, pay);
}

/**
 *
 * redeem — 解押担保资金
 * ============================================================
 * 支持三种情况：
 * 1️项目失败（FAILED / CANCELLED）  → 立即全额解押
 * 2️项目到期（return_end_time 已到） → 先补足收益差额，再解押剩余
 * 3️项目进行中                      → 按分红进度部分解押
 * 同步更新担保池与担保人余额。
 */
void guarantyrwa::withdraw(const name& guarantor, const uint64_t& plan_id, const asset& quantity) {
    require_auth(guarantor);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "withdraw amount must be positive");

    // 获取计划信息
    fundplan_t plan(plan_id);
    CHECKC(_db_invest.get(plan), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    // 获取担保池与配置
    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no guaranty stats");

    guaranty_conf_t conf(plan_id);
    if (!_db.get(conf)) conf.coverage_ratio_bp = 5000; // 默认 50%

    bool project_failed = (plan.status == PlanStatus::FAILED || plan.status == PlanStatus::CANCELLED);
    bool project_ended  = time_point_sec(current_time_point()) >= plan.return_end_time;

    // 根据状态分流
    if (project_failed) {
        _withdraw_failed_project(guarantor, plan, stats, quantity);
    } else if (project_ended) {
        _withdraw_project_end(guarantor, plan, stats, conf, quantity);
    } else {
        _withdraw_in_progress(guarantor, plan, stats, conf, quantity);
    }
}

/**
 *
 * 1️项目失败 / 取消 → 立即全额解押
 *
 */
void guarantyrwa::_withdraw_failed_project(const name& guarantor,
                                           const fundplan_t& plan,
                                           guaranty_stats_t& stats,
                                           const asset& quantity) {
    asset available = stats.available_guarantee_funds;
    CHECKC(available.amount > 0, err::QUANTITY_INSUFFICIENT, "no available funds");
    CHECKC(quantity <= available, err::QUANTITY_INSUFFICIENT, "withdraw exceeds available funds");

    _do_withdraw(guarantor, plan, stats, quantity,
                 "withdraw due to project failure, plan:" + std::to_string(plan.id));
}

/**
 *
 * 2️项目进行中 → 按进度与分红比例部分解押
 *
 */
void guarantyrwa::_withdraw_in_progress(const name& guarantor,
                                        const fundplan_t& plan,
                                        guaranty_stats_t& stats,
                                        const guaranty_conf_t& conf,
                                        const asset& quantity) {
    time_point_sec now = time_point_sec(current_time_point());

    // 已进行月数
    uint64_t elapsed_months =
        (now.sec_since_epoch() - plan.start_time.sec_since_epoch()) / (30ull * 86400ull);
    if (elapsed_months > plan.return_months) elapsed_months = plan.return_months;

    // 历史分红累计
    yield_log_t::idx_t yieldlogs(_gstate.yield_contract, plan.id);
    asset distributed(0, plan.goal_quantity.symbol);
    if (yieldlogs.begin() != yieldlogs.end()) {
        auto last = yieldlogs.end();
        --last;
        distributed = last->cumulative_yield;
    }

    // 预计应锁定资金
    asset monthly_min = _monthly_guarantee_min(plan);
    int128_t expected_cov =
        (int128_t)monthly_min.amount * elapsed_months * conf.coverage_ratio_bp / 10000;
    int128_t still_locked = expected_cov - distributed.amount;
    if (still_locked < 0) still_locked = 0;

    // 计算可解押金额
    int128_t unlocked = (int128_t)stats.available_guarantee_funds.amount - still_locked;
    if (unlocked < 0) unlocked = 0;

    asset unlocked_asset((int64_t)unlocked, plan.goal_quantity.symbol);
    CHECKC(unlocked_asset.amount > 0,
           err::QUANTITY_INSUFFICIENT,
           "no unlocked funds available for plan " + std::to_string(plan.id));
    CHECKC(quantity <= unlocked_asset,
           err::QUANTITY_INSUFFICIENT,
           "withdraw amount " + quantity.to_string() +
           " exceeds unlocked balance " + unlocked_asset.to_string());

    _do_withdraw(guarantor, plan, stats, quantity,
                 "withdraw unlocked guarantee funds ,plan:" + std::to_string(plan.id));
}

/**
 *
 * 3️项目到期 → 自动补足担保收益后解押剩余
 *
 */
void guarantyrwa::_withdraw_project_end(const name& guarantor,
                                        const fundplan_t& plan,
                                        guaranty_stats_t& stats,
                                        const guaranty_conf_t& conf,
                                        const asset& quantity) {
    time_point_sec now = time_point_sec(current_time_point());
    asset monthly_min = _monthly_guarantee_min(plan);

    // 理论应分红 = 年化收益 × 月数 × 担保比例
    int128_t theoretical_due =
        (int128_t)monthly_min.amount * plan.return_months * conf.coverage_ratio_bp / 10000;

    // 已分红总额
    yield_log_t::idx_t yieldlogs(_gstate.yield_contract, plan.id);
    asset distributed(0, plan.goal_quantity.symbol);
    if (yieldlogs.begin() != yieldlogs.end()) {
        auto last = yieldlogs.end();
        --last;
        distributed = last->cumulative_yield;
    }

    int128_t need_compensate = theoretical_due - distributed.amount;
    if (need_compensate < 0) need_compensate = 0;

    // 若需补偿担保收益
    if (need_compensate > 0) {
        asset pay((int64_t)need_compensate, plan.goal_quantity.symbol);
        if (stats.available_guarantee_funds < pay){
            pay = stats.available_guarantee_funds;
        }

        // 支付补偿
        TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay,
                 "final guarantee compensation: " + std::to_string(plan.id));

        // 更新池
        stats.available_guarantee_funds -= pay;
        stats.used_guarantee_funds      += pay;
        stats.updated_at = now;
        _db.set(stats);

        // 按比例扣减担保人余额
        _deduct_from_guarantors(plan.id, pay);

        // 记录分红日志
        record_yield_log_monthly(_gstate.yield_contract, get_self(), plan.id, pay);
    }

    // 结算后剩余资金可解押
    asset unlocked_asset = stats.available_guarantee_funds;
    CHECKC(unlocked_asset.amount > 0, err::QUANTITY_INSUFFICIENT, "no remaining funds to withdraw");
    CHECKC(quantity <= unlocked_asset, err::QUANTITY_INSUFFICIENT,
        "withdraw amount " + quantity.to_string() +
        " exceeds available balance " + unlocked_asset.to_string() +
        " for plan " + std::to_string(plan.id));

    _do_withdraw(guarantor, plan, stats, quantity,
                 "withdraw remaining guarantee funds (after settlement) for plan:" + std::to_string(plan.id));
}

/**
 *
 * 执行实际解押 + 更新担保人余额
 *
 */
void guarantyrwa::_do_withdraw(const name& guarantor,
                               const fundplan_t& plan,
                               guaranty_stats_t& stats,
                               const asset& quantity,
                               const string& memo) {
    time_point_sec now = time_point_sec(current_time_point());

    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto itr = stakes.find(guarantor.value);
    CHECKC(itr != stakes.end(), err::RECORD_NOT_FOUND, "guarantor stake not found");
    CHECKC(itr->total_funds >= quantity,
           err::QUANTITY_INSUFFICIENT,
           "guarantor " + guarantor.to_string() +
           " has insufficient staked balance for plan " + std::to_string(plan.id) +
           " — tried to withdraw " + quantity.to_string() +
           ", but only has " + itr->total_funds.to_string());

    //更新担保池
    stats.total_guarantee_funds     -= quantity;
    stats.available_guarantee_funds -= quantity;
    stats.updated_at = now;
    _db.set(stats);

    //更新担保人余额
    stakes.modify(itr, get_self(), [&](auto& s) {
        s.total_funds -= quantity;
        s.updated_at  = now;
    });

    // 实际转账
    TRANSFER(plan.goal_asset_contract, guarantor, quantity, memo);
}

/**
 *
 * 按比例从所有担保人扣减担保支付
 *
 */
void guarantyrwa::_deduct_from_guarantors(uint64_t plan_id, const asset& pay) {
    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "guaranty stats not found");

    asset total_pool = stats.available_guarantee_funds + stats.used_guarantee_funds;
    CHECKC(total_pool.amount > 0, err::PARAM_ERROR, "total pool is zero");

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors found");

    int64_t total_deducted = 0;
    auto end_itr = stakes.end();
    --end_itr; // 最后一个担保人用于补差额

    for (auto itr = stakes.begin(); itr != stakes.end(); ++itr) {
        int64_t deduct_amt = 0;

        if (itr != end_itr) {
            // 用 int128 精度计算占比扣减，防止浮点误差
            __int128 part = (__int128)pay.amount * itr->total_funds.amount / total_pool.amount;
            deduct_amt = (int64_t)part;
            total_deducted += deduct_amt;
        } else {
            // 最后一个担保人补足剩余部分
            deduct_amt = pay.amount - total_deducted;
        }

        if (deduct_amt <= 0) continue;

        stakes.modify(itr, get_self(), [&](auto& s) {
            s.total_funds.amount -= deduct_amt;
            if (s.total_funds.amount < 0) s.total_funds.amount = 0;
            s.updated_at = time_point_sec(current_time_point());
        });
    }
}