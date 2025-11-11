#!/bin/bash
shopt -s expand_aliases
source ~/.bashrc

guaranty_con=guaranty1111
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con guaranty.rwa
mcli set account permission $guaranty_con active --add-code