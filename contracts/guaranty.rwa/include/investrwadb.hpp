#pragma once

#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/privileged.hpp>
#include <eosio/singleton.hpp>
#include <eosio/system.hpp>
#include <eosio/time.hpp>

using namespace eosio;
using namespace std;
using std::string;

// using namespace wasm;
#define SYMBOL(sym_code, precision) symbol(symbol_code(sym_code), precision)

static constexpr eosio::name active_perm        {"active"_n};
static constexpr symbol SYS_SYMBOL              = SYMBOL("flon", 8);
static constexpr name SYS_BANK                  { "flon.token"_n };

static constexpr uint32_t MIN_SINGLE_REDPACK    = 100;
static constexpr uint64_t seconds_per_month     = 30 *  24 * 3600;
static constexpr uint64_t seconds_per_year      = 365 * 24 * 3600;
static constexpr uint64_t DAY_SECONDS           = 24 * 36000;
static constexpr uint32_t MAX_TITLE_SIZE        = 64;
static constexpr uint8_t  EXPIRY_HOURS          = 12;

namespace wasm { namespace db {

#define TBL [[eosio::table, eosio::contract("did.redpack")]]
#define TBL_NAME(name) [[eosio::table(name), eosio::contract("did.redpack")]]

inline uint128_t get_unionid( const name& rec, uint64_t packid ) {
     return ( (uint128_t) rec.value << 64 ) | packid;
}

struct TBL_NAME("global") global_t {
    name            admin;
    name            stake_contract      = "stake.rwa"_n;
    name            yield_contract      = "yield.rwa"_n;
    name            guanranty_contract  = "guanranty.rwa"_n;
    uint64_t        last_plan_id        = 0;

    EOSLIB_SERIALIZE( global_t, (admin)(stake_contract)(yield_contract)(guanranty_contract)(last_plan_id) )
};
typedef eosio::singleton< "global"_n, global_t > global_singleton;

// whitlisted investment tokens
//
struct TG_TBL allow_token_t {               //scope: _self
    symbol          token_symbol;           //PK: token symbol
    name            token_contract;         //token issuing contract
    bool            onshelf = true;

    uint64_t primary_key() const { return token_symbol.raw(); }

    allow_token_t(){}
    allow_token_t( const symbole& symb ): token_symbol(symb){}

    typedef eosio::multi_index<"allowtokens"_n, allow_token_t,
    > idx_t;

    EOSLIB_SERIALIZE( allow_token_t, (token_symbol)(token_contract)(onshelf) )
};

// fundraising plan status
namespace PlanStatus {
    static constexpr eosio::name PENDING        = "pending"_n;  // 待开始募资
    static constexpr eosio::name ACTIVE         = "active"_n;   // 募资成功
    static constexpr eosio::name CLOSED         = "closed"_n;   // 已达硬顶 (不可投)
    static constexpr eosio::name SUCCESS        = "success"_n;  // 募资成功
    static constexpr eosio::name FAILED         = "failed"_n;   // 募资失败 (处理退款)
    static constexpr eosio::name COMPLETED      = "completed"_n;// 已完成（回报发完）
    static constexpr eosio::name CANCELLED      = "cancelled"_n;// 已取消
    static constexpr eosio::name PENDING_PLEDGE = "pendingpldge"_n;//
};


struct TBL fundplan_t {                             //scope: _self
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
    uint16_t            return_years;               // 回报年限：8~10 年
    time_point          return_end_time;            // 回报结束时间（自动计算）

    // === 投资担保机制 ===
    double              guaranteed_yield_apr = 0.05; // 兜底年化收益率（5% → 0.05）

    // === 实时状态 ===
    asset               total_raised_funds;        // 已募集数量
    asset               total_issued_receipts;     // 已发凭证数量
    name                status = PlanStatus::PENDING; //募资计划状态
    
    uint64_t primary_key() const { return id; }
    // uint64_t by_updatedid() const { return ((uint64_t)updated_at.sec_since_epoch() << 32) | (code.value & 0x00000000FFFFFFFF); }
    // uint64_t by_creator() const { return creator.value; }
    fundplan_t(){}
    fundplan_t( const name& c ): code(c){}
    typedef eosio::multi_index<"fundplans"_n, fundplan_t
        // indexed_by<"updatedid"_n,  const_mem_fun<fundplan_t, uint64_t, &fundplan_t::by_updatedid> >,
        // indexed_by<"creatorid"_n,  const_mem_fun<fundplan_t, uint64_t, &fundplan_t::by_creator> >
    > idx_t;

    EOS_LIB_SERIALIZE( fundplan_t, (id)(title)(creator)(goal_asset_contract)(goal_quantity)
        (created_at)(receipt_asset_contract)(receipt_symbol)
        (soft_cap_percent)(hard_cap_percent)(start_time)(end_time)
        (return_years)(return_end_time)(guaranteed_yield_apr)
        (total_raised_funds)(total_issued_receipts)(status)
    )
 
};

} }