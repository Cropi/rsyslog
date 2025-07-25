/* The kernel log module.
 *
 * This is rsyslog Linux only module for reading structured kernel logs.
 * Module is based on imklog module so it retains its structure
 * and other part is currently in kmsg.c file instead of this (imkmsg.c)
 * For more information see that file.
 *
 * To test under Linux:
 * echo test1 > /dev/kmsg
 *
 * Copyright (C) 2008-2023 Adiscon GmbH
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "dirty.h"
#include "cfsysline.h"
#include "obj.h"
#include "msg.h"
#include "module-template.h"
#include "datetime.h"
#include "imkmsg.h"
#include "net.h"
#include "glbl.h"
#include "prop.h"
#include "errmsg.h"
#include "unicode-helper.h"

MODULE_TYPE_INPUT;
MODULE_TYPE_NOKEEP;
MODULE_CNFNAME("imkmsg")

/* Module static data */
DEF_IMOD_STATIC_DATA;
DEFobjCurrIf(datetime) DEFobjCurrIf(glbl) DEFobjCurrIf(prop) DEFobjCurrIf(net)

    /* config settings */
    typedef struct configSettings_s {
    int iFacilIntMsg; /* the facility to use for internal messages (set by driver) */
} configSettings_t;
static configSettings_t cs;

static modConfData_t *loadModConf = NULL; /* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL; /* modConf ptr to use for the current load process */
static int bLegacyCnfModGlobalsPermitted; /* are legacy module-global config parameters permitted? */

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {{"parsekerneltimestamp", eCmdHdlrGetWord, 0},
                                           {"readmode", eCmdHdlrGetWord, 0},
                                           {"expectedbootcompleteseconds", eCmdHdlrPositiveInt, 0}};
static struct cnfparamblk modpblk = {CNFPARAMBLK_VERSION, sizeof(modpdescr) / sizeof(struct cnfparamdescr), modpdescr};

static prop_t *pInputName = NULL;
/* there is only one global inputName for all messages generated by this module */
static prop_t *pLocalHostIP = NULL; /* a pseudo-constant propterty for 127.0.0.1 */

static inline void initConfigSettings(void) {
    cs.iFacilIntMsg = klogFacilIntMsg();
}


/* enqueue the the kernel message into the message queue.
 * The provided msg string is not freed - thus must be done
 * by the caller.
 * rgerhards, 2008-04-12
 */
static rsRetVal enqMsg(uchar *msg, uchar *pszTag, syslog_pri_t pri, struct timeval *tp, struct json_object *json) {
    struct syslogTime st;
    smsg_t *pMsg;
    DEFiRet;

    assert(msg != NULL);
    assert(pszTag != NULL);

    if (tp == NULL) {
        CHKiRet(msgConstruct(&pMsg));
    } else {
        datetime.timeval2syslogTime(tp, &st, TIME_IN_LOCALTIME);
        CHKiRet(msgConstructWithTime(&pMsg, &st, tp->tv_sec));
    }
    MsgSetFlowControlType(pMsg, eFLOWCTL_LIGHT_DELAY);
    MsgSetInputName(pMsg, pInputName);
    MsgSetRawMsgWOSize(pMsg, (char *)msg);
    MsgSetMSGoffs(pMsg, 0); /* we do not have a header... */
    MsgSetRcvFrom(pMsg, glbl.GetLocalHostNameProp());
    MsgSetRcvFromIP(pMsg, pLocalHostIP);
    MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
    MsgSetTAG(pMsg, pszTag, ustrlen(pszTag));
    msgSetPRI(pMsg, pri);
    pMsg->json = json;
    CHKiRet(submitMsg2(pMsg));

finalize_it:
    RETiRet;
}


/* log an imkmsg-internal message
 * rgerhards, 2008-04-14
 */
rsRetVal imkmsgLogIntMsg(syslog_pri_t priority, const char *fmt, ...) {
    DEFiRet;
    va_list ap;
    uchar msgBuf[2048]; /* we use the same size as sysklogd to remain compatible */

    va_start(ap, fmt);
    vsnprintf((char *)msgBuf, sizeof(msgBuf), fmt, ap);
    va_end(ap);

    logmsgInternal(NO_ERRCODE, priority, msgBuf, 0);

    RETiRet;
}


