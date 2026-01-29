#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

yield_con=yield.rwa
mreg flon $yield_con flonian
mtran flonian $yield_con "100 FLON"
mset $yield_con yield.rwa

mpush flon updateauth '{
"account": "yield.rwa",
"permission": "active",
"parent": "owner",
"auth": {
"threshold": 1,
"keys": [],
"accounts": [
{ "permission": { "actor": "flonian",   "permission": "active"    }, "weight": 1 },
{ "permission": { "actor": "guaranty.rwa", "permission": "active" }, "weight": 1 }
],
"waits": []
}
}' -p yield.rwa@owner

mcli set account permission $yield_con active --add-code



mpush  $yield_con init  '["flonian"]' -p $yield_con

mpush  $yield_con setslippage  '["flonian",8,200]' -p flonian


#测试没有成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "yield.rwa", "100.00000000 SING", "plan:1"]' -p flonian

#测试成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "yield.rwa", "100.00000000 SING", "plan:53"]' -p flonian

# 负例：SING 精度不匹配（应为 8 位）
if mpush sing.token transfer '["flonian", "yield.rwa", "1.0000 SING", "plan:35"]' -p flonian; then
  echo "unexpected success: yield transfer should fail with precision mismatch"
fi

mpush $yield_con buyback '["flonian",8]' -p flonian


#非admin，报错
mpush  $yield_con setslippage  '["gahbnbehaskk",8,200]' -p gahbnbehaskk
