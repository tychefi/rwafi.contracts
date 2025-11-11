#include "investrwa.hpp"
#include "stake.rwa/stakerwadb.hpp"
#include "stake.rwa/stakerwa.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include "guaranty.rwa/guarantyrwadb.hpp"
#include "flon/flon.token.hpp"

using std::chrono::system_clock;
using namespace wasm;
using namespace eosio;
using namespace rwafi;
using namespace flon;
static constexpr name RECEIPT_TOKEN_BANK{"rwafi.token"_n};
static constexpr eosio::name active_perm  {"active"_n};

// ------------------- Internal functions ------------------------------------------------------
asset investrwa::_get_balance(const name& token_contract, const name& owner, const symbol& sym) {
    eosio::multi_index<"accounts"_n, flon::token::account> account_tbl(token_contract, owner.value);
    auto itr = account_tbl.find(sym.code().raw());
    return itr == account_tbl.end() ? asset(0, sym) : itr->balance;
}

asset investrwa::_get_investor_stake_balance(const name& investor, const uint64_t& plan_id) {
    staker_t::tbl_t stakers(_gstate.stake_contract, plan_id);  // scope 是 plan_id
    auto itr = stakers.find(investor.value);
    CHECKC(itr != stakers.end(), err::RECORD_NOT_FOUND, "no stake record for plan: " + std::to_string(plan_id));
    return itr->avl_staked;   // ✅ 返回投资者可用质押余额
}

// asset investrwa::_get_collateral_stake_balance( const name& guanrantor, const uint64_t& plan_id ) {
//     auto stakes =  collateral_stake_t::stakes(_gstate.stake_contract, guanrantor.value);
//     CHECKC( _db.get( stakes ), err::RECORD_NOT_FOUND, "no collateral balance for plan: " + to_string( plan_id ))
//     return stakes.balance;
// }

void investrwa::_process_investment(
    const name& from,
    const name&,
    const asset& quantity,
    const string& memo,
    fundplan_t& plan
) {
    auto now = time_point_sec(current_time_point());

    // === 基础校验 ===
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "investment must be positive");
    CHECKC(now >= plan.start_time && now <= plan.end_time, err::INVALID_STATUS, "not in fundraising period");
    CHECKC(plan.status != PlanStatus::FAILED && plan.status != PlanStatus::CANCELLED,
           err::INVALID_STATUS, "plan unavailable");

    name token_contract = get_first_receiver();
    CHECKC(token_contract == plan.goal_asset_contract, err::CONTRACT_MISMATCH, "token contract mismatch");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    if (plan.status == PlanStatus::PENDING && now >= plan.start_time)
        plan.status = PlanStatus::ACTIVE;

    // === 计算硬顶 ===
    int64_t hard_cap = plan.goal_quantity.amount * plan.hard_cap_percent / 100;
    int64_t remaining = hard_cap - plan.total_raised_funds.amount;
    CHECKC(remaining > 0, err::INVALID_STATUS, "hard cap reached");

    // === 处理超额 ===
    asset accepted = quantity;
    asset refund(0, quantity.symbol);
    if (quantity.amount > remaining) {
        accepted.amount = remaining;
        refund.amount = quantity.amount - remaining;
    }

    // === 确认 receipt 参数有效性 ===
    CHECKC(plan.receipt_quantity_per_unit.amount > 0, err::INVALID_FORMAT,
           "receipt_quantity_per_unit must be positive");
    CHECKC(plan.receipt_quantity_per_unit.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH,
           "receipt_quantity_per_unit symbol mismatch");

    // === 精度换算函数 ===
    auto _pow10 = [](uint8_t p) -> int64_t {
        int64_t v = 1;
        for (uint8_t i = 0; i < p; ++i) v *= 10;
        return v;
    };

    const int64_t goal_precision    = plan.goal_quantity.symbol.precision();     // e.g. SING 精度 8
    const int64_t receipt_precision = plan.receipt_symbol.precision();           // e.g. RCP 精度 4

    // === 精度安全计算 ===
    // 每 1.00000000 SING 对应 plan.receipt_quantity_per_unit.amount 个最小单位 RCP
    __int128 numerator = (__int128)accepted.amount * (__int128)plan.receipt_quantity_per_unit.amount;
    __int128 denom     = (__int128)_pow10(goal_precision);
    int64_t issue_amount = (int64_t)(numerator / denom);

    asset issued_receipt(issue_amount, plan.receipt_symbol);

    CHECKC(issued_receipt.amount > 0, err::INVALID_FORMAT, "issued receipt amount is zero");

    // === 更新计划数据 ===
    plan.total_raised_funds    += accepted;
    plan.total_issued_receipts += issued_receipt;

    // === 发放投资凭证 ===
    ISSUE(plan.receipt_asset_contract, get_self(), issued_receipt,
          "plan:" + std::to_string(plan.id));

    TRANSFER(
        plan.receipt_asset_contract,
        _gstate.stake_contract,
        issued_receipt,
        std::string("stake:") + std::to_string(plan.id) + ":" + from.to_string()
    );

    // === 若超额则退款 ===
    if (refund.amount > 0) {
        TRANSFER(plan.goal_asset_contract, from, refund,
                 "refund: exceed hard cap " + std::to_string(plan.id));
    }

    // === 保存与状态更新 ===
    _db.set(plan, _self);
    _update_plan_status(plan);

