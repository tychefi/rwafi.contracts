#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

invest_con=rwaverse.io
mreg flon $invest_con flonian
mtran flonian $invest_con "100 FLON"
mset $invest_con rwaverse.io
mcli set account permission $invest_con active --add-code


mpush $invest_con init '["flonian"]' -p $invest_con
mpush $invest_con addtoken '["sing.token","8,SING"]' -p $invest_con

mpush $invest_con setoracle '["flonian", true]' -p $invest_con
mpush $invest_con setoracle '["rwastakepool", true]' -p $invest_con
mpush $invest_con setoracle '["rwayieldpool", true]' -p $invest_con
mpush $invest_con setoracle '["rwaguarapool", true]' -p $invest_con


# mpush $invest_con deltoken '["8,SING"]' -p $invest_con

# 参数含义:
# creator, title, goal_quantity, min_investment,
# receipt_asset_contract, receipt_quantity_per_unit,
# soft_cap_percent, hard_cap_percent,
# start_time, end_time, return_months, guaranteed_yield_apr

# 示例：目标筹 1000 RWA，分红期 18 个月，年化保底 12% (1200 bp)，
# 募资窗口今天开始到+30天；回执代币由 rwa.vtoken 托管，symbol("STRCP", 4）。
mpush $invest_con createplan '[
  "gahbnbehaskk",
  "2Z 未来 10年收益权",
  "1000.00000000 SING",
  "100.00000000 SING",
  "rwa.vtoken",
  "1.0000 ZAESING",
  60,
  120,
  "2026-02-09T8:00:00",
  "2026-02-09T14:45:00",
  36,
  1200
]' -p gahbnbehaskk




# 负例：goal_quantity 精度不匹配（应为 8 位）
if mpush $invest_con createplan '[
  "gahbnbehaskk",
  "plan bad precision",
  "1000.0000 SING",
  "100.0000 SING",
  "rwa.vtoken",
  "1.0000 STRBADP",
  60,
  120,
  "2026-01-20T06:00:00",
  "2026-01-20T06:15:00",
  36,
  1200
]' -p gahbnbehaskk; then
  echo "unexpected success: createplan should fail with SING precision mismatch"
fi

# 负例：receipt_asset_contract 不是 rwa.vtoken
if mpush $invest_con createplan '[
  "gahbnbehaskk",
  "plan bad receipt",
  "1000.00000000 SING",
  "100.00000000 SING",
  "bad.token",
  "1.0000 STRBAD",
  60,
  120,
  "2026-01-20T06:00:00",
  "2026-01-20T06:15:00",
  36,
  1200
]' -p gahbnbehaskk; then
  echo "unexpected success: createplan should fail with bad receipt contract"
fi

#mpush $invest_con  cancelplan '["gahbnbehaskk",6]' -p gahbnbehaskk
mpush $invest_con  delplan '[1]' -p flonian



mpush sing.token transfer '["gahbnbehaskk", "rwaverse.io", "1200.00000000 SING", "plan:1"]' -p gahbnbehaskk
mpush sing.token transfer '["flonian", "rwaverse.io", "600.00000000 SING", "plan:67"]' -p flonian

mpush sing.token transfer '["mywallet2", "rwaverse.io", "4000.00000000 SING", "plan:53"]' -p mywallet2


mpush $invest_con setoracle '["gahbnbehaskk", true]' -p $invest_con
mpush $invest_con refreshstat '["gahbnbehaskk",76]'  -p gahbnbehaskk

mpush $invest_con batchrefresh '["gahbnbehaskk",[66,59,61]]'  -p gahbnbehaskk




# 负例：refreshstat 不存在的 plan_id
if mpush $invest_con refreshstat '["gahbnbehaskk",999999]' -p gahbnbehaskk; then
  echo "unexpected success: refreshstat should fail for missing plan"
fi


mpush $invest_con endraisegain '["gahbnbehaskk",1]' -p gahbnbehaskk


mpush $invest_con delplan '[42]' -p flonian

