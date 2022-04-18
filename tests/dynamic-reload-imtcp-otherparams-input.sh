#!/bin/bash
#export RSYSLOG_DEBUG="debug nostdout noprintmutexaction"
#export RSYSLOG_DEBUGLOG="asd.debug"
. ${srcdir:=.}/diag.sh init
generate_conf
add_conf '
global(
	# enable dynamic configuration reload via HUP
	hup.reload.config="on"
)

module(load="../plugins/imtcp/.libs/imtcp")
input(type="imtcp" port="0" listenPortFileName="'$RSYSLOG_DYNNAME'.tcpflood_port")

template(name="outfmt" type="string" string="%msg:F,58:2%\n")
:msg, contains, "msgnum:" action(type="omfile" template="outfmt"
			         file="'$RSYSLOG_OUT_LOG'")
'
startup
add_conf '
input(type="imtcp" port="0" listenPortFileName="'$RSYSLOG_DYNNAME'.tcpflood_port2" RateLimit.Interval="5" RateLimit.Burst="20000" MaxListeners="10")
'
sleep 2
reloadConfig 1
sleep 2
assign_tcpflood_port2 "${RSYSLOG_DYNNAME}.tcpflood_port2"
tcpflood -p$TCPFLOOD_PORT2 -m5000
shutdown_when_empty
wait_shutdown
seq_check 0 4999
exit_test
