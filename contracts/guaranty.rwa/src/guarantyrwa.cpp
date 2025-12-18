#include "guarantyrwa.hpp"
#include "yield.rwa/yieldrwa.hpp"
#include "stake.rwa/stakerwadb.hpp"

#include <flon/flon.token.hpp>
#include <flon/utils.hpp>
#include <flon/consts.hpp>

using namespace rwafi;
using namespace eosio;
using namespace flon;
using std::string;

// 当前年月（YYYYMM）
uint64_t guarantyrwa::_current_period_yyyymm() {
    const time_t t = (time_t)current_time_point().sec_since_epoch();
    const tm* g = gmtime(&t);
    return ((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

// ------------------------------------------------------------
// 年度最低担保额度
// 规则：total_raised_funds / years / 2
// ------------------------------------------------------------
asset guarantyrwa::_yearly_guarantee_principal(const fundplan_t& plan) {

    uint16_t years = std::max<uint16_t>(1, (plan.return_months + 11) / 12);
    CHECKC(plan.total_raised_funds.amount > 0, err::PARAM_ERROR, "total_raised_funds is zero");
    int64_t yearly_amt = plan.total_raised_funds.amount / years / 2;

    return asset{ yearly_amt, plan.total_raised_funds.symbol };
}

// ------------------------------------------------------------
// 获取担保人表（multi_index wrapper）
// ------------------------------------------------------------
guarantor_stake_t::idx_t guarantyrwa::_get_stake_tbl(uint64_t plan_id) const {
    return guarantor_stake_t::idx_t(get_self(), plan_id);
}

// ------------------------------------------------------------
// 从 wasm_db 读取担保池统计
// ------------------------------------------------------------
guaranty_stats_t guarantyrwa::_get_stats_or_fail(uint64_t plan_id) {
    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "guaranty stats not found");
    return stats;
}

// ------------------------------------------------------------
// 读取单个担保人记录（按值返回）
// ------------------------------------------------------------
guarantor_stake_t  guarantyrwa::_get_stake_or_fail(uint64_t plan_id, const name& guarantor) {
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND,"guarantor not found");

    return *it;
}

// ------------------------------------------------------------
// 从 plan.end_time 起，已过去的完整自然年数
// ------------------------------------------------------------
uint32_t guarantyrwa::_years_passed(const fundplan_t& plan) const {
    const time_point_sec now = time_point_sec(current_time_point());
    if (now <= plan.end_time) {
        return 0;
    }
    uint64_t elapsed =  now.sec_since_epoch() - plan.end_time.sec_since_epoch();
    return elapsed / seconds_per_year;
}

// ------------------------------------------------------------
// 投资人累计收益（唯一权威口径）
// 来源：stake.rwa::stake_plan.reward_state.total_rewards
// ------------------------------------------------------------
int64_t guarantyrwa::_calc_investor_yield_sum(uint64_t plan_id,const symbol& sym) const {
    stake_plan_t::tbl_t plans( _gstate.stake_contract, _gstate.stake_contract.value);

    auto it = plans.find(plan_id);
    CHECKC(it != plans.end(),err::RECORD_NOT_FOUND,"stake plan not found");
    CHECKC(it->reward_state.total_rewards.symbol == sym, err::SYMBOL_MISMATCH, "reward symbol mismatch");

    return it->reward_state.total_rewards.amount;
}

// ============================================================
// 覆盖率统一计算（纯函数）
// ============================================================

guarantyrwa::CoverageInfo guarantyrwa::_calc_coverage(const fundplan_t& plan,const guaranty_stats_t& stats,const symbol& sym) const {
    CoverageInfo info{};

    const int64_t T = plan.total_raised_funds.amount;
    const int64_t H = T / 2;
    const int64_t G = stats.total_guarantee_funds.amount;

    const int64_t investor_yield = _calc_investor_yield_sum(plan.id, sym);
    const int64_t coverage = G + investor_yield;
    int64_t unlocked_pool = 0;

    if (coverage > H) {
        unlocked_pool = coverage - H;
        if (unlocked_pool > G)
            unlocked_pool = G;
    }

    info.G              = G;
    info.investor_yield = investor_yield;
    info.H              = H;
    info.unlocked_pool  = unlocked_pool;
    return info;
}

// ============================================================
// 初始化
// ============================================================

void guarantyrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID,"invalid admin");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

