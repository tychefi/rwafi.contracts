#include "stakerwa.hpp"
#include "flon/flon.token.hpp"
#include "invest.rwa/investrwadb.hpp"

namespace rwafi {

int128_t stakerwa::calc_reward_per_share_delta(const asset& rewards, const asset& total_staked) {
    if (rewards.amount <= 0 || total_staked.amount <= 0) return 0;
    int128_t delta = (int128_t)rewards.amount * HIGH_PRECISION / total_staked.amount;
    return delta;
}

asset stakerwa::calc_user_reward(const asset& staked, const int128_t& reward_per_share_delta, const symbol& reward_symbol) {
    if (staked.amount <= 0 || reward_per_share_delta <= 0) return asset(0, reward_symbol);
    int128_t reward_amt = (int128_t)staked.amount * reward_per_share_delta / HIGH_PRECISION;
    CHECKC(reward_amt <= std::numeric_limits<int64_t>::max(), err::INCORRECT_AMOUNT, "overflow in reward calc");
    return asset((int64_t)reward_amt, reward_symbol);
}

void stakerwa::init(const name& admin, const name& investrwa_contract) {
    require_auth(get_self());
    _gstate.admin              = admin;
    _gstate.investrwa_contract = investrwa_contract;
}

void stakerwa::addplan(const uint64_t& plan_id, const symbol& receipt_sym) {
    CHECKC(
        has_auth(_gstate.admin) || has_auth(_gstate.investrwa_contract),
        err::NO_AUTH, "missing required auth"
    );

    fundplan_t::idx_t fundplans(_gstate.investrwa_contract, _gstate.investrwa_contract.value);
    auto fund_itr = fundplans.find(plan_id);
    CHECKC(fund_itr != fundplans.end(), err::RECORD_NOT_FOUND, "fundplan not found in investrwa");
    CHECKC(fund_itr->receipt_symbol == receipt_sym, err::SYMBOL_MISMATCH, "receipt symbol mismatch with fundplan");

    // === 2. 检查是否已存在于 stakerwa ===
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto itr = stakeplans.find(plan_id);
    CHECKC(itr == stakeplans.end(), err::RECORD_EXISTING, "plan already exists");

    // === 3. 新建质押池 ===
    stakeplans.emplace(get_self(), [&](auto& p) {
        p.plan_id        = plan_id;
        p.receipt_symbol = receipt_sym;
        p.cum_staked     = asset(0, receipt_sym);
        p.total_staked   = asset(0, receipt_sym);
        p.reward_state   = stake_reward_st{};
        p.created_at     = time_point_sec(current_time_point());
    });
}

void stakerwa::delplan(const uint64_t& plan_id) {
    require_auth(_gstate.admin);

    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto itr = stakeplans.find(plan_id);
    CHECKC(itr != stakeplans.end(), err::RECORD_NOT_FOUND, "plan not found");
    CHECKC(itr->total_staked.amount == 0, err::ACTION_REDUNDANT, "plan still has active stakes");

    stakeplans.erase(itr);
}

void stakerwa::claim(const name& owner, const uint64_t& plan_id) {
    require_auth(owner);

    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    staker_t::tbl_t stakers(get_self(), plan_id);
    auto user_itr = stakers.find(owner.value);
    CHECKC(user_itr != stakers.end(), err::RECORD_NOT_FOUND, "user not found in plan");
    CHECKC(user_itr->avl_staked.amount > 0, err::INCORRECT_AMOUNT, "no active stake");

    auto pool_rps = plan_itr->reward_state.reward_per_share;
    auto user_rps = user_itr->stake_reward.last_reward_per_share;

    if (pool_rps <= user_rps && user_itr->stake_reward.unclaimed_rewards.amount == 0) {
        CHECKC(false, err::ACTION_REDUNDANT, "no new rewards to claim");
    }

    // 1. 计算差分奖励
    int128_t reward_per_share_delta = pool_rps - user_rps;
    asset new_reward = calc_user_reward(user_itr->avl_staked, reward_per_share_delta, plan_itr->reward_state.reward_symbol);

    // 2. 汇总总奖励（含之前未领取）
    asset total_claim = new_reward + user_itr->stake_reward.unclaimed_rewards;

    CHECKC(total_claim.amount > 0, err::INCORRECT_AMOUNT, "no claimable reward");

    // 3. 更新用户与池
    stakers.modify(user_itr, get_self(), [&](auto& u) {
        u.stake_reward.unclaimed_rewards = asset(0, total_claim.symbol);
        u.stake_reward.claimed_rewards  += total_claim;
        u.stake_reward.last_reward_per_share = pool_rps;
        u.last_claim_at = time_point_sec(current_time_point());
    });

    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        p.reward_state.claimed_rewards += total_claim;
    });

    // 4. 转账发放奖励
    TRANSFER(plan_itr->reward_state.reward_token_contract, owner, total_claim, "stake claim: " + std::to_string(plan_id));
}

// --- 用户质押 ---
void stakerwa::on_transfer_rwafi(const name& from, const name& to,const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must transfer positive amount");

    auto parts = split(memo, ":");
    CHECKC(parts.size() == 3 && parts[0] == "stake", err::MEMO_FORMAT_ERROR, "invalid memo format, expect stake:<plan_id>:<user>");

    uint64_t plan_id = std::stoull(parts[1]);
    name investor = name(parts[2]);

    _on_stake(investor, quantity, plan_id);
}