/* log a message from /dev/kmsg
 */
rsRetVal Syslog(syslog_pri_t priority, uchar *pMsg, struct timeval *tp, struct json_object *json) {
    DEFiRet;
    iRet = enqMsg((uchar *)pMsg, (uchar *)"kernel:", priority, tp, json);
    RETiRet;
}


/* helper for some klog drivers which need to know the MaxLine global setting. They can
 * not obtain it themselfs, because they are no modules and can not query the object hander.
 * It would probably be a good idea to extend the interface to support it, but so far
 * we create a (sufficiently valid) work-around. -- rgerhards, 2008-11-24
 */
int klog_getMaxLine(void) {
    return glbl.GetMaxLine(runModConf->pConf);
}


BEGINrunInput
    CODESTARTrunInput;
    /* this is an endless loop - it is terminated when the thread is
     * signalled to do so. This, however, is handled by the framework,
     * right into the sleep below.
     */
    while (!pThrd->bShallStop) {
        /* klogLogKMsg() waits for the next kernel message, obtains it
         * and then submits it to the rsyslog main queue.
         * rgerhards, 2008-04-09
         */
        CHKiRet(klogLogKMsg(runModConf));
    }
finalize_it:
ENDrunInput


BEGINbeginCnfLoad
    CODESTARTbeginCnfLoad;
    loadModConf = pModConf;
    pModConf->pConf = pConf;
    /* init our settings */
    pModConf->iFacilIntMsg = klogFacilIntMsg();
    pModConf->parseKernelStamp = KMSG_PARSE_TS_STARTUP_ONLY;
    pModConf->readMode = KMSG_READMODE_FULL_BOOT;
    pModConf->expected_boot_complete_secs = 90;
    loadModConf->configSetViaV2Method = 0;
    bLegacyCnfModGlobalsPermitted = 1;
    /* init legacy config vars */
    initConfigSettings();
ENDbeginCnfLoad


BEGINsetModCnf
    struct cnfparamvals *pvals = NULL;
    int i;
    CODESTARTsetModCnf;
    pvals = nvlstGetParams(lst, &modpblk, NULL);
    if (pvals == NULL) {
        LogError(0, RS_RET_MISSING_CNFPARAMS,
                 "error processing module "
                 "config parameters [module(...)]");
        ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
    }

    if (Debug) {
        dbgprintf("module (global) param blk for imkmsg:\n");
        cnfparamsPrint(&modpblk, pvals);
    }

    for (i = 0; i < modpblk.nParams; ++i) {
        if (!pvals[i].bUsed) continue;
        if (!strcmp(modpblk.descr[i].name, "parsekerneltimestamp")) {
            if (!es_strconstcmp(pvals[i].val.d.estr, "on") || !es_strconstcmp(pvals[i].val.d.estr, "always")) {
                loadModConf->parseKernelStamp = KMSG_PARSE_TS_ALWAYS;
            } else if (!es_strconstcmp(pvals[i].val.d.estr, "startup")) {
                loadModConf->parseKernelStamp = KMSG_PARSE_TS_STARTUP_ONLY;
            } else if (!es_strconstcmp(pvals[i].val.d.estr, "off")) {
                loadModConf->parseKernelStamp = KMSG_PARSE_TS_OFF;
            } else {
                const char *const cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
                LogError(0, RS_RET_PARAM_ERROR,
                         "imkmsg: unknown "
                         "parse mode '%s'",
                         cstr);
                free((void *)cstr);
            }
        } else if (!strcmp(modpblk.descr[i].name, "expectedbootcompleteseconds")) {
            loadModConf->expected_boot_complete_secs = pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "readmode")) {
            if (!es_strconstcmp(pvals[i].val.d.estr, "full-boot")) {
                loadModConf->readMode = KMSG_READMODE_FULL_BOOT;
            } else if (!es_strconstcmp(pvals[i].val.d.estr, "full-always")) {
                loadModConf->readMode = KMSG_READMODE_FULL_ALWAYS;
            } else if (!es_strconstcmp(pvals[i].val.d.estr, "new-only")) {
                loadModConf->readMode = KMSG_READMODE_NEW_ONLY;
            } else {
                const char *const cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
                LogError(0, RS_RET_PARAM_ERROR,
                         "imkmsg: unknown "
                         "read mode '%s', keeping default setting",
                         cstr);
                free((void *)cstr);
            }
        } else {
            LogMsg(0, RS_RET_INTERNAL_ERROR, LOG_WARNING,
                   "imkmsg: RSYSLOG BUG, non-handled param '%s' in "
                   "beginCnfLoad\n",
                   modpblk.descr[i].name);
        }
    }

    /* disable legacy module-global config directives */
    bLegacyCnfModGlobalsPermitted = 0;
    loadModConf->configSetViaV2Method = 1;