#ifdef DEBUG_LOG
    eosio::print_f("[INVEST] from=% , plan=% , accepted=% , issued=% , refund=%\n",
                   from, plan.id, accepted, issued_receipt, refund);
#endif
}

void investrwa::_process_refund(const name& from, const name&, const asset& quantity, const string& memo, fundplan_t& plan) {
    // === 1️⃣ 基础校验 ===
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "refund must be positive");
    CHECKC(plan.status == PlanStatus::CANCELLED ,  err::INVALID_STATUS, "refund not allowed");
    CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    // === 2️⃣ 解析 memo 格式: refund:<plan_id>:<investor> ===
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 3 && parts[0] == "refund", err::INVALID_FORMAT, "invalid memo format, expect refund:<plan_id>:<investor>");

    uint64_t memo_plan_id = std::stoull(parts[1]);
    CHECKC(memo_plan_id == plan.id, err::PARAM_ERROR, "memo plan_id mismatch");
    name investor = name(parts[2]);

    // === 3️⃣ 校验比例参数有效 ===
    CHECKC(plan.receipt_quantity_per_unit.amount > 0, err::INVALID_FORMAT, "invalid receipt ratio");
    CHECKC(plan.goal_quantity.amount > 0, err::INVALID_FORMAT, "invalid goal quantity");

    // === 计算 refund_amount（按精度严格换算） ===
    const int G = plan.goal_quantity.symbol.precision();                 // 例如 SING 8 位
    const int R = plan.receipt_quantity_per_unit.symbol.precision();     // 例如 RCP 4 位

    // quantity.amount = 用户提交的 RCP 最小单位
    __int128 q_rcp_min      = (__int128)quantity.amount;
    // plan.receipt_quantity_per_unit.amount = 每 1.00000000 SING 对应的 RCP 最小单位数量
    __int128 r_per_1g_min   = (__int128)plan.receipt_quantity_per_unit.amount;

    // pow10(G)：把 “1 个整 SING” 转换为 SING 的最小单位
    auto pow10 = [](int n)->int64_t { int64_t v = 1; while (n-- > 0) v *= 10; return v; };
    int64_t pow10G = pow10(G);

    // 公式：goal_min = receipt_min * 10^G / (RCP_per_1_goal_min)
    __int128 refund_min128 = (q_rcp_min * (__int128)pow10G) / r_per_1g_min;

    // 防溢出 & 非负校验
    CHECKC(refund_min128 >= 0 && refund_min128 <= std::numeric_limits<int64_t>::max(),
        err::PARAM_ERROR, "refund overflow");

    // 得到最终退款资产
    asset refund_amount((int64_t)refund_min128, plan.goal_quantity.symbol);

    // === 5️⃣ 校验资金充足 ===
    CHECKC(plan.total_raised_funds.amount >= refund_amount.amount,
           err::QUANTITY_INSUFFICIENT,
           "insufficient raised funds for refund");
    CHECKC(plan.total_issued_receipts.amount >= quantity.amount,
           err::QUANTITY_INSUFFICIENT,
           "insufficient issued receipts for refund");

    // === 6️⃣ 销毁 receipt（从 investrwa 合约账户中） ===
    // stake.rwa 已将 receipt 退回 investrwa112，这里销毁
    BURN(plan.receipt_asset_contract, quantity,
         "burn receipt after cancel plan: " + std::to_string(plan.id));

    // === 7️⃣ 返还本金给投资人 ===
    TRANSFER(plan.goal_asset_contract, investor, refund_amount,
             "refund principal for cancelled plan: " + std::to_string(plan.id));

    // === 8️⃣ 更新资金与收据统计 ===
    plan.total_raised_funds -= refund_amount;
    plan.total_issued_receipts -= quantity;

    // === 9️⃣ 若清零则标记为 REFUNDED（退款完毕） ===
    if (plan.total_raised_funds.amount == 0 && plan.total_issued_receipts.amount == 0)
        plan.status = PlanStatus::REFUNDED;

    _db.set(plan, _self);
}

