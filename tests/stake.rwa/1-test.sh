#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

stake_con=stake.rwa
mreg flon $stake_con flonian
mtran flonian $stake_con "100 FLON"
mset $stake_con stake.rwa
mcli set account permission $stake_con active --add-code




mpush $stake_con init '["flonian","invest.rwa"]' -p $stake_con

mpush stake.rwa claim '["gahbnbehaskk",7]' -p gahbnbehaskk

