#pragma once
#include <eosio/asset.hpp>
#include <eosio/name.hpp>

namespace flon {

// ===== 常量符号 =====
static constexpr eosio::symbol USDT_SYM                     = eosio::symbol("USDT", 6);
static constexpr eosio::symbol CISUM_SYM                   = eosio::symbol("CISUM", 4);
static constexpr eosio::symbol_code CISUM__SYMBOL_CODE     = eosio::symbol_code("CISUM");
static constexpr eosio::symbol SING_SYM                    = eosio::symbol("SING", 8);

// ===== 默认合约账户 =====

static constexpr eosio::name SHOW_CONTRACT          {"show.cisum"_n};
static constexpr eosio::name GRAB_CONTRACT          {"grab.cisum"_n};
static constexpr eosio::name CVTICKET_CONTRACT      {"ticket.cvnft"_n};
static constexpr eosio::name CVBADGE_CONTRACT       {"badge.cvnft"_n};
static constexpr eosio::name CVBADGESTORE_CONTRACT  {"badgecvstore"_n};
static constexpr eosio::name CISUMAUTH_CONTRACT      {"cisum.auth"_n};


static constexpr eosio::name CISUM_CONTRACT         {"cisum.token"_n};
static constexpr eosio::name SING_CONTRACT          {"sing.token"_n};

static constexpr eosio::name POE_CONTRACT           {"poe.cisum"_n};
static constexpr eosio::name POH_CONTRACT           {"poh.cisum"_n};
static constexpr eosio::name POP_CONTRACT           {"pop.cisum"_n};
static constexpr eosio::name POS_CONTRACT           {"pos.cisum"_n};

static constexpr eosio::name SWAP_CONTRACT          {"flon.swap"_n};
static constexpr eosio::name OPS_CONTRACT           {"cisumshowman"_n};  //演唱会聚合合约

static constexpr eosio::name SING_BANK              {"sing.token"_n};
static constexpr eosio::name CISUM_BANK             {"cisum.token"_n};
static constexpr eosio::name MUSIC_BANK             {"sing.token"_n};
static constexpr eosio::name USDT_BANK              {"flon.mtoken"_n};



}