// --- 管理员充值奖励 ---
void stakerwa::on_transfer_reward(const name& from, const name& to, const asset& quantity, const string& memo) {
    if (from == get_self() || to != get_self()) return;
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must transfer positive amount");
    //memo 格式： "reward:<plan_id>"
    auto params = split(memo, ":");
    CHECKC(params.size() == 2 && params[0] == "reward", err::MEMO_FORMAT_ERROR, "invalid memo format");

    uint64_t plan_id = std::stoull(params[1]);
    _on_reward_in(from, quantity, plan_id);
}


void stakerwa::unstake(const name& owner, const uint64_t& plan_id, const asset& quantity) {
    require_auth(owner);
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must unstake positive amount");

    // ✅ 结算奖励（自动发放）
    claim_action{
        get_self(),
        { permission_level{ get_self(), "active"_n } }
    }.send(owner, plan_id);

    // ✅ 再执行赎回逻辑
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    staker_t::tbl_t stakers(get_self(), plan_id);
    auto user_itr = stakers.find(owner.value);
    CHECKC(user_itr != stakers.end(), err::RECORD_NOT_FOUND, "user not found");
    CHECKC(user_itr->avl_staked >= quantity, err::INCORRECT_AMOUNT, "insufficient staked balance");

    stakers.modify(user_itr, get_self(), [&](auto& s) {
        s.avl_staked -= quantity;
    });

    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        p.total_staked -= quantity;
    });

    // ✅ 返还本金
    TRANSFER("rwafi.token"_n, owner, quantity, "unstake from plan: " + std::to_string(plan_id));
}

void stakerwa::batchunstake(const uint64_t& plan_id) {
    require_auth(INVEST_POOL);  // investrwa112 授权调用

    // === 查找计划 ===
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    // === 查找 stakers ===
    staker_t::tbl_t stakers(get_self(), plan_id);
    CHECKC(stakers.begin() != stakers.end(), err::RECORD_NOT_FOUND, "no stakers found for this plan");

    // === 遍历退款 ===
    for (auto itr = stakers.begin(); itr != stakers.end();) {
        if (itr->avl_staked.amount <= 0) {
            itr = stakers.erase(itr);
            continue;
        }

        const name& investor = itr->owner;
        const asset& refund_receipt = itr->avl_staked;

        string memo = "refund:" + std::to_string(plan_id) + ":" + investor.to_string();

        // ✅ 从 stake 合约账户发送给 INVEST_POOL
        TRANSFER("rwafi.token"_n,   INVEST_POOL,refund_receipt,memo);

        // ✅ 更新统计（安全减法）
        stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
            if (p.total_staked.amount >= refund_receipt.amount)
                p.total_staked -= refund_receipt;
            else
                p.total_staked.amount = 0;
        });

        // ✅ 删除当前用户记录
        itr = stakers.erase(itr);
    }

    // === 若无更多 stakers，则可删除该计划 ===
    if (stakers.begin() == stakers.end()) {
        stakeplans.erase(plan_itr);
    }
}


// ==============================
// ⚙️ 内部逻辑实现
// ==============================

void stakerwa::_on_stake(const name& from, const asset& quantity, const uint64_t& plan_id) {
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must stake positive amount");

    // ✅ 从 stakeplans 表中读取
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    staker_t::tbl_t stakers(get_self(), plan_id);
    auto itr = stakers.find(from.value);

    if (itr == stakers.end()) {
        // New staker
        stakers.emplace(get_self(), [&](auto& s) {
            s.owner = from;
            s.plan_id = plan_id;
            s.cum_staked = quantity;
            s.avl_staked = quantity;
            s.stake_reward.last_reward_per_share = plan_itr->reward_state.reward_per_share;
            s.stake_reward.unclaimed_rewards = asset(0, plan_itr->reward_state.reward_symbol);
            s.created_at = time_point_sec(current_time_point());
        });
    } else {
        // Existing staker: settle pending rewards first
        int128_t delta = plan_itr->reward_state.reward_per_share - itr->stake_reward.last_reward_per_share;
        asset pending = calc_user_reward(itr->avl_staked, delta, plan_itr->reward_state.reward_symbol);

        stakers.modify(itr, get_self(), [&](auto& s) {
            s.stake_reward.unclaimed_rewards += pending;
            s.stake_reward.last_reward_per_share = plan_itr->reward_state.reward_per_share;
            s.cum_staked += quantity;
            s.avl_staked += quantity;
            s.last_stake_at = time_point_sec(current_time_point());
        });
    }

    // update plan totals
    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        p.total_staked += quantity;
        p.cum_staked += quantity;
    });
}

void stakerwa::_on_reward_in(const name& from, const asset& quantity, const uint64_t& plan_id) {
    // 如果此函数由 [[eosio::on_notify("sing.token::transfer")]] 调用，则不需要 require_auth
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "invalid reward amount");

    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");
    CHECKC(quantity.symbol == plan_itr->reward_state.reward_symbol, err::SYMBOL_MISMATCH, "reward symbol mismatch");
    CHECKC(plan_itr->total_staked.amount > 0, err::INCORRECT_AMOUNT, "no staked tokens in pool");

    auto now = time_point_sec(current_time_point());
    int128_t delta_rps = calc_reward_per_share_delta(quantity, plan_itr->total_staked);

    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        auto& r = p.reward_state;
        r.reward_id++;
        r.total_rewards += quantity;
        r.last_rewards   = quantity;
        r.last_reward_per_share = r.reward_per_share;
        r.reward_per_share += delta_rps;
        r.prev_reward_added_at = r.reward_added_at;
        r.reward_added_at = now;

        // 若使用“即时分配”模式，可视情况清零未分配奖励
        r.unalloted_rewards += quantity;
    });
}

} // namespace rwafi