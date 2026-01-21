rwafi_token=rwafi.token
mreg flon $rwafi_token flonian
mtran flonian $rwafi_token "100 FLON"
mset $rwafi_token rwafi.token
mcli set account permission $rwafi_token active --add-code


mpush flon.swap settkbanks   "[[\"flon.token\",\"flon.mtoken\",\"sing.token\",\"rwafi.token\"]]" -p swap.admin -p flonian

mpush flon.swap  addlpcreator '["invest.rwa"]' -p swap.admin -p flonian