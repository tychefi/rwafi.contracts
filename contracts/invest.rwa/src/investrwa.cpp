
#include <flon.token.hpp>
#include "investrwa.hpp"
#include <did.ntoken/did.ntoken.db.hpp>
#include "utils.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>

using std::chrono::system_clock;
using namespace wasm;

static constexpr eosio::name active_permission{"active"_n};

// transfer out from contract self
#define TRANSFER_OUT(bank, to, quantity, memo) \
    { action(permission_level{get_self(), "active"_n }, bank, "transfer"_n, std::make_tuple( _self, to, quantity, memo )).send(); }

// issue receipt token
#define ISSUE_RECEIPT(bank, to, quantity, memo) \
    { action(permission_level{get_self(), "active"_n }, bank, "issue"_n, std::make_tuple( to, quantity, memo )).send(); }

#define BURN_RECEIPT(bank, from, quantity, memo) \
    { action(permission_level{get_self(), "active"_n }, bank, "burn"_n, std::make_tuple( from, quantity, memo )).send(); }

inline int64_t get_precision(const symbol &s) {
    int64_t digit = s.precision();
    CHECKC(digit >= 0 && digit <= 18, err::SYMBOL_MISMATCH, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
    return calc_precision(digit);
}

inline int64_t get_precision(const asset &a) {
    return get_precision(a.symbol);
}

void investrwa::addtoken(const name& contract, const symbol& sym ) {
    check( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )
    
    auto token          = allow_token_t( sym );
    CHECKC( !_db.get( token ), err::RECORD_NO_FOUND, "Token not found: " + symb.to_string() )

    token.token_symbol  = sym;
    token.token_contract= contract;

    _db.set(token, _self);
}

void investrwa::deltoken( const symbol& sym ) {
    check( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = tokenlist_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NO_FOUND, "no such token id: " + to_string( token_id ))
    _db.del( token );
}

void investrwa::onshelf( const symbol& sym, const bool& onshelf ) {
    check( has_auth( _self) || has_auth( _gstate.admin ), err::NO_AUTH, "no auth to add token" )

    auto token = allow_token_t( sym );
    CHECKC( _db.get( token ), err::RECORD_NO_FOUND, "no such token symbol: " + sym.to_string() )
    token.onshelf = onshelf;
    _db.set( token, _self );
}

// memo: plan:xxx
void investrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == _self || to != _self) return;

    CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )
    auto parts = split(memo, ":");
    CHECK( parts.size() == 2, err::INVALID_FORMAT, "invalid memo format" );
    auto plan_id = (uint64_t) stoi(string(parts[1]));
    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NO_FOUND, "no such fund plan id: " + to_string( plan_id ) )
    
    if ( quantity.symbol == plan.goal_quantity.symbol ) {
        _process_investment( from, to, quantity, memo, plan );
        return;

    } else if ( quantity.symbol == plan.receipt_symbol ) {
        _process_refund( from, to, quantity, memo, plan );
        return;

    } else {
        CHECKC( false, err::SYMBOL_MISMATCH, "symbol mismatch, expected: " + plan.goal_quantity.symbol.to_string() + " or " + plan.receipt_symbol.symbol.to_string() + ", actual: " + quantity.symbol.to_string() )
    }
}

void investrwa::_process_investment( const name& from, const name& to, const asset& quantity, const string& memo, fundplan_t& plan ) {
    CHECKC( plan.start_time > time_point(current_time_point()), err::INVALID_STATUS, "investments not started yet" )
    CHECKC( plan.status == PlanStatus::ACTIVE, err::INVALID_STATUS, "investments only allowed when plan is active" )

    // process investment: stake immediately
    TRANSFER_OUT( plan.goal_asset_contract, _gstate.stake_contract, quantity, string("stake:") + to_string( plan.id ) + ":" + from.to_string() )
    
    // issue receipt
    auto receipt_quantity = asset( quantity.amount, plan.receipt_symbol );
    ISSUE_RECEIPT( plan.receipt_asset_contract, from, receipt_quantity, string("plan:") + to_string( plan.id ))

    // update plan status
    plan.total_raised_funds         += quantity;
    plan.total_issued_receipts      += receipt_quantity;

    _update_plan_status( plan );

    _dbc.set( plan, _self );
}

void investrwa::_process_refund( const name& from, const name& to, const asset& quantity, const string& memo, const fundplan_t& plan ) {
    // process refund
    // check if plan is in REFUNDABLE status
    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NO_FOUND, "no such fund plan id: " + to_string( plan_id ) )
    CHECKC( plan.status == PlanStatus::CANCELLED, err::INVALID_STATUS, "refunds only allowed when plan is cancelled" )

    auto fund_balance = _get_balance( plan.goal_asset_contract, _self, plan.goal_quantity.symbol );
    CHECKC( fund_balance.amount >= quantity.amount, err::QUANTITY_NOT_ENOUGH, "contract fund balance not enough for refund" )
    auto fund_quantity = asset( quantity.amount, plan.goal_quantity.symbol );
    TRANSFER_OUT( plan.goal_asset_contract, from, fund_quantity, string("refund for plan:") + to_string( plan.id ) )
    BURN_RECEIPT( plan.receipt_asset_contract, _self, quantity, string("refund for plan:") + to_string( plan.id ) )

    plan.total_raised_funds         -= fund_quantity;
    plan.total_issued_receipts      -= quantity;
    _db.set( plan, _self );
}

