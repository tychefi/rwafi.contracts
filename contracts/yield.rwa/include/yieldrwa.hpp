#pragma once
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <flon/wasm_db.hpp>
#include "yieldrwadb.hpp"

using namespace eosio;
using namespace std;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }

enum class err: uint8_t {
   INVALID_FORMAT        = 0,
   TYPE_INVALID          = 1,
   QUANTITY_INSUFFICIENT = 3,
   NOT_POSITIVE          = 4,
   SYMBOL_MISMATCH       = 5,
   EXPIRED               = 6,
   PWHASH_INVALID        = 7,
   RECORD_NOT_FOUND      = 8,
   NOT_REPEAT_RECEIVE    = 9,
   NOT_EXPIRED           = 10,
   ACCOUNT_INVALID       = 11,
   PARAM_ERROR           = 12,
   STATUS_ERROR          = 13,
   INCORRECT_AMOUNT      = 14,
   NO_AUTH               = 15
};

namespace rwafi {

class [[eosio::contract("yield.rwa")]] yieldrwa : public eosio::contract {
private:
    dbc                 _db;
    global_singleton    _global;
    global_t            _gstate;

public:
    using contract::contract;

    yieldrwa(eosio::name receiver, eosio::name code, datastream<const char*> ds)
        : contract(receiver, code, ds),
          _db(_self),
          _global(_self, _self.value)
    {
        _gstate = _global.exists() ? _global.get() : global_t{};
    }

    ~yieldrwa() {
        _global.set(_gstate, get_self());
    }

    // ========== Actions ==========
    ACTION init(const name& admin);
    ACTION updateconfig(const name& key, const uint8_t& value);

    [[eosio::on_notify("sing.token::transfer")]]
    void on_transfer(const name& from, const name& to, const asset& quantity, const string& memo);

    // === External Query ===
    asset get_yearly_yield(const uint64_t& plan_id, const uint64_t& year, const string& type = "total") const;

    ACTION buyback(const name& submitter,const uint64_t& plan_id);

    ACTION setslippage(const name& submitter,const uint64_t& plan_id, const uint16_t& max_slippage);

private:
    // ========== Internal Helpers ==========

    // 执行分配 (stake / guaranty / swap)
    void _perform_distribution(const name& bank, const asset& total, const uint64_t& plan_id);

    // 写入分配日志（传入实际分配结果）
    void _log_yield(const uint64_t& plan_id,
                    const asset& total,
                    const asset& to_stake,
                    const asset& to_guaranty,
                    const asset& to_swap);

    // 计算年度收益核心逻辑（可按类型汇总）
    asset _calc_yearly_yield_core(const uint64_t& plan_id,
                                  const uint64_t& year,
                                  const string& type = "total") const;

    // 获取担保覆盖率（动态比例）
    double _get_coverage_ratio(const uint64_t& plan_id);

    name find_pair_by_symbols(const symbol& in_sym,const symbol& out_sym,const name& swap_contract);
};

} // namespace rwafi