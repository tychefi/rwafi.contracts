#include "rwaversedb.hpp"
#include "flon/flon.token.hpp"
#include "flon/utils.hpp"
#include <set>

using namespace std;
using namespace wasm::db;

namespace rwafi {


class [[eosio::contract("rwaverse.io")]] rwaverse: public eosio::contract {
public:
    using contract::contract;

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
    ACTION endraisegain( const name& caller, const uint64_t& plan_id );
    ACTION refreshstat(const name& submitter,const uint64_t& plan_id);
    ACTION batchrefresh(const name& submitter, const std::vector<uint64_t>& plan_ids, const uint64_t& now_ts);
    ACTION setoracle(const name& account, const bool& enabled);
    ACTION withdraw(const name& creator, const uint64_t& plan_id, const name& to, const asset& quantity);

    using addtoken_action           = eosio::action_wrapper<"addtoken"_n, &rwaverse::addtoken>;
    using refreshstat_action        = eosio::action_wrapper<"refreshstat"_n, &rwaverse::refreshstat>;
    using withdraw_action           = eosio::action_wrapper<"withdraw"_n, &rwaverse::withdraw>;

};
} // namespace rwafi
