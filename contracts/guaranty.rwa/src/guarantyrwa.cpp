
#include <flon.token.hpp>
#include "guarantyrwa.hpp"
#include "investrwa.hpp"
#include "investrwadb.hpp"

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

// --------------------------
// purpose: stake in guarantee funds for a particular RWA plan in memo
void guarantyrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    if (from == _self || to != _self) return;

	CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo params format:
    //${pwhash} : count : type : code
    //asset:contract
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
    TRANSFER_OUT( SYS_BANK, plan.stake_contract, yield_due, string("Guaranty pay yield for RWA plan ") + std::to_string(plan_id) )

}