


invest_con=rwaverse.io
mreg flon $invest_con flonian
mtran flonian $invest_con "100 FLON"
mset $invest_con rwaverse.io
mcli set account permission $invest_con active --add-code


mpush $invest_con init '["flonian"]' -p $invest_con
mpush $invest_con addtoken '["sing.token","8,SING"]' -p $invest_con


guaranty_con=rwaguarapool
mreg flon $guaranty_con flonian
mtran flonian $guaranty_con "100 FLON"
mset $guaranty_con rwaguarapool
mcli set account permission $guaranty_con active --add-code

mpush   $guaranty_con init  '["flonian"]' -p $guaranty_con


stake_con=rwastakepool
mreg flon $stake_con flonian
mtran flonian $stake_con "100 FLON"
mset $stake_con rwastakepool
mcli set account permission $stake_con active --add-code

mpush $stake_con init '["flonian","rwaverse.io"]' -p $stake_con

yield_con=rwayieldpool
mreg flon $yield_con flonian
mtran flonian $yield_con "100 FLON"
mset $yield_con rwayieldpool
mcli set account permission $yield_con active --add-code

mpush  $yield_con init  '["flonian"]' -p $yield_con


rwafi_token=rwa.vtoken
mreg flon $rwafi_token flonian
mtran flonian $rwafi_token "100 FLON"
mset $rwafi_token rwa.vtoken
mcli set account permission $rwafi_token active --add-code


mpush flon.swap settkbanks   "[[\"flon.token\",\"flon.mtoken\",\"sing.token\",\"rwa.vtoken\"]]" -p swap.admin -p flonian
mpush flon.swap  addlpcreator '["rwaverse.io"]' -p swap.admin -p flonian


mpush $invest_con setoracle '["flonian", true]' -p $invest_con
#合约刷新需要
mpush $invest_con setoracle '["rwastakepool", true]' -p $invest_con
mpush $invest_con setoracle '["rwayieldpool", true]' -p $invest_con
mpush $invest_con setoracle '["rwaguarapool", true]' -p $invest_con

