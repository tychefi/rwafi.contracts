#include "investrwa.hpp"
#include "stakerwadb.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include "guarantyrwadb.hpp"

using std::chrono::system_clock;
using namespace wasm;
using namespace eosio;
using namespace rwafi;
static constexpr name RECEIPT_TOKEN_BANK{"rwafi.token"_n};
static constexpr name active_permission{"active"_n};

#define CREATE_RECEIPT(maximum_supply) \
    action( { get_self(), "active"_n }, RECEIPT_TOKEN_BANK, "create"_n, \
            std::make_tuple(get_self(), maximum_supply)).send();

#define TRANSFER_OUT(bank, to, quantity, memo) \
    action({get_self(), "active"_n}, bank, "transfer"_n, \
            std::make_tuple(get_self(), to, quantity, memo)).send();

#define ISSUE_RECEIPT(bank, to, quantity, memo) \
    action({get_self(), "active"_n}, bank, "issue"_n, \
            std::make_tuple(to, quantity, memo)).send();

#define BURN_RECEIPT(bank, from, quantity, memo) \
    action({get_self(), "active"_n}, bank, "burn"_n, \
            std::make_tuple(from, quantity, memo)).send();

#define REFUND_BURN_RECEIPT(bank, from, quantity, memo) \
    action({get_self(), "active"_n}, bank, "refundburn"_n, \
            std::make_tuple(from, quantity, memo)).send();

// ------------------- Internal functions ------------------------------------------------------
asset investrwa::_get_balance(const name& token_contract, const name& owner, const symbol& sym) {
    eosio::multi_index<"accounts"_n, flon::token::account> account_tbl(token_contract, owner.value);
    auto itr = account_tbl.find(sym.code().raw());
    return itr == account_tbl.end() ? asset(0, sym) : itr->balance;
}

asset investrwa::_get_investor_stake_balance(const name& investor, const uint64_t& plan_id) {
    invest_stake_t stake(plan_id);
    CHECKC(_db_stake.get(investor.value, stake), err::RECORD_NOT_FOUND, "no yield stake balance for plan: " + to_string(plan_id))
    return stake.balance;
}

// asset investrwa::_get_collateral_stake_balance( const name& guanrantor, const uint64_t& plan_id ) {
//     auto stakes =  collateral_stake_t::stakes(_gstate.stake_contract, guanrantor.value);
//     CHECKC( _db.get( stakes ), err::RECORD_NOT_FOUND, "no collateral balance for plan: " + to_string( plan_id ))
//     return stakes.balance;
// }

void investrwa::_process_investment(const name& from,const name&,const asset& quantity,const string&,fundplan_t& plan) {
    auto now = time_point_sec(current_time_point());
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "investment must be positive");
    CHECKC(now >= plan.start_time && now <= plan.end_time, err::INVALID_STATUS, "not in fundraising period");
    CHECKC(plan.status != PlanStatus::FAILED && plan.status != PlanStatus::CANCELLED, err::INVALID_STATUS, "plan unavailable");

    name token_contract = get_first_receiver();
    CHECKC(token_contract == plan.goal_asset_contract, err::CONTRACT_MISMATCH, "token contract mismatch");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    if (plan.status == PlanStatus::PENDING && now >= plan.start_time)
        plan.status = PlanStatus::ACTIVE;

    int64_t hard_cap = plan.goal_quantity.amount * plan.hard_cap_percent / 100;
    int64_t remaining = hard_cap - plan.total_raised_funds.amount;
    CHECKC(remaining > 0, err::INVALID_STATUS, "hard cap reached");

    asset accepted = quantity;
    asset refund(0, quantity.symbol);
    if (quantity.amount > remaining) {
        accepted.amount = remaining;
        refund.amount = quantity.amount - remaining;
    }

    plan.total_raised_funds    += accepted;
    plan.total_issued_receipts += asset(accepted.amount, plan.receipt_symbol);

    ISSUE_RECEIPT(plan.receipt_asset_contract, from, asset(accepted.amount, plan.receipt_symbol),
                  "plan:" + to_string(plan.id));

    if (refund.amount > 0)
        TRANSFER_OUT(plan.goal_asset_contract, from, refund, "refund: exceed hard cap " + to_string(plan.id));

    _db.set(plan, _self);
    _update_plan_status(plan);

}

