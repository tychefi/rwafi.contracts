#include "stakerwa.hpp"
#include "flon/flon.token.hpp"
#include "invest.rwa/investrwadb.hpp"

namespace rwafi {

uint64_t stakerwa::calc_reward_per_share_delta(const asset& rewards,const asset& total_staked) {
    if (rewards.amount <= 0 || total_staked.amount <= 0) return 0;

    uint128_t num =(uint128_t)rewards.amount * (uint128_t)HIGH_PRECISION;
    uint128_t den =(uint128_t)total_staked.amount;
    uint128_t delta128 = num / den;
    CHECKC(delta128 <= std::numeric_limits<uint64_t>::max(), err::INCORRECT_AMOUNT,"reward_per_share overflow");

    return (uint64_t)delta128;
}

asset stakerwa::calc_user_reward(const asset& staked,const uint64_t& reward_per_share_delta,const symbol& reward_symbol) {
    if (staked.amount <= 0 || reward_per_share_delta == 0) {
        return asset(0, reward_symbol);
    }

    uint128_t reward128 =(uint128_t)staked.amount * (uint128_t)reward_per_share_delta / (uint128_t)HIGH_PRECISION;

    CHECKC(reward128 <= std::numeric_limits<int64_t>::max(), err::INCORRECT_AMOUNT,"overflow in reward calc");

    return asset((int64_t)reward128, reward_symbol);
}

void stakerwa::init(const name& admin, const name& investrwa_contract) {
    require_auth(get_self());

    CHECKC(is_account(admin), err::ACCOUNT_INVALID, "invalid admin");
    CHECKC(is_account(investrwa_contract), err::ACCOUNT_INVALID, "invalid invest contract");

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
    auto plan_itr = stakeplans.find(plan_id);

    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");
    CHECKC(plan_itr->total_staked.amount == 0,err::ACTION_REDUNDANT,"plan still has active stakes");

    // 读取 staker 表，必须为空才能删除计划
    staker_t::tbl_t stakers(get_self(), plan_id);
    CHECKC(stakers.begin() == stakers.end(),err::INVALID_STATUS, "cannot delete plan: stakers still exist");

    // 奖励必须全部被领取
    CHECKC(plan_itr->reward_state.total_rewards.amount ==plan_itr->reward_state.claimed_rewards.amount,
                err::INVALID_STATUS,"cannot delete plan: rewards not fully claimed");

    //  删除质押计划
    stakeplans.erase(plan_itr);
}

void stakerwa::claim(const name& owner, const uint64_t& plan_id) {
    require_auth(owner);

    // === 1. 读取 stake plan ===
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    // === 2. 读取 staker ===
    staker_t::tbl_t stakers(get_self(), plan_id);
    auto user_itr = stakers.find(owner.value);
    CHECKC(user_itr != stakers.end(), err::RECORD_NOT_FOUND, "user not found in plan");
    CHECKC(user_itr->avl_staked.amount > 0, err::INCORRECT_AMOUNT, "no active stake");

    auto pool_rps = plan_itr->reward_state.reward_per_share;
    auto user_rps = user_itr->stake_reward.last_reward_per_share;
    auto reward_sym = plan_itr->reward_state.reward_symbol;

    // === 3. 是否有新奖励 ===
    if (pool_rps <= user_rps && user_itr->stake_reward.unclaimed_rewards.amount == 0) {
        CHECKC(false, err::ACTION_REDUNDANT, "no new rewards to claim");
    }

    // === 4. 计算新增奖励 ===
    auto delta_rps = pool_rps - user_rps;
    asset new_reward = calc_user_reward(user_itr->avl_staked, delta_rps, reward_sym);

    // === 5. normalize unclaimed_rewards（防历史脏 symbol）===
    asset unclaimed = user_itr->stake_reward.unclaimed_rewards;
    if (unclaimed.symbol != reward_sym) {
        unclaimed = asset(unclaimed.amount, reward_sym);
    }

    asset total_claim = new_reward + unclaimed;
    CHECKC(total_claim.amount > 0,err::INCORRECT_AMOUNT, "no claimable reward");

    stakers.modify(user_itr, get_self(), [&](auto& u) {

        if (u.stake_reward.claimed_rewards.symbol != reward_sym) {
            u.stake_reward.claimed_rewards =
                asset(u.stake_reward.claimed_rewards.amount, reward_sym);
        }
        if (u.stake_reward.unclaimed_rewards.symbol != reward_sym) {
            u.stake_reward.unclaimed_rewards =
                asset(u.stake_reward.unclaimed_rewards.amount, reward_sym);
        }

        u.stake_reward.unclaimed_rewards = asset(0, reward_sym);
        u.stake_reward.claimed_rewards  += total_claim;
        u.stake_reward.last_reward_per_share = pool_rps;

        u.stake_reward.reward_id =plan_itr->reward_state.reward_id;
        u.last_claim_at = time_point_sec(current_time_point());
    });

    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        if (p.reward_state.claimed_rewards.symbol != reward_sym) {
            p.reward_state.claimed_rewards = asset(p.reward_state.claimed_rewards.amount, reward_sym);
        }
        p.reward_state.claimed_rewards += total_claim;
    });

    TRANSFER(plan_itr->reward_state.reward_token_contract,owner,total_claim,"stake claim:" + std::to_string(plan_id));
}

