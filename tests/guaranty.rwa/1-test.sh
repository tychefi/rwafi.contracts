#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

guaranty_con=guaranty1111
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con guaranty.rwa
mcli set account permission $guaranty_con active --add-code





mpush   $guaranty_con init  '["flonian"]' -p $guaranty_con



mpush sing.token transfer '["gahbnbehaskk", "guaranty1111", "300.00000000 SING", "guaranty:22"]' -p gahbnbehaskk
mpush sing.token transfer '["flonian", "guaranty1111", "300.00000000 SING", "guaranty:21"]' -p flonian

mpush $guaranty_con redeem '["flonian",21,"5.00000000 SING"]' -p flonian


mpush $invest_con  cancelplan '["gahbnbehaskk",13]' -p gahbnbehaskk

