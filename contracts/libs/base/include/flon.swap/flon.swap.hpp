#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>

#include "flon.swap.db.hpp"

static set<string> pool_token_whitelist = { "flon", "usdt" };
// static extended_asset create_pool_fee = extended_asset(asset(asset_from_string("10.00000000 FLON")), "flon.token"_n);


namespace flon {
using std::string;
using std::vector;
using namespace eosio;
using namespace wasm::db;

class [[eosio::contract("flon.swap")]] flonswap : public eosio::contract {
           public:
   using contract::contract;

   /* 设置币对配置
      default symbol_code is {left_pool_quant[0:4]}{right_pool_quant[0:3]}
   */

   [[eosio::action]] void settpconf(const name &admin,
                     const name        &tpcode,
                     const bool        &liquidity_redeem_flag,
                     const name        &fee_receiver,
                     const int16_t     &buy_fee_ratio,
                     const int16_t     &sell_fee_ratio,
                     const set<name>   &whitelist);

   [[eosio::action]] void settpredeem(const name &admin, const name &tpcode,
                         const bool &liquidity_redeem_flag);
   /*
    * @brief create a new market
    * @permission the user who creates the market
    * @param user - the user who creates the market
    * @param left_pool_quant - the first pool of the market
    * @param right_pool_quant - the second pool of the market
    * @param liquidity_symbol - the symbol of the liquidity token,
    *           default symbol_code is {left_pool_quant[0:4]}{right_pool_quant[0:3]}
    *           if symbol_code is existed, will use this parameter
    */

   [[eosio::action]] void create(const name &user,
                      const symbol &pool_symbol1,
                      const name &pool_contract1,
                      const symbol &pool_symbol2,
                      const name &pool_contract2,
                      const symbol_code &liquidity_symbol);

   using swapcreate_action    = eosio::action_wrapper<"create"_n, &flonswap::create>;

   [[eosio::action]] void rmmarket(const name &tpcode);


   [[eosio::action]] void reverseswap(const name &tpcode);

   /*
    * @brief set liquidity_symbol symbol_code name
    * @permission the admin
    * @param tpcode - the symbol pair of the market
    * @param liquidity_symbol - the symbol of the liquidity token
    */
   [[eosio::action]] void setliqsymbol(name tpcode, symbol_code liquidity_symbol);

   /*
    * @brief add liquidity pool creator whitelist
    * @permission the admin
    * @param creator - creator can create trade pairs
    */
   [[eosio::action]] void addlpcreator(const name &creator);

   /*
    * @brief delete liquidity pool creator whitelist
    * @permission the admin
    * @param creator - creator can not create trade pairs
    */
   [[eosio::action]] void dellpcreator(const name &creator);

   /*
    * @brief close a market
    * @permission the admin
    * @param tpcode - the symbol pair of the market
    * @param closed - true if the market is closed, false otherwise
    */
   [[eosio::action]] void close(name tpcode, bool closed);

   /*
    * @brief set the fee ratio of a market
    * @permission the admin
    * @param tpcode - the symbol pair of the market
    * @param fee_ratio - the fee ratio of the market
    */
   [[eosio::action]] void setfeeratio(name tpcode, uint16_t sys_fee_ratio, uint16_t lp_fee_ratio);

/*
    * @brief set the admin
    * @permission the contract
    * @param admin - the new admin
    */
   [[eosio::action]] void setadmin(name admin);

   /*
    * @brief set the fee of market creation
    * @permission the admin
    * @param mkt_create_fee - the fee of market creation
    */
   [[eosio::action]] void setmktcrtfee(asset mkt_create_fee);

   /*
    * @brief set the default fee ratio
    * @permission the admin
    * @param default_fee_ratio - the default fee ratio
    */
   [[eosio::action]] void setdefratio(uint16_t default_fee_ratio, uint16_t default_lp_fee_ratio);

   /*
    * @brief set the liqbank
    * @permission the admin
    * @param liqbank - the liqbank
    */
   [[eosio::action]] void setliqbank(name liqbank);

   [[eosio::action]] void setpaid(const name &market_sympair);

   /*
    * @brief set the support token banks
    * @permission the admin
    * @param tkbanks - the support token banks
    */
   [[eosio::action]] void settkbanks(vector<name> tkbanks);


   /*
    * @brief trigger by token transfer
    * @permission the user who transfers the token
    * @param from - the user who transfers the token
    * @param to - the user who receives the token, always the contract itself
    * @param quantity - the quantity of the token
    * @param memo - the memo of the transfer
    *        memo - "mint:tpcode:STEP:NONCE" - mint liquidity token
    *        memo - "burn:tpcode" - burn liquidity token
    *        memo - "swap:MIN_EXPECTED:tpcode:SYMPAIR2:SYMPAIR3..." - swap token, routed by sympairs
    */
   [[eosio::on_notify("*::transfer")]] void on_transfer(name from, name to, asset quantity, string memo);

   static symbol_code get_liq_symbol(const name& contract, const name& tpcode) {
      swap_markets m(contract, contract.value);
      auto itr = m.find(tpcode.value);
      check(itr != m.end(), "swap market not found");
      return itr->liquidity_symbol;
   }
   static constexpr name FLON_SWAP{"flon.swap"_n};
   static bool is_exists_pool(extended_symbol pool1, extended_symbol pool2) {
        auto            default_sympair = pool_symbol(pool1.get_symbol(), pool2.get_symbol());
        market_t::idx_t market_idx(FLON_SWAP, FLON_SWAP.value);
        auto            market_itr = market_idx.find(default_sympair.value);
        return market_itr != market_idx.end();
    }
   static inline name pool_symbol(symbol symbol0, symbol symbol1) {
      std::string code0 = symbol0.code().to_string();
      std::string code1 = symbol1.code().to_string();
      transform(code0.begin(), code0.end(), code0.begin(), ::tolower);
      transform(code1.begin(), code1.end(), code1.begin(), ::tolower);
      std::string code = code0 + "." + code1;
      return name(code);
   }

   std::string   _to_lower_str(const symbol_code &sym_code) {
      auto str = sym_code.to_string();
      std::transform(str.begin(), str.end(), str.begin(), ::tolower);
      return str;
   }
   static bool has_swap_market(const name& swap_contract, const name& tpcode) {
      flon::market_t::idx_t markets(swap_contract, swap_contract.value);
      return markets.find(tpcode.value) != markets.end();
   }

};

} // namespace flon