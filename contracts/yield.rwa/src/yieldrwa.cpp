
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

//--------------------------
// 由 Unix 时间戳计算年份（1970 年起）
static uint64_t year_from_unix_seconds(uint64_t unix_seconds) {
    uint64_t days = unix_seconds / 86400;  // 秒 → 天

    // 估算年份：使用格里高利平均年长 365.2425 天
    uint64_t year = (days * 10000 + 1756461) / 3652425;

    // 精调：检查该年起始天数与实际天数是否匹配
    while (true) {
        uint64_t days_to_year = days_to_year_start(year);
        uint64_t days_in_this_year = days_in_year(year);

        if (days < days_to_year) {
            --year;
            continue;
        }
        if (days >= days_to_year + days_in_this_year) {
            ++year;
            continue;
        }
        break;
    }
    return year;
}

// 天数到某年 1月1日 的累计天数（1970年起）
static uint64_t days_to_year_start(uint64_t year) {
    uint64_t y = year - 1;  // 前一年
    return y * 365 + y/4 - y/100 + y/400;
}

// 该年总天数（闰年 366，平年 365）
static uint64_t days_in_year(uint64_t year) {
    bool is_leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    return is_leap ? 366 : 365;
}
//--------------------------


void yieldrwa::_perform_distribution(const name& bank, const asset& total) {
    // 计算各部分金额
    auto cfg            = _gstate.yield_split_conf;
    auto amt_stake      = asset{(total.amount * cfg[ RWA_STAKE_POOL ]) / 100, total.symbol};
    auto amt_pool       = asset{(total.amount * cfg[ RWA_GUARANTY_POOL ]) / 100, total.symbol};
    auto amt_swap       = total - amt_stake - amt_pool;

    // 转移到stake.rwa
    TRANSFER_OUT( bank, cfg.stake_rwa, amt_stake, string("Stake RWA allocation") )

    // 转移到flon.swap（回购燃烧）TODO: must conform to swap memo format!!
    TRANSFER_OUT( bank, cfg.flon_swap, amt_swap, string("AMM Swap buyback & burn") )

    // 转移到担保人池
    TRANSFER_OUT( bank, cfg.guarantor_pool, amt_pool, string("Guarantor pool for RWA funding") )
}

void yieldrwa::on_transfer( const name& from, const name& to, const asset& quantity, const string& memo)
{
    if (from == _self || to != _self) return; // 只处理转入本合约的转账
    CHECKC( quantity.amount > 0, err::NOT_POSITIVE, "quantity must be positive" )

    //memo format: plan:xxx
    auto parts = split(memo, ":");
    CHECK( parts.size() == 2, err::INVALID_FORMAT, "invalid memo format" );
    auto plan_id = (uint64_t) stoi(string(parts[1]));
    // auto plan = fundplan_t( plan_id );

    auto bank = get_first_contract();
    _perform_distribution( bank, quantity );

    _log_yield( plan_id, quantity )
}

void yieldrwa::init(const name& admin) {
    require_auth( _self );
    _gstate.admin = _admin;
}

void yieldrwa::updateconfig( const name& key, const uint8_t& value ) {
    require_auth( _gstate.admin );
    _gstate.yield_split_conf[ key ] = value;
}

void yieldrwa::_log_yield( const uint64_t& plan_id, const asset& quantity ) {
    uint32_t year = year_from_unix_seconds( current_time_point().sec_since_epoch() );
    auto yeah_no = year;

    auto yieldlog_tbl = _db.get_table< yield_log_t >( _self, plan_id );
    auto idx = yieldlog_tbl.get_index<"byyear"_n>();
    auto itr = idx.find( year );
    if ( itr == idx.end() ) {
        yieldlog_tbl.emplace( get_self(), [&]( auto& row ) {
            row.id                     = yieldlog_tbl.available_primary_key();
            row.year                   = year;
            row.yeah_no                = yeah_no;
            row.yeah_total_quantity    = quantity;
            row.plan_total_quantity    = quantity;
            row.created_at             = time_point_sec( current_time_point() );
        });
    } else {
        idx.modify( itr, get_self(), [&]( auto& row ) {
            row.yeah_total_quantity    += quantity;
            row.plan_total_quantity    += quantity;
            row.updated_at             = time_point_sec( current_time_point() );
        });
    }
}