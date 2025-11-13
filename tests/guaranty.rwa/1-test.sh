#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

guaranty_con=guaranty1111
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con guaranty.rwa
mcli set account permission $guaranty_con active --add-code





mpush   $guaranty_con init  '["flonian"]' -p $guaranty_con



mpush sing.token transfer '["gahbnbehaskk", "guaranty1111", "300.00000000 SING", "guaranty:8"]' -p gahbnbehaskk
mpush $guaranty_con redeem '["gahbnbehaskk",7,"100.00000000 SING"]' -p gahbnbehaskk


mpush $invest_con  cancelplan '["gahbnbehaskk",7]' -p gahbnbehaskk