finalize_it:
    if (pvals != NULL) cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf


BEGINendCnfLoad
    CODESTARTendCnfLoad;
    if (!loadModConf->configSetViaV2Method) {
        /* persist module-specific settings from legacy config system */
        loadModConf->iFacilIntMsg = cs.iFacilIntMsg;
    }

    loadModConf = NULL; /* done loading */
ENDendCnfLoad


BEGINcheckCnf
    CODESTARTcheckCnf;
ENDcheckCnf


BEGINactivateCnfPrePrivDrop
    CODESTARTactivateCnfPrePrivDrop;
    runModConf = pModConf;
    iRet = klogWillRunPrePrivDrop(runModConf);
ENDactivateCnfPrePrivDrop


BEGINactivateCnf
    CODESTARTactivateCnf;
ENDactivateCnf


BEGINfreeCnf
    CODESTARTfreeCnf;
ENDfreeCnf


BEGINwillRun
    CODESTARTwillRun;
    iRet = klogWillRunPostPrivDrop(runModConf);
ENDwillRun


BEGINafterRun
    CODESTARTafterRun;
    iRet = klogAfterRun(runModConf);
ENDafterRun


BEGINmodExit
    CODESTARTmodExit;
    if (pInputName != NULL) prop.Destruct(&pInputName);
    if (pLocalHostIP != NULL) prop.Destruct(&pLocalHostIP);

    /* release objects we used */
    objRelease(glbl, CORE_COMPONENT);
    objRelease(net, CORE_COMPONENT);
    objRelease(datetime, CORE_COMPONENT);
    objRelease(prop, CORE_COMPONENT);
ENDmodExit


BEGINqueryEtryPt
    CODESTARTqueryEtryPt;
    CODEqueryEtryPt_STD_IMOD_QUERIES;
    CODEqueryEtryPt_STD_CONF2_QUERIES;
    CODEqueryEtryPt_STD_CONF2_PREPRIVDROP_QUERIES;
    CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES;
ENDqueryEtryPt

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) * pp, void __attribute__((unused)) * pVal) {
    cs.iFacilIntMsg = klogFacilIntMsg();
    return RS_RET_OK;
}

BEGINmodInit()
    CODESTARTmodInit;
    *ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
    CODEmodInit_QueryRegCFSLineHdlr CHKiRet(objUse(datetime, CORE_COMPONENT));
    CHKiRet(objUse(glbl, CORE_COMPONENT));
    CHKiRet(objUse(prop, CORE_COMPONENT));
    CHKiRet(objUse(net, CORE_COMPONENT));

    /* we need to create the inputName property (only once during our lifetime) */
    CHKiRet(prop.CreateStringProp(&pInputName, UCHAR_CONSTANT("imkmsg"), sizeof("imkmsg") - 1));
    CHKiRet(prop.CreateStringProp(&pLocalHostIP, UCHAR_CONSTANT("127.0.0.1"), sizeof("127.0.0.1") - 1));

    /* init legacy config settings */
    initConfigSettings();

    CHKiRet(omsdRegCFSLineHdlr((uchar *)"debugprintkernelsymbols", 0, eCmdHdlrGoneAway, NULL, NULL,
                               STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogsymbollookup", 0, eCmdHdlrGoneAway, NULL, NULL, STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogsymbolstwice", 0, eCmdHdlrGoneAway, NULL, NULL, STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr((uchar *)"klogusesyscallinterface", 0, eCmdHdlrGoneAway, NULL, NULL,
                               STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL,
                               STD_LOADABLE_MODULE_ID));
ENDmodInit
