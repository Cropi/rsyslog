#!/bin/bash
# added 2015-11-16 by singh.janmejay
# This file is part of the rsyslog project, released under ASL 2.0
export RSYSLOG_DEBUGLOG="xd.log"
export RSYSLOG_DEBUG="debug nostdout"
echo \[dynstats_ctr_reset.sh\]: test to ensure correctness of stats-ctr reset
. ${srcdir:=.}/diag.sh init
generate_conf
add_conf '
ruleset(name="stats") {
  action(type="omfile" file="'${RSYSLOG_DYNNAME}'.out.stats.log")
}

global(hup.reload.config="on")

template(name="outfmt" type="string" string="%msg%\n")

dyn_stats(name="msg_stats_resettable_on" resettable="on")
dyn_stats(name="msg_stats_resettable_off" resettable="off")
dyn_stats(name="msg_stats_resettable_default")

set $.msg_prefix = field($msg, 32, 1);

set $.x = dyn_inc("msg_stats_resettable_on", $.msg_prefix);
set $.y = dyn_inc("msg_stats_resettable_off", $.msg_prefix);
set $.z = dyn_inc("msg_stats_resettable_default", $.msg_prefix);

action(type="omfile" file=`echo $RSYSLOG_OUT_LOG` template="outfmt")
'
startup
sleep 1
reloadConfig 1
sleep 1

injectmsg_file $srcdir/testsuites/dynstats_input_1
injectmsg_file $srcdir/testsuites/dynstats_input_2
wait_queueempty
sleep 1
injectmsg_file $srcdir/testsuites/dynstats_input_3
wait_queueempty
sleep 1
echo doing shutdown
shutdown_when_empty
echo wait on shutdown
wait_shutdown
exit_test
