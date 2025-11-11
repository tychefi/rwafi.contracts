#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

stake_con=stake1111
mreg flon $stake_con flonian
mtran flonian $stake_con "100 FLON"
mset $stake_con stake.rwa
mcli set account permission $stake_con active --add-code




mpush $stake_con init '["flonian","investrwa112"]' -p $stake_con

