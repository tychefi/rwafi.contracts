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

void investrwa::_process_investment(const name& from, const name&, const asset& quantity,const string& memo,fundplan_t& plan) {
    const time_point_sec now = time_point_sec(current_time_point());

    // === Step 1: 基础校验 ===
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "investment must be positive");
    CHECKC(now >= plan.start_time, err::INVALID_STATUS, "fundraising not started");
    CHECKC(now <= plan.end_time, err::INVALID_STATUS, "fundraising period ended");

    const name token_contract = get_first_receiver();
    CHECKC(token_contract == plan.goal_asset_contract, err::CONTRACT_MISMATCH, "token contract mismatch");
    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "symbol mismatch");

    // === Step 2: 检查计划状态是否允许投资 ===
    bool can_invest =
        plan.status == PlanStatus::PENDING ||
        plan.status == PlanStatus::RAISEACTIVE ||
        plan.status == PlanStatus::SUCCESS;  // 募资成功仍可补充投资（未封顶）

    CHECKC(can_invest, err::INVALID_STATUS,
        "plan not open for investment (status: " + plan.status.to_string() + ")");

    // === Step 3: 检查币种是否在白名单中 ===
    allow_token_t::idx_t tokens(_self, _self.value);
    auto token_itr = tokens.find(quantity.symbol.raw());
    CHECKC(token_itr != tokens.end() && token_itr->token_contract == token_contract,
           err::TOKEN_NOT_ALLOWED,
           "token not allowed: " + quantity.symbol.code().to_string());

    // === Step 4: 计算可接受金额与硬顶 ===
    const int64_t hard_cap = plan.goal_quantity.amount * plan.hard_cap_percent / 100;
    const int64_t remaining = hard_cap - plan.total_raised_funds.amount;
    CHECKC(remaining > 0, err::INVALID_STATUS, "hard cap reached");

    asset accepted = quantity;
    asset refund(0, quantity.symbol);
    if (quantity.amount > remaining) {
        accepted.amount = remaining;
        refund.amount = quantity.amount - remaining;
    }

    // === Step 5: 校验回执参数 ===
    CHECKC(plan.receipt_quantity_per_unit.amount > 0, err::INVALID_FORMAT, "invalid receipt ratio");
    CHECKC(plan.receipt_quantity_per_unit.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "receipt symbol mismatch");

    // === Step 6: 精度安全计算 ===
    auto _pow10 = [](uint8_t p) -> int64_t {
        int64_t v = 1;
        while (p-- > 0) v *= 10;
        return v;
    };

    __int128 raw = (__int128)accepted.amount * plan.receipt_quantity_per_unit.amount;
    int64_t issue_amount = (int64_t)(raw / _pow10(plan.goal_quantity.symbol.precision()));

    CHECKC(issue_amount > 0, err::INVALID_FORMAT, "issued receipt amount too small");
    asset issued_receipt(issue_amount, plan.receipt_symbol);

    // === Step 7: 更新募资统计 ===
    plan.total_raised_funds    += accepted;
    plan.total_issued_receipts += issued_receipt;

    // === Step 8: 发放回执并转入 stake 池 ===
    ISSUE(plan.receipt_asset_contract, get_self(), issued_receipt, "plan:" + std::to_string(plan.id));
    TRANSFER(plan.receipt_asset_contract, _gstate.stake_contract, issued_receipt,
             "stake:" + std::to_string(plan.id) + ":" + from.to_string());

    // === Step 9: 处理超额退款 ===
    if (refund.amount > 0) {
        TRANSFER(plan.goal_asset_contract, from, refund,
                 "refund: exceed hard cap " + std::to_string(plan.id));
    }

    // === Step 10: 更新状态 ===
    _db.set(plan, _self);
    _update_plan_status(plan);
}

