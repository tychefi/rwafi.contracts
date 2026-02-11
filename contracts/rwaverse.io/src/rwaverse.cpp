#include "rwaverse.hpp"
#include "rwastakepool/rwastakepooldb.hpp"
#include "rwastakepool/rwastakepool.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>
#include "rwaguarapool/rwaguarapooldb.hpp"
#include "flon/flon.token.hpp"
#include "flon.swap/flon.swap.db.hpp"
#include "flon.swap/flon.swap.hpp"
#include "flon/utils.hpp"


#define SWAPCREATE(contract, user, sym1, contract1, sym2, contract2, liq_sym) \
{ \
    flonswap::swapcreate_action act{ contract, { {user, active_perm} } }; \
    act.send(user, sym1, contract1, sym2, contract2, liq_sym); \
}

#define SWAPSETTPCONF(contract, admin, tpcode, redeem_flag, sys_fee_receiver, split_id, extra_buy_fee_ratio, extra_sell_fee_ratio, extra_fee_receiver, whitelist) \
{ \
    eosio::action( \
        permission_level{admin, active_perm}, \
        contract, \
        "settpconf"_n, \
        std::make_tuple( \
            admin, \
            tpcode, \
            redeem_flag, \
            sys_fee_receiver, \
            split_id, \
            extra_buy_fee_ratio, \
            extra_sell_fee_ratio, \
            extra_fee_receiver, \
            whitelist \
        ) \
    ).send(); \
}

using std::chrono::system_clock;
using namespace wasm;
using namespace eosio;
using namespace rwafi;
using namespace flon;
static constexpr name RECEIPT_TOKEN_BANK{"rwa.vtoken"_n};
static constexpr name GOAL_ASSET_CONTRACT{"sing.token"_n};
static constexpr eosio::name active_perm  {"active"_n};

namespace {

void validate_createplan_inputs(const string& title,
                                const asset& goal_quantity,
                                const asset& min_investment,
                                const asset& receipt_quantity_per_unit,
                                const uint8_t& soft_cap_percent,
                                const uint8_t& hard_cap_percent,
                                const time_point& start_time,
                                const time_point& end_time,
                                const uint16_t& return_months,
                                const uint32_t& guaranteed_yield_apr,
                                const name& receipt_asset_contract) {
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
    CHECKC(receipt_asset_contract == RECEIPT_TOKEN_BANK,                    err::CONTRACT_MISMATCH, "receipt must be issued by rwa.vtoken");
}

asset calc_receipt_amount_from_goal_amount(int64_t goal_amount,
                                           int64_t goal_unit,
                                           const fundplan_t& plan,
                                           const char* precision_err,
                                           const char* amount_err) {
    uint128_t issue_raw = (uint128_t)goal_amount * (uint128_t)plan.receipt_quantity_per_unit.amount;
    CHECKC(issue_raw % goal_unit == 0, err::INVALID_FORMAT, precision_err);

    uint128_t issue_amt128 = issue_raw / goal_unit;
    CHECKC(issue_amt128 > 0 && issue_amt128 <= std::numeric_limits<int64_t>::max(),
           err::INVALID_FORMAT, amount_err);
    return asset((int64_t)issue_amt128, plan.receipt_symbol);
}

time_point_sec current_time_sec() {
    return time_point_sec(current_time_point());
}

bool liquidity_market_exists(const fundplan_t& plan) {
    auto tpcode = flon::flonswap::pool_symbol(plan.receipt_symbol ,SING_SYM);
    market_t::idx_t markets(SWAP_POOL, SWAP_POOL.value);
    return markets.find(tpcode.value) != markets.end();
}

bool is_oracle(const global_t& gstate, const name& account) {
    return gstate.oracles.find(account) != gstate.oracles.end();
}

} // namespace

int64_t rwaverse::pow10(uint8_t p) {
    CHECKC(p <= 18, err::INVALID_FORMAT, "precision too large");
    int64_t v = 1;
    while (p--) v *= 10;
    return v;
}

