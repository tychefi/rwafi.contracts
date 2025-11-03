#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/privileged.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>

using namespace eosio;
using namespace std;
using std::string;

// using namespace wasm;
#define SYMBOL(sym_code, precision) symbol(symbol_code(sym_code), precision)

static constexpr eosio::name active_perm        {"active"_n};
static constexpr symbol SYS_SYMBOL              = SYMBOL("flon", 8);
static constexpr name SYS_BANK                  { "flon.token"_n };
static constexpr uint32_t MIN_SINGLE_REDPACK    = 100;
static constexpr uint64_t seconds_per_month     = 24 * 3600 * 30;

#ifndef DAY_SECONDS_FOR_TEST
static constexpr uint64_t DAY_SECONDS           = 24 * 60 * 60;
#else
#warning "DAY_SECONDS_FOR_TEST should be used only for test!!!"
static constexpr uint64_t DAY_SECONDS           = DAY_SECONDS_FOR_TEST;
#endif//DAY_SECONDS_FOR_TEST

static constexpr uint32_t MAX_TITLE_SIZE        = 64;
static constexpr uint8_t    EXPIRY_HOURS        = 12;

namespace wasm { namespace db {

#define TBL [[eosio::table, eosio::contract("guarantyrwa")]]
#define TBL_NAME(name) [[eosio::table(name), eosio::contract("guarantyrwa")]]

struct TBL_NAME("global") global_t {
    name            admin;
    name            guarantor_pool = "guarantorpool"_n;

    EOSLIB_SERIALIZE( global_t, (admin) )
};
typedef eosio::singleton< "global"_n, global_t > global_singleton;

TBL guaranty_stats_t {                      //scope: _self
    uint64_t        plan_id;
    asset           total_guarantee_funds;
    time_point      created_at;
    time_point      updated_at;
    
    uint64_t primary_key() const { return plan_id; }

    typedef eosio::multi_index<"guarantystats"_n, guaranty_stats_t > idx_t;

    EOSLIB_SERIALIZE( guaranty_stats_t, (plan_id)(total_guarantee_funds)(created_at)(updated_at) )
}

TBL guaranty_t {                            // scope: guanrantor
    uint64_t        plan_id;                // PK
    asset           accmu_guarantor_funds;  // yield shared between guarantors
    asset           accmu_plan_funds;       // accumulated funds in the plan, wont change once finalized 
                                            // for calculating each guarantor's share

    time_point      created_at;
    time_point      updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_t(){}
    guaranty_t( const uint64_t& pid ): plan_id(pid) {}

    typedef eosio::multi_index<"guaranties"_n, guaranty_t
    > idx_t;

    EOSLIB_SERIALIZE( guaranty_t, (plan_id)(accmu_guarantor_funds)(accmu_plan_funds)(created_at)(updated_at) )
};

} }