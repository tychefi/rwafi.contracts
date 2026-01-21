#include "investrwa.hpp"
#include "stake.rwa/stakerwadb.hpp"
#include "stake.rwa/stakerwa.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include "guaranty.rwa/guarantyrwadb.hpp"
#include "flon/flon.token.hpp"
#include "flon.swap/flon.swap.db.hpp"
#include "flon.swap/flon.swap.hpp"
#include "flon/utils.hpp"


#define SWAPCREATE(contract, user, sym1, contract1, sym2, contract2, liq_sym) \
{ \
    flonswap::swapcreate_action act{ contract, { {user, active_perm} } }; \
    act.send(user, sym1, contract1, sym2, contract2, liq_sym); \
}

using std::chrono::system_clock;
using namespace wasm;
using namespace eosio;
using namespace rwafi;
using namespace flon;
static constexpr name RECEIPT_TOKEN_BANK{"rwafi.token"_n};
static constexpr eosio::name active_perm  {"active"_n};

int64_t investrwa::pow10(uint8_t p) {
    int64_t v = 1;
    while (p--) v *= 10;
    return v;
}

std::string   _to_lower_str(const symbol_code &sym_code) {
    auto str = sym_code.to_string();
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

name calc_swap_tpcode(const symbol& s1, const symbol& s2) {

    auto c1 = _to_lower_str(s1.code());
    auto c2 = _to_lower_str(s2.code());

    bool is1_whitelisted = pool_token_whitelist.count(c1);
    bool is2_whitelisted = pool_token_whitelist.count(c2);

    symbol left;
    symbol right;

    if (is2_whitelisted) {
        left = s1; right = s2;
    } else if (is1_whitelisted) {
        left = s2; right = s1;
    } else {
        if (c1 < c2) {
            left = s2; right = s1;
        } else {
            left = s1; right = s2;
        }
    }

    return pool_symbol(left, right);
}

// ------------------- Internal functions ------------------------------------------------------
asset investrwa::_get_balance(const name& token_contract, const name& owner, const symbol& sym){
    eosio::multi_index<"accounts"_n, flon::token::account> acnts(token_contract, owner.value);
    auto it = acnts.find(sym.code().raw());
    return it == acnts.end() ? asset(0, sym) : it->balance;
}

asset investrwa::_get_investor_stake_balance(const name& investor, const uint64_t& plan_id) {
    staker_t::tbl_t stakers(_gstate.stake_contract, plan_id);
    auto itr = stakers.find(investor.value);
    CHECKC(itr != stakers.end(), err::RECORD_NOT_FOUND, "no stake record for plan: " + std::to_string(plan_id));
    return itr->avl_staked;
}

void investrwa::_process_investment(const name& from, const asset& quantity, fundplan_t& plan) {


    const time_point_sec now = time_point_sec(current_time_point());

    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "investment must be positive");
    CHECKC(now >= plan.start_time, err::INVALID_STATUS, "fundraising not started");
    CHECKC(now <= plan.end_time,   err::INVALID_STATUS, "fundraising ended");

    _update_plan_status(plan);
    // plan 状态门控
    CHECKC(plan.status == PlanStatus::PENDING ||plan.status == PlanStatus::RAISEACTIVE || plan.status == PlanStatus::SUCCESS,
                                                                            err::INVALID_STATUS,"plan not open for investment");
    // 硬顶控制
    const uint128_t hard_cap128  = plan.goal_quantity.amount * plan.hard_cap_percent / 100;
    CHECKC(hard_cap128 <= std::numeric_limits<int64_t>::max(),err::INVALID_FORMAT, "hard cap overflow");
    const uint64_t hard_cap = (uint64_t)hard_cap128;
    const int64_t remaining = hard_cap - plan.total_raised_funds.amount;
    CHECKC(remaining > 0, err::INVALID_STATUS, "hard cap reached");
    CHECKC(plan.min_investment.amount > 0, err::PARAM_ERROR, "plan min investment invalid");
    if (quantity.amount < plan.min_investment.amount) {
        CHECKC(remaining < plan.min_investment.amount && quantity.amount >= remaining,
               err::PARAM_ERROR, "investment below minimum");
    }

    asset accepted = quantity;
    asset refund{0, quantity.symbol};

    if (quantity.amount > remaining) {
        accepted.amount = remaining;
        refund.amount   = quantity.amount - remaining;
    }

    // === receipt 计算（整数安全）===
    const int64_t goal_unit =  pow10(plan.goal_quantity.symbol.precision());
    uint128_t issue_raw =(uint128_t)accepted.amount *(uint128_t)plan.receipt_quantity_per_unit.amount;
    CHECKC(issue_raw % goal_unit == 0, err::INVALID_FORMAT, "receipt precision mismatch");

    uint128_t issue_amt128 = issue_raw / goal_unit;
    CHECKC(issue_amt128 > 0 &&issue_amt128 <= std::numeric_limits<int64_t>::max(),err::INVALID_FORMAT,"invalid receipt amount");
    asset issued_receipt{(int64_t)issue_amt128,plan.receipt_symbol};

    // === 发放 receipt 并立刻转入 stake ===
    ISSUE(plan.receipt_asset_contract,get_self(), issued_receipt, "plan:" + std::to_string(plan.id));
    TRANSFER(plan.receipt_asset_contract,_gstate.stake_contract,issued_receipt, "stake:" + std::to_string(plan.id) + ":" + from.to_string());

    // === 更新统计 ===
    plan.total_raised_funds    += accepted;
    plan.total_issued_receipts += issued_receipt;

    if (refund.amount > 0) {
        TRANSFER(plan.goal_asset_contract,from,refund,"refund:hardcap:" + std::to_string(plan.id));
    }

    _update_plan_status(plan);
    _db.set(plan, _self);
}

