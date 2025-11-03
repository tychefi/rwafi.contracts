
#include <flon.token.hpp>
#include "guarantyrwa.hpp"
#include "investrwa.hpp"
#include "investrwadb.hpp"
#include "yieldrwadb.hpp"

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

// inline int64_t get_precision(const symbol &s) {
//     int64_t digit = s.precision();
//     CHECKC(digit >= 0 && digit <= 18, err::SYMBOL_MISMATCH, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
//     return calc_precision(digit);
// }

// inline int64_t get_precision(const asset &a) {
//     return get_precision(a.symbol);
// }

//--------------------------
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
//--------------------------

void guarantyrwa::_calculate_yield_due( const uint64_t& plan_id, asset& due ) {
    // get plan yield distriubtion of last year from yield
    uint32_t last_yeah = year_from_unix_seconds( current_time_point().sec_since_epoch() ) - 1;

    uint32_t year = year_from_unix_seconds( current_time_point().sec_since_epoch() );
    auto yieldlog = yield_log_t( year );
    if ( !_db.get( plan_id, yieldlog ) ) return;

    auto plan = fundplan_t( plan_id );
    CHECKC( _db.get( plan ), err::RECORD_NO_FOUND, "plan not found: " + to_string( plan_id ) )

    auto min_year_total = plan.guaranteed_yield_apr * goal_quantity / 100;
    if ( yield_log.year_total_quantity >= min_year_total ) return;
    due = min_year_total - yield_log.year_total_quantity;
}

// --------------------------
// purpose: stake in guarantee funds for a particular RWA plan in memo
void guarantyrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    if (from == _self || to != _self) return;

	CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo params format: plan:xxx
    auto parts = split(memo, ":");
    CHECKC( parts.zie() == 2, err::INVALID_FORMAT, "invalid memo format" )
    auto plan_id = (uint64_t) stoi(string(parts[1]));
    auto plan = guaranty_t( plan_id );

    // update guaranty stats
    auto stats_tbl = _db.get_table< guaranty_stats_t >( _self, _self.value );
    auto stats_itr = stats_tbl.find( plan_id );
    if ( stats_itr == stats_tbl.end() ) {
        stats_tbl.emplace( get_self(), [&]( auto& row ) {
            row.plan_id                 = plan_id;
            row.total_guarantee_funds   = quantity;
            row.created_at              = time_point_sec( current_time_point() );
            row.updated_at              = time_point_sec( current_time_point() );
        });
    } else {
        stats_tbl.modify( stats_itr, get_self(), [&]( auto& row ) {
            row.total_guarantee_funds   += quantity;
            row.updated_at              = time_point_sec( current_time_point() );
        });
    }

    // update guaranty record
    auto guaranty_tbl = _db.get_table< guaranty_t >( _self, from.value );
    auto guaranty_itr = guaranty_tbl.find( plan_id );
    if ( guaranty_itr == guaranty_tbl.end() ) {
        guaranty_tbl.emplace( get_self(), [&]( auto& row ) {
            row.plan_id                 = plan_id;
            row.total_guarantee_funds   = quantity;
            row.created_at              = time_point_sec( current_time_point() );
            row.updated_at              = time_point_sec( current_time_point() );
        });
    } else {
        guaranty_tbl.modify( guaranty_itr, get_self(), [&]( auto& row ) {
            row.total_guarantee_funds   += quantity;
            row.updated_at              = time_point_sec( current_time_point() );
        });
    }

}

// purpose: pay out to RWA investors from guaranty funds for a particular RWA plan
//          to ensure the promised yield APY is met
void guarantyrwa::guarantpay( const name& submitter, const uint64_t& plan_id ) {
    require_auth( submitter );

    //check plan
    auto plan = plan_t( plan_id );
    CHECKC( plan.status == PlanStatus::ACTIVE, err::RECORD_NO_FOUND, "plan not active" )

    //check yield due
    auto yield_due = _calculate_yield_due( plan_id );
    CHECKC( yield_due.amount > 0, err::NOT_POSITIVE, "no yield due" )

    //check guaranty stats
    auto stats_tbl = _db.get_table< guaranty_stats_t >( _self, _self.value );
    auto stats_itr = stats_tbl.find( plan_id );
    CHECKC( stats_itr != stats_tbl.end(), err::RECORD_NO_FOUND, "guaranty stats not found" )
    CHECKC( stats_itr->total_guarantee_funds.amount >= yield_due.amount, err::QUANTITY_INSUFFICIENT, "insufficient guaranty funds" )
    //deduct guaranty stats
    stats_tbl.modify( stats_itr, get_self(), [&]( auto& row ) {
        row.total_guarantee_funds   -= yield_due;
        row.updated_at              = time_point_sec( current_time_point() );
    });

    //distribute yield to investors via stake.rwa
    //transfer out from guaranty.rwa to stake.rwa
    auto bank = plan.goal_asset_contract;
    auto to = plan.stake_contract;
    auto memo = string("Guaranty pay yield for RWA plan: ") + std::to_string(plan_id);

    TRANSFER_OUT( bank, to, yield_due, memo )

}