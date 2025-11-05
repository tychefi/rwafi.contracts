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

using namespace eosio;

#define TBL struct [[eosio::table, eosio::contract("guarantyrwa")]]
#define NTBL(name) struct [[eosio::table(name), eosio::contract("guarantyrwa")]]

TBL guaranty_stats_t {                      //scope: _self
    uint64_t        plan_id;                //PK
    asset           total_guarantee_funds;
    asset           available_guarantee_funds;
    asset           used_guarantee_funds;
    time_point      created_at;
    time_point      updated_at;

    uint64_t primary_key() const { return plan_id; }

    guaranty_stats_t(){}
    guaranty_stats_t( const uint64_t& pid ): plan_id(pid) {}

    typedef eosio::multi_index<"guarantystat"_n, guaranty_stats_t > idx_t;

    EOSLIB_SERIALIZE( guaranty_stats_t, (plan_id)(total_guarantee_funds)(available_guarantee_funds)(used_guarantee_funds)
                                        (created_at)(updated_at) )
};

TBL guarantor_stake_t {                     // scope: guanrantor
    uint64_t        plan_id;                // PK
    asset           total_funds;            // yield shared between guarantors based on this amount against total in stats
    time_point      created_at;
    time_point      updated_at;

    uint64_t primary_key() const { return plan_id; }

    guarantor_stake_t(){}
    guarantor_stake_t( const uint64_t& pid ): plan_id(pid) {}

    typedef eosio::multi_index<"stakes"_n, guarantor_stake_t> idx_t;

    EOSLIB_SERIALIZE( guarantor_stake_t, (plan_id)(total_funds)(created_at)(updated_at) )
};

TBL plan_payment_t {                        // scope: plan_id
    uint64_t        year;                   // PK, paid in this year, but for the last year's due
    asset           total_paid;          // total amount paid in this year for the last year, out of the guanranty funds
    time_point      created_at;

    uint64_t primary_key() const { return year; }

    plan_payment_t(){}
    plan_payment_t( const uint64_t y ): year(y) {}

    typedef eosio::multi_index<"payments"_n, plan_payment_t > idx_t;

    EOSLIB_SERIALIZE( plan_payment_t, (year)(total_paid)(created_at) )
};

} }