int64_t rwaverse::_goal_unit(const fundplan_t& plan) {
    return pow10(plan.goal_quantity.symbol.precision());
}

// ------------------- Plan Status -------------------
void rwaverse::_update_plan_status(fundplan_t& plan) {
    const time_point_sec now    = current_time_sec();
    const int64_t raised        = plan.total_raised_funds.amount;
    const uint128_t soft_cap128 = (uint128_t)plan.goal_quantity.amount * plan.soft_cap_percent / 100;
    CHECKC(soft_cap128 <= std::numeric_limits<int64_t>::max(), err::INVALID_FORMAT, "soft cap overflow");
    const int64_t soft_cap      = (int64_t)soft_cap128;

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

    // status updated in-memory; persistence handled by callers
}

void rwaverse::_refresh_and_require_status(fundplan_t& plan,
                                            std::initializer_list<eosio::name> allowed,
                                            const char* err_msg) {
    _update_plan_status(plan);
    for (const auto& status : allowed) {
        if (plan.status == status) {
            return;
        }
    }
    CHECKC(false, err::INVALID_STATUS, err_msg);
}

void rwaverse::notify(const name& contract,
                            const name& from,
                            const name& to,
                            const asset& quantity,
                            const string& memo,
                            const string& type,
                            const uint64_t& plan_id) {
    require_auth(get_self());
}

// ------------------- Plan Lifecycle -------------------
void rwaverse::createplan(const name& creator,
                                        const string& title,
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
    validate_createplan_inputs(title, goal_quantity, min_investment, receipt_quantity_per_unit,
                               soft_cap_percent, hard_cap_percent, start_time, end_time,
                               return_months, guaranteed_yield_apr, receipt_asset_contract);
    CHECKC(goal_quantity.symbol == SING_SYM, err::SYMBOL_MISMATCH, "goal symbol must be SING");
    CHECKC(goal_quantity.symbol.precision() == SING_SYM.precision(), err::INVALID_FORMAT, "goal precision mismatch");

    // ===  Receipt Token 校验 ===
    const string sym_code = receipt_quantity_per_unit.symbol.code().to_string();
    CHECKC(sym_code.size() <= 8,                                            err::INVALID_SYMBOL, "symbol code too long (max 8 chars)");

    auto plan_id = ++_gstate.last_plan_id;
    fundplan_t plan(plan_id);

    // ===  确保回执代币不存在 ===
    flon::token::stats statstable(RECEIPT_TOKEN_BANK, receipt_quantity_per_unit.symbol.code().raw());
    auto existing = statstable.find(receipt_quantity_per_unit.symbol.code().raw());
    CHECKC(existing == statstable.end(),                                    err::PARAM_ERROR,"receipt token already exists: " + sym_code);

    // ===  计算最大供应量（根据硬顶） ===
    const int64_t goal_unit = pow10(goal_quantity.symbol.precision());

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
    plan.goal_asset_contract       = GOAL_ASSET_CONTRACT;
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
    plan.withdrawn_funds          = asset(0, goal_quantity.symbol);
    plan.status                    = PlanStatus::PENDING;
    plan.created_at                = time_point(current_time_point());
    plan.updated_at                = plan.created_at;

    // ===  通知 stake 合约同步创建计划 ===
    rwafi::rwastakepool::addplan_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id, receipt_quantity_per_unit.symbol, GOAL_ASSET_CONTRACT, goal_quantity.symbol);
    _db.set(plan, _self);
}


