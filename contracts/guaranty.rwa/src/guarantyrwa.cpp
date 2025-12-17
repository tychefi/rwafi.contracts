#include "guarantyrwa.hpp"
#include "yield.rwa/yieldrwa.hpp"

#include <flon/flon.token.hpp>
#include <flon/utils.hpp>
#include <flon/consts.hpp>

using namespace rwafi;
using namespace eosio;
using namespace flon;
using std::string;

// ========================================
// 工具函数 & 内部结构
// ========================================

uint64_t guarantyrwa::_current_period_yyyymm() {
    const time_t t = (time_t) current_time_point().sec_since_epoch();
    const tm*    g = gmtime(&t);
    return ((g->tm_year + 1900) * 100 + (g->tm_mon + 1));
}

// 每年最低担保额度：goal / years / 2
asset guarantyrwa::_yearly_guarantee_principal(const fundplan_t& plan) {
    uint16_t years = std::max<uint16_t>(1, (plan.return_months + 11) / 12);
    auto  yearly_amt = plan.goal_quantity.amount / years / 2;
    return { yearly_amt, plan.goal_quantity.symbol };
}

// 1) 取某个 plan 的担保人表（multi_index 封装）
guarantor_stake_t::idx_t guarantyrwa::_get_stake_tbl(uint64_t plan_id) const {
    return guarantor_stake_t::idx_t(get_self(), plan_id);
}

// 2) 从 wasm_db 中取出 guaranty_stats_t（取不到就抛错）
guaranty_stats_t guarantyrwa::_get_stats_or_fail(uint64_t plan_id) {
    guaranty_stats_t stats(plan_id);
    CHECKC(_db.get(stats), err::RECORD_NOT_FOUND, "guaranty stats not found");
    return stats;
}

// 3) 从 multi_index 中取某个担保人记录（按值返回）
guarantor_stake_t guarantyrwa::_get_stake_or_fail(uint64_t plan_id,
                                                  const name& guarantor) {
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");
    return *it;
}

// 4) 以 plan.end_time 为起点，算已经过去了多少“完整自然年”
uint32_t guarantyrwa::_years_passed(const fundplan_t& plan) const {
    const time_point_sec now = time_point_sec(current_time_point());
    if (now <= plan.end_time) {
        return 0;
    }
    uint64_t elapsed_sec =
        now.sec_since_epoch() - plan.end_time.sec_since_epoch();
    uint32_t years = elapsed_sec / seconds_per_year;
    return years;
}

// 5) 累计某个 plan 的投资人收益 Σ investor_yield
int64_t guarantyrwa::_calc_investor_yield_sum(uint64_t plan_id,
                                              const symbol& sym) const {
    yield_log_t::idx_t logs(_gstate.yield_contract, plan_id);
    int64_t sum = 0;

    for (const auto& y : logs) {
        CHECKC(y.investor_yield.symbol == sym,
               err::SYMBOL_MISMATCH,
               "yield symbol mismatch");
        sum += y.investor_yield.amount;
    }
    return sum;
}

// 6) 统一计算 coverage 相关指标：G / investor_yield / H / unlocked_pool
guarantyrwa::CoverageInfo guarantyrwa::_calc_coverage(const fundplan_t& plan,
                                         const guaranty_stats_t& stats,
                                         const symbol& sym) const
{
    guarantyrwa::CoverageInfo info;
    int64_t T = plan.total_raised_funds.amount;
    int64_t H = T / 2;
    int64_t G = stats.total_guarantee_funds.amount;

    int64_t investor_sum = _calc_investor_yield_sum(plan.id, sym);
    int64_t coverage = G + investor_sum;
    int64_t unlocked_pool = 0;
    if (coverage > H) {
        unlocked_pool = coverage - H;
        if (unlocked_pool > G) unlocked_pool = G;
    }

    info.G              = G;
    info.investor_yield = investor_sum;
    info.H              = H;
    info.unlocked_pool  = unlocked_pool;
    return info;
}

void guarantyrwa::init(const name& admin) {
    require_auth(get_self());
    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin");
    _gstate.admin = admin;
    _global.set(_gstate, get_self());
}

