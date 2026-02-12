#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

yield_con=rwayieldpool
mreg flon $yield_con flonian
mtran flonian $yield_con "100 FLON"
mset $yield_con rwayieldpool
mcli set account permission $yield_con active --add-code


mpush  $yield_con init  '["flonian"]' -p $yield_con

mpush  $yield_con setslippage  '["flonian",8,200]' -p flonian


#测试没有成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "rwayieldpool", "100.00000000 SING", "plan:1"]' -p flonian

#测试成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "rwayieldpool", "100.00000000 SING", "plan:1"]' -p flonian

# 负例：SING 精度不匹配（应为 8 位）
if mpush sing.token transfer '["flonian", "rwayieldpool", "1.0000 SING", "plan:35"]' -p flonian; then
  echo "unexpected success: yield transfer should fail with precision mismatch"
fi

mpush $yield_con buyback '["flonian",5,"10.00000000 SING",100]' -p flonian


#非admin，报错
mpush  $yield_con setslippage  '["gahbnbehaskk",5,500]' -p gahbnbehaskk
yield_con=rwayieldpool
mpush  $yield_con delbuyback  '[1]' -p $yield_con