// ============================================================
// on_transfer
//  - guaranty:<plan_id>  担保本金
//  - reward:<plan_id>    担保收益（外部注入）
// ============================================================

void guarantyrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo){
    if (from == get_self() || to != get_self()) return;

    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid transfer amount");
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2, err::INVALID_FORMAT, "memo must be <type>:<plan_id>");

    const string   action  = parts[0];
    const uint64_t plan_id = std::stoull(parts[1]);

    // 读取 plan
    fundplan_t::idx_t plans( _gstate.invest_contract,_gstate.invest_contract.value);
    auto pit = plans.find(plan_id);
    CHECKC(pit != plans.end(),err::RECORD_NOT_FOUND,"plan not found");

    const fundplan_t& plan = *pit;
    CHECKC(get_first_receiver() == plan.goal_asset_contract,err::CONTRACT_MISMATCH,"token contract mismatch");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH,"symbol mismatch");

    if (action == "guaranty") {
        _handle_guaranty_transfer(from, plan, quantity);
        return;
    }

    if (action == "reward") {
        _handle_reward_transfer(plan, quantity);
        return;
    }

    CHECKC(false,err::PARAM_ERROR,"unsupported transfer type");
}

// ============================================================
// 担保本金注入（shares = 永久权重）
// ============================================================

void guarantyrwa::_handle_guaranty_transfer(const name& from,const fundplan_t& plan,const asset& quantity) {
    CHECKC(quantity.amount > 0,err::NOT_POSITIVE,"invalid guaranty amount");

    const uint64_t       plan_id = plan.id;
    const symbol         sym     = quantity.symbol;
    const time_point_sec now     = time_point_sec(current_time_point());

    // 仅允许募资期间
    CHECKC(now >= plan.start_time,err::INVALID_STATUS, "guaranty not started");
    CHECKC(now < plan.end_time, err::INVALID_STATUS, "guaranty already ended");
    CHECKC(plan.status == PlanStatus::PENDING || plan.status == PlanStatus::RAISEACTIVE || plan.status == PlanStatus::SUCCESS,
                                err::INVALID_STATUS,"guaranty not allowed in this status");

    // --------------------------------------------------------
    // 更新担保池统计（wasm_db）
    // --------------------------------------------------------
    guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
    auto sit = stats_tbl.find(plan_id);

    if (sit == stats_tbl.end()) {
        stats_tbl.emplace(get_self(), [&](auto& s) {
            s.plan_id               = plan_id;
            s.total_guarantee_funds = quantity;
            s.used_guarantee_funds  = asset(0, sym);
            s.cumulative_yield      = asset(0, sym);
            s.created_at            = now;
            s.updated_at            = now;
        });
    } else {
        stats_tbl.modify(sit, same_payer, [&](auto& s) {
            CHECKC(s.total_guarantee_funds.symbol == sym, err::SYMBOL_MISMATCH, "guarantee stats symbol mismatch");
            s.total_guarantee_funds += quantity;
            s.updated_at = now;
        });
    }

    // --------------------------------------------------------
    // 更新担保人仓位（shares 永不减少）
    // --------------------------------------------------------
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(from.value);

    if (it == stakes.end()) {
        stakes.emplace(get_self(), [&](auto& s) {
            s.guarantor    = from;
            s.shares       = quantity;   // 永久权重
            s.total_stake  = quantity;
            s.earned_yield = asset(0, sym);
            s.withdrawn    = asset(0, sym);
            s.created_at   = now;
            s.updated_at   = now;
        });
    } else {
        stakes.modify(it, same_payer, [&](auto& s) {
            CHECKC(s.shares.symbol == sym, err::SYMBOL_MISMATCH, "shares symbol mismatch");
            CHECKC(s.total_stake.symbol == sym, err::SYMBOL_MISMATCH,"stake symbol mismatch");

            s.shares.amount      += quantity.amount;
            s.total_stake.amount += quantity.amount;
            s.updated_at          = now;
        });
    }
}

// ============================================================
// 担保收益分红（按 shares 比例滚入 total_stake）
// ============================================================