// on_transfer: 担保本金 / 担保收益
// memo: "guaranty:<plan_id>" 或 "reward:<plan_id>"
void guarantyrwa::on_transfer(const name& from,
                              const name& to,
                              const asset& quantity,
                              const string& memo)
{
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid transfer amount");

    // 解析 memo: <type>:<plan_id>
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 2, err::INVALID_FORMAT, "memo must be <type>:<plan_id>");

    const string   action  = parts[0];
    const uint64_t plan_id = std::stoull(parts[1]);

    // 从 invest.rwa 读取计划
    fundplan_t::idx_t fundplans(_gstate.invest_contract, _gstate.invest_contract.value);
    auto itr = fundplans.find(plan_id);
    CHECKC(itr != fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    const fundplan_t plan = *itr;

    CHECKC(get_first_receiver() == plan.goal_asset_contract,    err::CONTRACT_MISMATCH, "token contract mismatch");
    CHECKC(quantity.symbol      == plan.goal_quantity.symbol,   err::SYMBOL_MISMATCH, "symbol mismatch");
    if (action == "guaranty") {
        _handle_guaranty_transfer(from, plan, quantity);
        return;
    }
    if (action == "reward") {
        _handle_reward_transfer(plan, quantity);
        return;
    }

    CHECKC(false, err::PARAM_ERROR, "unsupported transfer type");
}

// 担保本金充值（shares 模型）
void guarantyrwa::_handle_guaranty_transfer(const name&       from,
                                            const fundplan_t& plan,
                                            const asset&      quantity)
{
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid guaranty amount");

    const symbol           sym    = quantity.symbol;
    const uint64_t         plan_id = plan.id;
    const time_point_sec   now    = time_point_sec(current_time_point());

    // 1. 仅在 [start_time, end_time) 募资期间允许担保
    CHECKC(now >= plan.start_time,err::INVALID_STATUS,"guaranty not allowed: fundraising not started");
    CHECKC(now < plan.end_time,err::INVALID_STATUS,"guaranty not allowed: fundraising already ended");
    // 状态允许 PENDING、RAISEACTIVE、SUCCESS：
    // - PENDING: 募资刚开始
    // - RAISEACTIVE: 募资进行中
    // - SUCCESS: 已达 soft cap，但募资期尚未结束，仍可追加担保
    // end_time 之后无论状态如何都不允许担保
    CHECKC(plan.status == PlanStatus::PENDING || plan.status == PlanStatus::RAISEACTIVE ||plan.status == PlanStatus::SUCCESS,
                                err::INVALID_STATUS,"guaranty only allowed during fundraising stage");

    // 2. 更新担保池统计（wasm_db）
    guaranty_stats_t::idx_t stats_tbl(get_self(), get_self().value);
    auto sit = stats_tbl.find(plan_id);

    if (sit == stats_tbl.end()) {
            stats_tbl.emplace(get_self(), [&](auto& s) {
                s.plan_id               = plan_id;
                s.total_guarantee_funds = quantity;        // 当前池子总额
                s.used_guarantee_funds  = asset(0, sym);   // 尚未使用
                s.cumulative_yield      = asset(0, sym);   // 尚无分红
                s.created_at            = now;
                s.updated_at            = now;
            });
        } else {
            stats_tbl.modify(sit, same_payer, [&](auto& s) {
                CHECKC(s.total_guarantee_funds.symbol == sym,err::SYMBOL_MISMATCH,"guarantee stats symbol mismatch");
                s.total_guarantee_funds += quantity;
                s.updated_at             = now;
            });
        }

    // 3. 更新担保人份额（shares = 永久权重）
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(from.value);

    if (it == stakes.end()) {
        stakes.emplace(get_self(), [&](auto& s) {
            s.guarantor    = from;
            s.shares       = quantity;   // shares = 首次质押金额
            s.total_stake  = quantity;   // 当前总持仓（含分红滚入）
            s.earned_yield = asset(0, sym);
            s.withdrawn    = asset(0, sym);
            s.created_at   = now;
            s.updated_at   = now;
        });
    } else {
        stakes.modify(it, same_payer, [&](auto& s) {
            CHECKC(s.shares.symbol == sym &&
                   s.total_stake.symbol == sym,
                   err::SYMBOL_MISMATCH,
                   "stake symbol mismatch");

            s.shares.amount      += quantity.amount;
            s.total_stake.amount += quantity.amount;
            s.updated_at          = now;
        });
    }
}

// ========================================
// 担保收益分红（按 shares 比例分配，滚入 total_stake）
// ========================================