asset investrwa::_calc_refund_amount( const asset& receipt_qty,const fundplan_t& plan) {
    CHECKC(receipt_qty.amount > 0, err::NOT_POSITIVE, "receipt must be positive");
    const int64_t goal_unit = pow10(plan.goal_quantity.symbol.precision());

    int128_t numerator = (int128_t)receipt_qty.amount * goal_unit;
    int128_t denom     = (int128_t)plan.receipt_quantity_per_unit.amount;

    CHECKC(denom > 0, err::PARAM_ERROR, "invalid receipt ratio");
    CHECKC(numerator % denom == 0, err::PARAM_ERROR, "refund precision mismatch");

    int128_t refund128 = numerator / denom;
    CHECKC(refund128 > 0 && refund128 <= std::numeric_limits<int64_t>::max(),err::PARAM_ERROR, "refund overflow");

    return asset((int64_t)refund128, plan.goal_quantity.symbol);
}

void investrwa::_process_refund( const name& from, const asset& quantity,const string& memo, fundplan_t& plan) {
    // === 1. 基础校验 ===
    _update_plan_status(plan);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "refund must be positive");
    CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "receipt symbol mismatch");

    CHECKC( plan.status == PlanStatus::CANCELLED || plan.status == PlanStatus::FAILED,
                                    err::INVALID_STATUS, "refund not allowed in current plan status");

    // === 2. memo 解析 ===
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 3 && parts[0] == "refund", err::INVALID_FORMAT, "expect refund:<plan_id>:<investor>");

    uint64_t memo_plan_id = std::stoull(parts[1]);
    CHECKC(memo_plan_id == plan.id, err::PARAM_ERROR, "plan_id mismatch");

    name investor(parts[2]);
    CHECKC(is_account(investor), err::ACCOUNT_INVALID, "invalid investor");

    // === 3. 计算退款金额（核心逻辑） ===
    asset refund_amount = _calc_refund_amount(quantity, plan);

    // === 4. 资金安全校验 ===
    CHECKC(plan.total_raised_funds.amount >= refund_amount.amount, err::QUANTITY_INSUFFICIENT, "insufficient raised funds");
    CHECKC(plan.total_issued_receipts.amount >= quantity.amount, err::QUANTITY_INSUFFICIENT, "insufficient issued receipts");

    // === 5. 执行退款 ===
    BURN( plan.receipt_asset_contract, quantity, "burn receipt for refund, plan:" + std::to_string(plan.id));
    TRANSFER( plan.goal_asset_contract, investor,refund_amount,"refund principal for plan:" + std::to_string(plan.id));

    // === 6. 状态更新 ===
    plan.total_raised_funds.amount = std::max<int64_t>(0, plan.total_raised_funds.amount - refund_amount.amount);
    plan.total_issued_receipts.amount = std::max<int64_t>(0, plan.total_issued_receipts.amount - quantity.amount);

    if (plan.total_raised_funds.amount == 0 && plan.total_issued_receipts.amount == 0) {
        plan.status = PlanStatus::REFUNDED;
    }

    _db.set(plan, _self);
}

