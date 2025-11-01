
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

void yieldrwa::_perform_distribution(const name& bank, const asset& total) {
    // 计算各部分金额
    auto cfg            = _gstate.yield_split_conf;
    auto amt_stake      = asset{(total.amount * cfg[ RWA_STAKE_POOL ]) / 100, total.symbol};
    auto amt_pool       = asset{(total.amount * cfg[ RWA_GUARANTY_POOL ]) / 100, total.symbol};
    auto amt_swap       = total - amt_stake - amt_pool;

    // 转移到stake.rwa
    TRANSFER_OUT( bank, cfg.stake_rwa, amt_stake, string("Stake RWA allocation") )

    // 转移到flon.swap（回购燃烧）
    TRANSFER_OUT( bank, cfg.flon_swap, amt_swap, string("AMM Swap buyback & burn") )

    // 转移到担保人池
    TRANSFER_OUT( bank, cfg.guarantor_pool, amt_pool, string("Guarantor pool for RWA funding") )
}

void yieldrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    if (from == _self || to != _self) return; // 只处理转入本合约的转账
    CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    auto bank = get_first_contract();
    _perform_distribution( bank, quantity );
}