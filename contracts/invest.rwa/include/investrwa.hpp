#include "investrwadb.hpp"
#include "flon/flon.token.hpp"
#include "flon/utils.hpp"
#include <initializer_list>
#include <set>
#include <vector>

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
   TOKEN_NOT_ALLOWED    = 35,
   SYSTEM_ERROR         = 36
};

enum class investrwa_type: uint8_t {
   RANDOM       = 0,
   MEAN         = 1,
   DID_RANDOM   = 10,
   DID_MEAN     = 11

};

class [[eosio::contract("invest.rwa")]] investrwa: public eosio::contract {
private:
    dbc                 _db;
    dbc                 _db_stake;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    investrwa(eosio::name receiver, eosio::name code, datastream<const char*> ds):
        _db(_self),
        _db_stake(_self),
        contract(receiver, code, ds),
        _global(_self, _self.value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
    }

    ~investrwa() {
            _global.set(_gstate, get_self());
    }


    ACTION addtoken( const name& contract, const symbol& sym );
    ACTION deltoken( const symbol& sym );
    ACTION onshelf( const symbol& sym, const bool& onshelf );
    ACTION createplan(
                        const name& creator,
                        const string& title,
                        const asset& goal_quantity,
                        const asset& min_investment,
                        const name& receipt_asset_contract,
                        const asset& receipt_quantity_per_unit,
                        const uint8_t& soft_cap_percent,
                        const uint8_t& hard_cap_percent,
                        const time_point& start_time,
                        const time_point& end_time,
                        const uint16_t& return_months,
                        const uint32_t& guaranteed_yield_apr  );

    ACTION cancelplan( const name& creator, const uint64_t& plan_id );
    ACTION refreshstat(const name& submitter,const uint64_t& plan_id);
    ACTION batchrefresh(const name& submitter, const std::vector<uint64_t>& plan_ids, const uint64_t& now_ts);
    ACTION setoracle(const name& account, const bool& enabled);
    ACTION liquidity(const uint64_t& plan_id,const name& tpcode );
    ACTION withdraw(const name& creator, const uint64_t& plan_id, const name& to, const asset& quantity);

    ACTION notify(const name& contract,const name& from,const name& to,const asset& quantity,const string& memo,const string& type,const uint64_t& plan_id);


    [[eosio::on_notify("rwafi.token::transfer")]]
    void on_rwafi_transfer(const name& from, const name& to, const asset& quantity, const std::string& memo);

    [[eosio::on_notify("sing.token::transfer")]]
    void on_sing_transfer(const name& from, const name& to, const asset& quantity, const std::string& memo);

    ACTION init(const name& admin) {
        require_auth( _self );
        CHECKC( is_account(admin), err::ACCOUNT_INVALID, "account invalid" );
        _gstate.admin = admin;

    }

    ACTION delplan(const uint64_t& plan_id);

    using addtoken_action           = eosio::action_wrapper<"addtoken"_n, &investrwa::addtoken>;
    using liquidity_action          = eosio::action_wrapper<"liquidity"_n, &investrwa::liquidity>;
    using refreshstat_action        = eosio::action_wrapper<"refreshstat"_n, &investrwa::refreshstat>;
    using withdraw_action           = eosio::action_wrapper<"withdraw"_n, &investrwa::withdraw>;
    using notify_action             = eosio::action_wrapper<"notify"_n, &investrwa::notify>;

private:

    void _token_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    void _process_refund( const name& investor, const asset& quantity, fundplan_t& plan );
    void _process_investment( const name& from, const asset& quantity, fundplan_t& plan );
    void _update_plan_status( fundplan_t& plan );
    void _refresh_and_require_status(fundplan_t& plan, std::initializer_list<eosio::name> allowed, const char* err_msg);

    void _create_liquidity(fundplan_t& plan);
    asset _calc_receipt_liq_from_goal(const asset& goal_liq, const fundplan_t& plan);
    asset _convert_precision(const asset& src, const symbol& dst_sym);

    asset _calc_refund_amount( const asset& receipt_qty,const fundplan_t& plan);
    int64_t pow10(uint8_t p);
    int64_t _goal_unit(const fundplan_t& plan);

};
} // namespace rwafi