void investrwa::_update_plan_status(fundplan_t& plan) {
    const time_point_sec now    = time_point_sec(current_time_point());
    const int64_t raised        = plan.total_raised_funds.amount;
    const int64_t soft_cap      = plan.goal_quantity.amount * plan.soft_cap_percent / 100;

    auto old_status = plan.status;
    // === 1. PENDING：募资尚未开始 or 还未进入募资逻辑 ===
    if (plan.status == PlanStatus::PENDING) {

        // 1.1 未到开始时间 → 保持 PENDING
        if (now < plan.start_time) {
            // do nothing
        }

        // 1.2 募资期内
        else if (now <= plan.end_time) {
            if (raised >= soft_cap)
                plan.status = PlanStatus::SUCCESS;
            else
                plan.status = PlanStatus::RAISEACTIVE;
        }

        // 1.3 募资已结束
        else { // now > end_time
            plan.status = (raised >= soft_cap)
                ? PlanStatus::SUCCESS
                : PlanStatus::FAILED;
        }
    }

    // === 2. RAISEACTIVE：募资中 ===
    else if (plan.status == PlanStatus::RAISEACTIVE) {

        // 2.1 募资期内
        if (now <= plan.end_time) {
            if (raised >= soft_cap)
                plan.status = PlanStatus::SUCCESS;
        }

        // 2.2 募资期结束
        else {
            plan.status = (raised >= soft_cap) ? PlanStatus::SUCCESS : PlanStatus::FAILED;
        }
    }

    // === 3. SUCCESS：募资成功，等待收益期结束 ===
    else if (plan.status == PlanStatus::SUCCESS) {
        if (now >= plan.return_end_time)
            plan.status = PlanStatus::COMPLETED;
    }

    // === 4. 终态：保持原样 ===
    // COMPLETED、FAILED、CANCELLED、REFUNDED 是终态，不继续推进

    // === 写入数据库 ===
    if (plan.status != old_status) {
        _db.set(plan, get_self());
    }
}

asset investrwa::_calc_receipt_liq_from_goal(const asset& goal_liq, const fundplan_t& plan) {
    CHECKC(goal_liq.amount > 0, err::NOT_POSITIVE, "goal liquidity must be positive");

    const int64_t goal_unit = pow10(plan.goal_quantity.symbol.precision());
    uint128_t issue_raw = (uint128_t)goal_liq.amount * (uint128_t)plan.receipt_quantity_per_unit.amount;
    CHECKC(issue_raw % goal_unit == 0, err::INVALID_FORMAT, "receipt liquidity precision mismatch");

    uint128_t issue_amt128 = issue_raw / goal_unit;
    CHECKC(issue_amt128 > 0 && issue_amt128 <= std::numeric_limits<int64_t>::max(),
           err::INVALID_FORMAT, "receipt liquidity amount invalid");

    return asset((int64_t)issue_amt128, plan.receipt_symbol);
}

