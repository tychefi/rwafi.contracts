
#include <flon.token.hpp>
#include "guarantyrwa.hpp"
#include "investrwa.hpp"
#include "investrwadb.hpp"
#include "yieldrwadb.hpp"
#include "utils.hpp"

#include "wasm_db.hpp"

#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>

using std::chrono::system_clock;
using namespace wasm;
using namespace eosio;

// inline int64_t get_precision(const symbol &s) {
//     int64_t digit = s.precision();
//     CHECKC(digit >= 0 && digit <= 18, err::SYMBOL_MISMATCH, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
//     return calc_precision(digit);
// }

// inline int64_t get_precision(const asset &a) {
//     return get_precision(a.symbol);
// }

//--------------------------
// 天数到某年 1月1日 的累计天数（1970年起）
static uint64_t days_to_year_start(uint64_t year) {
    uint64_t y = year - 1;  // 前一年
    return y * 365 + y/4 - y/100 + y/400;
}

// 该年总天数（闰年 366，平年 365）
static uint64_t days_in_year(uint64_t year) {
    bool is_leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    return is_leap ? 366 : 365;
}

// 由 Unix 时间戳计算年份（1970 年起）
static uint64_t year_from_unix_seconds(uint64_t unix_seconds) {
    uint64_t days = unix_seconds / 86400;  // 秒 → 天

    // 估算年份：使用格里高利平均年长 365.2425 天
    uint64_t year = (days * 10000 + 1756461) / 3652425;

    // 精调：检查该年起始天数与实际天数是否匹配
    while (true) {
        uint64_t days_to_year = days_to_year_start(year);
        uint64_t days_in_this_year = days_in_year(year);

        if (days < days_to_year) {
            --year;
            continue;
        }
        if (days >= days_to_year + days_in_this_year) {
            ++year;
            continue;
        }
        break;
    }
    return year;
}
//--------------------------

void guarantyrwa::_calculate_yield_due( const fundplan_t& plan, asset& due ) {
    uint32_t last_year = year_from_unix_seconds( current_time_point().sec_since_epoch() ) - 1;
    auto lastyieldlog = yield_log_t( last_year );
    CHECKC( _db_yield.get( plan.id, lastyieldlog ), err::RECORD_NOT_FOUND, "yield log not found for plan: " 
                                                        + to_string( plan.id ) + " on last year: " + to_string( last_year ) )

    auto min_year_total = plan.guaranteed_yield_apr * plan.goal_quantity / 100;
    if ( lastyieldlog.year_total_quantity >= min_year_total ) return;
    due = min_year_total - lastyieldlog.year_total_quantity;

    CHECKC( due.amount > 0, err::NOT_POSITIVE, "no yield due for plan: " + to_string( plan.id ) + " on last year: " 
                                                + to_string( last_year ) )
}

// --------------------------
// purpose: stake in guarantee funds for a particular RWA plan in memo
void guarantyrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    if (from == _self || to != _self) return;

	CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo params format: plan:xxx
    auto parts = split(memo, ":");
    CHECKC( parts.size() == 2, err::INVALID_FORMAT, "invalid memo format" )
    auto plan_id = (uint64_t) stoi(string(parts[1]));
    auto plan = fundplan_t( plan_id );

    // update guaranty stats
    auto now = time_point_sec( current_time_point() );
    auto guaranty_stats = guaranty_stats_t( plan_id );
    if ( !_db.get( guaranty_stats ) ) {
        guaranty_stats.plan_id                     = plan_id;
        guaranty_stats.total_guarantee_funds       = quantity;
        guaranty_stats.available_guarantee_funds   = quantity;
        guaranty_stats.used_guarantee_funds        = asset{0, quantity.symbol};
        guaranty_stats.created_at                  = now;
        guaranty_stats.updated_at                  = now;

    } else {
        guaranty_stats.total_guarantee_funds       += quantity;
        guaranty_stats.available_guarantee_funds   += quantity;
        guaranty_stats.updated_at                  = now;
    }
    _db.set( guaranty_stats );

    // update guaranty record
    auto stake = guarantor_stake_t( plan_id );
    if ( !_db.get( from.value, stake ) ) {
        stake.plan_id                               = plan_id;
        stake.total_funds                           = quantity;
        stake.created_at                            = now;
        stake.updated_at                            = now;

    } else {
        stake.total_funds                           += quantity;
        stake.updated_at                            = now;
    }
    _db.set( from.value, stake );

}

