#include "guarantyrwa.hpp"
#include "yield.rwa/yieldrwadb.hpp"

#include <flon/flon.token.hpp>
#include <flon/utils.hpp>

using namespace rwafi;
using namespace eosio;
using namespace flon;

uint64_t guarantyrwa::_current_period_yyyymm() {
    const time_t t = (time_t) current_time_point().sec_since_epoch();
    const tm* g = gmtime(&t);
    return ((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

asset guarantyrwa::_yearly_guarantee_principal(const fundplan_t& plan) {
    uint16_t years = std::max<uint16_t>(1, (plan.return_months + 11) / 12);
    int64_t yearly_amt = (int64_t)((__int128)plan.goal_quantity.amount / years / 2);
    return {yearly_amt, plan.goal_quantity.symbol};
}

static void record_yield_log_monthly(const name& yield_contract,const name& payer,uint64_t plan_id,const asset& pay)
{
    yield_log_t::idx_t logs(yield_contract, plan_id);
    const uint64_t period = []() {
        const time_t t = (time_t) current_time_point().sec_since_epoch();
        const tm* g = gmtime(&t);
        return ((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
    }();

    asset cumulative_prev(0, pay.symbol);
    if (auto it = logs.rbegin(); it != logs.rend()) cumulative_prev = it->cumulative_yield;

    auto itr = logs.find(period);
    if (itr == logs.end()) {
        logs.emplace(payer, [&](auto& y) {
            y.period           = period;
            y.period_yield     = pay;
            y.cumulative_yield = cumulative_prev + pay;
            y.created_at       = y.updated_at = time_point_sec(current_time_point());
        });
    } else {
        logs.modify(itr, payer, [&](auto& y) {
            y.period_yield     += pay;
            y.cumulative_yield += pay;
            y.updated_at        = time_point_sec(current_time_point());
        });
    }
}

void guarantyrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

// 担保本金 / 分红
void guarantyrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid transfer amount");

    // 解析 memo: 格式 <type>:<plan_id>
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2, err::INVALID_FORMAT, "memo must be <type>:<plan_id>");

    const string action = parts[0];
    const uint64_t plan_id = std::stoull(parts[1]);

    // 从 investrwa 合约中读取计划
    fundplan_t::idx_t fundplans(_gstate.invest_contract, _gstate.invest_contract.value);
    auto itr = fundplans.find(plan_id);
    CHECKC(itr != fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    const fundplan_t plan = *itr;

    // 校验资产来源与符号
    CHECKC(get_first_receiver() == plan.goal_asset_contract, err::CONTRACT_MISMATCH, "token contract mismatch");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    // 分派逻辑
    if (action == "guaranty") return _handle_guaranty_transfer(from, plan, quantity);
    if (action == "reward")   return _handle_reward_transfer(plan, quantity);

    CHECKC(false, err::PARAM_ERROR, "unsupported transfer type");
}

// 担保本金充值
void guarantyrwa::_handle_guaranty_transfer(const name& from,
                                            const fundplan_t& plan,
                                            const asset& quantity)
{
    const time_point_sec now = time_point_sec(current_time_point());
    const uint64_t plan_id   = plan.id;
    const symbol sym         = quantity.symbol;

    guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
    auto stats_itr = stats_tbl.find(plan_id);

    if (stats_itr == stats_tbl.end()) {
        // 首次创建
        stats_tbl.emplace(get_self(), [&](auto& s) {
            s.plan_id               = plan_id;
            s.total_guarantee_funds = quantity;
            s.used_guarantee_funds  = asset(0, sym);
            s.created_at = s.updated_at = now;
        });
    } else {
        // 累加担保金额
        stats_tbl.modify(stats_itr, same_payer, [&](auto& s) {
            s.total_guarantee_funds += quantity;
            s.updated_at             = now;
        });
    }

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto itr = stakes.find(from.value);

    if (itr == stakes.end()) {
        stakes.emplace(get_self(), [&](auto& s) {
            s.guarantor       = from;
            s.total_stake     = quantity;
            s.locked_stake    = quantity;
            s.available_stake = asset(0, sym);
            s.earned_yield    = asset(0, sym);
            s.withdrawn       = asset(0, sym);
            s.created_at = s.updated_at = now;
        });
    } else {
        stakes.modify(itr, same_payer, [&](auto& s) {
            s.total_stake  += quantity;
            s.locked_stake += quantity;
            s.updated_at    = now;
        });
    }
}

// 担保收益分红
void guarantyrwa::_handle_reward_transfer(const fundplan_t& plan, const asset& quantity) {
    const time_point_sec now = time_point_sec(current_time_point());
    const uint64_t plan_id = plan.id;

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors");

    asset total_stake(0, quantity.symbol);
    for (const auto& s : stakes) total_stake += s.total_stake;
    CHECKC(total_stake.amount > 0, err::PARAM_ERROR, "total stake is zero");

    int64_t distributed = 0;
    const auto end_itr = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t share_amt = (it != end_itr)
            ? (int64_t)((__int128)quantity.amount * it->total_stake.amount / total_stake.amount)
            : (quantity.amount - distributed);
        if (share_amt <= 0) continue;

        distributed += share_amt;
        const asset share(share_amt, quantity.symbol);

        stakes.modify(it, get_self(), [&](auto& s) {
            s.earned_yield    += share;
            s.available_stake += share;
            s.updated_at = now;
        });
    }
}

// 担保收益补足（年度保障）
void guarantyrwa::guarantpay(const name& submitter, const uint64_t& plan_id, const uint64_t& year) {
    require_auth(submitter);

    fundplan_t::idx_t fundplans(_gstate.invest_contract, _gstate.invest_contract.value);
    auto plan_itr = fundplans.find(plan_id);
    CHECKC(plan_itr != fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    const auto& plan = *plan_itr;

    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "no stats");
    CHECKC(stats.total_guarantee_funds.amount > 0, err::QUANTITY_INSUFFICIENT, "empty guarantee pool");

    const uint16_t total_years = std::max<uint16_t>(1, (plan.return_months + 11) / 12);
    CHECKC(year > 0 && year <= total_years, err::PARAM_ERROR, "invalid year");

    const asset yearly_due = _yearly_guarantee_principal(plan);
    CHECKC(yearly_due.amount > 0, err::PARAM_ERROR, "invalid yearly principal");

    yield_log_t::idx_t logs(_gstate.yield_contract, plan_id);
    asset distributed(0, yearly_due.symbol);
    if (auto it = logs.rbegin(); it != logs.rend()) distributed = it->period_yield;

    const int64_t diff = yearly_due.amount - distributed.amount;
    if (diff <= 0) return;

    asset pay(std::min<int64_t>(diff, stats.total_guarantee_funds.amount), yearly_due.symbol);
    CHECKC(pay.amount > 0, err::QUANTITY_INSUFFICIENT, "insufficient guarantee pool");

    stats.total_guarantee_funds -= pay;
    stats.used_guarantee_funds  += pay;
    stats.updated_at = time_point_sec(current_time_point());
    _db.set(stats);

    _deduct_from_guarantors(plan_id, pay);
    TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay,
             "guarantee payout for plan:" + std::to_string(plan_id));
    record_yield_log_monthly(_gstate.yield_contract, get_self(), plan_id, pay);
}


// 担保资金解押
void guarantyrwa::redeem(const name& guarantor, const uint64_t& plan_id, const asset& quantity) {
    require_auth(guarantor);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid redeem amount");

    fundplan_t::idx_t fundplans(_gstate.invest_contract, _gstate.invest_contract.value);
    auto itr_plan = fundplans.find(plan_id);
    CHECKC(itr_plan != fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    fundplan_t plan = *itr_plan;

    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
    auto it_stats = stats_tbl.find(plan_id);
    CHECKC(it_stats != stats_tbl.end(), err::RECORD_NOT_FOUND, "no guaranty pool");

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found in this plan");

    CHECKC(it->total_stake.amount > 0, err::PARAM_ERROR, "guarantor has no active stake");
    CHECKC(quantity.amount <= (it->available_stake.amount + it->locked_stake.amount + it->earned_yield.amount),
           err::QUANTITY_INSUFFICIENT, "redeem exceeds guarantor balance");

    const bool failed = (plan.status == PlanStatus::FAILED || plan.status == PlanStatus::CANCELLED);
    const bool ended  = (time_point_sec(current_time_point()) >= plan.return_end_time);

    if (failed)  return _redeem_failed_project(guarantor, plan, *it_stats, quantity);
    if (ended)   return _redeem_project_end(guarantor, plan, *it_stats, quantity);
    return _redeem_in_progress(guarantor, plan, *it_stats, quantity);
}

// === (1) 项目失败或取消 ===
void guarantyrwa::_redeem_failed_project(const name& guarantor,
                                         const fundplan_t& plan,
                                         const guaranty_stats_t& stats,
                                         const asset& quantity) {
    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");

    // === 汇总可解押金额 ===
    asset redeemable = it->available_stake + it->locked_stake;
    CHECKC(quantity <= redeemable, err::QUANTITY_INSUFFICIENT, "exceeds redeemable funds");

    // === 将锁仓资金解锁 ===
    stakes.modify(it, same_payer, [&](auto& s) {
        if (s.locked_stake.amount > 0) {
            s.available_stake += s.locked_stake;
            s.locked_stake.amount = 0;
        }

        // 异常防护：失败项目不应有收益
        if (s.earned_yield.amount > 0) {
            s.earned_yield.amount = 0;
        }

        s.updated_at = time_point_sec(current_time_point());
    });
    _do_redeem(guarantor, plan, quantity, "redeem (failed project)");
}

// === (2) 项目进行中 ===
// === (2) 项目进行中 ===
void guarantyrwa::_redeem_in_progress(const name& guarantor,
                                      const fundplan_t& plan,
                                      const guaranty_stats_t& stats,
                                      const asset& quantity) {
    const time_point_sec now = time_point_sec(current_time_point());

    __int128 required_cover = (__int128)plan.goal_quantity.amount / 2;  // 50% 担保线

    // === 1️⃣ 从收益日志中读取动态比例 ===
    yield_log_t::idx_t logs(_gstate.yield_contract, plan.id);
    CHECKC(logs.begin() != logs.end(), err::RECORD_NOT_FOUND, "no yield logs found");

    asset total_yield(0, plan.goal_quantity.symbol);
    asset total_investor(0, plan.goal_quantity.symbol);
    asset total_guarantor(0, plan.goal_quantity.symbol);
    asset total_buyback(0, plan.goal_quantity.symbol);

    for (auto itr = logs.begin(); itr != logs.end(); ++itr) {
        total_yield     += itr->period_yield;
        total_investor  += itr->investor_yield;
        total_guarantor += itr->guarantor_yield;
        total_buyback   += itr->buyback_yield;
    }

    CHECKC(total_yield.amount > 0, err::PARAM_ERROR, "invalid yield log (empty)");

    double investor_share_ratio  = double(total_investor.amount)  / double(total_yield.amount);
    double guarantor_share_ratio = double(total_guarantor.amount) / double(total_yield.amount);
    double buyback_share_ratio   = double(total_buyback.amount)   / double(total_yield.amount);

    // === 2️⃣ 计算当前累计分红 ===
    asset distributed(0, plan.goal_quantity.symbol);
    if (auto lit = logs.rbegin(); lit != logs.rend())
        distributed = lit->cumulative_yield;

    __int128 guarantor_total_share = (__int128)(distributed.amount * guarantor_share_ratio);
    __int128 buyback_amount        = (__int128)(distributed.amount * buyback_share_ratio);
    __int128 net_distributed       = distributed.amount - buyback_amount;

    // === 3️⃣ 计算担保资金真实覆盖量 ===
    __int128 actual_cover = (__int128)stats.total_guarantee_funds.amount + guarantor_total_share;

    // === 4️⃣ 担保人信息 ===
    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");

    asset total_stake(0, plan.goal_quantity.symbol);
    for (auto s : stakes) total_stake += s.total_stake;
    CHECKC(total_stake.amount > 0, err::PARAM_ERROR, "zero total stake");

    // === 5️⃣ 覆盖不足 (<50%) → 回锁所有可用资金和收益 ===
    if (actual_cover < required_cover) {
        stakes.modify(it, get_self(), [&](auto& s) {
            s.locked_stake.amount    += s.available_stake.amount + s.earned_yield.amount;
            s.total_stake.amount     += s.earned_yield.amount;
            s.available_stake.amount  = 0;
            s.earned_yield.amount     = 0;
            s.updated_at = now;
        });
        CHECKC(false, err::INVALID_STATUS, "coverage below 50%, all funds relocked");
    }

    // === 6️⃣ 可解锁额度计算 ===
    int64_t unlock_pool = (int64_t)(actual_cover - required_cover);
    CHECKC(unlock_pool > 0, err::INVALID_STATUS, "no unlockable coverage margin");

    __int128 unlockable = (__int128)unlock_pool * it->total_stake.amount / total_stake.amount;
    int64_t unlocked = (int64_t)std::min<__int128>(unlockable, it->locked_stake.amount);

    if (unlocked > 0) {
        stakes.modify(it, get_self(), [&](auto& s) {
            s.locked_stake.amount    -= unlocked;
            s.available_stake.amount += unlocked;
            s.updated_at = now;
        });
    }

    // === 7️⃣ 优先使用 earned_yield 提现 ===
    asset available_all = it->available_stake + it->earned_yield;
    CHECKC(quantity.amount <= available_all.amount, err::QUANTITY_INSUFFICIENT, "redeem exceeds available+earned");

    stakes.modify(it, get_self(), [&](auto& s) {
        int64_t remain = quantity.amount;
        int64_t use_yield = std::min(remain, s.earned_yield.amount);
        s.earned_yield.amount -= use_yield;
        remain -= use_yield;

        if (remain > 0) {
            int64_t use_available = std::min(remain, s.available_stake.amount);
            s.available_stake.amount -= use_available;
            remain -= use_available;
        }
        s.updated_at = now;
    });

    // === 8️⃣ 解押执行 ===
    _do_redeem(guarantor, plan, quantity, "redeem (in progress)");
}

// === (3) 项目到期 ===
void guarantyrwa::_redeem_project_end(const name& guarantor,
                                      const fundplan_t& plan,
                                      const guaranty_stats_t& stats,
                                      const asset& quantity) {
    const time_point_sec now = time_point_sec(current_time_point());

    // === 1️⃣ 从收益日志读取真实分配比例 ===
    yield_log_t::idx_t logs(_gstate.yield_contract, plan.id);
    CHECKC(logs.begin() != logs.end(), err::RECORD_NOT_FOUND, "no yield logs found");

    asset total_yield(0, plan.goal_quantity.symbol);
    asset total_investor(0, plan.goal_quantity.symbol);
    asset total_guarantor(0, plan.goal_quantity.symbol);
    asset total_buyback(0, plan.goal_quantity.symbol);

    for (auto itr = logs.begin(); itr != logs.end(); ++itr) {
        total_yield     += itr->period_yield;
        total_investor  += itr->investor_yield;
        total_guarantor += itr->guarantor_yield;
        total_buyback   += itr->buyback_yield;
    }

    CHECKC(total_yield.amount > 0, err::PARAM_ERROR, "invalid yield log");

    double investor_share_ratio  = double(total_investor.amount)  / double(total_yield.amount);
    double guarantor_share_ratio = double(total_guarantor.amount) / double(total_yield.amount);
    double buyback_share_ratio   = double(total_buyback.amount)   / double(total_yield.amount);

    // === 2️⃣ 取累计分红 ===
    asset distributed(0, plan.goal_quantity.symbol);
    if (auto lit = logs.rbegin(); lit != logs.rend())
        distributed = lit->cumulative_yield;

    // === 3️⃣ 计算理论目标与已分配差额 ===
    __int128 theoretical_total = (__int128)plan.goal_quantity.amount / 2;
    __int128 buyback_amount    = (__int128)(distributed.amount * buyback_share_ratio);
    __int128 net_distributed   = distributed.amount - buyback_amount;
    __int128 guarantor_total_share = (__int128)(distributed.amount * guarantor_share_ratio);
    __int128 diff = theoretical_total - net_distributed;

    // === 4️⃣ 若仍需补偿担保池 ===
    if (diff > 0) {
        asset pay(std::min<int64_t>(diff, stats.total_guarantee_funds.amount), plan.goal_quantity.symbol);

        if (pay.amount > 0) {
            TRANSFER(plan.goal_asset_contract, _gstate.stake_contract, pay,
                     "final guarantee payout:" + std::to_string(plan.id));

            guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
            auto it_stats = stats_tbl.find(plan.id);
            CHECKC(it_stats != stats_tbl.end(), err::RECORD_NOT_FOUND, "no stats");

            stats_tbl.modify(it_stats, same_payer, [&](auto& s) {
                s.total_guarantee_funds -= pay;
                s.used_guarantee_funds  += pay;
                s.updated_at = now;
            });

            _deduct_from_guarantors(plan.id, pay);
            record_yield_log_monthly(_gstate.yield_contract, get_self(), plan.id, pay);
        }
    }

    // === 5️⃣ 可赎回余额 ===
    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");

    asset redeemable = it->available_stake + it->locked_stake + it->earned_yield;
    CHECKC(quantity <= redeemable, err::QUANTITY_INSUFFICIENT, "redeem exceeds balance");

    _do_redeem(guarantor, plan, quantity, "redeem after project end");
}

// 实际解押执行
void guarantyrwa::_do_redeem(const name& guarantor,
                             const fundplan_t& plan,
                             const asset& quantity,
                             const string& memo) {
    guarantor_stake_t::idx_t stakes(get_self(), plan.id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");

    CHECKC(it->available_stake.amount >= quantity.amount,
           err::QUANTITY_INSUFFICIENT, "insufficient available stake");

    stakes.modify(it, get_self(), [&](auto& s) {
        s.available_stake -= quantity;
        s.withdrawn       += quantity;
        s.updated_at = time_point_sec(current_time_point());
    });

    TRANSFER(plan.goal_asset_contract, guarantor, quantity, memo);
}

// ============================================================
// 担保成本分摊
// ============================================================

void guarantyrwa::_deduct_from_guarantors(uint64_t plan_id, const asset& pay) {
    CHECKC(pay.amount > 0, err::NOT_POSITIVE, "invalid pay amount");

    // === 1️⃣ 读取担保池 ===
    guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
    auto it_stats = stats_tbl.find(plan_id);
    CHECKC(it_stats != stats_tbl.end(), err::RECORD_NOT_FOUND, "no stats");
    CHECKC(it_stats->total_guarantee_funds.amount > 0, err::PARAM_ERROR, "empty pool");

    // === 2️⃣ 读取担保人表 ===
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors");

    // === 3️⃣ 汇总总担保量 ===
    __int128 total_stake = 0;
    for (const auto& s : stakes) {
        total_stake += s.total_stake.amount;
    }
    CHECKC(total_stake > 0, err::PARAM_ERROR, "invalid total stake");

    // === 4️⃣ 计算扣减比例 ===
    int64_t total_deducted = 0;
    const auto end_itr = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        // ⛓ 按比例扣减
        int64_t deduct_amt = (it != end_itr)
            ? static_cast<int64_t>(((__int128)pay.amount * it->total_stake.amount) / total_stake)
            : (pay.amount - total_deducted);

        if (deduct_amt <= 0) continue;

        total_deducted += deduct_amt;
        asset deduct_asset(deduct_amt, pay.symbol);

        // 🔐 更新锁定额度
        stakes.modify(it, get_self(), [&](auto& s) {
            int64_t new_locked = std::max<int64_t>(0, s.locked_stake.amount - deduct_amt);
            s.locked_stake.amount = new_locked;
            s.updated_at = time_point_sec(current_time_point());
        });
    }

    // === 5️⃣ 同步更新担保池 ===
    stats_tbl.modify(it_stats, same_payer, [&](auto& s) {
        s.total_guarantee_funds.amount = std::max<int64_t>(0, s.total_guarantee_funds.amount - pay.amount);
        s.used_guarantee_funds.amount  += pay.amount;
        s.updated_at = time_point_sec(current_time_point());
    });
}