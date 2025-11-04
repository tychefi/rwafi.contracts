#include "guarantyrwadb.hpp"
#include <wasm_db.hpp>

using namespace std;
using namespace wasm::db;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT       = 0,
   TYPE_INVALID         = 1,
   QUANTITY_INSUFFICIENT= 3,
   NOT_POSITIVE         = 4,
   SYMBOL_MISMATCH      = 5,
   EXPIRED              = 6,
   STATUS_ERROR         = 7,
   RECORD_NOT_FOUND      = 8,
   NOT_REPEAT_RECEIVE   = 9,
   NOT_EXPIRED          = 10,
   ACCOUNT_INVALID      = 11,
   VAILD_TIME_INVALID   = 13,
   MIN_UNIT_INVALID     = 14,
   UNDER_MAINTENANCE    = 17,
   NONE_DELETED         = 19,
   IN_THE_WHITELIST     = 20,
   NON_RENEWAL          = 21,
};


class [[eosio::contract("guarantyrwa")]] guarantyrwa: public eosio::contract {
private:
    dbc                 _db;
    dbc                 _db_invest;
    dbc                 _db_yield;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    guarantyrwa(eosio::name receiver, eosio::name code, datastream<const char*> ds):
        _db(_self),
        _db_invest(_self),
        _db_yield(_self),
        contract(receiver, code, ds),
        _global(_self, _self.value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
        
        _db_invest = dbc( _gstate.invest_contract );
        _db_yield = dbc( _gstate.yield_contract );
    }

    ~guarantyrwa() {
        _global.set(_gstate, get_self());
    }

    [[eosio::on_notify("flon.token::transfer")]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    ACTION init(const name& admin ) {
        require_auth( _self );
        CHECKC( is_account(admin), err::ACCOUNT_INVALID, "account invalid" );

        _gstate.admin = admin;
    }

private:
    // void _token_transfer( const name& from, const name& to, const asset& quantity, const string& memo );
    asset _calculate_yield_due( const uint64_t& plan_id );


}; //contract guarantyrwa