#include "investrwadb.hpp"
#include <wasm_db.hpp>
#include "flon.token.hpp"

using namespace std;
using namespace wasm::db;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT       = 0,
   TYPE_INVALID         = 1,
   FEE_NOT_FOUND        = 2,
   QUANTITY_NOT_ENOUGH  = 3,
   NOT_POSITIVE         = 4,
   SYMBOL_MISMATCH      = 5,
   EXPIRED              = 6,
   PWHASH_INVALID       = 7,
   RECORD_NO_FOUND      = 8,
   NOT_REPEAT_RECEIVE   = 9,
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
   INVALID_STATUS       = 31
};

enum class investrwa_type: uint8_t {
   RANDOM       = 0,
   MEAN         = 1,
   DID_RANDOM   = 10,
   DID_MEAN     = 11

};

class [[eosio::contract("investrwa")]] investrwa: public eosio::contract {
private:
    dbc                 _db;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    investrwa(eosio::name receiver, eosio::name code, datastream<const char*> ds):
        _db(_self),
        contract(receiver, code, ds),
        _global(_self, _self.value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
    }

    ~investrwa() {
        _global.set(_gstate, get_self());
        _global2.set(_gstate2, get_self());
    }

    ACTION addtoken(const name& contract, const symbol& sym, const time_point_sec& expired_time);
    ACTION deltoken( const symbol& sym );
    ACTION onshelf( const symbol& sym, const bool& onshelf );

    ACTION createplan( const name& creator,
                       const string& title,
                       const name& goal_asset_contract,
                       const asset& goal_quantity,
                       const name& receipt_asset_contract,
                       const asset& receipt_quantity_per_unit,
                       const uint8_t& invest_unit_size,
                       const uint8_t& soft_cap_percent,
                       const uint8_t& hard_cap_percent,
                       const time_point& start_time,
                       const time_point& end_time,
                       const uint16_t& return_years,
                       const double& guaranteed_yield_apr );

    ACTION cancelplan( const name& creator, const uint64_t& plan_id );

    // Invest with some allowed token
    [[eosio::on_notify("*::transfer")]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    ACTION refund( const name& investor, const uint64_t& plan_id ); //refund with receipt token staked in stake contract


    ACTION claiminvestrwa( const name& claimer, const name& code, const string& pwhash );
    ACTION cancel( const name& code );
    ACTION delclaims( const uint64_t& max_rows );

    ACTION init(const name& admin, const uint16_t& hours, const bool& did_supported, const uint64_t& did_id, const name& did_contract) {
        require_auth( _self );
        CHECKC( is_account(admin), err::ACCOUNT_INVALID, "account invalid" );
        CHECKC( hours > 0, err::VAILD_TIME_INVALID, "valid time must be positive" );

        _gstate.admin = admin;
        _gstate.expire_hours = hours;
        _gstate.did_supported = did_supported;
    }

private:    
    void _process_refund( const name& from, const name& to, const asset& quantity, const string& memo, fundplan_t& plan );
    void _process_investment( const name& from, const name& to, const asset& quantity, const string& memo, fundplan_t& plan );
    void _update_plan_status( fundplan_t& plan );

    asset _get_balance(const name& token_contract, const name& owner, const symbol& sym);
    asset _get_investor_stake_balance( const name& investor, const uint64_t& plan_id );
    asset _get_collateral_stake_balance( const name& guanrantor, const uint64_t& plan_id );

}; //contract investrwa