void guarantyrwa::_handle_reward_transfer(const fundplan_t& plan, const asset&      quantity) {

    CHECKC(quantity.amount > 0, err::NOT_POSITIVE,"invalid reward amount");

    const uint64_t       plan_id = plan.id;
    const symbol         sym     = quantity.symbol;
    const time_point_sec now     = time_point_sec(current_time_point());

    CHECKC(plan.status == PlanStatus::SUCCESS|| plan.status == PlanStatus::COMPLETED, err::INVALID_STATUS,"reward not allowed in this status");
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "reward not allowed before fundraising end");
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors");

    // 汇总 shares
    __int128 total_shares = 0;
    for (const auto& s : stakes) {
        CHECKC(s.shares.symbol == sym, err::SYMBOL_MISMATCH, "shares symbol mismatch");
        total_shares += s.shares.amount;
    }
    CHECKC(total_shares > 0, err::PARAM_ERROR, "total shares is zero");

    // 分配
    int64_t distributed = 0;
    auto last = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t share_amt = 0;

        if (it == last) {
            share_amt = quantity.amount - distributed;
        } else {
            share_amt = (int64_t)(((__int128)quantity.amount * it->shares.amount) / total_shares);
        }

        if (share_amt <= 0) continue;
        distributed += share_amt;

        stakes.modify(it, same_payer, [&](auto& s) {
            s.earned_yield.amount += share_amt;
            s.total_stake.amount  += share_amt;
            s.updated_at           = now;
        });
    }

    // 更新担保池统计
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);
    CHECKC(stats.total_guarantee_funds.symbol == sym,err::SYMBOL_MISMATCH,"guarantee stats symbol mismatch");

    stats.total_guarantee_funds += quantity;
    stats.cumulative_yield      += quantity;
    stats.updated_at             = now;
    _db.set(stats);
}

// ============================================================
// 年度担保补贴（guarantpay）
// 规则：
// - 募资结束后才能触发
// - 按“已完成完整自然年”计算
// - 每年最低担保 = total_raised_funds / years / 2
// - 只补差额，不预支未来年度
// - 只从担保池扣，不涉及 shares
// ============================================================

void guarantyrwa::guarantpay(const name& submitter, const uint64_t& plan_id){
    require_auth(submitter);

    // --------------------------------------------------------
    // 1) 读取计划
    // --------------------------------------------------------
    fundplan_t::idx_t plans(_gstate.invest_contract,_gstate.invest_contract.value);
    auto pit = plans.find(plan_id);
    CHECKC(pit != plans.end(),err::RECORD_NOT_FOUND,"plan not found");
    const fundplan_t& plan = *pit;

    CHECKC(plan.goal_quantity.symbol == plan.total_raised_funds.symbol,err::SYMBOL_MISMATCH,"plan symbol mismatch");
    CHECKC(plan.status != PlanStatus::FAILED && plan.status != PlanStatus::CANCELLED, err::INVALID_STATUS,"plan failed or cancelled");
    const time_point_sec now = time_point_sec(current_time_point());
    CHECKC(now >= plan.end_time, err::INVALID_STATUS,"guarantpay not allowed before fundraising end");

    // --------------------------------------------------------
    // 2) 计算项目总年数
    // --------------------------------------------------------
    const uint32_t total_years = std::max<uint32_t>(1, (plan.return_months + 11) / 12);

    // --------------------------------------------------------
    // 3) 已完成的完整自然年
    // --------------------------------------------------------
    uint64_t elapsed = (now.sec_since_epoch() > plan.end_time.sec_since_epoch())
                        ? (now.sec_since_epoch() - plan.end_time.sec_since_epoch())
                        : 0;

    uint32_t years_passed =std::min<uint32_t>( elapsed / seconds_per_year, total_years);
    CHECKC(years_passed > 0, err::INVALID_STATUS,"no guarantee year completed yet");

    // --------------------------------------------------------
    // 4) 年度最低担保额度
    //    = total_raised_funds / years / 2
    // --------------------------------------------------------
    asset yearly_cap = _yearly_guarantee_principal(plan);
    CHECKC(yearly_cap.amount > 0,err::PARAM_ERROR,"yearly guarantee cap is zero");

    // 累计允许的最大担保金额
    const int64_t max_allowed = (int64_t)((__int128)yearly_cap.amount * years_passed);

    // --------------------------------------------------------
    // 5) 已经真实发放给投资人的收益（唯一可信来源）
    // --------------------------------------------------------
    const int64_t investor_paid = _calc_investor_yield_sum(plan_id, yearly_cap.symbol);

    // --------------------------------------------------------
    // 6) 缺口计算（只补差额）
    // --------------------------------------------------------
    int64_t need = max_allowed - investor_paid;
    if (need <= 0) {
        // 已满足担保义务
        return;
    }

    // --------------------------------------------------------
    // 7) 担保池余额限制
    // --------------------------------------------------------
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);
    CHECKC(stats.total_guarantee_funds.symbol == yearly_cap.symbol, err::SYMBOL_MISMATCH, "guarantee stats symbol mismatch");
    const int64_t pool_avail = stats.total_guarantee_funds.amount;

    CHECKC(pool_avail > 0, err::QUANTITY_INSUFFICIENT, "guarantee pool empty");
    const int64_t pay_amt = std::min<int64_t>(need, pool_avail);
    CHECKC(pay_amt > 0, err::QUANTITY_INSUFFICIENT, "no guarantee available to pay");

    asset pay{ pay_amt, yearly_cap.symbol };

    // --------------------------------------------------------
    // 8) 从担保人仓位中分摊扣减（内部账）
    // --------------------------------------------------------
    _deduct_from_guarantors(plan_id, pay);

    // --------------------------------------------------------
    // 9) 转账给 stake.rwa，作为投资人补贴收益
    // --------------------------------------------------------
    TRANSFER( plan.goal_asset_contract,_gstate.stake_contract, pay,"reward:" + std::to_string(plan_id));

    // --------------------------------------------------------
    // 10) 记录投资人收益（yield.rwa）
    // --------------------------------------------------------
    rwafi::yieldrwa::recordyield_action{
        _gstate.yield_contract,
        { { get_self(), "active"_n } }
    }.send( plan_id, pay);
}

