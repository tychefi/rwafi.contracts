#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

guaranty_con=guaranty.rwa
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con guaranty.rwa
mcli set account permission $guaranty_con active --add-code





mpush   $guaranty_con init  '["flonian"]' -p $guaranty_con



mpush sing.token transfer '["gahbnbehaskk", "guaranty.rwa", "10000.00000000 SING", "guaranty:9"]' -p gahbnbehaskk
mpush sing.token transfer '["flonian", "guaranty.rwa", "20.00000000 SING", "guaranty:6"]' -p flonian

mpush $guaranty_con redeem '["gahbnbehaskk",9,"100.00000000 SING"]' -p gahbnbehaskk


mpush $invest_con  cancelplan '["gahbnbehaskk",13]' -p gahbnbehaskk

mpush $guaranty_con guarantpay '["gahbnbehaskk",9]' -p gahbnbehaskk