// --- 质押凭证 ---
void stakerwa::on_transfer_rwafi(const name& from, const name& to, const asset& quantity, const string& memo){

    if (from == get_self() || to != get_self()) return;

    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must transfer positive amount");

    // ✅ 只接受来自 INVEST_POOL（investrwa）的质押转入
    CHECKC(from == INVEST_POOL,err::NO_AUTH,"only invest.rwa contract can stake receipts");

    // ✅ memo = stake:<plan_id>:<user>
    auto parts = split(memo, ":");
    CHECKC(parts.size() == 3 && parts[0] == "stake",
           err::MEMO_FORMAT_ERROR,
           "invalid memo format, expect stake:<plan_id>:<user>");

    auto plan_id = std::stoull(parts[1]);
    name user = name(parts[2]);
    CHECKC(is_account(user), err::ACCOUNT_INVALID, "invalid stake user");

    // ✅ 校验符号匹配当前 plan 的 receipt_symbol，防止乱币充进来
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    CHECKC(quantity.symbol == plan_itr->receipt_symbol,
           err::SYMBOL_MISMATCH,
           "stake symbol mismatch with plan receipt");

    _on_stake(user, quantity, plan_id);
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

void stakerwa::batchunstake(const uint64_t& plan_id) {
    // ===  只有 invest.rwa 主合约可触发退款流程 ===
    require_auth(INVEST_POOL);

    // ===  查询 invest.rwa 上的 fundplan 状态 ===
    fundplan_t::idx_t fundplans(_gstate.investrwa_contract, _gstate.investrwa_contract.value);
    auto fplan_itr = fundplans.find(plan_id);
    CHECKC(fplan_itr != fundplans.end(), err::RECORD_NOT_FOUND, "fundplan not found");

    // ===  仅 FAILED / CANCELLED 状态允许退款 ===
    CHECKC(fplan_itr->status == PlanStatus::FAILED ||fplan_itr->status == PlanStatus::CANCELLED,
                err::INVALID_STATUS, "refund only allowed when plan is cancelled or failed");

    // === 查询 stake.rwa 内的质押池 ===
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    // ===  查询所有投资人记录 ===
    staker_t::tbl_t stakers(get_self(), plan_id);

    // 如果没人质押 → 删除空计划即可
    if (stakers.begin() == stakers.end()) {
        stakeplans.erase(plan_itr);
        return;
    }

    // ===  遍历所有质押人，逐个退款 ===
    for (auto itr = stakers.begin(); itr != stakers.end();) {

        if (itr->avl_staked.amount <= 0) {
            itr = stakers.erase(itr);
            continue;
        }

        const name& investor = itr->owner;
        const asset refund_amount = itr->avl_staked;

        // 退款 memo: refund:<plan_id>:<investor>
        string memo = "refund:" + std::to_string(plan_id) + ":" + investor.to_string();

        // === 资金流：stake.rwa → invest.rwa（退回 receipt token）===
        TRANSFER("rwafi.token"_n, INVEST_POOL, refund_amount, memo);

        // === 减少池中已质押数量 ===
        stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
            p.total_staked.amount = std::max<int64_t>(0,p.total_staked.amount - refund_amount.amount);
        });

        itr = stakers.erase(itr);
    }

    if (stakers.begin() == stakers.end()) {
        stakeplans.erase(plan_itr);
    }
}


// ==============================
// ⚙️ 内部逻辑实现
// ==============================

