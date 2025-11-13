#include "investrwadb.hpp"
#include "flon/flon.token.hpp"
#include "flon/utils.hpp"

using namespace std;
using namespace wasm::db;

namespace rwafi {

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT       = 0,
   TYPE_INVALID         = 1,
   FEE_NOT_FOUND        = 2,
   QUANTITY_INSUFFICIENT  = 3,
   NOT_POSITIVE         = 4,
   SYMBOL_MISMATCH      = 5,
   EXPIRED              = 6,
   PWHASH_INVALID       = 7,
   RECORD_NOT_FOUND     = 8,
   RECORD_EXISTS        = 9,
   NOT_EXPIRED          = 10,
   ACCOUNT_INVALID      = 11,
   FEE_NOT_POSITIVE     = 12,
   VAILD_TIME_INVALID   = 13,
   MIN_UNIT_INVALID     = 14,
   RED_PACK_EXIST       = 15,
   NO_AUTH              = 16,
   UNDER_MAINTENANCE    = 17,
   NONE_DELETED         = 19,
   IN_THE_WHITELIST     = 20,
   NON_RENEWAL          = 21,
   INVALID_STATUS       = 31,
   CONTRACT_MISMATCH    = 32,
   PARAM_ERROR          = 33,
   INVALID_SYMBOL       = 34,
   TOKEN_NOT_ALLOWED    = 35
};

enum class investrwa_type: uint8_t {
   RANDOM       = 0,
   MEAN         = 1,
   DID_RANDOM   = 10,
   DID_MEAN     = 11

};

class [[eosio::contract("invest.rwa")]] investrwa: public eosio::contract {
public:
    using contract::contract;

    ACTION addtoken( const name& contract, const symbol& sym );
    ACTION deltoken( const symbol& sym );
    ACTION onshelf( const symbol& sym, const bool& onshelf );
    ACTION createplan(
                        const name& creator,
                        const string& title,
                        const name& goal_asset_contract,
                        const asset& goal_quantity,
                        const name& receipt_asset_contract,
                        const asset& receipt_quantity_per_unit,
                        const uint8_t& soft_cap_percent,
                        const uint8_t& hard_cap_percent,
                        const time_point& start_time,
                        const time_point& end_time,
                        const uint16_t& return_months,
                        const uint32_t& guaranteed_yield_apr  );

    ACTION cancelplan( const name& creator, const uint64_t& plan_id );

    using addtoken_action    = eosio::action_wrapper<"addtoken"_n, &investrwa::addtoken>;


};
} // namespace rwafi