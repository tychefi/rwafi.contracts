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

namespace wasm { namespace db {

#define TBL struct [[eosio::table, eosio::contract("stakerwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("stakerwa")]]

TBL invest_stake_t {               //scope: investor
    uint64_t        plan_id;            //PK
    asset           balance;

    uint64_t primary_key() const { return plan_id; }

    invest_stake_t(){}
    invest_stake_t( const uint64_t& pid ): plan_id(pid){}

    typedef eosio::multi_index<"investstakes"_n, invest_stake_t> idx_t;

    EOSLIB_SERIALIZE( invest_stake_t, (plan_id)(balance) )
};

} }