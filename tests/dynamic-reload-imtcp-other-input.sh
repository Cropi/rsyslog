#!/bin/bash
. ${srcdir:=.}/diag.sh init
generate_conf
add_conf '
global(
	# enable dynamic configuration reload via HUP
	hup.reload.config="on"
)

module(load="../plugins/imtcp/.libs/imtcp")
input(type="imtcp" port="514")

template(name="outfmt" type="string" string="%msg:F,58:2%\n")
:msg, contains, "msgnum:" action(type="omfile" template="outfmt"
			         file="'$RSYSLOG_OUT_LOG'")
'
startup

generate_conf
rewrite_conf '
global(
	# enable dynamic configuration reload via HUP
	hup.reload.config="on"
)

module(load="../plugins/imtcp/.libs/imtcp")
input(type="imtcp" port="515")

template(name="outfmt" type="string" string="%msg:F,58:2%\n")
:msg, contains, "msgnum:" action(type="omfile" template="outfmt"
			         file="'$RSYSLOG_OUT_LOG'")
'
reloadConfig 1
sleep 2
tcpflood -p515 -m5000
shutdown_when_empty
wait_shutdown
seq_check 0 4999
exit_test
