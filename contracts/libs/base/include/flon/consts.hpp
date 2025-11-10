#pragma once
#include <eosio/asset.hpp>
#include <eosio/name.hpp>

namespace flon {

static constexpr eosio::name USDT_BANK              {"flon.mtoken"_n};
static constexpr eosio::name RECEIPT_BANK           {"rwafi.token"_n};   // 凭证币发行合约
static constexpr eosio::name SING_BANK              {"sing.token"_n};    // SING 代币合约

static constexpr eosio::symbol SING_SYM             = eosio::symbol("SING", 8);

static constexpr eosio::name STAKE_POOL             = "stake.rwa"_n;
static constexpr eosio::name INVEST_POOL            = "invest.rwa"_n;
static constexpr eosio::name GUARANTY_POOL          = "guaranty.rwa"_n;
static constexpr eosio::name YIELD_POOL             = "yield.rwa"_n;
static constexpr eosio::name SWAP_POOL              = "flon.swap"_n;

}






