#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

stake_con=stake.rwa
mreg flon $stake_con flonian
mtran flonian $stake_con "100 FLON"
mset $stake_con stake.rwa
mcli set account permission $stake_con active --add-code




mpush $stake_con init '["flonian","invest.rwa"]' -p $stake_con

mpush stake.rwa claim '["gahbnbehaskk",26]' -p gahbnbehaskk



mpush stake.rwa unstake '["gahbnbehaskk",26,"400.0000 STRDXB"]' -p gahbnbehaskk



mpush rwafi.token transfer '["gahbnbehaskk","stake.rwa","400.0000 STRDXB","stake:26:gahbnbehaskk"]' -p gahbnbehaskk