void stakerwa::_on_stake(const name& from,const asset& quantity,const uint64_t& plan_id) {
    CHECKC(quantity.amount > 0, err::NOT_POSITIVE, "must stake positive amount");

    // === 1. load stake plan ===
    stake_plan_t::tbl_t stakeplans(get_self(), get_self().value);
    auto plan_itr = stakeplans.find(plan_id);
    CHECKC(plan_itr != stakeplans.end(), err::RECORD_NOT_FOUND, "stake plan not found");

    const auto& reward_sym = plan_itr->reward_state.reward_symbol;

    // === 2. load staker table ===
    staker_t::tbl_t stakers(get_self(), plan_id);
    auto itr = stakers.find(from.value);

    if (itr == stakers.end()) {
        // === 3A. new staker ===
        stakers.emplace(get_self(), [&](auto& s) {
            s.owner      = from;
            s.plan_id    = plan_id;
            s.cum_staked = quantity;
            s.avl_staked = quantity;

            // reward state init (IMPORTANT)
            s.stake_reward.last_reward_per_share =
                plan_itr->reward_state.reward_per_share;

            s.stake_reward.unclaimed_rewards = asset(0, reward_sym);
            s.stake_reward.claimed_rewards   = asset(0, reward_sym);
            s.stake_reward.total_rewards     = asset(0, reward_sym);
            s.stake_reward.last_rewards      = asset(0, reward_sym);
            s.stake_reward.unalloted_rewards = asset(0, reward_sym);

            s.created_at = time_point_sec(current_time_point());
        });

    } else {
        // === 3B. existing staker ===

        // 3B-1. settle pending rewards first
        CHECKC(
            plan_itr->reward_state.reward_per_share >=
                itr->stake_reward.last_reward_per_share,
            err::INCORRECT_AMOUNT,
            "reward_per_share regression"
        );

        auto delta =
            plan_itr->reward_state.reward_per_share
          - itr->stake_reward.last_reward_per_share;

        asset pending =
            calc_user_reward(
                itr->avl_staked,
                delta,
                reward_sym
            );

        stakers.modify(itr, get_self(), [&](auto& s) {

            // === normalize legacy reward symbols (CRITICAL) ===
            if (s.stake_reward.unclaimed_rewards.symbol != reward_sym) {
                s.stake_reward.unclaimed_rewards =
                    asset(s.stake_reward.unclaimed_rewards.amount, reward_sym);
            }
            if (s.stake_reward.claimed_rewards.symbol != reward_sym) {
                s.stake_reward.claimed_rewards =
                    asset(s.stake_reward.claimed_rewards.amount, reward_sym);
            }
            if (s.stake_reward.total_rewards.symbol != reward_sym) {
                s.stake_reward.total_rewards =
                    asset(s.stake_reward.total_rewards.amount, reward_sym);
            }
            if (s.stake_reward.last_rewards.symbol != reward_sym) {
                s.stake_reward.last_rewards =
                    asset(s.stake_reward.last_rewards.amount, reward_sym);
            }
            if (s.stake_reward.unalloted_rewards.symbol != reward_sym) {
                s.stake_reward.unalloted_rewards =
                    asset(s.stake_reward.unalloted_rewards.amount, reward_sym);
            }

            // === apply pending reward ===
            if (pending.amount > 0) {
                s.stake_reward.unclaimed_rewards += pending;
                s.stake_reward.total_rewards     += pending;
            }

            // === update reward cursor ===
            s.stake_reward.last_reward_per_share =
                plan_itr->reward_state.reward_per_share;

            // === update stake amounts ===
            s.cum_staked += quantity;
            s.avl_staked += quantity;
            s.last_stake_at = time_point_sec(current_time_point());
        });
    }

    // === 4. update plan totals ===
    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        p.total_staked += quantity;
        p.cum_staked   += quantity;
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
    // check(false,
    // std::string("DEBUG reward_in | ")
    // + "reward.amount=" + std::to_string(quantity.amount)
    // + ", reward.symbol=" + quantity.symbol.code().to_string()
    // + ", total_staked.amount=" + std::to_string(plan_itr->total_staked.amount)
    // + ", total_staked.symbol=" + plan_itr->total_staked.symbol.code().to_string()
    // );
    auto delta_rps = calc_reward_per_share_delta(quantity, plan_itr->total_staked);

    stakeplans.modify(plan_itr, get_self(), [&](auto& p) {
        auto& r = p.reward_state;

        // 防御性修正：如果 total_rewards 还是默认 symbol，主动重建为 reward_symbol
        if (r.total_rewards.amount == 0 && r.total_rewards.symbol.raw() == 0) {
            r.total_rewards     = asset(0, r.reward_symbol);
            r.last_rewards      = asset(0, r.reward_symbol);
            r.unalloted_rewards = asset(0, r.reward_symbol);
            r.unclaimed_rewards = asset(0, r.reward_symbol);
            r.claimed_rewards   = asset(0, r.reward_symbol);
        }

        r.reward_id++;
        r.total_rewards       += quantity;
        r.last_rewards         = quantity;
        r.last_reward_per_share = r.reward_per_share;
        r.reward_per_share    += delta_rps;
        r.prev_reward_added_at = r.reward_added_at;
        r.reward_added_at      = now;
        r.unalloted_rewards   += quantity;
    });
}

} // namespace rwafi