void investrwa::_update_plan_status(fundplan_t& plan) {
    auto now = time_point_sec(current_time_point());

    int64_t goal_amount     = plan.goal_quantity.amount;
    int64_t hard_cap_amount = goal_amount * plan.hard_cap_percent / 100;
    int64_t soft_cap_amount = goal_amount * plan.soft_cap_percent / 100;
    int64_t raised_amount   = plan.total_raised_funds.amount;

    // ========== 计划未开始 ==========
    if (plan.status == PlanStatus::PENDING) {
        if (now < plan.start_time) return;  // 尚未到期
        if (now >= plan.start_time && now <= plan.end_time) {
            plan.status = (raised_amount >= hard_cap_amount)
                        ? PlanStatus::CLOSED
                        : PlanStatus::ACTIVE;
        } else {
            // 超过结束时间：判断是否达软顶
            plan.status = (raised_amount >= soft_cap_amount)
                        ? PlanStatus::PENDING_PLEDGE
                        : PlanStatus::FAILED;
        }
    }

    // ========== 募资进行中 ==========
    else if (plan.status == PlanStatus::ACTIVE) {
        if (now <= plan.end_time) {
            if (raised_amount >= hard_cap_amount)
                plan.status = PlanStatus::CLOSED;   // 提前满额关闭
        } else {
            // 募资结束：判断是否达软顶
            plan.status = (raised_amount >= soft_cap_amount)
                        ? PlanStatus::PENDING_PLEDGE
                        : PlanStatus::FAILED;
        }
    }

    // ========== 募资已满额 ==========
    else if (plan.status == PlanStatus::CLOSED) {
        if (now > plan.end_time)
            plan.status = PlanStatus::PENDING_PLEDGE; // 等待担保
    }

    // ========== 担保验证阶段 ==========
    else if (plan.status == PlanStatus::PENDING_PLEDGE) {
        if (plan.guaranteed_yield_apr <= 0) {
            plan.status = PlanStatus::FAILED;
        } else {
            guaranty_stats_t::idx_t guar_tbl(_gstate.guaranty_contract, _gstate.guaranty_contract.value);
            auto guar_itr = guar_tbl.find(plan.id);

            if (guar_itr == guar_tbl.end()) {
                plan.status = PlanStatus::FAILED;
            } else {
                // ======== 计算剩余担保期（月） ========
                uint16_t total_months   = plan.return_months;
                uint16_t elapsed_months = (now.sec_since_epoch() - plan.start_time.sec_since_epoch()) / seconds_per_month;
                uint16_t remaining_months = total_months > elapsed_months ? (total_months - elapsed_months) : 1;

                // APR 是年化利率，需要按月折算
                // 所以 / 12 是换算成月化收益率
                int64_t required_amount = goal_amount *
                                        plan.guaranteed_yield_apr *
                                        remaining_months / 10000 / 12 / 2;

                asset required_guarantee(required_amount, plan.goal_quantity.symbol);

                plan.status = (guar_itr->available_guarantee_funds.amount >= required_guarantee.amount)
                            ? PlanStatus::SUCCESS
                            : PlanStatus::FAILED;
            }
        }
    }

    // ========== 担保成功 → 收益期中 ==========
    else if (plan.status == PlanStatus::SUCCESS) {
        if (now >= plan.return_end_time)
            plan.status = PlanStatus::COMPLETED;
    }

    // ========== 已完成 / 已失败：保持不动 ==========
    else if (plan.status == PlanStatus::FAILED || plan.status == PlanStatus::COMPLETED) {
        return;
    }
    else if (plan.status == PlanStatus::CANCELLED) {
        return; // 已取消计划不再自动状态转换
    }

    _db.set(plan, _self);
}