void guarantyrwa::_handle_reward_transfer(const fundplan_t& plan,
                                          const asset&      quantity)
{
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid reward amount");
    const symbol         sym     = quantity.symbol;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now     = time_point_sec(current_time_point());

    // 1. 仅在：SUCCESS 且 [end_time, return_end_time] 期间允许 reward
    CHECKC(plan.status == PlanStatus::SUCCESS|| plan.status == PlanStatus::COMPLETED,
           err::INVALID_STATUS,
           "reward not allowed: plan not SUCCESS");

    CHECKC(now >= plan.end_time ,
           err::INVALID_STATUS,
           "reward not allowed: out of reward window");

    // 2. 汇总 shares
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(), err::RECORD_NOT_FOUND, "no guarantors");

    __int128 total_shares = 0;
    for (const auto& s : stakes) {
        CHECKC(s.shares.symbol == sym, err::SYMBOL_MISMATCH, "symbol mismatch");
        CHECKC(s.shares.amount >= 0,   err::PARAM_ERROR,     "invalid shares amount");
        total_shares += s.shares.amount;
    }
    CHECKC(total_shares > 0, err::PARAM_ERROR, "total shares = 0");

    // 3. 按 shares 比例分红：分红直接进入 total_stake（滚仓），earned_yield 仅做记录
    int64_t    distributed = 0;
    auto       last        = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t share_amt;

        if (it == last) {
            share_amt = quantity.amount - distributed; // 兜底舍入
        } else {
            share_amt = (int64_t)
                (((__int128)quantity.amount * it->shares.amount) / total_shares);
            if (share_amt < 0) share_amt = 0;
        }

        if (share_amt <= 0) continue;
        distributed += share_amt;

        asset share(share_amt, sym);
        stakes.modify(it, same_payer, [&](auto& s) {
            s.earned_yield    += share;   // 统计给用户看的
            s.total_stake     += share;   // 分红计入本金池
            s.updated_at       = now;
        });
    }

    // 4. 更新担保池 stats（把 reward 也视为担保池补充 + 统计为“累计收益”）
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);
    CHECKC(stats.total_guarantee_funds.symbol == sym,
           err::SYMBOL_MISMATCH,
           "guarantee stats symbol mismatch");

    stats.total_guarantee_funds += quantity;  // 池子资金加上分红
    stats.cumulative_yield      += quantity;  // 记录总分红
    stats.updated_at             = now;
    _db.set(stats);
}

// ========================================
// 年度担保补足（guarantpay）
// 条件：
//   - now ∈ [end_time, return_end_time]
//   - plan.status == SUCCESS
//   - 按年度保证：投资人收益 + 历史担保补贴 >= yearly_due * 年数
// ========================================