void rwaverse::cancelplan(const name& caller, const uint64_t& plan_id) {
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
    rwafi::rwastakepool::batchunstake_action{
        _gstate.stake_contract,
        { permission_level{ get_self(), "active"_n } }
    }.send(plan_id);
}
//提前结束募资进入收益期
void rwaverse::endraisegain(const name& caller, const uint64_t& plan_id) {
    require_auth(caller);

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    const bool is_creator = (caller == plan.creator);
    CHECKC( is_creator, err::NO_AUTH, "only creator can end fundraising");

    const time_point_sec now = current_time_sec();
    CHECKC(now >= plan.start_time, err::INVALID_STATUS, "fundraising not started");
    CHECKC(now < plan.end_time, err::INVALID_STATUS, "fundraising already ended");

    _update_plan_status(plan);
    CHECKC(plan.status == PlanStatus::SUCCESS, err::INVALID_STATUS, "plan must be success");

    plan.end_time = now;
    plan.return_end_time = time_point_sec(now.sec_since_epoch() + plan.return_months * seconds_per_month);

    _update_plan_status(plan);
    plan.updated_at = time_point(current_time_point());
    _db.set(plan, _self);

    if (now >= plan.end_time && now < plan.return_end_time &&
        plan.status == PlanStatus::SUCCESS &&
        !liquidity_market_exists(plan)) {
        _create_liquidity(plan);
        SWAPSETTPCONF(
            SWAP_POOL,
            get_self(),
            flon::flonswap::pool_symbol(plan.receipt_symbol, SING_SYM),
            true,
            get_self(),
            (uint64_t)1,
            (int16_t)30,
            (int16_t)30,
            get_self(),
            std::set<name>{}
        );
    }
}