asset investrwa::_convert_precision(const asset& src, const symbol& dst_sym) {
    int8_t from_p = src.symbol.precision();
    int8_t to_p = dst_sym.precision();

    int128_t amount = src.amount;
    if (to_p > from_p) {
        int64_t factor = pow10((uint8_t)(to_p - from_p));
        amount = amount * factor;
    } else if (to_p < from_p) {
        int64_t factor = pow10((uint8_t)(from_p - to_p));
        amount = amount / factor;
    }

    CHECKC(amount > 0 && amount <= std::numeric_limits<int64_t>::max(), err::INVALID_FORMAT, "precision conversion overflow");

    return asset((int64_t)amount, dst_sym);
}

void investrwa::_create_liquidity(const fundplan_t& plan) {

    // 0. 状态 & 幂等校验
    const time_point_sec now = time_point_sec(current_time_point());
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "liquidity only after fundraising end");
    CHECKC(plan.status == PlanStatus::SUCCESS || plan.status == PlanStatus::COMPLETED,
           err::INVALID_STATUS, "liquidity only for successful plans");

    auto tpcode = flon::flonswap::pool_symbol(plan.receipt_symbol ,SING_SYM);

    market_t::idx_t markets(SWAP_POOL, SWAP_POOL.value);
    auto itr = markets.find(tpcode.value);
    CHECKC(itr == markets.end(), err::PARAM_ERROR, "market already exists");

    // 2. 查 / 建 swap market（不要依赖 tpcode）
    bool exists = flon::flonswap::is_exists_pool( extended_symbol(SING_SYM, SING_BANK), extended_symbol(plan.receipt_symbol, plan.receipt_asset_contract)   );

    if (!exists) {
        string lp_symbol_str = add_symbol(SING_SYM, plan.receipt_symbol, 1);
        SWAPCREATE(SWAP_POOL,_self,SING_SYM, SING_BANK,plan.receipt_symbol, plan.receipt_asset_contract,symbol_code(lp_symbol_str) );
    }

    investrwa::liquidity_action mine{ get_self(), { get_self(), "active"_n } };
    mine.send(plan.id,tpcode);
}

// 3) 真正注入流动性（独立 action 调用）
void investrwa::liquidity(const uint64_t& plan_id,const name& tpcode ) {
    require_auth(_self);

    // 0. 状态 & 幂等校验
    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    const time_point_sec now = time_point_sec(current_time_point());
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "liquidity only after fundraising end");
    CHECKC(plan.status == PlanStatus::SUCCESS || plan.status == PlanStatus::COMPLETED,
           err::INVALID_STATUS, "liquidity only for successful plans");

    CHECKC(plan.total_raised_funds.amount > 0, err::NOT_POSITIVE, "no raised funds");

    // 1. 计算注入数量（你确认这段没问题）
    int64_t goal_liq_amount = plan.total_raised_funds.amount / 100; // 1%
    CHECKC(goal_liq_amount > 0, err::PARAM_ERROR, "liquidity amount too small");

    asset goal_liq(goal_liq_amount, plan.total_raised_funds.symbol);
    asset receipt_liq = _calc_receipt_liq_from_goal(goal_liq, plan);
    asset sing_liq    = _convert_precision(receipt_liq, SING_SYM);

    // 2. 读 market（必须存在）
    market_t::idx_t markets(SWAP_POOL, SWAP_POOL.value);
    auto itr = markets.find(tpcode.value);
    CHECKC(itr != markets.end(), err::RECORD_NOT_FOUND, "market not found");
    const auto& market = *itr;

    // 3. 校验 market 的 token 构成（contract + symbol）
    bool left_is_sing = (market.left_pool_quant.contract == SING_BANK &&
                        market.left_pool_quant.quantity.symbol == SING_SYM);
    bool right_is_sing = (market.right_pool_quant.contract == SING_BANK &&
                         market.right_pool_quant.quantity.symbol == SING_SYM);

    bool left_is_receipt = (market.left_pool_quant.contract == plan.receipt_asset_contract &&
                           market.left_pool_quant.quantity.symbol == plan.receipt_symbol);
    bool right_is_receipt = (market.right_pool_quant.contract == plan.receipt_asset_contract &&
                            market.right_pool_quant.quantity.symbol == plan.receipt_symbol);

    CHECKC( (left_is_sing && right_is_receipt) || (right_is_sing && left_is_receipt),err::SYSTEM_ERROR, "market token mismatch");

    // 4. mint 注入（按 market 的左右顺序转）,先补发行 receipt_liq
    ISSUE(plan.receipt_asset_contract, get_self(), receipt_liq, "liquidity:plan:" + std::to_string(plan.id));

    const uint64_t nonce = plan.id; // plan.id 最稳
    const string memo1 = "mint:" + market.tpcode.to_string() + ":1:" + std::to_string(nonce) + ":" + _self.to_string();
    const string memo2 = "mint:" + market.tpcode.to_string() + ":2:" + std::to_string(nonce) + ":" + _self.to_string();

    if (left_is_sing) {
        TRANSFER(plan.goal_asset_contract,     SWAP_POOL, sing_liq,    memo1);
        TRANSFER(plan.receipt_asset_contract,  SWAP_POOL, receipt_liq, memo2);
    } else {
        TRANSFER(plan.receipt_asset_contract,  SWAP_POOL, receipt_liq, memo1);
        TRANSFER(plan.goal_asset_contract,     SWAP_POOL, sing_liq,    memo2);
    }
}

