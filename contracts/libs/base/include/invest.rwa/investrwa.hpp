#include "investrwadb.hpp"
#include "flon/flon.token.hpp"
#include "flon/utils.hpp"

using namespace std;
using namespace wasm::db;

namespace rwafi {

class [[eosio::contract("invest.rwa")]] investrwa: public eosio::contract {

public:
    using contract::contract;

    ACTION addtoken( const name& contract, const symbol& sym );
    ACTION deltoken( const symbol& sym );
    ACTION onshelf( const symbol& sym, const bool& onshelf );
    ACTION refund(const name& submitter, const name& investor, const uint64_t& plan_id);
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
                       const uint32_t& guaranteed_yield_apr );

    ACTION cancelplan( const name& creator, const uint64_t& plan_id );

};
} // namespace rwafi