// ============================================================
// 担保赎回入口
// ============================================================

void guarantyrwa::redeem(const name& guarantor, const uint64_t& plan_id, const asset& quantity){
    require_auth(guarantor);
    CHECKC(quantity.amount > 0,err::NOT_POSITIVE,"invalid redeem amount");

    // 读取计划
    fundplan_t::idx_t plans( _gstate.invest_contract, _gstate.invest_contract.value);
    auto pit = plans.find(plan_id);
    CHECKC(pit != plans.end(), err::RECORD_NOT_FOUND, "plan not found");

    const fundplan_t plan = *pit;
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH,"symbol mismatch");

    // 读取担保池
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);

    // 担保人记录
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(),err::RECORD_NOT_FOUND, "guarantor not found");

    CHECKC(it->total_stake.amount > 0,err::QUANTITY_INSUFFICIENT, "no remaining stake");
    const time_point_sec now = time_point_sec(current_time_point());
    const bool failed =(plan.status == PlanStatus::FAILED|| plan.status == PlanStatus::CANCELLED);

    const bool ended = (now >= plan.return_end_time);
    if (failed) {
        _redeem_failed_project(guarantor, plan, stats, quantity );
        return;
    }
    if (ended) {
        _redeem_project_end( guarantor, plan, stats, quantity);
        return;
    }

    const bool fundraising_ended = (now >= plan.end_time);
    const bool project_running   = (now < plan.return_end_time);

    if (plan.status == PlanStatus::SUCCESS && fundraising_ended && project_running)
    {
        _redeem_in_progress(guarantor, plan, stats, quantity);
        return;
    }
    CHECKC(false, err::INVALID_STATUS, "plan not redeemable at current stage");
}

// ============================================================
// (1) FAILED / CANCELLED：全额解锁
// 规则：
// - earned_yield 优先抵扣
// - 再扣 total_stake
// ============================================================

void guarantyrwa::_redeem_failed_project(const name& guarantor,const fundplan_t& plan,guaranty_stats_t& stats,const asset& quantity){

    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(),err::RECORD_NOT_FOUND,"guarantor not found");

    int64_t redeemable = it->total_stake.amount+ it->earned_yield.amount;
    CHECKC(q <= redeemable, err::QUANTITY_INSUFFICIENT, "redeem exceeds user balance");
    CHECKC(stats.total_guarantee_funds.amount >= q, err::QUANTITY_INSUFFICIENT,"guarantee pool insufficient");

    int64_t need = q;

    stakes.modify(it, same_payer, [&](auto& s) {
        int64_t use_yield = std::min<int64_t>(need, s.earned_yield.amount);
        s.earned_yield.amount -= use_yield;
        need -= use_yield;

        if (need > 0) {
            s.total_stake.amount -= need;
            need = 0;
        }

        s.withdrawn.amount += q;
        s.updated_at = now;
    });

    stats.total_guarantee_funds.amount -= q;
    stats.used_guarantee_funds.amount  += q;
    stats.updated_at = now;
    _db.set(stats);

    TRANSFER( plan.goal_asset_contract, guarantor, quantity,"redeem (failed project)");
}

