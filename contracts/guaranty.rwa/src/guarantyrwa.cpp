
#include <flon.token.hpp>
#include "guarantyrwa.hpp"
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


// issue-in op: transfer tokens to the contract and lock them according to the given plan
void guarantyrwa::on_atoken_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    _token_transfer( from, to, quantity, memo );
}

void guarantyrwa::_token_transfer( const name& from, const name& to, const asset& quantity, const string& memo )
{
    if (from == _self || to != _self) return;

	CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo params format:
    //${pwhash} : count : type : code
    //asset:contract
    auto parts = split(memo, ":");
    if (parts.size() == 4) {    //deposit tokens into the guarantyrwa
        
        name receiver_contract = get_first_receiver();
        
        tokenlist_t::idx_t tokenlist_tbl(_self, _self.value);
        auto tokenlist_index = tokenlist_tbl.get_index<"symcontract"_n>();
        uint128_t sec_index = get_unionid(receiver_contract, quantity.symbol.raw());
        auto tokenlist_iter = tokenlist_index.find(sec_index);
        CHECKC( tokenlist_iter != tokenlist_index.end(), err::NON_RENEWAL, "non-renewal" );
        CHECKC( tokenlist_iter->expired_time > time_point_sec(current_time_point()), err::NON_RENEWAL, "non-renewal" );

        auto code = name(parts[3]);
        guarantyrwa_t guarantyrwa(code);
        CHECKC(!_db.get(guarantyrwa), err::RED_PACK_EXIST, "code is already exists");
        
        auto count = stoi(string(parts[1]));
        auto rp_type = (guarantyrwa_type) stoi(string(parts[2]));
        CHECKC( rp_type == guarantyrwa_type::RANDOM || rp_type == guarantyrwa_type::MEAN ||
                rp_type == guarantyrwa_type::DID_RANDOM || rp_type == guarantyrwa_type::DID_MEAN, err::TYPE_INVALID, "guarantyrwa type invalid" )

        auto symb = quantity.symbol.code().to_string();
        auto is_did_type = ( rp_type == guarantyrwa_type::DID_RANDOM || rp_type == guarantyrwa_type::DID_MEAN );
        if (is_did_type) {
            CHECKC( _gstate.did_supported, err::UNDER_MAINTENANCE, "did guarantyrwa not enabled" )
            CHECKC( symb == "flon" || symb == "MUSDT" || symb == "MUSDC" || symb == "TYCHE", err::DID_PACK_SYMBOL_ERR, "DID guarantyrwa tokens can only be flon|MUSDT|MUSDC|TYCHE" )

            // auto min_quant = ( symb == "flon" ) ? 1 : 10;
            // CHECKC( quantity.amount / get_precision(quantity) >= min_quant, err::QUANTITY_INSUFFICIENT, "Minimal total " + to_string(min_quant) + symb + " required" )

        } else {
            CHECKC( quantity.amount / count >= 100, err::QUANTITY_INSUFFICIENT,  "Minimal unit 100 " + symb + " required" )
        }

        // CHECKC( symb == "flon" || symb == "MUSDT" || symb == "MUSDC", err::DID_PACK_SYMBOL_ERR, "guarantyrwa can only be flon|MUSDT|MUSDC" )
        // CHECKC( count <= 200, err::INVALID_FORMAT, "guarantyrwa count cannot be greater than 200" )
        // auto min_quant = ( symb == "flon" ) ? 1 : 10;
        // CHECKC( quantity.amount / get_precision(quantity) >= min_quant, err::QUANTITY_INSUFFICIENT, "Minimal total " + to_string(min_quant) + symb + " required" )


        guarantyrwa_t::idx_t guarantyrwas(_self, _self.value);
        auto now = current_time_point();
        guarantyrwas.emplace(_self, [&](auto &row){
            row.code 					    = code;
            row.sender 			            = from;
            row.pw_hash                     = string( parts[0] ) + ":" + get_first_receiver().to_string();
            row.total_quantity              = quantity;
            row.receiver_count		        = count;
            row.remain_quantity		        = quantity;
            row.remain_count	            = count;
            row.status			            = guarantyrwa_status::CREATED;
            row.type			            = (uint16_t) rp_type;
            row.created_at                  = now;
            row.updated_at                  = now; 
        });

    } else if (parts.size() == 2) {  //pay fees of issuing the guarantyrwa out
        name receiver_contract = get_first_receiver();
        extended_asset extended_quantity = extended_asset(quantity, receiver_contract); 
        CHECKC(extended_quantity >= _gstate2.fee, err::QUANTITY_INSUFFICIENT, "insufficient payment for fees");

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

void guarantyrwa::claimguarantyrwa( const name& claimer, const name& code, const string& pwhash )
{
    require_auth(_gstate.admin);

    guarantyrwa_t guarantyrwa(code);
    CHECKC( _db.get(guarantyrwa), err::RECORD_NO_FOUND, "guarantyrwa not found" );
    auto pw_hash = split(guarantyrwa.pw_hash, ":");
    auto contract_name = name(pw_hash[1]);
    if (contract_name.length() == 0) {
        tokenlist_t::idx_t tokenlist_tbl(_self, _self.value);
        auto tokenlist_index = tokenlist_tbl.get_index<"sym"_n>();
        auto tokenlist_iter = tokenlist_index.find(guarantyrwa.total_quantity.symbol.raw());
        CHECKC( tokenlist_iter != tokenlist_index.end(), err::RECORD_NO_FOUND, "token list not found" );
        contract_name = tokenlist_iter->contract;
    } 
    CHECKC( pw_hash[0] == pwhash, err::PWHASH_INVALID, "incorrect password");
    CHECKC( guarantyrwa.status == guarantyrwa_status::CREATED, err::EXPIRED, "guarantyrwa has expired" );

    bool is_auth = false;
    if((guarantyrwa_type)guarantyrwa.type == guarantyrwa_type::DID_RANDOM || (guarantyrwa_type)guarantyrwa.type == guarantyrwa_type::DID_MEAN){
        auto claimer_acnts = flon::account_t::idx_t( _gstate2.did_contract, claimer.value );
        bool is_auth = false;
        for( auto claimer_acnts_iter = claimer_acnts.begin(); claimer_acnts_iter!=claimer_acnts.end(); claimer_acnts_iter++ ){
            if(claimer_acnts_iter->balance.amount > 0){
                is_auth = true;
                break;
            }
        }
        CHECKC( is_auth, err::DID_NOT_AUTH, "did is not authenticated" );
    }

    claim_t::idx_t claims(_self, _self.value);
    auto claims_index = claims.get_index<"unionid"_n>();
    uint128_t sec_index = get_unionid(claimer, code.value);
    auto claims_iter = claims_index.find(sec_index);
    CHECKC( claims_iter == claims_index.end() ,err::NOT_REPEAT_RECEIVE, "Can't repeat to receive" );

    asset guarantyrwa_quantity;
    switch((guarantyrwa_type)guarantyrwa.type){
        case guarantyrwa_type::RANDOM:
        case guarantyrwa_type::DID_RANDOM:
            guarantyrwa_quantity = _calc_red_amt(guarantyrwa);
            break;

        case guarantyrwa_type::MEAN:
        case guarantyrwa_type::DID_MEAN:
            guarantyrwa_quantity = guarantyrwa.remain_count == 1 ? guarantyrwa.remain_quantity : guarantyrwa.total_quantity/guarantyrwa.receiver_count;
            break;
    }
    TRANSFER_OUT(contract_name, claimer, guarantyrwa_quantity, string("red pack transfer"));

    guarantyrwa.remain_count--;
    guarantyrwa.remain_quantity-=guarantyrwa_quantity;
    guarantyrwa.updated_at = time_point_sec( current_time_point() );
    if(guarantyrwa.remain_count == 0){
        guarantyrwa.status = guarantyrwa_status::FINISHED;
    }
    _db.set(guarantyrwa, _self);

    auto id = claims.available_primary_key();
    claims.emplace( _self, [&]( auto& row ) {
        row.id                  = id;
        row.red_pack_code 	    = code;
        row.sender              = guarantyrwa.sender;
        row.receiver            = claimer;
        row.quantity            = guarantyrwa_quantity;
        row.claimed_at		    = time_point_sec( current_time_point() );
   });

}

void guarantyrwa::cancel( const name& code )
{
    guarantyrwa_t guarantyrwa(code);
    CHECKC( _db.get(guarantyrwa), err::RECORD_NO_FOUND, "guarantyrwa not found" );
    CHECKC( current_time_point() > guarantyrwa.created_at + eosio::hours(_gstate.expire_hours), err::NOT_EXPIRED, "expiration date is not reached" );
    if(guarantyrwa.status == guarantyrwa_status::CREATED){
        auto pw_hash = split(guarantyrwa.pw_hash, ":");
        auto contract = pw_hash[1];
        if (contract.size() == 0) {
            tokenlist_t::idx_t tokenlist_tbl(_self, _self.value);
            auto tokenlist_index = tokenlist_tbl.get_index<"sym"_n>();
            auto tokenlist_iter = tokenlist_index.find(guarantyrwa.total_quantity.symbol.raw());
            CHECKC( tokenlist_iter != tokenlist_index.end(), err::RECORD_NO_FOUND, "token list not found" );
            TRANSFER_OUT(tokenlist_iter->contract, guarantyrwa.sender, guarantyrwa.remain_quantity, string("red pack cancel transfer"));
        } else {
            auto contract_name = name(pw_hash[1]);
            TRANSFER_OUT(contract_name, guarantyrwa.sender, guarantyrwa.remain_quantity, string("red pack cancel transfer"));
        }
    }
    _db.del(guarantyrwa);
}

void guarantyrwa::delclaims( const uint64_t& max_rows )
{
    set<name> is_not_exist;

    claim_t::idx_t claim_idx(_self, _self.value);
    auto claim_itr = claim_idx.begin();

    size_t count = 0;
    for (; count < max_rows && claim_itr != claim_idx.end(); ) {

        bool guarantyrwa_not_existed = is_not_exist.count(claim_itr->red_pack_code) > 0 ? true : false;
        if (!guarantyrwa_not_existed){
            guarantyrwa_t guarantyrwa(claim_itr->red_pack_code);
            guarantyrwa_not_existed = !_db.get(guarantyrwa);
            if (guarantyrwa_not_existed){
                claim_itr = claim_idx.erase(claim_itr);
                is_not_exist.insert(claim_itr->red_pack_code);
                count++;
            } else {
                break;
            }
        } else {
            claim_itr = claim_idx.erase(claim_itr);
            count++;
        }
    }
    CHECKC(count > 0, err::NONE_DELETED, "delete invalid");
}

// asset guarantyrwa::_calc_fee(const asset& fee, const uint64_t count) {
//     // calc order quantity value by price
//     auto value = multiply<uint64_t>(fee.amount, count);

//     return asset(value, fee.symbol);
// }

asset guarantyrwa::_calc_red_amt(const guarantyrwa_t& guarantyrwa) {
    // calc order quantity value by price
    if ( guarantyrwa.remain_count == 1 ) {
        return guarantyrwa.remain_quantity;
    } else {
        uint64_t quantity = guarantyrwa.remain_quantity.amount / guarantyrwa.remain_count * 2;
        uint8_t precision = 0;
        if (guarantyrwa.remain_quantity.symbol.precision() <= 2)
            precision = 0;
        else
            precision = guarantyrwa.remain_quantity.symbol.precision() - 2;

        return asset(rand(asset(quantity, guarantyrwa.remain_quantity.symbol), precision), guarantyrwa.remain_quantity.symbol);
    }
}

uint64_t guarantyrwa::rand(asset max_quantity,  uint16_t min_unit) {
    auto mixedBlock = tapos_block_prefix() * tapos_block_num();
    const char *mixedChar = reinterpret_cast<const char *>(&mixedBlock);
    auto hash = sha256( (char *)mixedChar, sizeof(mixedChar));
    int64_t min_unit_throot = power10(min_unit);

    auto r1 = (uint64_t)hash.data()[0];
    float rand = 1/min_unit_throot+r1 % 100 / 100.00;
    int64_t round_throot = power10(max_quantity.symbol.precision() - min_unit);

    uint64_t rand_value = (uint64_t)(max_quantity.amount * rand) / round_throot * round_throot;
    uint64_t min_value = get_precision(max_quantity) / min_unit_throot;
    return rand_value < min_value ? min_value : rand_value;
    

}

