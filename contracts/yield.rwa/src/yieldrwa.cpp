
#include <flon.token.hpp>
#include "yieldrwa.hpp"
#include "utils.hpp"
#include <algorithm>
#include <chrono>
#include <eosio/transaction.hpp>
#include <eosio/crypto.hpp>

using std::chrono::system_clock;
using namespace wasm;

static constexpr eosio::name active_permission{"active"_n};

// transfer out from contract self
#define TRANSFER_OUT(bank, to, quantity, memo) \
    { action(permission_level{get_self(), "active"_n }, bank, "transfer"_n, std::make_tuple( _self, to, quantity, memo )).send(); }

inline int64_t get_precision(const symbol &s) {
    int64_t digit = s.precision();
    CHECKC(digit >= 0 && digit <= 18, err::SYMBOL_MISMATCH, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
    return calc_precision(digit);
}

inline int64_t get_precision(const asset &a) {
    return get_precision(a.symbol);
}

void yieldrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    _token_transfer( from, to, quantity, memo );
}


void yieldrwa::_token_transfer( const name& from, const name& to, const asset& quantity, const string& memo )
{
    if (from == _self || to != _self) return;

	CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo params format:
    //${pwhash} : count : type : code
    //asset:contract
    auto parts = split(memo, ":");
    if (parts.size() == 4) {    //deposit tokens into the yieldrwa
        
        name receiver_contract = get_first_receiver();
        
        tokenlist_t::idx_t tokenlist_tbl(_self, _self.value);
        auto tokenlist_index = tokenlist_tbl.get_index<"symcontract"_n>();
        uint128_t sec_index = get_unionid(receiver_contract, quantity.symbol.raw());
        auto tokenlist_iter = tokenlist_index.find(sec_index);
        CHECKC( tokenlist_iter != tokenlist_index.end(), err::NON_RENEWAL, "non-renewal" );
        CHECKC( tokenlist_iter->expired_time > time_point_sec(current_time_point()), err::NON_RENEWAL, "non-renewal" );

        auto code = name(parts[3]);
        yieldrwa_t yieldrwa(code);
        CHECKC(!_db.get(yieldrwa), err::RED_PACK_EXIST, "code is already exists");
        
        auto count = stoi(string(parts[1]));
        auto rp_type = (yieldrwa_type) stoi(string(parts[2]));
        CHECKC( rp_type == yieldrwa_type::RANDOM || rp_type == yieldrwa_type::MEAN ||
                rp_type == yieldrwa_type::DID_RANDOM || rp_type == yieldrwa_type::DID_MEAN, err::TYPE_INVALID, "yieldrwa type invalid" )

        auto symb = quantity.symbol.code().to_string();
        auto is_did_type = ( rp_type == yieldrwa_type::DID_RANDOM || rp_type == yieldrwa_type::DID_MEAN );
        if (is_did_type) {
            CHECKC( _gstate.did_supported, err::UNDER_MAINTENANCE, "did yieldrwa not enabled" )
            CHECKC( symb == "flon" || symb == "MUSDT" || symb == "MUSDC" || symb == "TYCHE", err::DID_PACK_SYMBOL_ERR, "DID yieldrwa tokens can only be flon|MUSDT|MUSDC|TYCHE" )

            // auto min_quant = ( symb == "flon" ) ? 1 : 10;
            // CHECKC( quantity.amount / get_precision(quantity) >= min_quant, err::QUANTITY_NOT_ENOUGH, "Minimal total " + to_string(min_quant) + symb + " required" )

        } else {
            CHECKC( quantity.amount / count >= 100, err::QUANTITY_NOT_ENOUGH,  "Minimal unit 100 " + symb + " required" )
        }

        // CHECKC( symb == "flon" || symb == "MUSDT" || symb == "MUSDC", err::DID_PACK_SYMBOL_ERR, "yieldrwa can only be flon|MUSDT|MUSDC" )
        // CHECKC( count <= 200, err::INVALID_FORMAT, "yieldrwa count cannot be greater than 200" )
        // auto min_quant = ( symb == "flon" ) ? 1 : 10;
        // CHECKC( quantity.amount / get_precision(quantity) >= min_quant, err::QUANTITY_NOT_ENOUGH, "Minimal total " + to_string(min_quant) + symb + " required" )


        yieldrwa_t::idx_t yieldrwas(_self, _self.value);
        auto now = current_time_point();
        yieldrwas.emplace(_self, [&](auto &row){
            row.code 					    = code;
            row.sender 			            = from;
            row.pw_hash                     = string( parts[0] ) + ":" + get_first_receiver().to_string();
            row.total_quantity              = quantity;
            row.receiver_count		        = count;
            row.remain_quantity		        = quantity;
            row.remain_count	            = count;
            row.status			            = yieldrwa_status::CREATED;
            row.type			            = (uint16_t) rp_type;
            row.created_at                  = now;
            row.updated_at                  = now; 
        });

    } else if (parts.size() == 2) {  //pay fees of issuing the yieldrwa out
        name receiver_contract = get_first_receiver();
        extended_asset extended_quantity = extended_asset(quantity, receiver_contract); 
        CHECKC(extended_quantity >= _gstate2.fee, err::QUANTITY_NOT_ENOUGH, "insufficient payment for fees");

        symbol redpcak_symbol= symbol_from_string(parts[0]);
        name contract = name(parts[1]);
        asset value = flon::token::get_supply(contract, redpcak_symbol.code());
        CHECKC( value.amount > 0, err::SYMBOL_MISMATCH, "symbol mismatch" );
        CHECKC( value.symbol == redpcak_symbol, err::SYMBOL_MISMATCH, "symbol mismatch" );
        
        tokenlist_t::idx_t tokenlist_tbl(_self, _self.value);
        auto tokenlist_index = tokenlist_tbl.get_index<"symcontract"_n>();
        uint128_t sec_index = get_unionid(contract, redpcak_symbol.raw());
        auto tokenlist_iter = tokenlist_index.find(sec_index);
        bool found = tokenlist_iter != tokenlist_index.end();
        if (found)
            CHECKC( tokenlist_iter->expired_time < time_point_sec(current_time_point()), err::NOT_EXPIRED, "not expired" );
            
        auto tid = found ? tokenlist_iter->id : tokenlist_tbl.available_primary_key();
        tokenlist_t token(tid);
        token.expired_time  = time_point_sec(current_time_point()) + seconds_per_month;
        token.sym           = redpcak_symbol;
        token.contract      = contract;
        _db.set(token, _self);

    } else {
        CHECKC( false, err::INVALID_FORMAT, "invalid memo format" );
    }
}