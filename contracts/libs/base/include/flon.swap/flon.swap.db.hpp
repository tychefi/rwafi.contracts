#pragma once

#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>

#include "utils.hpp"
#include "wasm_db.hpp"

namespace flon {

using namespace std;
using namespace eosio;

//scope: _self
struct [[eosio::table, eosio::contract("flon.swap")]] market_t {
   name           tpcode;                             // PK, Full name liquidity symbol: left_pool_quant.right_pool_quant
   symbol_code    liquidity_symbol;                   // Short Name liquidity symbol: L left_pool_quant[0:3] right_pool_quant[0:3]
                                                      // admin 可以指定liquidity_symbol
   extended_asset left_pool_quant;                    // left_pool_quant 和 right_pool_quant 按字母排序
   extended_asset right_pool_quant;
   uint16_t       sys_fee_ratio;                      //平台0.3%  （包含lp_fee_ratio）
   uint16_t       lp_fee_ratio;                       //流动性提供商0.1% 用户一次swap需要支付的手续费
   bool           liquidity_redeem_flag = true;      //是否允许提取流动性
   name           fee_receiver;                       //额外手续费接收账号
   int16_t        buy_fee_ratio;                      //平台0.3%,剩余的给fee_receiver
   int16_t        sell_fee_ratio;                     //平台0.3%,剩余的给fee_receiver (sell_fee_ratio - sys_fee_ratio)
   set<name>      fee_whitelist;                      //允许免手续费的账号
   bool           closed                  = false;    //是否关闭交易对
   int16_t        paid_type               = 2;        //是否支付了创建费用:0:未支付, 1:已支付, 2:免除支付
   time_point     created_at;
   time_point     updated_at;
   uint64_t primary_key() const { return tpcode.value; }
   uint128_t get_pool() const { return (uint128_t)(left_pool_quant.quantity.symbol.raw()) << 64 || right_pool_quant.quantity.symbol.raw(); }

   market_t() {}
   market_t(const name &msympair) : tpcode(msympair) {}

   EOSLIB_SERIALIZE(market_t, (tpcode)(liquidity_symbol)(left_pool_quant)(right_pool_quant)
                              (sys_fee_ratio)(lp_fee_ratio)(liquidity_redeem_flag)
                              (fee_receiver)(buy_fee_ratio)(sell_fee_ratio)(fee_whitelist)
                              (closed)(paid_type)
                              (created_at)(updated_at))

   typedef eosio::multi_index<"markets"_n, market_t,
            indexed_by<"by.poolidx"_n, const_mem_fun<market_t, uint128_t, &market_t::get_pool>>
      >idx_t;
};



} // namespace flon