void guarantyrwa::guarantpay(const name& submitter,const uint64_t& plan_id){
    require_auth(submitter);

    fundplan_t::idx_t fundplans( _gstate.invest_contract, _gstate.invest_contract.value);
    auto itr_plan = fundplans.find(plan_id);
    CHECKC(itr_plan != fundplans.end(),err::RECORD_NOT_FOUND,"plan not found");

    const fundplan_t& plan = *itr_plan;
    CHECKC(plan.goal_quantity.amount > 0,err::PARAM_ERROR,"invalid plan goal");
    CHECKC(plan.goal_quantity.symbol == plan.total_raised_funds.symbol,err::SYMBOL_MISMATCH,"plan symbol mismatch");

    // ------------------------------------------------------------
    // 2) 时间窗口硬约束（避免状态滞后）
    // ------------------------------------------------------------
    const time_point_sec now = time_point_sec(current_time_point());
    const bool in_yield_window =(now >= plan.end_time && now <= plan.return_end_time);
    const bool after_yield_end = (now > plan.return_end_time);

    CHECKC(in_yield_window || after_yield_end, err::INVALID_STATUS, "guarantpay not in valid time window");

    // ------------------------------------------------------------
    // 3) 状态软约束（只排除失败态）
    // ------------------------------------------------------------
    CHECKC(plan.status != PlanStatus::FAILED &&plan.status != PlanStatus::CANCELLED,err::INVALID_STATUS, "plan failed or cancelled");

    // ------------------------------------------------------------
    // 4) 项目总年数（至少 1 年）
    // ------------------------------------------------------------
    uint32_t total_years = std::max<uint32_t>(1, (plan.return_months + 11) / 12);

    // ------------------------------------------------------------
    // 5) 读取担保池统计
    // ------------------------------------------------------------
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);

    // ------------------------------------------------------------
    // 6) 已经过多少“完整年”（从 end_time 起算）
    // ------------------------------------------------------------
    uint64_t elapsed_sec =
        (now.sec_since_epoch() > plan.end_time.sec_since_epoch())
            ? (now.sec_since_epoch() - plan.end_time.sec_since_epoch())
            : 0;

    uint32_t years_passed =
        std::min<uint32_t>(elapsed_sec / seconds_per_year,
                           total_years);

    CHECKC(years_passed > 0, err::INVALID_STATUS, "no guarantee year completed yet");

    // ------------------------------------------------------------
    // 7) 每年最低保障额度
    // yearly_due = goal / years / 2
    // ------------------------------------------------------------
    asset yearly_due = _yearly_guarantee_principal(plan);
    CHECKC(yearly_due.amount > 0,err::PARAM_ERROR,"yearly principal is zero");

    CHECKC(yearly_due.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "yearly principal symbol mismatch");

    // 截止当前年度的保障目标线
    int64_t floor_target = (int64_t)((__int128)yearly_due.amount * years_passed);

    // ------------------------------------------------------------
    // 8) 累计投资人收益（只用 investor_yield）
    // ------------------------------------------------------------
    int64_t investor_cum = _calc_investor_yield_sum(plan.id, yearly_due.symbol);
    // 历史已由担保池补贴的金额
    int64_t already_paid = stats.used_guarantee_funds.amount;

    // ------------------------------------------------------------
    // 9) 计算缺口
    // ------------------------------------------------------------
    int64_t need = floor_target - (investor_cum + already_paid);
    if (need <= 0) {
        // 已满足保障线
        return;
    }

    // ------------------------------------------------------------
    // 10) 从担保池出钱（不超过池子余额）
    // ------------------------------------------------------------
    int64_t pool_avail = stats.total_guarantee_funds.amount;
    CHECKC(pool_avail > 0, err::QUANTITY_INSUFFICIENT, "guarantee pool empty");

    int64_t pay_amt = std::min(need, pool_avail);
    CHECKC(pay_amt > 0, err::QUANTITY_INSUFFICIENT, "no guarantee available to pay");

    asset pay(pay_amt, yearly_due.symbol);

    // ------------------------------------------------------------
    // 11) 在担保人之间按 total_stake 分摊成本
    // （内部会更新 stats.total / used）
    // ------------------------------------------------------------
    _deduct_from_guarantors(plan_id, pay);

    // ------------------------------------------------------------
    // 12) 转给 stake.rwa（补投资人收益池）
    // ------------------------------------------------------------
    TRANSFER( plan.goal_asset_contract, _gstate.stake_contract,  pay,"reward:" + std::to_string(plan_id));

    // ------------------------------------------------------------
    // 13) 记入 yield 日志（投资人视角）
    // ------------------------------------------------------------
    rwafi::yieldrwa::recordyield_action{
        _gstate.yield_contract,
        { { get_self(), "active"_n } }
    }.send(
        plan_id,
        pay
    );
}

// 担保资金赎回入口
void guarantyrwa::redeem(const name& guarantor, const uint64_t& plan_id, const asset& quantity){
    require_auth(guarantor);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid redeem amount");

    fundplan_t::idx_t fundplans(_gstate.invest_contract, _gstate.invest_contract.value);
    auto itr_plan = fundplans.find(plan_id);
    CHECKC(itr_plan != fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");

    fundplan_t plan = *itr_plan;
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);

    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND,"guarantor not found in this plan");
    CHECKC(it->total_stake.amount > 0, err::PARAM_ERROR,"guarantor has no active stake");

    const time_point_sec now = time_point_sec(current_time_point());
    const bool failed = (plan.status == PlanStatus::FAILED || plan.status == PlanStatus::CANCELLED);
    const bool ended  = (now >= plan.return_end_time);

    if (failed) {
        _redeem_failed_project(guarantor, plan, stats, quantity);
        return;
    }
    if (ended) {
        _redeem_project_end(guarantor, plan, stats, quantity);
        return;
    }

    const bool fundraising_ended = (now >= plan.end_time);
    const bool project_not_ended = (now < plan.return_end_time);

    if (plan.status == PlanStatus::SUCCESS && fundraising_ended &&  project_not_ended)
    {
        _redeem_in_progress(guarantor, plan, stats, quantity);
        return;
    }

    CHECKC(false, err::INVALID_STATUS,"plan not redeemable at current stage"  );
}

