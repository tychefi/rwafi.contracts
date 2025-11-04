#include "yieldrwadb.hpp"
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
   PWHASH_INVALID       = 7,
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


class [[eosio::contract("yieldrwa")]] yieldrwa: public eosio::contract {
private:
    dbc                 _db;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    yieldrwa(eosio::name receiver, eosio::name code, datastream<const char*> ds):
        _db(_self),
        contract(receiver, code, ds),
        _global(_self, _self.value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
    }

    ~yieldrwa() {
        _global.set(_gstate, get_self());
    }


    [[eosio::on_notify("*::transfer")]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    ACTION init( const name& admin );
    ACTION updateconfig( const name& key, const uint8_t& value );

private:
    void _perform_distribution(const name& bank, const asset& total);
    void _log_yield( const uint64_t& plan_id, const asset& quantity );

}; //contract yieldrwa