void investrwa::_process_refund(const name& from,const name&,const asset& quantity,const string&,fundplan_t& plan) {
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "refund must be positive");
    CHECKC(plan.status == PlanStatus::FAILED || plan.status == PlanStatus::CANCELLED, err::INVALID_STATUS, "refund not allowed");
    CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    CHECKC(plan.total_issued_receipts.amount >= quantity.amount &&
           plan.total_raised_funds.amount >= quantity.amount, err::QUANTITY_INSUFFICIENT, "plan funds insufficient");

    asset refund_amount(quantity.amount, plan.goal_quantity.symbol);
    BURN_RECEIPT(plan.receipt_asset_contract, from, quantity, "burn receipt for refund: " + to_string(plan.id));
    TRANSFER_OUT(plan.goal_asset_contract, from, refund_amount, "refund for plan: " + to_string(plan.id));

    plan.total_raised_funds -= refund_amount;
    plan.total_issued_receipts -= quantity;
    if (plan.total_raised_funds.amount <= 0) plan.total_raised_funds.amount = 0;
    if (plan.total_issued_receipts.amount <= 0) plan.total_issued_receipts.amount = 0;

    if (plan.total_issued_receipts.amount == 0 && plan.total_raised_funds.amount == 0)
        plan.status = PlanStatus::FAILED;

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
void investrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == _self || to != _self) return;

    CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )
    auto parts = split(memo, ":");
    CHECKC( parts.size() == 2, err::INVALID_FORMAT, "invalid memo format" );
    auto plan_id = (uint64_t) stoi(string(parts[1]));
    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NOT_FOUND, "no such fund plan id: " + to_string( plan_id ) )

    if ( quantity.symbol == plan.goal_quantity.symbol ) {
        _process_investment( from, to, quantity, memo, plan );
        return;

    } else if ( quantity.symbol == plan.receipt_symbol ) {
        _process_refund( from, to, quantity, memo, plan );
        return;

    } else {
        CHECKC( false, err::SYMBOL_MISMATCH, "symbol mismatch, expected: " + plan.goal_quantity.symbol.code().to_string() + " or " + plan.receipt_symbol.code().to_string() + ", actual: " + quantity.symbol.code().to_string() )
    }
}

void investrwa::refund(const name& submitter, const name& investor, const uint64_t& plan_id) {
    require_auth(submitter);

    auto plan = fundplan_t(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "no such fund plan id: " + to_string(plan_id))
    CHECKC(plan.status == PlanStatus::CANCELLED, err::INVALID_STATUS, "refunds only allowed when plan is cancelled")

    auto stake_balance = _get_investor_stake_balance(investor, plan_id);
    CHECKC(stake_balance.amount > 0, err::NOT_POSITIVE, "no stake found for investor");

    auto fund_balance_total = _get_balance(plan.goal_asset_contract, _self, plan.goal_quantity.symbol);
    CHECKC(fund_balance_total.amount >= stake_balance.amount, err::QUANTITY_INSUFFICIENT,
        "contract fund balance not enough for refund");

    // 调用 stakerwa 合约的 unstake 动作，返还 receipt token 给投资人
    {
        action(
            permission_level{get_self(), "active"_n},
            _gstate.stake_contract,
            "unstake"_n,
            std::make_tuple(investor, plan_id, stake_balance)
        ).send();
    }

    // ========== 退还本金 ==========
    {
        TRANSFER_OUT(
            plan.goal_asset_contract,
            investor,
            asset(stake_balance.amount, plan.goal_quantity.symbol),
            string("refund for plan:") + to_string(plan.id)
        )
    }

    // ========== 销毁凭证（Receipt Token）==========
    {
        REFUND_BURN_RECEIPT(
            plan.receipt_asset_contract,
            _gstate.stake_contract,
            stake_balance,
            string("refund:") + to_string(plan.id) + ":" + investor.to_string()
        )
    }

    plan.total_raised_funds   -= asset(stake_balance.amount, plan.goal_quantity.symbol);
    plan.total_issued_receipts -= stake_balance;
    _db.set(plan, _self);
}