void rwaverse::delplan(const uint64_t& plan_id) {
    require_auth(_gstate.admin);
    fundplan_t::idx_t _fundplans(_self, _self.value);
    auto itr = _fundplans.find(plan_id);
    CHECKC(itr != _fundplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    _fundplans.modify(itr, _self, [&](auto& p){
        _update_plan_status(p);
        // CHECKC(p.status == PlanStatus::CANCELLED || p.status == PlanStatus::FAILED,
        //        err::INVALID_STATUS, "only cancelled or failed plans can be erased");
    });
    _fundplans.erase(itr);
}

void rwaverse::refreshstat(const name& submitter,const uint64_t& plan_id){
    require_auth(submitter);
    CHECKC(is_oracle(_gstate, submitter), err::NO_AUTH, "submitter not in oracle list");

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    const time_point_sec now = current_time_sec();
    const auto old_status = plan.status;
    _update_plan_status(plan);
    if (plan.status != old_status) {
        plan.updated_at = time_point(current_time_point());
        _db.set(plan, _self);
    }

    if (plan.status == old_status) {
        _db.set(plan, _self);
    }

    if (plan.status == PlanStatus::FAILED && old_status != PlanStatus::FAILED) {
        rwafi::rwastakepool::batchunstake_action{
            _gstate.stake_contract,
            { permission_level{ get_self(), "active"_n } }
        }.send(plan_id);
    }

    if (now >= plan.end_time && now < plan.return_end_time &&
        plan.status == PlanStatus::SUCCESS &&
        !liquidity_market_exists(plan)) {
        _create_liquidity(plan);
        SWAPSETTPCONF(
            SWAP_POOL,
            get_self(),
            flon::flonswap::pool_symbol(plan.receipt_symbol, SING_SYM),
            true,
            get_self(),
            (uint64_t)1,
            (int16_t)0,
            (int16_t)0,
            get_self(),
            std::set<name>{}
        );
    }
}

void rwaverse::batchrefresh(const name& submitter, const std::vector<uint64_t>& plan_ids, const uint64_t& now_ts) {
    require_auth(submitter);
    // CHECKC(is_oracle(_gstate, submitter), err::NO_AUTH, "submitter not in oracle list");
    CHECKC(!plan_ids.empty(), err::PARAM_ERROR, "plan_ids empty");

    const time_point_sec now = time_point_sec(now_ts);

    for (const auto& plan_id : plan_ids) {
        fundplan_t plan(plan_id);
        CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

        const auto old_status = plan.status;
        _update_plan_status(plan);
        if (plan.status != old_status) {
            plan.updated_at = time_point(current_time_point());
            _db.set(plan, _self);
        }

        if (plan.status == old_status) {
            _db.set(plan, _self);
        }

        if (plan.status == PlanStatus::FAILED && old_status != PlanStatus::FAILED) {
            rwafi::rwastakepool::batchunstake_action{
                _gstate.stake_contract,
                { permission_level{ get_self(), "active"_n } }
            }.send(plan_id);
        }

        if (now >= plan.end_time && now < plan.return_end_time &&
            plan.status == PlanStatus::SUCCESS &&
            !liquidity_market_exists(plan)) {
            _create_liquidity(plan);
            SWAPSETTPCONF(
                SWAP_POOL,
                get_self(),
                flon::flonswap::pool_symbol(plan.receipt_symbol, SING_SYM),
                true,
                get_self(),
                (uint64_t)1,
                (int16_t)0,
                (int16_t)0,
                get_self(),
                std::set<name>{}
            );
        }
    }
}

void rwaverse::withdraw(const name& caller, const uint64_t& plan_id, const name& to, const asset& quantity) {
    require_auth(caller);
    CHECKC(is_account(to), err::ACCOUNT_INVALID, "invalid recipient");
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "withdraw must be positive");

    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    const bool is_admin   = (caller == _gstate.admin);
    const bool is_creator = (caller == plan.creator);
    CHECKC(is_admin || is_creator, err::NO_AUTH, "only creator or admin can withdraw");

    CHECKC(quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "withdraw symbol mismatch");

    const time_point_sec now = current_time_sec();
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "withdraw only after fundraising end");
    _refresh_and_require_status(plan, {PlanStatus::SUCCESS, PlanStatus::COMPLETED}, "withdraw only for successful plans");

    if (plan.withdrawn_funds.symbol != plan.goal_quantity.symbol) {
        plan.withdrawn_funds = asset(0, plan.goal_quantity.symbol);
    }

    int64_t liquidity_reserved = plan.total_raised_funds.amount / 100; // 1% reserved for liquidity
    if (liquidity_market_exists(plan)) {
        liquidity_reserved = 0; // already injected, no need to reserve
    }
    int64_t max_withdrawable = plan.total_raised_funds.amount - liquidity_reserved - plan.withdrawn_funds.amount;
    if (max_withdrawable < 0) {
        max_withdrawable = 0;
    }
    CHECKC(quantity.amount <= max_withdrawable, err::QUANTITY_INSUFFICIENT, "withdraw exceeds available funds");

    const asset balance = flon::token::get_balance(plan.goal_asset_contract, get_self(), plan.goal_quantity.symbol.code());
    CHECKC(balance.amount >= quantity.amount, err::QUANTITY_INSUFFICIENT, "contract balance insufficient");

    plan.withdrawn_funds += quantity;
    plan.updated_at = time_point(current_time_point());
    _db.set(plan, _self);

    TRANSFER(plan.goal_asset_contract, to, quantity, "withdraw:" + std::to_string(plan.id));
    // === 通知提现操作 ===
    rwaverse::notify_action act{ get_self(), { {get_self(), "active"_n} } };
    act.send(_self, _self, to, quantity, "withdraw:" + std::to_string(plan.id), "Withdraw",plan.id);

}

void rwaverse::setoracle(const name& account, const bool& enabled) {
    CHECKC(_gstate.admin.value != 0, err::RECORD_NOT_FOUND, "admin not initialized");
    require_auth(get_self());
    CHECKC(account.value != 0,  err::ACCOUNT_INVALID, "oracle account cannot be empty");
    CHECKC(is_account(account), err::ACCOUNT_INVALID, "oracle account not exist");

    auto it = _gstate.oracles.find(account);
    if (enabled) {
        CHECKC(it == _gstate.oracles.end(), err::RECORD_EXISTS, "oracle already exists");
        _gstate.oracles.insert(account);
        _global.set(_gstate, get_self());
    } else {
        if (it != _gstate.oracles.end()) {
            _gstate.oracles.erase(it);
            _global.set(_gstate, get_self());
        }
    }
}

