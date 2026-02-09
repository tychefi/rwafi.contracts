rwafi_token=rwa.vtoken
mreg flon $rwafi_token flonian
mtran flonian $rwafi_token "100 FLON"
mset $rwafi_token rwa.vtoken
mcli set account permission $rwafi_token active --add-code


mpush flon.swap settkbanks   "[[\"flon.token\",\"flon.mtoken\",\"sing.token\",\"rwa.vtoken\"]]" -p swap.admin -p flonian

mpush flon.swap  addlpcreator '["rwaverse.io"]' -p swap.admin -p flonian