void investrwa::_process_refund(const name& from,const name&,const asset& quantity,const string& memo,fundplan_t& plan) {
    // ===  基础检查 ===
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "refund must be positive");
    CHECKC(plan.status == PlanStatus::CANCELLED || plan.status == PlanStatus::FAILED,
           err::INVALID_STATUS, "refund not allowed in current plan status");
    CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "receipt symbol mismatch");

    // ===  memo 解析: refund:<plan_id>:<investor> ===
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 3 && parts[0] == "refund",
           err::INVALID_FORMAT, "invalid memo format, expect refund:<plan_id>:<investor>");

    uint64_t memo_plan_id = std::stoull(parts[1]);
    CHECKC(memo_plan_id == plan.id, err::PARAM_ERROR, "plan_id mismatch in memo");

    name investor = name(parts[2]);
    CHECKC(is_account(investor), err::ACCOUNT_INVALID, "invalid investor account");

    // ===  参数有效性校验 ===
    CHECKC(plan.receipt_quantity_per_unit.amount > 0, err::INVALID_FORMAT, "invalid receipt ratio");
    CHECKC(plan.goal_quantity.amount > 0, err::INVALID_FORMAT, "invalid goal quantity");

    // ===  精度换算 ===
    auto pow10 = [](int n)->int64_t { int64_t v = 1; while (n-- > 0) v *= 10; return v; };

    const int goal_precision   = plan.goal_quantity.symbol.precision();
    const int receipt_precision = plan.receipt_symbol.precision();

    __int128 q_rcp_min    = (__int128)quantity.amount;
    __int128 r_per_1g_min = (__int128)plan.receipt_quantity_per_unit.amount;
    int64_t pow10G = pow10(goal_precision);

    // refund_min = 用户RCP × 10^goal_precision / 每1SING对应RCP
    __int128 refund_min128 = (q_rcp_min * (__int128)pow10G) / r_per_1g_min;
    CHECKC(refund_min128 > 0 && refund_min128 <= std::numeric_limits<int64_t>::max(),
           err::PARAM_ERROR, "refund overflow or invalid ratio");

    asset refund_amount((int64_t)refund_min128, plan.goal_quantity.symbol);

    // ===  资金充足性验证 ===
    CHECKC(plan.total_raised_funds.amount >= refund_amount.amount,
           err::QUANTITY_INSUFFICIENT, "insufficient raised funds");
    CHECKC(plan.total_issued_receipts.amount >= quantity.amount,
           err::QUANTITY_INSUFFICIENT, "insufficient issued receipts");

    // ===  销毁 receipt（investrwa 已收回的收据） ===
    BURN(plan.receipt_asset_contract, quantity,
         "burn receipt for refund, plan:" + std::to_string(plan.id));

    // ===  返还本金 ===
    TRANSFER(plan.goal_asset_contract, investor, refund_amount,
             "refund principal for plan:" + std::to_string(plan.id));

    // ===  更新统计 ===
    plan.total_raised_funds.amount =
        std::max<int64_t>(0, plan.total_raised_funds.amount - refund_amount.amount);
    plan.total_issued_receipts.amount =
        std::max<int64_t>(0, plan.total_issued_receipts.amount - quantity.amount);

    // ===  若所有退款完成 ===
    if (plan.total_raised_funds.amount == 0 && plan.total_issued_receipts.amount == 0) {
        plan.status = PlanStatus::REFUNDED;
    }

    _db.set(plan, _self);
}