void investrwa::addtoken(const name& contract, const symbol& sym ) {
    CHECKC( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token          = allow_token_t( sym );
    CHECKC( !_db.get( token ), err::RECORD_NOT_FOUND, "Token symbol already existing" )

    token.token_symbol  = sym;
    token.token_contract= contract;

    _db.set(token, _self);
}

void investrwa::deltoken( const symbol& sym ) {
    CHECKC( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = allow_token_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NOT_FOUND, "no such token symbol" )
    _db.del( token );
}

void investrwa::onshelf( const symbol& sym, const bool& onshelf ) {
    CHECKC( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = allow_token_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NOT_FOUND, "no such token symbol" )
    token.onshelf = onshelf;
    _db.set( token, _self );
}

// memo: plan:xxx
// memo: refund:<id>:<user>
void investrwa::on_transfer(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == _self || to != _self) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive");

    auto bank = get_first_receiver();
    auto parts = split(memo, ":");

    CHECKC(parts.size() >= 2, err::INVALID_FORMAT, "invalid memo format");

    string action = parts[0];
    uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "no such fund plan id: " + std::to_string(plan_id));

    // === 投资逻辑 ===
    if (action == "plan") {
        // === 检查是否在允许的 token 白名单中 ===
        allow_token_t allow_token(quantity.symbol);
        CHECKC(_db.get(allow_token), err::INVALID_SYMBOL,
            "token not registered: " + quantity.symbol.code().to_string());

        CHECKC(allow_token.token_contract == bank, err::CONTRACT_MISMATCH,
            "contract mismatch: expected " + allow_token.token_contract.to_string() +
            ", got " + bank.to_string());

        // === 校验币种与计划 ===
        CHECKC(quantity.symbol == plan.goal_quantity.symbol,
            err::SYMBOL_MISMATCH,
            "symbol mismatch: expected " + plan.goal_quantity.symbol.code().to_string() +
            ", got " + quantity.symbol.code().to_string());

        // === 执行投资 ===
        _process_investment(from, to, quantity, memo, plan);
        return;
    }

    // === 退款逻辑 ===
    else if (action == "refund") {
        CHECKC(parts.size() == 3, err::INVALID_FORMAT, "expect memo format: refund:<id>:<user>");
        name investor = name(parts[2]);

        CHECKC(bank == plan.receipt_asset_contract, err::CONTRACT_MISMATCH, "invalid refund contract");
        CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "symbol mismatch for refund");

        // ⚙️ refund 由 stake.rwa 调用，来源是 receipt token
        _process_refund(from, to, quantity, memo, plan);
        return;
    }

    // === 未识别 ===
    CHECKC(false, err::INVALID_FORMAT, "unsupported memo action: " + action);
}