// ------------------- Token Whitelist -------------------
void rwaverse::addtoken(const name& contract, const symbol& sym ) {
    CHECKC( has_auth( _self ) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )
    auto token  = allow_token_t( sym );
    CHECKC( !_db.get( token ), err::RECORD_NOT_FOUND, "Token symbol already existing" )

    token.token_symbol  = sym;
    token.token_contract= contract;
    _db.set(token, _self);
}

void rwaverse::deltoken( const symbol& sym ) {
    CHECKC( has_auth( _self ) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = allow_token_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NOT_FOUND, "no such token symbol" )
    _db.del( token );
}


void rwaverse::onshelf( const symbol& sym, const bool& onshelf ) {
    CHECKC( has_auth( _self ) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = allow_token_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NOT_FOUND, "no such token symbol" )
    token.onshelf = onshelf;
    _db.set( token, _self );
}


// ------------------- Investment & Refund -------------------
void rwaverse::on_rwafi_transfer( const name& from, const name& to, const asset& quantity, const string& memo){
    _token_transfer( from, to, quantity, memo );
}


void rwaverse::on_sing_transfer( const name& from, const name& to, const asset& quantity, const string& memo){
    _token_transfer( from, to, quantity, memo );
}


// 支持两种格式：
// ① memo: plan:<plan_id>
// ② memo: refund:<plan_id>:<investor>
void rwaverse::_token_transfer(const name& from,const name& to,const asset& quantity,const string& memo) {
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
        // --- 执行退款 ---
        uint64_t memo_plan_id = std::stoull(parts[1]);
        CHECKC(memo_plan_id == plan.id, err::PARAM_ERROR, "plan_id mismatch");
        name investor(parts[2]);
        CHECKC(is_account(investor), err::ACCOUNT_INVALID, "invalid investor");

        _process_refund(investor,quantity, plan);
        return;
    }

    CHECKC(false, err::INVALID_FORMAT, "unsupported memo action: " + action);
}


void rwaverse::_process_investment(const name& from, const asset& quantity, fundplan_t& plan) {


    const time_point_sec now = current_time_sec();
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "investment must be positive");
    CHECKC(now >= plan.start_time, err::INVALID_STATUS, "fundraising not started");
    CHECKC(now <= plan.end_time,   err::INVALID_STATUS, "fundraising ended");
    // plan 状态门控
    _refresh_and_require_status(plan, {PlanStatus::PENDING, PlanStatus::RAISEACTIVE, PlanStatus::SUCCESS},
                                "plan not open for investment");
    // 硬顶控制
    const uint128_t hard_cap128  = plan.goal_quantity.amount * plan.hard_cap_percent / 100;
    CHECKC(hard_cap128 <= std::numeric_limits<int64_t>::max(), err::INVALID_FORMAT, "hard cap overflow");
    const int64_t hard_cap = (int64_t)hard_cap128;
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
    const int64_t goal_unit = _goal_unit(plan);
    asset issued_receipt = calc_receipt_amount_from_goal_amount(
        accepted.amount, goal_unit, plan, "receipt precision mismatch", "invalid receipt amount");

    // === 发放 receipt 并立刻转入 stake ===
    ISSUE(plan.receipt_asset_contract,get_self(), issued_receipt, "plan:" + std::to_string(plan.id));
    TRANSFER(plan.receipt_asset_contract,_gstate.stake_contract,issued_receipt, "stake:" + std::to_string(plan.id) + ":" + from.to_string());

    // === 通知 stake 合约同步投资记录 ===
    rwaverse::notify_action act{ get_self(), { {get_self(), "active"_n} } };
    act.send(_self, _self, _gstate.stake_contract, issued_receipt, "stake:" + std::to_string(plan.id) + ":" + from.to_string(), "Staking", plan.id);

    // === 更新统计 ===
    plan.total_raised_funds    += accepted;
    plan.total_issued_receipts += issued_receipt;

    // === 通知投资成功 ===
    rwaverse::notify_action act2{ get_self(), { {get_self(), "active"_n} } };
    act2.send(_self, from, _self, accepted, "plan:" + std::to_string(plan.id) , "Investment",plan.id);

    if (refund.amount > 0) {
        TRANSFER(plan.goal_asset_contract,from,refund,"refund:hardcap:" + std::to_string(plan.id));
    }

    _update_plan_status(plan);
    _db.set(plan, _self);
}