void investrwa::_update_plan_status(fundplan_t& plan) {
    const time_point_sec now = time_point_sec(current_time_point());
    const int64_t raised    = plan.total_raised_funds.amount;
    const int64_t soft_cap  = plan.goal_quantity.amount * plan.soft_cap_percent / 100;
    const int64_t hard_cap  = plan.goal_quantity.amount * plan.hard_cap_percent / 100;

    // === Step 1: 待开始 → 募资中 ===
    if (plan.status == PlanStatus::PENDING && now >= plan.start_time) {
        plan.status = PlanStatus::RAISEACTIVE;
    }

    // === Step 2: 募资中 ===
    if (plan.status == PlanStatus::RAISEACTIVE) {

        // === 募资期内 ===
        if (now <= plan.end_time) {
            if (raised >= hard_cap) {
                plan.status = PlanStatus::SUCCESS;          // 达到硬顶立即成功
            }
            else if (raised >= soft_cap) {
                plan.status = PlanStatus::SUCCESS;          // 达到软顶提前成功
            }
        }

        // === 募资期结束 ===
        else {
            if (raised >= soft_cap) {
                plan.status = PlanStatus::SUCCESS;          // 达标但自然到期
            } else {
                plan.status = PlanStatus::FAILED;           // 未达软顶 → 失败
            }
        }
    }

    // === Step 3: 募资成功（进入收益期前） ===
    if (plan.status == PlanStatus::SUCCESS) {
        // 到达收益结束时间后，标记为 COMPLETED
        if (now >= plan.return_end_time) {
            plan.status = PlanStatus::COMPLETED;
        }
    }

    // === Step 4: 已完成（收益期结束） ===
    if (plan.status == PlanStatus::COMPLETED) {
        return;
    }

    // === Step 5: 失败、取消、退款状态 ===
    if (plan.status == PlanStatus::FAILED ||
        plan.status == PlanStatus::CANCELLED ||
        plan.status == PlanStatus::REFUNDED) {
        return;
    }

    // === Step 6: 持久化状态 ===
    _db.set(plan, get_self());
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

// 支持两种格式：
// ① memo: plan:<plan_id>
// ② memo: refund:<plan_id>:<investor>
void investrwa::on_transfer(const name& from,const name& to,const asset& quantity,const string& memo) {
    if (from == _self || to != _self) return;

    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive");
    CHECKC(!memo.empty(), err::INVALID_FORMAT, "memo required");

    const name bank = get_first_receiver();
    auto parts = split(memo, ":");
    CHECKC(parts.size() >= 2, err::INVALID_FORMAT, "invalid memo format");

    const string action = parts[0];
    const uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND,"no such fund plan id: " + std::to_string(plan_id));

    // === 投资逻辑 ===
    if (action == "plan") {
        // --- 校验白名单 ---
        allow_token_t::idx_t allow_tokens(_self, _self.value);
        auto token_itr = allow_tokens.find(quantity.symbol.raw());
        CHECKC(token_itr != allow_tokens.end(), err::INVALID_SYMBOL,
               "token not registered: " + quantity.symbol.code().to_string());
        CHECKC(token_itr->token_contract == bank, err::CONTRACT_MISMATCH,
               "invalid token contract, expected " + token_itr->token_contract.to_string() +
               ", got " + bank.to_string());

        // --- 校验计划匹配 ---
        CHECKC(bank == plan.goal_asset_contract, err::CONTRACT_MISMATCH,
               "invalid investment token contract for plan");
        CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH,
               "symbol mismatch, expected " + plan.goal_quantity.symbol.code().to_string() +
               ", got " + quantity.symbol.code().to_string());

        // --- 执行投资 ---
        _process_investment(from, to, quantity, memo, plan);
        return;
    }

    // === 退款逻辑 ===
    if (action == "refund") {
        CHECKC(parts.size() == 3, err::INVALID_FORMAT,
               "expect memo format: refund:<id>:<user>");
        const name investor = name(parts[2]);

        // refund 来源检查：必须是 receipt token 合约
        CHECKC(bank == plan.receipt_asset_contract, err::CONTRACT_MISMATCH,
               "refund must come from receipt contract: " +
               bank.to_string() + " ≠ " + plan.receipt_asset_contract.to_string());
        CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH,
               "symbol mismatch for refund token");

        // --- 计划状态检查 ---
        CHECKC(plan.status == PlanStatus::CANCELLED ||
               plan.status == PlanStatus::FAILED,
               err::INVALID_STATUS,
               "refund not allowed (plan status: " + plan.status.to_string() + ")");

        // --- 执行退款 ---
        _process_refund(from, to, quantity, memo, plan);
        return;
    }

    // === 其他无效 memo ===
    CHECKC(false, err::INVALID_FORMAT, "unsupported memo action: " + action);
}