void investrwa::addtoken(const name& contract, const symbol& sym ) {
    CHECKC( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )
    auto token  = allow_token_t( sym );
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

void investrwa::on_rwafi_transfer( const name& from, const name& to, const asset& quantity, const string& memo){
    _token_transfer( from, to, quantity, memo );
}

void investrwa::on_sing_transfer( const name& from, const name& to, const asset& quantity, const string& memo){
    _token_transfer( from, to, quantity, memo );
}

// 支持两种格式：
// ① memo: plan:<plan_id>
// ② memo: refund:<plan_id>:<investor>
void investrwa::_token_transfer(const name& from,const name& to,const asset& quantity,const string& memo) {
    if (from == _self || to != _self) return;

    CHECKC(quantity.amount > 0,                                             err::NOT_POSITIVE, "quantity must be positive");
    CHECKC(!memo.empty(),                                                   err::INVALID_FORMAT, "memo required");

    const name bank = get_first_receiver();
    auto parts = split(memo, ":");
    CHECKC(parts.size() >= 2,                                               err::INVALID_FORMAT, "invalid memo format");

    const string action = parts[0];
    const uint64_t plan_id = std::stoull(parts[1]);

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan),                                                   err::RECORD_NOT_FOUND,"no such fund plan id: " + std::to_string(plan_id));

    // === 投资逻辑 ===
    if (action == "plan") {
        // --- 校验白名单 ---
        allow_token_t::idx_t allow_tokens(_self, _self.value);
        auto token_itr = allow_tokens.find(quantity.symbol.raw());

        CHECKC(token_itr != allow_tokens.end(),                             err::INVALID_SYMBOL, "token not registered: " + quantity.symbol.code().to_string());
        CHECKC(token_itr->token_contract == bank,                           err::CONTRACT_MISMATCH,"invalid token contract, expected " +
                                                                                token_itr->token_contract.to_string() + ", got " + bank.to_string());
        CHECKC(bank == plan.goal_asset_contract,                            err::CONTRACT_MISMATCH, "invalid investment token contract for plan");
        CHECKC(quantity.symbol == plan.goal_quantity.symbol,                err::SYMBOL_MISMATCH, "symbol mismatch, expected " +
                                                        plan.goal_quantity.symbol.code().to_string() + ", got " + quantity.symbol.code().to_string());

        // --- 执行投资 ---
        _process_investment(from, quantity, plan);
        return;
    }
    // === 退款逻辑 ===
    if (action == "refund") {
        CHECKC(parts.size() == 3,                                           err::INVALID_FORMAT, "expect memo format: refund:<id>:<user>");
        CHECKC(bank == plan.receipt_asset_contract,                         err::CONTRACT_MISMATCH,"refund must come from receipt contract: " +
                                                                                bank.to_string() + " ≠ " + plan.receipt_asset_contract.to_string());
        CHECKC(quantity.symbol == plan.receipt_symbol,                      err::SYMBOL_MISMATCH, "symbol mismatch for refund token");
        CHECKC(plan.status == PlanStatus::CANCELLED || plan.status == PlanStatus::FAILED,
                                                                            err::INVALID_STATUS,"refund not allowed (plan status: " + plan.status.to_string() + ")");
        // --- 执行退款 ---
        _process_refund(from, quantity, memo, plan);
        return;
    }

    CHECKC(false, err::INVALID_FORMAT, "unsupported memo action: " + action);
}