void investrwa::refund( const name& investor, const uint64_t& plan_id ) {
    require_auth( investor );

    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NO_FOUND, "no such fund plan id: " + to_string( plan_id ) )
    CHECKC( plan.status == PlanStatus::CANCELLED, err::INVALID_STATUS, "refunds only allowed when plan is cancelled" )

    auto receipt_balance = _get_balance( plan.receipt_asset_contract, investor, plan.receipt_symbol );
    CHECKC( receipt_balance.amount > 0, err::QUANTITY_NOT_ENOUGH, "no receipt token balance for refund" )
    auto receipt_quantity = asset( receipt_balance.amount, plan.receipt_symbol );

    auto fund_balance = _get_balance( plan.goal_asset_contract, _self, plan.goal_quantity.symbol );
    CHECKC( fund_balance.amount >= receipt_quantity.amount, err::QUANTITY_NOT_ENOUGH, "contract fund balance not enough for refund" )
    auto fund_quantity = asset( receipt_quantity.amount, plan.goal_quantity.symbol );

    // transfer out fund
    TRANSFER_OUT( plan.goal_asset_contract, investor, fund_quantity, string("refund for plan:") + to_string( plan.id ) )
    // burn receipt
    BURN_RECEIPT( plan.receipt_asset_contract, investor, receipt_quantity, string("refund for plan:") + to_string( plan.id ) )

    plan.total_raised_funds         -= fund_quantity;
    plan.total_issued_receipts      -= receipt_quantity;
    _db.set( plan, _self );
}
void investrwa::_update_plan_status( fundplan_t& plan ) {
    auto now = time_point_sec( current_time_point() );

    if ( now >= plan.start_time && now <= plan.end_time ) {
        plan.status = PlanStatus::ACTIVE;

        if ( plan.total_raised_funds.amount >= plan.goal_quantity.amount * plan.hard_cap_percent / 100 ) {
            plan.status = PlanStatus::CLOSED;
        }
    } else if ( now > plan.end_time ) {
        // check if soft cap met
        int64_t soft_cap_amount = plan.goal_quantity.amount * plan.soft_cap_percent / 100;
        if ( plan.total_raised_funds.amount >= soft_cap_amount ) {
            plan.status = PlanStatus::SUCCESS;
        } else {
            plan.status = PlanStatus::FAILED;
        }
    }
}

void investrwa::createplan( const name& creator,
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
                             const uint16_t& return_years,
                             const double& guaranteed_yield_apr )
{
    require_auth( creator );

    CHECKC( title.size() > 0 && title.size() <= MAX_TITLE_SIZE, err::INVALID_FORMAT, "title size invalid" );
    CHECKC( goal_quantity.amount > 0, err::NOT_POSITIVE, "goal quantity must be positive" );
    CHECKC( receipt_quantity_per_unit.amount > 0, err::NOT_POSITIVE, "receipt quantity per unit must be positive" );
    CHECKC( invest_unit_size > 0, err::MIN_UNIT_INVALID, "invest unit size must be positive" );
    CHECKC( soft_cap_percent > 0 && soft_cap_percent <= 100, err::INVALID_FORMAT, "soft cap percent invalid" );
    CHECKC( hard_cap_percent >= soft_cap_percent, err::INVALID_FORMAT, "hard cap percent invalid" );
    CHECKC( end_time > start_time, err::INVALID_FORMAT, "end time must be after start time" );
    CHECKC( return_years >= 0, err::INVALID_FORMAT, "return years must be positive" );
    CHECKC( guaranteed_yield_apr >= 0.0, err::INVALID_FORMAT, "guaranteed yield apr must be non-negative" );

    dbc::idx_t<fundplan_t> fundplan_tbl( _self, _self.value );

    fundplan_tbl.emplace( get_self(), [&]( auto& fp ) {
        fp.id                          = ++ _gstate.last_plan_id;
        fp.title                       = title;
        fp.creator                     = creator;
        fp.goal_asset_contract         = goal_asset_contract;
        fp.goal_quantity               = goal_quantity;
        fp.created_at                  = time_point(current_time_point());
        fp.receipt_asset_contract      = receipt_asset_contract;
        fp.receipt_quantity_per_unit   = receipt_quantity_per_unit;
        fp.invest_unit_size            = invest_unit_size;
        fp.soft_cap_percent            = soft_cap_percent;
        fp.hard_cap_percent            = hard_cap_percent;
        fp.start_time                  = start_time;
        fp.end_time                    = end_time;
        fp.return_years                = return_years;
        fp.return_end_time             = time_point( start_time.sec_since_epoch() + return_years * seconds_per_year );
        fp.guaranteed_yield_apr        = guaranteed_yield_apr;
        fp.total_raised_funds          = asset(0, goal_quantity.symbol);
        fp.total_issued_receipts       = asset(0, receipt_quantity_per_unit.symbol);
    } );
}

void investrwa::cancelplan( const name& creator, const uint64_t& plan_id ) {
    require_auth( creator );

    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NO_FOUND, "no such fund plan id: " + to_string( plan_id ) )
    CHECKC( plan.creator == creator, err::NO_AUTH, "no auth to cancel this plan" )
    
    _update_plan_status( plan );
    CHECKC( plan.status == PlanStatus::PENDING || PlanStatus::ACTIVE || PlanStatus::CLOSED, 
                err::INVALID_STATUS, "cannot cancel in current status: " + plan.status.to_string() )

    plan.status = PlanStatus::CANCELLED;
    _db.set( plan, _self );
}