void investrwa::createplan(const name& creator,
                           const string& title,
                           const name& goal_asset_contract,
                           const asset& goal_quantity,
                           const name& receipt_asset_contract,
                           const asset& receipt_quantity_per_unit,
                           const uint8_t& soft_cap_percent,
                           const uint8_t& hard_cap_percent,
                           const time_point& start_time,
                           const time_point& end_time,
                           const uint16_t& return_months,
                           const uint32_t& guaranteed_yield_apr) {
    require_auth(creator);

    CHECKC(!title.empty() && title.size() <= MAX_TITLE_SIZE, err::INVALID_FORMAT, "title invalid");
    CHECKC(goal_quantity.amount > 0 && receipt_quantity_per_unit.amount > 0, err::NOT_POSITIVE, "invalid quantities");
    CHECKC(soft_cap_percent >= 60 && soft_cap_percent <= 100, err::INVALID_FORMAT, "soft cap invalid");
    CHECKC(hard_cap_percent >= soft_cap_percent, err::INVALID_FORMAT, "hard cap invalid");
    CHECKC(end_time > start_time, err::INVALID_FORMAT, "end time must follow start");
    CHECKC(return_months > 0, err::INVALID_FORMAT, "return months invalid");
    CHECKC(guaranteed_yield_apr > 0, err::INVALID_FORMAT, "yield apr invalid");
    CHECKC(receipt_asset_contract == RECEIPT_TOKEN_BANK, err::CONTRACT_MISMATCH, "receipt must be rwafi.token");

    // ✅ 校验符号前缀 & 长度
    std::string sym_code = receipt_quantity_per_unit.symbol.code().to_string();
    CHECKC(sym_code.size() <= 8, err::INVALID_SYMBOL, "symbol code too long, max 8 chars");
    CHECKC(sym_code.rfind("ST", 0) == 0, err::INVALID_SYMBOL, "receipt token symbol must start with 'ST' (e.g. STUSD)");

    // ✅ 继续原逻辑
    auto plan_id = ++_gstate.last_plan_id;
    fundplan_t plan(plan_id);

    flon::token::stats statstable(RECEIPT_TOKEN_BANK, receipt_quantity_per_unit.symbol.code().raw());
    auto existing = statstable.find(receipt_quantity_per_unit.symbol.code().raw());
    CHECKC(existing == statstable.end(), err::PARAM_ERROR,
           "receipt token already exists: " + sym_code);

    int64_t goal_unit = 1;
    for (int i = 0; i < goal_quantity.symbol.precision(); ++i)
        goal_unit *= 10;

    __int128 G_hard = (__int128)goal_quantity.amount * hard_cap_percent / 100;
    __int128 R = (__int128)receipt_quantity_per_unit.amount;
    __int128 max_amt128 = (G_hard * R) / goal_unit;
    int64_t max_amt = (int64_t)max_amt128;

    CHECKC(max_amt > 0, err::INVALID_FORMAT, "computed receipt max supply must be positive");

    CREATE(RECEIPT_TOKEN_BANK, _self, asset(max_amt, receipt_quantity_per_unit.symbol));

    plan.title                  = title;
    plan.creator                = creator;
    plan.goal_asset_contract    = goal_asset_contract;
    plan.goal_quantity          = goal_quantity;
    plan.receipt_asset_contract = RECEIPT_TOKEN_BANK;
    plan.receipt_symbol         = receipt_quantity_per_unit.symbol;
    plan.receipt_quantity_per_unit = receipt_quantity_per_unit;
    plan.soft_cap_percent       = soft_cap_percent;
    plan.hard_cap_percent       = hard_cap_percent;
    plan.start_time             = time_point_sec(start_time.sec_since_epoch());
    plan.end_time               = time_point_sec(end_time.sec_since_epoch());
    plan.return_months          = return_months;
    plan.return_end_time        = time_point_sec(start_time.sec_since_epoch() + return_months * seconds_per_month);
    plan.guaranteed_yield_apr   = guaranteed_yield_apr;
    plan.total_raised_funds     = asset(0, goal_quantity.symbol);
    plan.total_issued_receipts  = asset(0, receipt_quantity_per_unit.symbol);
    plan.status                 = PlanStatus::PENDING;
    plan.created_at             = time_point(current_time_point());

    rwafi::stakerwa::addplan_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id, receipt_quantity_per_unit.symbol);

    _db.set(plan, _self);
}

void investrwa::cancelplan(const name& creator, const uint64_t& plan_id) {
    require_auth(creator);

    // === 读取计划 ===
    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "no such fund plan id");
    CHECKC(plan.creator == creator, err::NO_AUTH, "no auth to cancel this plan");

    // === 校验状态 ===
    CHECKC(plan.status == PlanStatus::PENDING ||
           plan.status == PlanStatus::ACTIVE ||
           plan.status == PlanStatus::CLOSED,
           err::INVALID_STATUS, "cannot cancel in current status");

    // === 更新状态为 CANCELLED（退款中） ===
    plan.status = PlanStatus::CANCELLED;
    _db.set(plan, _self);

    // === 触发 stake 合约执行批量退款 ===
    // 这一步会遍历 stake.rwa::stakers 并将 receipt 退回 investrwa
    rwafi::stakerwa::batchunstake_action{
        _gstate.stake_contract,                            // stake 合约账户
        { permission_level{ get_self(), "active"_n } }      // investrwa 授权
    }.send(plan_id);                                        // 参数：plan_id
}