void investrwa::createplan(const name& creator,
                                        const string& title,
                                        const name& goal_asset_contract,
                                        const asset& goal_quantity,
                                        const asset& min_investment,
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
    CHECKC(!title.empty() && title.size() <= MAX_TITLE_SIZE,                err::INVALID_FORMAT, "invalid title");
    CHECKC(goal_quantity.amount > 0 && receipt_quantity_per_unit.amount > 0,err::NOT_POSITIVE, "invalid asset quantities");
    CHECKC(min_investment.amount > 0,                                      err::NOT_POSITIVE, "min investment must be positive");
    CHECKC(min_investment.symbol == goal_quantity.symbol,                  err::SYMBOL_MISMATCH, "min investment symbol mismatch");
    CHECKC(min_investment.amount <= goal_quantity.amount,                  err::INVALID_FORMAT, "min investment exceeds goal");
    CHECKC(soft_cap_percent >= 60 && soft_cap_percent <= 100,               err::INVALID_FORMAT, "soft cap percent invalid");

    CHECKC(hard_cap_percent >= soft_cap_percent,                            err::INVALID_FORMAT, "hard cap percent invalid");
    CHECKC(end_time > start_time,                                           err::INVALID_FORMAT, "end time must follow start");
    CHECKC(return_months > 0,                                               err::INVALID_FORMAT, "invalid return months");

    CHECKC(guaranteed_yield_apr > 0,                                        err::INVALID_FORMAT, "yield apr must be positive");
    CHECKC(receipt_asset_contract == RECEIPT_TOKEN_BANK,                    err::CONTRACT_MISMATCH, "receipt must be issued by rwafi.token");

    // ===  Receipt Token 校验 ===
    const string sym_code = receipt_quantity_per_unit.symbol.code().to_string();
    CHECKC(sym_code.size() <= 8,                                            err::INVALID_SYMBOL, "symbol code too long (max 8 chars)");
    CHECKC(sym_code.rfind("ST", 0) == 0,                                    err::INVALID_SYMBOL, "receipt symbol must start with 'ST' (e.g. STUSD)");

    auto plan_id = ++_gstate.last_plan_id;
    fundplan_t plan(plan_id);

    // ===  确保回执代币不存在 ===
    flon::token::stats statstable(RECEIPT_TOKEN_BANK, receipt_quantity_per_unit.symbol.code().raw());
    auto existing = statstable.find(receipt_quantity_per_unit.symbol.code().raw());
    CHECKC(existing == statstable.end(),                                    err::PARAM_ERROR,"receipt token already exists: " + sym_code);

    // ===  计算最大供应量（根据硬顶） ===
    int64_t goal_unit = 1;
    for (int i = 0; i < goal_quantity.symbol.precision(); ++i)
        goal_unit *= 10;

    uint128_t G_hard                = (uint128_t)goal_quantity.amount * hard_cap_percent / 100;         // 募资硬顶
    uint128_t R                     = (uint128_t)receipt_quantity_per_unit.amount;                      // 每单位目标资产对应 receipt 单位

    CHECKC(G_hard >= goal_unit,err::INVALID_FORMAT,"hard cap too small relative to goal unit");
    CHECKC((G_hard * R) % goal_unit == 0,err::INVALID_FORMAT,"receipt max supply not divisible by goal unit");
    uint128_t max_amt128            = (G_hard * R) / goal_unit;
    CHECKC(max_amt128 <= std::numeric_limits<int64_t>::max(),err::INVALID_FORMAT,"receipt max supply overflow");
    int64_t max_amt                 = (int64_t)max_amt128;
    int64_t extra_liq_amt            = max_amt / 100; // 1% for liquidity
    max_amt += extra_liq_amt;

    CHECKC(max_amt > 0, err::INVALID_FORMAT, "computed max supply invalid");

    // ===  创建 Receipt Token ===
    CREATE(RECEIPT_TOKEN_BANK, _self, asset(max_amt, receipt_quantity_per_unit.symbol));

    plan.title                     = title;
    plan.creator                   = creator;
    plan.goal_asset_contract       = goal_asset_contract;
    plan.goal_quantity             = goal_quantity;
    plan.min_investment            = min_investment;
    plan.receipt_asset_contract    = RECEIPT_TOKEN_BANK;
    plan.receipt_symbol            = receipt_quantity_per_unit.symbol;
    plan.receipt_quantity_per_unit = receipt_quantity_per_unit;
    plan.soft_cap_percent          = soft_cap_percent;
    plan.hard_cap_percent          = hard_cap_percent;
    plan.start_time                = time_point_sec(start_time.sec_since_epoch());
    plan.end_time                  = time_point_sec(end_time.sec_since_epoch());
    plan.return_months             = return_months;
    plan.return_end_time           = time_point_sec(end_time.sec_since_epoch() + return_months * seconds_per_month);
    plan.guaranteed_yield_apr      = guaranteed_yield_apr;
    plan.total_raised_funds        = asset(0, goal_quantity.symbol);
    plan.total_issued_receipts     = asset(0, receipt_quantity_per_unit.symbol);
    plan.status                    = PlanStatus::PENDING;
    plan.created_at                = time_point(current_time_point());

    // ===  通知 stake 合约同步创建计划 ===
    rwafi::stakerwa::addplan_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id, receipt_quantity_per_unit.symbol,goal_asset_contract,goal_quantity.symbol);
    _db.set(plan, _self);
}