// (1) 失败 / 取消项目：全部解锁，收益优先扣除
void guarantyrwa::_redeem_failed_project(const name& guarantor,const fundplan_t& plan, guaranty_stats_t& stats, const asset& quantity){
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid redeem amount");
    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    // 担保人记录
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);

    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");
    CHECKC(it->total_stake.symbol == sym, err::SYMBOL_MISMATCH, "stake symbol mismatch");
    CHECKC(sym == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "plan symbol mismatch");

    int64_t stake_v    = it->total_stake.amount;
    int64_t yield_v    = it->earned_yield.amount;
    int64_t redeemable = stake_v + yield_v;

    CHECKC(redeemable > 0, err::QUANTITY_INSUFFICIENT,"guarantor has nothing to redeem");
    CHECKC(q <= redeemable, err::QUANTITY_INSUFFICIENT,"redeem exceeds user redeemable amount: redeemable=" +
                                                         asset(redeemable, sym).to_string() + ", required=" + quantity.to_string());
    CHECKC(stats.total_guarantee_funds.symbol == sym, err::SYMBOL_MISMATCH, "stats symbol mismatch");
    CHECKC(stats.total_guarantee_funds.amount >= q, err::QUANTITY_INSUFFICIENT, "guarantee pool insufficient");

    // 先扣 earned_yield，再扣 total_stake
    int64_t need = q;
    stakes.modify(it, same_payer, [&](auto& s) {
        int64_t use_yield = std::min<int64_t>(need, s.earned_yield.amount);
        s.earned_yield.amount -= use_yield;
        need                  -= use_yield;

        if (need > 0) {
            CHECKC(s.total_stake.amount >= need, err::QUANTITY_INSUFFICIENT, "internal error: total_stake insufficient");
            s.total_stake.amount -= need;
            need                  = 0;
        }
        CHECKC(need == 0, err::INVALID_STATUS, "internal error: redeem not fully covered");
        s.withdrawn.amount += q;
        s.updated_at        = now;
    });

    // 更新担保池
    stats.total_guarantee_funds.amount -= q;
    stats.used_guarantee_funds.amount  += q;
    stats.updated_at                    = now;
    _db.set(stats);

    // 转账
    TRANSFER(plan.goal_asset_contract, guarantor, quantity, "redeem (failed project)");
}

// ========================================
// (2) 项目进行中：coverage >= 50% 才允许赎回
// coverage = G + Σ(investor_yield)
// 安全线 = T / 2, T = total_raised_funds
// user_available = unlocked_pool * user_shares / total_shares
// ========================================

