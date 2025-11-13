#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

yield_con=yieldrwa1111
mreg flon $yield_con flonian
mtran flonian $yield_con "100 FLON"
mset $yield_con yield.rwa
mcli set account permission $yield_con active --add-code


mpush  $yield_con init  '["flonian"]' -p $yield_con

mpush  $yield_con setslippage  '["flonian",8,200]' -p flonian


#测试没有成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "yieldrwa1111", "100.00000000 SING", "plan:1"]' -p flonian

#测试成功的计划是否可以发分红
mpush sing.token transfer '["flonian", "yieldrwa1111", "100.00000000 SING", "plan:8"]' -p flonian




mpush $yield_con buyback '["flonian",8]' -p flonian


#非admin，报错
mpush  $yield_con setslippage  '["gahbnbehaskk",8,200]' -p gahbnbehaskk