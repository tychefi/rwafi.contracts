#pragma once
#include <eosio/asset.hpp>
#include <eosio/name.hpp>

namespace flon {

// ===== 常量符号 =====
static constexpr eosio::symbol USDT_SYM                     = eosio::symbol("USDT", 6);
static constexpr eosio::symbol CISUM_SYM                   = eosio::symbol("CISUM", 4);
static constexpr eosio::symbol_code CISUM__SYMBOL_CODE     = eosio::symbol_code("CISUM");

// ===== 默认合约账户 =====

static constexpr eosio::name CISUMAUTH_CONTRACT      {"cisum.auth"_n};


static constexpr eosio::name CISUM_CONTRACT         {"cisum.token"_n};
static constexpr eosio::name SING_CONTRACT          {"sing.token"_n};


static constexpr eosio::name SWAP_CONTRACT          {"flon.swap"_n};


static constexpr eosio::name CISUM_BANK             {"cisum.token"_n};
static constexpr eosio::name MUSIC_BANK             {"sing.token"_n};
static constexpr eosio::name USDT_BANK              {"flon.mtoken"_n};

static constexpr eosio::name RECEIPT_BANK = "rwafi.token"_n;   // 凭证币发行合约
static constexpr eosio::name SING_BANK    = "sing.token"_n;    // SING 代币合约
static constexpr eosio::symbol SING_SYM   = eosio::symbol("SING", 8);


}