void guarantyrwa::_redeem_in_progress(const name& guarantor, const fundplan_t& plan,guaranty_stats_t& stats, const asset& quantity) {
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid redeem amount");

    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    // 担保人记录
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto uit = stakes.find(guarantor.value);
    CHECKC(uit != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");
    CHECKC(uit->total_stake.symbol == sym, err::SYMBOL_MISMATCH, "stake symbol mismatch");
    CHECKC(uit->total_stake.amount > 0, err::PARAM_ERROR, "guarantor total_stake is zero");

    // 担保池符号检查
    CHECKC(stats.total_guarantee_funds.symbol == sym, err::SYMBOL_MISMATCH, "guarantee stats symbol mismatch");
    CHECKC(stats.total_guarantee_funds.amount >= q, err::QUANTITY_INSUFFICIENT, "guarantee pool insufficient");

    // 汇总担保人总仓位（用于按比例分摊可赎回额度）
    __int128 total_stake_sum = 0;
    for (const auto& s : stakes) {
        CHECKC(s.total_stake.symbol == sym, err::SYMBOL_MISMATCH, "stake symbol mismatch");
        CHECKC(s.total_stake.amount >= 0,  err::PARAM_ERROR,     "invalid total_stake");
        total_stake_sum += s.total_stake.amount;
    }
    CHECKC(total_stake_sum > 0, err::PARAM_ERROR, "total_stake_sum is zero");

    const int64_t user_stake = uit->total_stake.amount;

    // ------------------------------------------------------------
    // 核心：安全线校验（用 investor_yield，不用 earned_yield）
    // 目标：赎回后仍满足  G_after + investor_yield >= H
    // ------------------------------------------------------------
    const int64_t T = plan.total_raised_funds.amount;
    CHECKC(T >= 0, err::PARAM_ERROR, "invalid total raised");

    const int64_t H = T / 2;  // 50% 安全线（整数向下取整）

    // 累计投资人收益（来自 yield.rwa 日志）
    const int64_t investor_yield = _calc_investor_yield_sum(plan_id, sym);

    // 赎回后的担保池余额
    const int64_t G_after = stats.total_guarantee_funds.amount - q;
    CHECKC(G_after >= 0, err::QUANTITY_INSUFFICIENT, "guarantee pool would go negative");

    // 赎回后是否仍满足安全线
    CHECKC(
        (__int128)G_after + (__int128)investor_yield >= (__int128)H,
        err::INVALID_STATUS,
        string("redeem not allowed: would break 50% safety line, ")
            + "G_after=" + asset(G_after, sym).to_string()
            + ", investor_yield=" + asset(investor_yield, sym).to_string()
            + ", H=" + asset(H, sym).to_string()
    );

    // ------------------------------------------------------------
    // 进一步：给单个担保人一个“按占比分摊”的可赎回上限
    // 可赎回总额度 = (G + investor_yield) - H
    // 用户可赎回额度 = 可赎回总额度 * user_stake / total_stake_sum
    // ------------------------------------------------------------
    const int64_t G_before = stats.total_guarantee_funds.amount;
    __int128 total_unlock128 = (__int128)G_before + (__int128)investor_yield - (__int128)H;
    int64_t total_unlock = (total_unlock128 > 0) ? (int64_t)total_unlock128 : 0;

    // 用户份额可赎回额度
    int64_t user_unlock = 0;
    if (total_unlock > 0) {
        user_unlock = (int64_t)(((__int128)total_unlock * (__int128)user_stake) / total_stake_sum);
        if (user_unlock < 0) user_unlock = 0;
    }

    CHECKC(
        user_unlock >= q,
        err::QUANTITY_INSUFFICIENT,
        string("not enough unlocked guarantee for this guarantor: ")
            + "user_unlock=" + asset(user_unlock, sym).to_string()
            + ", required=" + quantity.to_string()
    );

    // ------------------------------------------------------------
    // 扣减担保人仓位 + 扣减担保池余额
    // ------------------------------------------------------------
    stakes.modify(uit, same_payer, [&](auto& s) {
        s.total_stake.amount -= q;
        if (s.total_stake.amount < 0) s.total_stake.amount = 0;
        s.withdrawn.amount   += q;
        s.updated_at          = now;
    });

    stats.total_guarantee_funds.amount -= q;
    if (stats.total_guarantee_funds.amount < 0) stats.total_guarantee_funds.amount = 0;
    stats.updated_at = now;
    _db.set(stats);

    TRANSFER(plan.goal_asset_contract, guarantor, quantity, "redeem (in progress)");
}

// 项目到期：全部可赎回（收益优先）
void guarantyrwa::_redeem_project_end(const name& guarantor,const fundplan_t& plan, guaranty_stats_t& stats,const asset& quantity) {
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid redeem amount");

    const symbol         sym = quantity.symbol;
    const int64_t        q   = quantity.amount;
    const uint64_t       plan_id = plan.id;
    const time_point_sec now = time_point_sec(current_time_point());

    // ------------------------------------------------------------
    // 1) 担保人记录
    // ------------------------------------------------------------
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    auto it = stakes.find(guarantor.value);
    CHECKC(it != stakes.end(), err::RECORD_NOT_FOUND, "guarantor not found");

    CHECKC(it->total_stake.symbol == sym,err::SYMBOL_MISMATCH, "stake symbol mismatch");
    const int64_t user_stake = it->total_stake.amount;
    CHECKC(user_stake > 0, err::QUANTITY_INSUFFICIENT, "guarantor has no stake");

    CHECKC(stats.total_guarantee_funds.symbol == sym, err::SYMBOL_MISMATCH,"guarantee stats symbol mismatch");
    CHECKC(stats.total_guarantee_funds.amount >= q, err::QUANTITY_INSUFFICIENT, "guarantee pool insufficient");

    // ------------------------------------------------------------
    // 2) 50% 安全线校验（项目结束也不能破坏）
    // ------------------------------------------------------------
    const int64_t T = plan.total_raised_funds.amount;
    const int64_t H = T / 2;

    // 累计投资人收益（唯一权威口径）
    const int64_t investor_yield = _calc_investor_yield_sum(plan_id, sym);

    const int64_t G_after = stats.total_guarantee_funds.amount - q;

    CHECKC(
        (__int128)G_after + (__int128)investor_yield >= (__int128)H,
        err::INVALID_STATUS,
        string("redeem not allowed: would break 50% safety line, ")
            + "G_after=" + asset(G_after, sym).to_string()
            + ", investor_yield=" + asset(investor_yield, sym).to_string()
            + ", H=" + asset(H, sym).to_string()
    );

    // ------------------------------------------------------------
    // 3) 不能超过本人仓位
    // ------------------------------------------------------------
    CHECKC(q <= user_stake, err::QUANTITY_INSUFFICIENT, "redeem exceeds guarantor stake");

    // ------------------------------------------------------------
    // 4) 扣减担保人仓位 + 池子
    // ------------------------------------------------------------
    stakes.modify(it, same_payer, [&](auto& s) {
        s.total_stake.amount -= q;
        if (s.total_stake.amount < 0) s.total_stake.amount = 0;
        s.withdrawn.amount   += q;
        s.updated_at          = now;
    });

    stats.total_guarantee_funds.amount -= q;
    if (stats.total_guarantee_funds.amount < 0)
        stats.total_guarantee_funds.amount = 0;

    stats.updated_at = now;
    _db.set(stats);

    TRANSFER( plan.goal_asset_contract, guarantor, quantity, "redeem (project end)");
}

// 担保成本在担保人之间分摊（按 total_stake 比例）
void guarantyrwa::_deduct_from_guarantors(uint64_t plan_id, const asset& pay)
{
    CHECKC(pay.amount > 0, err::NOT_POSITIVE, "invalid pay amount");

    // 当前 stats
    guaranty_stats_t stats = _get_stats_or_fail(plan_id);
    CHECKC(stats.total_guarantee_funds.symbol == pay.symbol,err::SYMBOL_MISMATCH,"guarantee stats symbol mismatch");
    CHECKC(stats.total_guarantee_funds.amount >= pay.amount,err::QUANTITY_INSUFFICIENT,"guarantee pool insufficient");

    const time_point_sec now = time_point_sec(current_time_point());

    // 担保人列表
    guarantor_stake_t::idx_t stakes(get_self(), plan_id);
    CHECKC(stakes.begin() != stakes.end(),err::RECORD_NOT_FOUND,"no guarantors");

    // 汇总total_stake
    __int128 total_stake = 0;
    for (const auto& s : stakes) {
        CHECKC(s.total_stake.symbol == pay.symbol,err::SYMBOL_MISMATCH,"stake symbol mismatch");
        total_stake += s.total_stake.amount;
    }
    CHECKC(total_stake > 0, err::PARAM_ERROR, "total_stake is zero");

    // 按 total_stake 占比分摊扣减
    int64_t    total_deducted = 0;
    auto       last           = std::prev(stakes.end());

    for (auto it = stakes.begin(); it != stakes.end(); ++it) {
        int64_t deduct_amt = 0;

        if (it == last) {
            deduct_amt = pay.amount - total_deducted;  // 兜底
        } else {
            deduct_amt = (int64_t)
                (((__int128)pay.amount * it->total_stake.amount) / total_stake);
            if (deduct_amt < 0) deduct_amt = 0;
        }

        if (deduct_amt <= 0) continue;
        total_deducted += deduct_amt;

        stakes.modify(it, same_payer, [&](auto& s) {
            s.total_stake.amount -= deduct_amt;
            if (s.total_stake.amount < 0) s.total_stake.amount = 0;
            s.updated_at = now;
        });
    }

    // 更新担保池状态：扣减池子、增加 used_guarantee_funds
    stats.total_guarantee_funds.amount -= pay.amount;
    if (stats.total_guarantee_funds.amount < 0)
        stats.total_guarantee_funds.amount = 0;

    stats.used_guarantee_funds.amount  += pay.amount;
    stats.updated_at                    = now;
    _db.set(stats);
}