void investrwa::createplan(const name& creator,
                           const string& title,
                           const name& goal_asset_contract,
                           const asset& goal_quantity,
                           const name& receipt_asset_contract,
                           const asset& receipt_quantity_per_unit,
                           const uint8_t& invest_unit_size,
                           const uint8_t& soft_cap_percent,
                           const uint8_t& hard_cap_percent,
                           const time_point& start_time,
                           const time_point& end_time,
                           const uint16_t& return_months,
                           const uint32_t& guaranteed_yield_apr) {
    require_auth(creator);

    CHECKC(!title.empty() && title.size() <= MAX_TITLE_SIZE, err::INVALID_FORMAT, "title invalid");
    CHECKC(goal_quantity.amount > 0 && receipt_quantity_per_unit.amount > 0, err::NOT_POSITIVE, "invalid quantities");
    CHECKC(soft_cap_percent > 0 && soft_cap_percent <= 100, err::INVALID_FORMAT, "soft cap invalid");
    CHECKC(hard_cap_percent >= soft_cap_percent, err::INVALID_FORMAT, "hard cap invalid");
    CHECKC(end_time > start_time, err::INVALID_FORMAT, "end time must follow start");
    CHECKC(return_months > 0, err::INVALID_FORMAT, "return years invalid");
    CHECKC(guaranteed_yield_apr > 0, err::INVALID_FORMAT, "yield apr invalid");
    CHECKC(receipt_asset_contract == RECEIPT_TOKEN_BANK, err::CONTRACT_MISMATCH, "receipt must be rwafi.token");

    fundplan_t plan(++_gstate.last_plan_id);

    flon::token::stats statstable(RECEIPT_TOKEN_BANK, receipt_quantity_per_unit.symbol.code().raw());
    if (statstable.find(receipt_quantity_per_unit.symbol.code().raw()) == statstable.end())
        CREATE_RECEIPT(asset(goal_quantity.amount, receipt_quantity_per_unit.symbol));

    plan.title                = title;
    plan.creator              = creator;
    plan.goal_asset_contract  = goal_asset_contract;
    plan.goal_quantity        = goal_quantity;
    plan.receipt_asset_contract = RECEIPT_TOKEN_BANK;
    plan.receipt_symbol       = receipt_quantity_per_unit.symbol;
    plan.soft_cap_percent     = soft_cap_percent;
    plan.hard_cap_percent     = hard_cap_percent;
    plan.start_time           = time_point_sec(start_time.sec_since_epoch());
    plan.end_time             = time_point_sec(end_time.sec_since_epoch());
    plan.return_months         = return_months;
    plan.return_end_time      = time_point_sec(start_time.sec_since_epoch() + return_months * seconds_per_month);
    plan.guaranteed_yield_apr = guaranteed_yield_apr;
    plan.total_raised_funds   = asset(0, goal_quantity.symbol);
    plan.total_issued_receipts= asset(0, receipt_quantity_per_unit.symbol);
    plan.status               = PlanStatus::PENDING;
    plan.created_at           = time_point(current_time_point());

    _db.set(plan, _self);
}

void investrwa::cancelplan(const name& creator, const uint64_t& plan_id) {
    require_auth(creator);

    auto plan = fundplan_t(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "no such fund plan id: " + to_string(plan_id));
    CHECKC(plan.creator == creator, err::NO_AUTH, "no auth to cancel this plan");

    _update_plan_status(plan);

    CHECKC(
        plan.status == PlanStatus::PENDING ||
        plan.status == PlanStatus::ACTIVE ||
        plan.status == PlanStatus::CLOSED,
        err::INVALID_STATUS,
        "cannot cancel in current status: " + plan.status.to_string()
    );

    plan.status = PlanStatus::CANCELLED;
    _db.set(plan, _self);
}