asset rwaverse::_calc_refund_amount( const asset& receipt_qty,const fundplan_t& plan) {
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


void rwaverse::_process_refund(const name& investor,const asset& quantity, fundplan_t& plan ) {
    // === 1. 基础校验 ===
    _update_plan_status(plan);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "refund must be positive");
    CHECKC(quantity.symbol == plan.receipt_symbol, err::SYMBOL_MISMATCH, "receipt symbol mismatch");
    CHECKC(plan.status == PlanStatus::CANCELLED || plan.status == PlanStatus::FAILED,
                                    err::INVALID_STATUS, "refund not allowed in current plan status");

    // === 2. memo 已由调用方校验并解析 ===

    // === 3. 计算退款金额（核心逻辑） ===
    asset refund_amount = _calc_refund_amount(quantity, plan);

    // === 4. 资金安全校验 ===
    CHECKC(plan.total_raised_funds.amount >= refund_amount.amount, err::QUANTITY_INSUFFICIENT, "insufficient raised funds");
    CHECKC(plan.total_issued_receipts.amount >= quantity.amount, err::QUANTITY_INSUFFICIENT, "insufficient issued receipts");

    // === 5. 执行退款 ===
    BURN( plan.receipt_asset_contract, quantity, "burn receipt for refund, plan:" + std::to_string(plan.id));
    TRANSFER( plan.goal_asset_contract, investor,refund_amount,"refund:" + std::to_string(plan.id));

    // === 通知退款操作 ===
    rwaverse::notify_action act{ get_self(), { {get_self(), "active"_n} } };
    act.send(_self, _self, investor, quantity, "refund:" + std::to_string(plan.id) , "Refund",plan.id);

    // === 6. 状态更新 ===
    plan.total_raised_funds.amount = std::max<int64_t>(0, plan.total_raised_funds.amount - refund_amount.amount);
    plan.total_issued_receipts.amount = std::max<int64_t>(0, plan.total_issued_receipts.amount - quantity.amount);

    if (plan.total_raised_funds.amount == 0 && plan.total_issued_receipts.amount == 0) {
        plan.status = PlanStatus::REFUNDED;
    }

    _db.set(plan, _self);
}


