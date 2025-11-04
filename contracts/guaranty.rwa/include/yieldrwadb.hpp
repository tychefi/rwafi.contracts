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
static constexpr uint64_t seconds_per_month     = 24 * 3600 * 30;

#ifndef DAY_SECONDS_FOR_TEST
static constexpr uint64_t DAY_SECONDS           = 24 * 60 * 60;
#else
#warning "DAY_SECONDS_FOR_TEST should be used only for test!!!"
static constexpr uint64_t DAY_SECONDS           = DAY_SECONDS_FOR_TEST;
#endif//DAY_SECONDS_FOR_TEST

static constexpr uint32_t MAX_TITLE_SIZE        = 64;
static constexpr uint8_t    EXPIRY_HOURS        = 12;

static constexpr name STAKE_POOL            { "stake.rwa"_n };
static constexpr name GUARANTY_POOL         { "guarant.rwa"_n };
static constexpr name SWAP_POOL              { "flon.swap"_n };

namespace wasm { namespace db {

#define TBL [[eosio::table, eosio::contract("yieldrwa")]]
#define NTBL(name) [[eosio::table(name), eosio::contract("yieldrwa")]]

struct NTBL("global") global_t {
    name                admin;

    map<name, uin8_t>   yield_split_conf = {
        { STAKE_POOL,       80 },
        { SWAP_POOL,         10 },
        { GUARANTY_POOL,    10 }
    }

    EOSLIB_SERIALIZE( global_t, (admin)(yield_split_conf) )
};
typedef eosio::singleton< "global"_n, global_t > global_singleton;



TBL yield_log_t {                       //scope: plan_id
    uint64_t        year;               //PK
    uint64_t        year_no = 1;
    asset           year_total_quantity;
    asset           plan_total_quantity;
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return id; }

    yield_log_t(){}
    yield_log_t( const uint64_t y ): year(y){}

    typedef eosio::multi_index<"yieldlogs"_n, yield_log_t> idx_t;

    EOSLIB_SERIALIZE( yield_log_t, (year)(year_no)(year_total_quantity)(plan_total_quantity)(created_at)(updated_at) )
};


} }