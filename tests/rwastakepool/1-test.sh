#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

stake_con=rwastakepool
mreg flon $stake_con flonian
mtran flonian $stake_con "100 FLON"
mset $stake_con rwastakepool
mcli set account permission $stake_con active --add-code




mpush $stake_con init '["flonian","rwaverse.io"]' -p $stake_con

# 负例：reward_symbol 精度不匹配（应为 8 位）
if mpush $stake_con addplan '[26,"4,STRDXB","sing.token","4,SING"]' -p flonian; then
  echo "unexpected success: addplan should fail with reward precision mismatch"
fi

mpush rwastakepool claim '["gahbnbehaskk",1]' -p gahbnbehaskk



mpush rwastakepool unstake '["gahbnbehaskk",26,"400.0000 STRDXB"]' -p gahbnbehaskk



mpush rwa.vtoken transfer '["flonian","rwastakepool","400.0000 STRDXB","stake:26:flonian"]' -p flonian

