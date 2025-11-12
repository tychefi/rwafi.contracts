#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

invest_con=investrwa112
mreg flon $invest_con flonian
mtran flonian $invest_con "100 FLON"
mset $invest_con invest.rwa
mcli set account permission $invest_con active --add-code


mpush $invest_con init '["flonian"]' -p $invest_con

mpush $invest_con addtoken '["sing.token","8,SING"]' -p $invest_con

# mpush $invest_con deltoken '["8,SING"]' -p $invest_con

# 参数含义:
# creator, title, goal_asset_contract, goal_quantity,
# receipt_asset_contract, receipt_quantity_per_unit,
# soft_cap_percent, hard_cap_percent,
# start_time, end_time, return_months, guaranteed_yield_apr

# 示例：目标筹 1000 RWA，分红期 18 个月，年化保底 12% (1200 bp)，
# 募资窗口今天开始到+30天；回执代币由 rwafi.token 托管，symbol("STRCP", 4）。
mpush $invest_con createplan '[
  "gahbnbehaskk",
  "plan tests",
  "sing.token",
  "10000.00000000 SING",
  "rwafi.token",
  "1.0000 STRCPD",
  60,
  120,
  8000,
  "2025-11-10T03:45:00",
  "2025-12-25T03:45:00",
  18,
  1200
]' -p gahbnbehaskk

#mpush $invest_con  cancelplan '["gahbnbehaskk",3]' -p gahbnbehaskk




mpush sing.token transfer '["gahbnbehaskk", "investrwa112", "100.00000000 SING", "plan:3"]' -p gahbnbehaskk