// purpose: pay out to RWA investors from guaranty funds for a particular RWA plan
//          to ensure the promised yield APY is met
void guarantyrwa::guarantpay( const name& submitter, const uint64_t& plan_id ) {
    require_auth( submitter );

    //check plan
    auto plan = fundplan_t( plan_id );
    CHECKC( _db_invest.get( plan ), err::RECORD_NOT_FOUND, "plan not found: " + to_string( plan_id ) )
    CHECKC( plan.status == PlanStatus::SUCCESS, err::STATUS_ERROR, "plan not in SUCCESS mode" )

    //check yield due in last year
    auto yield_due = asset(0, plan.goal_quantity.symbol);
    _calculate_yield_due( plan, yield_due );
    
    //check guaranty stats
    auto stats = guaranty_stats_t( plan_id );
    CHECKC( _db.get( stats ), err::RECORD_NOT_FOUND, "guaranty stats not found for plan: " + to_string( plan_id ) )
    CHECKC( stats.available_guarantee_funds.amount > 0, err::QUANTITY_INSUFFICIENT, "insufficient guaranty funds" )

    if ( stats.available_guarantee_funds < yield_due ) {
        yield_due = stats.available_guarantee_funds;
    }

    //deduct in guaranty stats
    stats.available_guarantee_funds -= yield_due;
    stats.used_guarantee_funds      += yield_due;
    stats.updated_at                = time_point_sec( current_time_point() );
    _db.set( stats );

    //distribute yield to investors via stake.rwa
    //transfer out from guaranty.rwa to stake.rwa
    global_singleton _invest_global(_gstate.invest_contract, _gstate.invest_contract.value);
    global_t _invest_gstate = _invest_global.get();

    auto bank = plan.goal_asset_contract;
    auto to = _invest_gstate.stake_contract;
    auto memo = string("Guaranty pay yield for RWA plan: ") + std::to_string(plan_id);
    transfer_out( bank, to, yield_due, memo );

    auto year = year_from_unix_seconds( current_time_point().sec_since_epoch() );
    auto payment = plan_payment_t( year );
    if ( !_db.get( plan_id, payment ) ) {
        payment.total_paid = yield_due;

    } else {
        payment.total_paid += yield_due;
    }
    _db.set( plan_id, payment );
}

void guarantyrwa::withdraw(const name& guarantor, const uint64_t& plan_id, const asset& quantity) {
    require_auth( guarantor );

    CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    // check plan exists
    auto plan = fundplan_t( plan_id );
    CHECKC( _db_invest.get( plan ), err::RECORD_NOT_FOUND, "plan not found: " + to_string( plan_id ) )

    // symbol must match plan's asset
    CHECKC( quantity.symbol == plan.goal_quantity.symbol, err::SYMBOL_MISMATCH, "token symbol mismatch" )

    // stats exist and have enough available funds
    auto stats = guaranty_stats_t( plan_id );
    CHECKC( _db.get( stats ), err::RECORD_NOT_FOUND, "guaranty stats not found for plan: " + to_string( plan_id ) )
    CHECKC( stats.available_guarantee_funds >= quantity, err::QUANTITY_INSUFFICIENT, "insufficient available guaranty funds" )

    // guarantor stake record exists and has enough total_funds
    auto stake = guarantor_stake_t( plan_id );
    CHECKC( _db.get( guarantor.value, stake ), err::RECORD_NOT_FOUND, "guarantor stake not found for: " + guarantor.to_string() )
    CHECKC( stake.total_funds >= quantity, err::QUANTITY_INSUFFICIENT, "guarantor stake insufficient" )

    // deduct amounts and update DB
    stats.available_guarantee_funds -= quantity;
    stats.total_guarantee_funds     -= quantity;
    stats.updated_at                = time_point_sec( current_time_point() );
    _db.set( stats );

    stake.total_funds   -= quantity;
    stake.updated_at    = time_point_sec( current_time_point() );
    _db.set( guarantor.value, stake );

    // transfer out to guarantor
    auto bank = plan.goal_asset_contract;
    auto memo = string("Withdraw guaranty funds for RWA plan: ") + std::to_string( plan_id );
    transfer_out( bank, guarantor, quantity, memo );
}