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

static constexpr name STAKE_POOL            { "stake.rwa"_n };
static constexpr name GUARANTY_POOL         { "guarant.rwa"_n };
static constexpr name SWAP_POOL              { "flon.swap"_n };

namespace wasm { namespace db {

using namespace eosio;

#define TBL struct [[eosio::table, eosio::contract("yieldrwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("yieldrwa")]]

// NTBL("global") global_t {
//     name                admin;

//     map<name, uint8_t>   yield_split_conf = {
//         { STAKE_POOL,       80 },
//         { SWAP_POOL,        10 },
//         { GUARANTY_POOL,    10 }
//     };

//     EOSLIB_SERIALIZE( global_t, (admin)(yield_split_conf) )
// };
// typedef eosio::singleton< "global"_n, global_t > global_singleton;

TBL yield_log_t {                   //scope: plan_id
    uint64_t        year;           //PK
    uint64_t        year_no = 0;
    asset           year_total_quantity;
    asset           plan_total_quantity;
    time_point_sec  created_at;
    time_point_sec  updated_at;

    uint64_t primary_key() const { return year; }

    yield_log_t(){}
    yield_log_t( const uint64_t& y ): year(y){}

    typedef eosio::multi_index<"yieldlogs"_n, yield_log_t> idx_t;

    EOSLIB_SERIALIZE( yield_log_t, (year)(year_no)(year_total_quantity)(plan_total_quantity)(created_at)(updated_at) )
};


} }