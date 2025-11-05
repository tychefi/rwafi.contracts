rwafi_token=rwafi.token
mreg flon $rwafi_token flonian
mtran flonian $rwafi_token "100 FLON"
mset $rwafi_token flon.token
mcli set account permission $rwafi_token active --add-code