// ============================================================
// (2) SUCCESS 进行中：按 global_unlock + shares
// ============================================================

void guarantyrwa::_redeem_in_progress(const name& guarantor,const fundplan_t& plan,guaranty_stats_t& stats,const asset& quantity){

    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(),err::RECORD_NOT_FOUND,"guarantor not found");

    CHECKC(stats.total_guarantee_funds.amount >= q, err::QUANTITY_INSUFFICIENT,"guarantee pool insufficient");
    const redeem_cap_t cap = _calc_guarantor_redeemable_in_progress(plan_id,plan,stats,sym, *it, q);
    CHECKC(cap.remaining_unlock >= q,err::QUANTITY_INSUFFICIENT,"not enough unlocked guarantee");

    stakes.modify(it, same_payer, [&](auto& s) {
        s.total_stake.amount -= q;
        s.withdrawn.amount   += q;
        s.updated_at          = now;
    });

    stats.total_guarantee_funds.amount -= q;
    stats.used_guarantee_funds.amount  += q;
    stats.updated_at = now;
    _db.set(stats);

    TRANSFER(plan.goal_asset_contract,guarantor,quantity,"redeem (in progress)");
}

// ============================================================
// (3) PROJECT_END：
// 规则：
// - 先满足担保义务 H = total_raised / 2
// - investor_yield 优先覆盖
// - 剩余 obligation 必须由担保池覆盖
// ============================================================

void guarantyrwa::_redeem_project_end(const name& guarantor,const fundplan_t& plan,guaranty_stats_t& stats,const asset& quantity){

    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(),err::RECORD_NOT_FOUND,"guarantor not found");
    CHECKC(it->total_stake.amount >= q,err::QUANTITY_INSUFFICIENT,"redeem exceeds stake");

    const int64_t T = plan.total_raised_funds.amount;
    const int64_t H = T / 2;
    const int64_t investor_yield = _calc_investor_yield_sum(plan_id, sym);

    int64_t remaining_obligation = 0;
    if (H > investor_yield)
        remaining_obligation = H - investor_yield;

    const int64_t G_after = stats.total_guarantee_funds.amount - q;
    CHECKC(G_after >= remaining_obligation, err::INVALID_STATUS, "remaining guarantee obligation not satisfied");

    stakes.modify(it, same_payer, [&](auto& s) {
        s.total_stake.amount -= q;
        s.withdrawn.amount   += q;
        s.updated_at          = now;
    });

    stats.total_guarantee_funds.amount -= q;
    stats.used_guarantee_funds.amount  += q;
    stats.updated_at = now;
    _db.set(stats);

    TRANSFER( plan.goal_asset_contract, guarantor,quantity, "redeem (project end)");
}

