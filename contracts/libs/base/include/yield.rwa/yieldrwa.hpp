#pragma once
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <flon/wasm_db.hpp>
#include "yieldrwadb.hpp"

using namespace eosio;
using namespace std;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT        = 0,
   TYPE_INVALID          = 1,
   QUANTITY_INSUFFICIENT = 3,
   NOT_POSITIVE          = 4,
   SYMBOL_MISMATCH       = 5,
   EXPIRED               = 6,
   PWHASH_INVALID        = 7,
   RECORD_NOT_FOUND      = 8,
   NOT_REPEAT_RECEIVE    = 9,
   NOT_EXPIRED           = 10,
   ACCOUNT_INVALID       = 11,
   PARAM_ERROR           = 12,
};

namespace rwafi {

class [[eosio::contract("yield.rwa")]] yieldrwa : public eosio::contract {
private:
    dbc                 _db;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    // ========== Actions ==========
    ACTION init(const name& admin);
    ACTION updateconfig(const name& key, const uint8_t& value);

    // === External Query ===
    asset get_yearly_yield(const uint64_t& plan_id, const uint64_t& year, const string& type = "total") const;


};

} // namespace rwafi