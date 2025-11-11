#pragma once
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <flon/wasm_db.hpp>
#include "yieldrwadb.hpp"

using namespace eosio;
using namespace std;

namespace rwafi {

class [[eosio::contract("yield.rwa")]] yieldrwa: public eosio::contract {

public:
    using contract::contract;

    ACTION updateconfig(const name& key, const uint8_t& value);

    asset get_yearly_yield(const uint64_t& plan_id, const uint64_t& year) const;

};

} // namespace rwafi