// ------------------- Liquidity -------------------
asset rwaverse::_convert_precision(const asset& src, const symbol& dst_sym) {
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


asset rwaverse::_calc_receipt_liq_from_goal(const asset& goal_liq, const fundplan_t& plan) {
    CHECKC(goal_liq.amount > 0, err::NOT_POSITIVE, "goal liquidity must be positive");

    const int64_t goal_unit = _goal_unit(plan);
    return calc_receipt_amount_from_goal_amount(
        goal_liq.amount, goal_unit, plan, "receipt liquidity precision mismatch", "receipt liquidity amount invalid");
}


void rwaverse::_create_liquidity(fundplan_t& plan) {

    // 0. 状态 & 幂等校验
    const time_point_sec now = current_time_sec();
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "liquidity only after fundraising end");
    CHECKC(now < plan.return_end_time, err::INVALID_STATUS, "liquidity only before return end");
    const auto old_status = plan.status;
    _refresh_and_require_status(plan, {PlanStatus::SUCCESS, PlanStatus::COMPLETED}, "liquidity only for successful plans");
    if (plan.status != old_status) {
        _db.set(plan, _self);
    }

    auto tpcode = flon::flonswap::pool_symbol(plan.receipt_symbol ,SING_SYM);

    // 2. 查 / 建 swap market
    market_t::idx_t markets(SWAP_POOL, SWAP_POOL.value);
    auto itr = markets.find(tpcode.value);

    CHECKC(itr == markets.end(), err::PARAM_ERROR, "market already exists,tpcode="+ tpcode.to_string());

    bool exists = flon::flonswap::is_exists_pool( extended_symbol(plan.receipt_symbol, plan.receipt_asset_contract),extended_symbol(SING_SYM, SING_BANK)   );

    if (!exists) {
        string lp_symbol_str = add_symbol(plan.receipt_symbol,SING_SYM, 1);
        // CHECKC(false, err::PARAM_ERROR,
        //        "swapcreate params: contract=" + SWAP_POOL.to_string() +
        //        ", user=" + _self.to_string() +
        //        ", sym1=" + plan.receipt_symbol.code().to_string() +
        //        ", contract1=" + plan.receipt_asset_contract.to_string() +
        //        ", sym2=" + SING_SYM.code().to_string() +
        //        ", contract2=" + SING_BANK.to_string() +
        //        ", liq_sym=" + lp_symbol_str);
        SWAPCREATE(SWAP_POOL,_self,plan.receipt_symbol, plan.receipt_asset_contract,SING_SYM, SING_BANK,symbol_code(lp_symbol_str) );
    }

    // CHECKC(false, err::PARAM_ERROR, "tpcode=" + tpcode.to_string());
    rwaverse::liquidity_action mine{ get_self(), { get_self(), "active"_n } };
    mine.send(plan.id,tpcode);
}


// 3) 真正注入流动性（独立 action 调用）
void rwaverse::liquidity(const uint64_t& plan_id,const name& tpcode ) {
    require_auth(_self);

    // 0. 状态 & 幂等校验
    fundplan_t plan(plan_id);
    CHECKC(_db.get(plan), err::RECORD_NOT_FOUND, "plan not found");

    const time_point_sec now = current_time_sec();
    CHECKC(now >= plan.end_time, err::INVALID_STATUS, "liquidity only after fundraising end");
    CHECKC(now < plan.return_end_time, err::INVALID_STATUS, "liquidity only before return end");
    const auto old_status = plan.status;
    _refresh_and_require_status(plan, {PlanStatus::SUCCESS, PlanStatus::COMPLETED}, "liquidity only for successful plans");
    if (plan.status != old_status) {
        _db.set(plan, _self);
    }

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
        // === 通知流动性注入操作 ===
        rwaverse::notify_action act{ get_self(), { {get_self(), "active"_n} } };
        act.send(_self, _self, SWAP_POOL, sing_liq, memo1 , "LPPrimary",plan.id);

        TRANSFER(plan.receipt_asset_contract,  SWAP_POOL, receipt_liq, memo2);
        // === 通知流动性注入操作 ===
        rwaverse::notify_action act2{ get_self(), { {get_self(), "active"_n} } };
        act2.send(_self, _self, SWAP_POOL, receipt_liq, memo2 , "LPSecondary",plan.id);
    } else {
        TRANSFER(plan.receipt_asset_contract,  SWAP_POOL, receipt_liq, memo1);
        // === 通知流动性注入操作 ===
        rwaverse::notify_action act{ get_self(), { {get_self(), "active"_n} } };
        act.send(_self, _self, SWAP_POOL, receipt_liq, memo1 , "LPSecondary",plan.id);

        TRANSFER(plan.goal_asset_contract,     SWAP_POOL, sing_liq,    memo2);
        // === 通知流动性注入操作 ===
        rwaverse::notify_action act2{ get_self(), { {get_self(), "active"_n} } };
        act2.send(_self, _self, SWAP_POOL, sing_liq, memo2 , "LPPrimary",plan.id);
    }

    if (plan.withdrawn_funds.symbol != plan.goal_quantity.symbol) {
        plan.withdrawn_funds = asset(0, plan.goal_quantity.symbol);
    }
    plan.withdrawn_funds += goal_liq;
    plan.updated_at = time_point(current_time_point());
    _db.set(plan, _self);
}