void investrwa::cancelplan(const name& caller, const uint64_t& plan_id) {
    require_auth(caller);

    fundplan_t::idx_t _fundplans(_self, _self.value);
    auto itr = _fundplans.find(plan_id);
    CHECKC(itr != _fundplans.end(), err::RECORD_NOT_FOUND, "no such fund plan id: " + std::to_string(plan_id));

    const bool is_admin   = (caller == _gstate.admin);
    const bool is_creator = (caller == itr->creator);
    CHECKC(is_admin || is_creator, err::NO_AUTH, "only creator or admin can cancel");

    _fundplans.modify(itr, _self, [&](auto& p){
        _update_plan_status(p);
        const time_point_sec now = time_point_sec(current_time_point());
        CHECKC(!(now >= p.end_time && (p.status == PlanStatus::SUCCESS || p.status == PlanStatus::COMPLETED)),
               err::INVALID_STATUS, "cannot cancel: plan already succeeded");

        p.status = PlanStatus::CANCELLED;
    });

    // 执行 stake 合约操作
    rwafi::stakerwa::batchunstake_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id);
}


void investrwa::delplan(const uint64_t& plan_id) {
    require_auth(_gstate.admin);
    fundplan_t::idx_t _fundplans(_self, _self.value);
    auto itr = _fundplans.find(plan_id);
    CHECKC(itr != _fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(itr->status == PlanStatus::CANCELLED || itr->status == PlanStatus::FAILED, err::INVALID_STATUS, "only cancelled or failed plans can be erased");
    _fundplans.erase(itr);
}



void investrwa::updatestatus(const name& submitter,const uint64_t& plan_id){
    require_auth(submitter);

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    _update_plan_status(plan);
    _create_liquidity(plan);
}