void investrwa::createplan(
                        const name& creator,
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
                        const uint32_t& guaranteed_yield_apr  ) {
    require_auth(creator);

    // ===  基础参数校验 ===
    CHECKC(!title.empty() && title.size() <= MAX_TITLE_SIZE, err::INVALID_FORMAT, "invalid title");
    CHECKC(goal_quantity.amount > 0 && receipt_quantity_per_unit.amount > 0,
           err::NOT_POSITIVE, "invalid asset quantities");
    CHECKC(soft_cap_percent >= 60 && soft_cap_percent <= 100,
           err::INVALID_FORMAT, "soft cap percent invalid");
    CHECKC(hard_cap_percent >= soft_cap_percent,
           err::INVALID_FORMAT, "hard cap percent invalid");
    CHECKC(end_time > start_time, err::INVALID_FORMAT, "end time must follow start");
    CHECKC(return_months > 0, err::INVALID_FORMAT, "invalid return months");
    CHECKC(guaranteed_yield_apr > 0, err::INVALID_FORMAT, "yield apr must be positive");
    CHECKC(receipt_asset_contract == RECEIPT_TOKEN_BANK,
           err::CONTRACT_MISMATCH, "receipt must be issued by rwafi.token");

    // ===  Receipt Token 校验 ===
    const string sym_code = receipt_quantity_per_unit.symbol.code().to_string();
    CHECKC(sym_code.size() <= 8, err::INVALID_SYMBOL, "symbol code too long (max 8 chars)");
    CHECKC(sym_code.rfind("ST", 0) == 0, err::INVALID_SYMBOL,
           "receipt symbol must start with 'ST' (e.g. STUSD)");

    // ===  生成 plan_id 并实例化 ===
    auto plan_id = ++_gstate.last_plan_id;
    fundplan_t plan(plan_id);

    // ===  确保回执代币不存在 ===
    flon::token::stats statstable(RECEIPT_TOKEN_BANK, receipt_quantity_per_unit.symbol.code().raw());
    auto existing = statstable.find(receipt_quantity_per_unit.symbol.code().raw());
    CHECKC(existing == statstable.end(), err::PARAM_ERROR,
           "receipt token already exists: " + sym_code);

    // ===  计算最大供应量（根据硬顶） ===
    int64_t goal_unit = 1;
    for (int i = 0; i < goal_quantity.symbol.precision(); ++i)
        goal_unit *= 10;

    __int128 G_hard = (__int128)goal_quantity.amount * hard_cap_percent / 100; // 募资硬顶
    __int128 R = (__int128)receipt_quantity_per_unit.amount;                   // 每单位目标资产对应 receipt 单位
    __int128 max_amt128 = (G_hard * R) / goal_unit;
    int64_t max_amt = (int64_t)max_amt128;

    CHECKC(max_amt > 0, err::INVALID_FORMAT, "computed max supply invalid");

    // ===  创建 Receipt Token ===
    CREATE(RECEIPT_TOKEN_BANK, _self, asset(max_amt, receipt_quantity_per_unit.symbol));

    // ===  填充计划数据 ===
    plan.title                     = title;
    plan.creator                   = creator;
    plan.goal_asset_contract       = goal_asset_contract;
    plan.goal_quantity             = goal_quantity;
    plan.receipt_asset_contract    = RECEIPT_TOKEN_BANK;
    plan.receipt_symbol            = receipt_quantity_per_unit.symbol;
    plan.receipt_quantity_per_unit = receipt_quantity_per_unit;
    plan.soft_cap_percent          = soft_cap_percent;
    plan.hard_cap_percent          = hard_cap_percent;
    plan.start_time                = time_point_sec(start_time.sec_since_epoch());
    plan.end_time                  = time_point_sec(end_time.sec_since_epoch());
    plan.return_months             = return_months;
    plan.return_end_time           = time_point_sec(start_time.sec_since_epoch() + return_months * seconds_per_month);
    plan.guaranteed_yield_apr      = guaranteed_yield_apr;
    plan.total_raised_funds        = asset(0, goal_quantity.symbol);
    plan.total_issued_receipts     = asset(0, receipt_quantity_per_unit.symbol);
    plan.status                    = PlanStatus::PENDING;
    plan.created_at                = time_point(current_time_point());

    // ===  通知 stake 合约同步创建计划 ===
    rwafi::stakerwa::addplan_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id, receipt_quantity_per_unit.symbol);

    // ===  写入数据库 ===
    _db.set(plan, _self);
}

void investrwa::cancelplan(const name& creator, const uint64_t& plan_id) {
    require_auth(creator);

    // === 读取计划 ===
    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND,
           "no such fund plan id: " + std::to_string(plan_id));
    CHECKC(plan.creator == creator, err::NO_AUTH,
           "no auth to cancel this plan");

    // === 校验状态合法性 ===
    CHECKC(
        plan.status == PlanStatus::PENDING ||
        plan.status == PlanStatus::RAISEACTIVE ||
        plan.status == PlanStatus::SOFTCAPHIT ||
        plan.status == PlanStatus::HARDCAPHIT,
        err::INVALID_STATUS,
        "cannot cancel in current status: " + plan.status.to_string()
    );

    // === 更新状态为 CANCELLED ===
    plan.status = PlanStatus::CANCELLED;
    _db.set(plan, _self);

    // === 触发 stake 合约执行批量退款 ===
    rwafi::stakerwa::batchunstake_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id);

}
