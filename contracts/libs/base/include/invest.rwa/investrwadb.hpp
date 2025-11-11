#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/privileged.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>
#include <flon/wasm_db.hpp>
#include "flon/consts.hpp"

using namespace eosio;
using namespace std;
using std::string;
using namespace wasm::db;
using namespace flon;


namespace rwafi {

// fundraising plan status
namespace PlanStatus {
    static constexpr eosio::name PENDING        = "pending"_n;  // 待开始募资
    static constexpr eosio::name ACTIVE         = "active"_n;   // 募资开始
    static constexpr eosio::name CLOSED         = "closed"_n;   // 已达硬顶 (不可投)
    static constexpr eosio::name SUCCESS        = "success"_n;  // 募资成功
    static constexpr eosio::name FAILED         = "failed"_n;   // 募资失败 (处理退款)
    static constexpr eosio::name COMPLETED      = "completed"_n;// 已完成（回报发完）
    static constexpr eosio::name CANCELLED      = "cancelled"_n;// 已取消
    static constexpr eosio::name PENDING_PLEDGE = "pendingpldge"_n;//
};
// whitlisted investment tokens
   //scope: _self
struct [[eosio::table, eosio::contract("invest.rwa")]] allow_token_t {
    symbol          token_symbol;           //PK: token symbol
    name            token_contract;         //token issuing contract
    bool            onshelf = true;

    uint64_t primary_key() const { return token_symbol.raw(); }

    allow_token_t(){}
    allow_token_t( const symbol& symb ): token_symbol(symb){}

    typedef eosio::multi_index<"allowtokens"_n, allow_token_t> idx_t;

    EOSLIB_SERIALIZE( allow_token_t, (token_symbol)(token_contract)(onshelf) )
};
 //scope: _self
struct [[eosio::table, eosio::contract("invest.rwa")]] fundplan_t {
    uint64_t            id;                         //PK: 募资计划ID
    string              title;                      //plan title: <=64 chars
    name                creator;                    //plan owner

    // === 募资目标 ===
    name                goal_asset_contract;        //goal asset issuing contract (FRC20)
    asset               goal_quantity;              //goal quantity to raise (FRC20)
    time_point          created_at;                 //create time

    // === 投资凭证 ===
    name                receipt_asset_contract;     //receipt issuing contract (FRC20)
    symbol              receipt_symbol;             //receipt symbol (1:1 ratio with invest unit)

    // === 软顶 & 硬顶 ===
    uint8_t             soft_cap_percent;           // 软顶比例：最低成功门槛（如 60 → 60%）
    uint8_t             hard_cap_percent;           // 硬顶比例：最高募资上限（如 100 → 100%，120 → 允许超募）

    // === 时间控制 ===
    time_point_sec      start_time;                 // 募资开始时间
    time_point_sec      end_time;                   // 募资截止时间

    // === 回报分配期限（新增）===
    uint16_t            return_months;               // 回报年限：8~10 年
    time_point_sec      return_end_time;            // 回报结束时间（自动计算）

    // === 投资担保机制 ===
    uint32_t            guaranteed_yield_apr = 500; // 兜底年化收益率（5% → 500），0为不担保

    // === 实时状态 ===
    asset               total_raised_funds;        // 已募集数量
    asset               total_issued_receipts;     // 已发凭证数量
    name                status = PlanStatus::PENDING; //募资计划状态

    uint64_t primary_key() const { return id; }

    fundplan_t(){}
    fundplan_t( const uint64_t& i ): id(i){}

    typedef eosio::multi_index<"fundplans"_n, fundplan_t> idx_t;

    EOSLIB_SERIALIZE( fundplan_t, (id)(title)(creator)(goal_asset_contract)(goal_quantity)(created_at)
                                        (receipt_asset_contract)(receipt_symbol)
                                        (soft_cap_percent)(hard_cap_percent)
                                        (start_time)(end_time)
                                        (return_months)(return_end_time)
                                        (guaranteed_yield_apr)
                                        (total_raised_funds)(total_issued_receipts)(status) )

};

} // namespace rwafi