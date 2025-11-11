#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

yield_con=yieldrwa1111
mreg flon $yield_con flonian
mtran flonian $yield_con "100 FLON"
mset $yield_con yield.rwa
mcli set account permission $yield_con active --add-code