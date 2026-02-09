#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

guaranty_con=rwaguarapool
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con rwaguarapool
mcli set account permission $guaranty_con active --add-code





mpush   $guaranty_con init  '["flonian"]' -p $guaranty_con



mpush sing.token transfer '["gahbnbehaskk", "rwaguarapool", "200.00000000 SING", "guaranty:35"]' -p gahbnbehaskk
mpush sing.token transfer '["flonian", "rwaguarapool", "200.00000000 SING", "guaranty:35"]' -p flonian

# 负例：SING 精度不匹配（应为 8 位）
if mpush sing.token transfer '["flonian", "rwaguarapool", "1.0000 SING", "guaranty:35"]' -p flonian; then
  echo "unexpected success: guaranty transfer should fail with precision mismatch"
fi

mpush $guaranty_con redeem '["gahbnbehaskk",18,"10.00000000 SING"]' -p gahbnbehaskk


mpush $invest_con  cancelplan '["gahbnbehaskk",14]' -p gahbnbehaskk

mpush $guaranty_con guarantpay '["gahbnbehaskk",18]' -p gahbnbehaskk