void guarantyrwa::_deduct_from_guarantors(uint64_t plan_id,const asset& pay) {

    CHECKC(pay.amount > 0, err::NOT_POSITIVE, "invalid pay amount");

    // ------------------------------------------------------------
    // 1) 读取担保池状态
    // ------------------------------------------------------------
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);
    CHECKC(stats.total_guarantee_funds.symbol == pay.symbol,err::SYMBOL_MISMATCH,"guarantee stats symbol mismatch");
    CHECKC(stats.total_guarantee_funds.amount >= pay.amount,err::QUANTITY_INSUFFICIENT,"guarantee pool insufficient");
    const time_point_sec now = time_point_sec(current_time_point());

    // ------------------------------------------------------------
    // 2) 担保人列表
    // ------------------------------------------------------------
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND,"no guarantors");

    // ------------------------------------------------------------
    // 3) 汇总 total_stake（作为分摊权重）
    // ------------------------------------------------------------
    __int128 total_stake_sum = 0;
    for (const auto& s : stakes) {
        CHECKC(s.total_stake.symbol == pay.symbol,err::SYMBOL_MISMATCH, "stake symbol mismatch");
        CHECKC(s.total_stake.amount >= 0,err::PARAM_ERROR, "invalid total_stake");
        total_stake_sum += s.total_stake.amount;
    }
    CHECKC(total_stake_sum > 0, err::PARAM_ERROR, "total_stake is zero");

    // ------------------------------------------------------------
    // 4) 按 total_stake 比例分摊扣减
    // ------------------------------------------------------------
    int64_t deducted = 0;
    auto last = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t part = 0;

        if (it == last) {
            // 兜底，避免舍入误差
            part = pay.amount - deducted;
        } else {
            part = (int64_t)(
                (__int128)pay.amount
                * it->total_stake.amount
                / total_stake_sum
            );
        }

        if (part <= 0) continue;
        deducted += part;

        stakes.modify(it, same_payer, [&](auto& s) {
            s.total_stake.amount -= part;
            if (s.total_stake.amount < 0)
                s.total_stake.amount = 0;
            s.updated_at = now;
        });
    }

    CHECKC(deducted == pay.amount, err::INVALID_STATUS, "internal error: deducted amount mismatch");

    // ------------------------------------------------------------
    // 5) 更新担保池状态
    // ------------------------------------------------------------
    stats.total_guarantee_funds.amount -= pay.amount;
    stats.used_guarantee_funds.amount  += pay.amount;
    stats.updated_at                    = now;
    _db.set(stats);
}


redeem_cap_t guarantyrwa::_calc_guarantor_redeemable_in_progress(uint64_t plan_id, const fundplan_t& plan, guaranty_stats_t& stats, const symbol& sym,const guarantor_stake_t& user_row,int64_t redeem_q) const{
    redeem_cap_t cap;

    // ------------------------------------------------------------
    // 1) 半仓安全线 H = T / 2
    // ------------------------------------------------------------
    const int64_t T = plan.total_raised_funds.amount;
    CHECKC(T >= 0, err::PARAM_ERROR, "invalid total_raised_funds");
    cap.H = T / 2;

    // ------------------------------------------------------------
    // 2) 投资人累计收益（唯一权威来源）
    // ------------------------------------------------------------
    cap.investor_yield = _calc_investor_yield_sum(plan_id, sym);

    // ------------------------------------------------------------
    // 3) G0 = 当前余额 + 已赎回（用于回溯总担保规模）
    // ------------------------------------------------------------
    const int64_t G0 = stats.total_guarantee_funds.amount + stats.used_guarantee_funds.amount;

    // ------------------------------------------------------------
    // 4) 全局可解锁量：G0 + investor_yield - H
    // ------------------------------------------------------------
    __int128 global_unlock128 =(__int128)G0 +(__int128)cap.investor_yield -(__int128)cap.H;
    cap.global_unlock = global_unlock128 > 0 ? (int64_t)global_unlock128 : 0;

    // ------------------------------------------------------------
    // 5) 本次 redeem 后仍需满足 50% 安全线
    // ------------------------------------------------------------
    const int64_t G_after = stats.total_guarantee_funds.amount - redeem_q;
    CHECKC(G_after >= 0, err::QUANTITY_INSUFFICIENT, "guarantee pool would go negative");
    CHECKC( (__int128)G_after + (__int128)cap.investor_yield >= (__int128)cap.H, err::INVALID_STATUS,"redeem would break 50% guarantee safety line");

    // ------------------------------------------------------------
    // 6) 汇总全局 shares
    // ------------------------------------------------------------
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    __int128 total_shares = 0;
    for (const auto& s : stakes) {
        CHECKC(s.shares.amount >= 0, err::PARAM_ERROR, "invalid shares");
        total_shares += s.shares.amount;
    }
    CHECKC(total_shares > 0, err::PARAM_ERROR, "total shares is zero");

    // ------------------------------------------------------------
    // 7) 按 shares 比例计算用户累计可解锁上限
    // ------------------------------------------------------------
    __int128 user_unlock_cap128 = (__int128)cap.global_unlock * (__int128)user_row.shares.amount /total_shares;
    cap.total_unlock_cap =user_unlock_cap128 > 0 ? (int64_t)user_unlock_cap128 : 0;

    // ------------------------------------------------------------
    // 8) 扣掉已提现 withdrawn
    // ------------------------------------------------------------
    cap.remaining_unlock = cap.total_unlock_cap - user_row.withdrawn.amount;

    if (cap.remaining_unlock < 0)
        cap.remaining_unlock = 0;

    return cap;
}