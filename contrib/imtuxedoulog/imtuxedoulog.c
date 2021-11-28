/* imtuxedoulog.c
 *
 * This is the input module for reading Tuxedo ULOG files. The particularity of this file
 * is that the timestamp is split between the filename (date) and the log line (time).
 * So this module switches on the date base betwwen files to open only the current file.
 * The log line is parsed according to the Tuxedo format. The ECID is extracted as a
 * structured data attribute.
 *
 * Work originally begun on 2019-01-11 by Philippe Duveau
 *
 * This file is contribution of rsyslog.
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
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>		/* do NOT remove: will soon be done by the module generation macros */
#include <sys/types.h>
#include <unistd.h>
#include <glob.h>
#include <poll.h>
#include <fnmatch.h>
#ifdef HAVE_SYS_TIME_H
#	include <sys/time.h>
#endif
#ifdef HAVE_SYS_STAT_H
#	include <sys/stat.h>
#endif
#ifdef _AIX
# include <alloca.h>
#endif
#include "rsyslog.h"		/* error codes etc... */
#include "dirty.h"
#include "cfsysline.h"		/* access to config file objects */
#include "module-template.h"	/* generic module interface code - very important, read it! */
#include "srUtils.h"		/* some utility functions */
#include "msg.h"
#include "stream.h"
#include "errmsg.h"
#include "glbl.h"
#include "unicode-helper.h"
#include "prop.h"
#include "stringbuf.h"
#include "ruleset.h"
#include "ratelimit.h"

struct instanceConf_s {
	uchar *pszUlogBaseName;
	uchar *pszCurrFName;
	struct tm currTm;
	uchar *pszTag;
	size_t lenTag;
	uchar *pszStateFile;
	uchar *pszBindRuleset;
	int nMultiSub;
	int iPersistStateInterval;
	int iFacility;
	int iSeverity;
	strm_t *pStrm;	/* its stream (NULL if not assigned) */
	int maxLinesAtOnce;
	ruleset_t *pBindRuleset;	/* ruleset to bind listener to (use system default if unspecified) */
	ratelimit_t *ratelimiter;
	multi_submit_t multiSub;
	int nRecords;
	struct instanceConf_s *next;
	struct instanceConf_s *prev;
};

/* config variables */
struct modConfData_s {
	rsconf_t *pConf;  /* our overall config object */
};

static instanceConf_t *confRoot = NULL;
static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for run process */

MODULE_TYPE_INPUT	/* must be present for input modules, do not remove */
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imtuxedoulog")

/* defines */

/* Module static data */
DEF_IMOD_STATIC_DATA	/* must be present, starts static data */
DEFobjCurrIf(glbl)
DEFobjCurrIf(strm)
DEFobjCurrIf(prop)
DEFobjCurrIf(ruleset)

#define NUM_MULTISUB 1024 /* default max number of submits */
#define DFLT_PollInterval 10

int iPollInterval = DFLT_PollInterval;
int iPersistStateInterval = 0;	/* how often if state file to be persisted? (default 0->never) */

struct syslogTime syslogTz;

static prop_t *pInputName = NULL;
/* there is only one global inputName for all messages generated by this input */

/* module-global parameters */
/* input instance parameters */
static struct cnfparamdescr inppdescr[] = {
	{ "ulogbase", eCmdHdlrString, CNFPARAM_REQUIRED },
	{ "tag", eCmdHdlrString, CNFPARAM_REQUIRED },
	{ "severity", eCmdHdlrSeverity, 0 },
	{ "facility", eCmdHdlrFacility, 0 },
	{ "ruleset", eCmdHdlrString, 0 },
	{ "maxlinesatonce", eCmdHdlrInt, 0 },
	{ "persiststateinterval", eCmdHdlrInt, 0 },
	{ "maxsubmitatonce", eCmdHdlrInt, 0 }
};
static struct cnfparamblk inppblk =
	{ CNFPARAMBLK_VERSION,
		sizeof(inppdescr)/sizeof(struct cnfparamdescr),
		inppdescr
	};

#include "im-helper.h" /* must be included AFTER the type definitions! */

static uchar * mkFileNameWithTime(instanceConf_t *in)
{
	uchar out[MAXFNAME];
	struct timeval tp;
#if defined(__hpux)
	struct timezone tz;
	gettimeofday(&tp, &tz);
#else
	gettimeofday(&tp, NULL);
#endif
	localtime_r(&tp.tv_sec, &(in->currTm));
	snprintf((char*)out, MAXFNAME, "%s.%02d%02d%02d", (char*)in->pszUlogBaseName,
			in->currTm.tm_mon+1, in->currTm.tm_mday, in->currTm.tm_year % 100);
	return ustrdup(out);
}

/*
*	Helper function to combine statefile and workdir
*/
static int getFullStateFileName(uchar* pszstatefile, uchar* pszout, int ilenout)
{
	int lenout;
	const uchar* pszworkdir;

	/* Get Raw Workdir, if it is NULL we need to propper handle it */
	pszworkdir = glblGetWorkDirRaw(runModConf);

	/* Construct file name */
	lenout = snprintf((char*)pszout, ilenout, "%s/%s",
					(char*) (pszworkdir == NULL ? "." : (char*) pszworkdir), (char*)pszstatefile);

	/* return out length */
	return lenout;
}

/* this generates a state file name suitable for the current file. To avoid
 * malloc calls, it must be passed a buffer which should be MAXFNAME large.
 * Note: the buffer is not necessarily populated ... always ONLY use the
 * RETURN VALUE!
 */
static uchar * ATTR_NONNULL(2) getStateFileName(instanceConf_t *const __restrict__ pInst,
		uchar *const __restrict__ buf,
		size_t lenbuf,
		const uchar *pszFileName)
{
	uchar *ret;

	/* Use pszFileName parameter if set */
	pszFileName = pszFileName == NULL ? pInst->pszUlogBaseName : pszFileName;

	DBGPRINTF("getStateFileName for '%s'\n", pszFileName);
	if(pInst == NULL || pInst->pszStateFile == NULL) {
		lenbuf = snprintf((char*)buf, lenbuf - 1, "imtuxedoulog-state:%s", pszFileName);
		buf[lenbuf] = '\0'; /* be on the safe side... */
		uchar *p = buf;
		for( ; *p ; ++p) {
			if(*p == '/')
				*p = '-';
		}
		ret = buf;
	} else {
		ret = pInst->pszStateFile;
	}
	return ret;
}

/* this func parses the line according to samples described in README.md
 */
static rsRetVal parseMsg(smsg_t *pMsg, char *rawMsg, size_t msgLen,
						instanceConf_t *const __restrict__ pInst) {
	char *prog, *host, *text = NULL, *strtData = NULL, *tmp;
	int hour, min, sec;
	rsRetVal ret;

	hour = (rawMsg[0] ^ 0x30) * 10 + (rawMsg[1] ^ 0x30);
	min  = (rawMsg[2] ^ 0x30) * 10 + (rawMsg[3] ^ 0x30);
	sec  = (rawMsg[4] ^ 0x30) * 10 + (rawMsg[5] ^ 0x30);

	if (hour < 0 || hour > 23 || min < 0 || min > 59
							 || sec < 0 || sec > 59)
		return RS_RET_COULD_NOT_PARSE;

	host = rawMsg + ((rawMsg[10] == '.') ? 11 : 10);

	prog = memchr(host, '!', msgLen-(host - rawMsg));

	if (prog == NULL)
		return RS_RET_COULD_NOT_PARSE;

	prog++;

	strtData = memchr(prog, ':', msgLen-(prog - rawMsg));

	if (strtData == NULL)
		return RS_RET_COULD_NOT_PARSE;

	pMsg->tTIMESTAMP.year = pInst->currTm.tm_year + 1900;
	pMsg->tTIMESTAMP.month = pInst->currTm.tm_mon + 1;
	pMsg->tTIMESTAMP.day = pInst->currTm.tm_mday;
	pMsg->tTIMESTAMP.hour = hour;
	pMsg->tTIMESTAMP.minute = min;
	pMsg->tTIMESTAMP.second = sec;
	pMsg->tTIMESTAMP.OffsetMode = syslogTz.OffsetMode;
	pMsg->tTIMESTAMP.OffsetHour = syslogTz.OffsetHour;
	pMsg->tTIMESTAMP.OffsetMinute = syslogTz.OffsetMinute;

	pMsg->tTIMESTAMP.secfrac = atoi(rawMsg+7);
	/* secfracprecision depends on the char on position 9 (case 1 or case 2) */
	pMsg->tTIMESTAMP.secfracPrecision = (rawMsg[9]=='.') ? 2 : 3;

	for (tmp = strtData ; prog < tmp && *tmp!='.'; tmp--)
		;
	if (tmp > prog)
		*tmp = '\0';
	else
		*strtData = '\0';

	strtData = strtData + 2;

	/* Case 4 */
	if (memcmp(strtData, "gtrid", 5) == 0)
	{
		strtData = memchr(strtData, ':', msgLen-(strtData - rawMsg));
		if (strtData != NULL) strtData += 2;
	}

	text = strtData;

	/* ecid point to message text or the word ECID */
	if (strtData != NULL && memcmp(strtData, "ECID", 4) == 0)
	{
		text = memchr(strtData+6, '>', msgLen-(strtData-rawMsg));
		/* case 3 : we have the word ECID */
		if (text != NULL)
		{
			*(--strtData) = '[';
			strtData[5]   = '=';
			strtData[6]   = '\"';
			*text++ = '\"';
			*text++ = ']';
			text++;
			ret = MsgAddToStructuredData(pMsg, (uchar*)strtData, text - strtData);
			if (ret!=RS_RET_OK)
				LogMsg(0, ret, LOG_WARNING, "Add StructuredData to message failed.");
		}
	}

	/* now compute the new length */
	msgLen -= text - rawMsg;

	if (text != NULL)
		MsgSetRawMsg(pMsg, text, msgLen);

	MsgSetMSGoffs(pMsg, 0);

	/* set hostname */
	MsgSetHOSTNAME(pMsg, (const uchar*)host, prog - host - 1);

	if (*prog == '\0')
		return 0;

	/* set procid */
	ret = MsgSetPROCID(pMsg, prog);
	if (ret != RS_RET_OK)
		LogMsg(0, ret, LOG_WARNING, "Set PROCID to message failed.");

	return RS_RET_OK;
}

/* enqueue the read file line as a message. The provided string is
 * not freed - this must be done by the caller.
 */
#define MAX_OFFSET_REPRESENTATION_NUM_BYTES 20
static rsRetVal enqLine(instanceConf_t *const __restrict__ pInst,
			cstr_t *const __restrict__ cstrLine)
{
	DEFiRet;
	smsg_t *pMsg;
	const size_t msgLen = cstrLen(cstrLine);
	rsRetVal ret;

	if(msgLen == 0) {
		/* we do not process empty lines */
		FINALIZE;
	}

	CHKiRet(msgConstruct(&pMsg));

	if (parseMsg(pMsg, (char*)rsCStrGetSzStrNoNULL(cstrLine), msgLen, pInst) != RS_RET_OK) {
		if ((ret = msgDestruct(&pMsg)) != RS_RET_OK)
			LogMsg(0, ret, LOG_ERR, "msgDestruct failed.");
		FINALIZE;
	}

	MsgSetInputName(pMsg, pInputName);
	MsgSetTAG(pMsg, pInst->pszTag, pInst->lenTag);
	msgSetPRI(pMsg, pInst->iFacility | pInst->iSeverity);
	MsgSetRuleset(pMsg, pInst->pBindRuleset);
	if ((ret = MsgSetFlowControlType(pMsg, eFLOWCTL_FULL_DELAY)) != RS_RET_OK)
		LogMsg(0, ret, LOG_WARNING, "Set Flow Control to message failed.");
	if ((ret = MsgSetAPPNAME(pMsg, (const char*)pInst->pszTag)) != RS_RET_OK)
		LogMsg(0, ret, LOG_WARNING, "Set APPNAME to message failed.");

	if ((iRet = ratelimitAddMsg(pInst->ratelimiter, &pInst->multiSub, pMsg)) != RS_RET_OK) {
		if ((ret = msgDestruct(&pMsg)) != RS_RET_OK)
			LogMsg(0, ret, LOG_ERR, "msgDestruct failed.");
	}
finalize_it:
	RETiRet;
}

/* try to open a file which has a state file. If the state file does not
 * exist or cannot be read, an error is returned.
 */
static rsRetVal ATTR_NONNULL(1) openFileWithStateFile(instanceConf_t *const __restrict__ pInst)
{
	DEFiRet;
	strm_t *psSF = NULL;
	uchar pszSFNam[MAXFNAME];
	size_t lenSFNam;
	struct stat stat_buf;
	uchar statefile[MAXFNAME];

	uchar *const statefn = getStateFileName(pInst, statefile, sizeof(statefile), NULL);
	DBGPRINTF("trying to open state for '%s', state file '%s'\n",
			pInst->pszUlogBaseName, statefn);

	/* Get full path and file name */
	lenSFNam = getFullStateFileName(statefn, pszSFNam, sizeof(pszSFNam));

	/* check if the file exists */
	if(stat((char*) pszSFNam, &stat_buf) == -1) {
		if(errno == ENOENT) {
			DBGPRINTF("NO state file (%s) exists for '%s'\n", pszSFNam, pInst->pszUlogBaseName);
			ABORT_FINALIZE(RS_RET_FILE_NOT_FOUND);
		} else {
			char errStr[1024];
			rs_strerror_r(errno, errStr, sizeof(errStr));
			DBGPRINTF("error trying to access state file for '%s':%s\n",
					pInst->pszUlogBaseName, errStr);
			ABORT_FINALIZE(RS_RET_IO_ERROR);
		}
	}

	/* If we reach this point, we have a state file */

	CHKiRet(strm.Construct(&psSF));
	CHKiRet(strm.SettOperationsMode(psSF, STREAMMODE_READ));
	CHKiRet(strm.SetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strm.SetFName(psSF, pszSFNam, lenSFNam));
	CHKiRet(strm.SetFileNotFoundError(psSF, 1));
	CHKiRet(strm.ConstructFinalize(psSF));

	/* read back in the object */
	CHKiRet(obj.Deserialize(&pInst->pStrm, (uchar*) "strm", psSF, NULL, pInst));
	DBGPRINTF("deserialized state file, state file base name '%s', "
			"configured base name '%s'\n", pInst->pStrm->pszFName,
			pInst->pszUlogBaseName);
	if(ustrcmp(pInst->pStrm->pszFName, pInst->pszCurrFName)) {
		LogError(0, RS_RET_STATEFILE_WRONG_FNAME, "imtuxedoulog: state file '%s' "
				"contains file name '%s', but is used for file '%s'. State "
				"file deleted, starting from begin of file.",
				pszSFNam, pInst->pStrm->pszFName, pInst->pszCurrFName);

		unlink((char*)pszSFNam);
		ABORT_FINALIZE(RS_RET_STATEFILE_WRONG_FNAME);
	}

	strm.CheckFileChange(pInst->pStrm);
	CHKiRet(strm.SeekCurrOffs(pInst->pStrm));

	/* note: we do not delete the state file, so that the last position remains
	 * known even in the case that rsyslogd aborts for some reason (like powerfail)
	 */

finalize_it:
	if(psSF != NULL)
		strm.Destruct(&psSF);

	RETiRet;
}

/* try to open a file for which no state file exists. This function does NOT
 * check if a state file actually exists or not -- this must have been
 * checked before calling it.
 */
static rsRetVal openFileWithoutStateFile(instanceConf_t *const __restrict__ pInst)
{
	DEFiRet;

	DBGPRINTF("clean startup withOUT state file for '%s'\n", pInst->pszUlogBaseName);
	if(pInst->pStrm != NULL)
		strm.Destruct(&pInst->pStrm);
	CHKiRet(strm.Construct(&pInst->pStrm));
	CHKiRet(strm.SettOperationsMode(pInst->pStrm, STREAMMODE_READ));
	CHKiRet(strm.SetsType(pInst->pStrm, STREAMTYPE_FILE_MONITOR));
	CHKiRet(strm.SetFName(pInst->pStrm, pInst->pszCurrFName, strlen((char*) pInst->pszCurrFName)));
	CHKiRet(strm.SetFileNotFoundError(pInst->pStrm, 1));
	CHKiRet(strm.ConstructFinalize(pInst->pStrm));

finalize_it:
	RETiRet;
}

/* try to open a file. This involves checking if there is a status file and,
 * if so, reading it in. Processing continues from the last known location.
 */
static rsRetVal openFile(instanceConf_t *const __restrict__ pInst)
{
	DEFiRet;

	CHKiRet_Hdlr(openFileWithStateFile(pInst)) {
		CHKiRet(openFileWithoutStateFile(pInst));
	}

	CHKiRet(strm.SetbReopenOnTruncate(pInst->pStrm, 1));

finalize_it:
	RETiRet;
}

/* This function persists information for a specific file being monitored.
 * To do so, it simply persists the stream object. We do NOT abort on error
 * iRet as that makes matters worse (at least we can try persisting the others...).
 * rgerhards, 2008-02-13
 */
static void persistStrmState(instanceConf_t *pInst)
{
	DEFiRet;
	strm_t *psSF = NULL; /* state file (stream) */
	size_t lenDir;
	uchar statefile[MAXFNAME];

	uchar *const statefn = getStateFileName(pInst, statefile, sizeof(statefile), NULL);
	DBGPRINTF("persisting state for '%s' to file '%s'\n",
			pInst->pszUlogBaseName, statefn);
	CHKiRet(strm.Construct(&psSF));
	lenDir = ustrlen(glbl.GetWorkDir(runModConf));
	if(lenDir > 0)
		CHKiRet(strm.SetDir(psSF, glbl.GetWorkDir(runModConf), lenDir));
	CHKiRet(strm.SettOperationsMode(psSF, STREAMMODE_WRITE_TRUNC));
	CHKiRet(strm.SetsType(psSF, STREAMTYPE_FILE_SINGLE));
	CHKiRet(strm.SetFName(psSF, statefn, strlen((char*) statefn)));
	CHKiRet(strm.SetFileNotFoundError(psSF, 1));
	CHKiRet(strm.ConstructFinalize(psSF));

	CHKiRet(strm.Serialize(pInst->pStrm, psSF));
	CHKiRet(strm.Flush(psSF));

	CHKiRet(strm.Destruct(&psSF));

finalize_it:
	if(psSF != NULL)
		strm.Destruct(&psSF);

	if(iRet != RS_RET_OK) {
		LogError(0, iRet, "imtuxedoulog: could not persist state "
				"file %s - data may be repeated on next "
				"startup. Is WorkDirectory set?",
				statefn);
	}
}

/* The following is a cancel cleanup handler for strmReadLine(). It is necessary in case
 * strmReadLine() is cancelled while processing the stream. -- rgerhards, 2008-03-27
 */
static void pollFileCancelCleanup(void *pArg)
{
	cstr_t **ppCStr = (cstr_t**) pArg;
	if(*ppCStr != NULL) {
		rsCStrDestruct(ppCStr);
	}
}

static void pollFileReal(instanceConf_t *pInst, int *pbHadFileData, cstr_t **ppCStr)
{
	DEFiRet;

	int nProcessed = 0;
	if(pInst->pStrm == NULL) {
		CHKiRet(openFile(pInst)); /* open file */
	}

	/* loop below will be exited when strmReadLine() returns EOF */
	while(glbl.GetGlobalInputTermState() == 0) {
		if(pInst->maxLinesAtOnce != 0 && nProcessed >= pInst->maxLinesAtOnce)
			break;
		CHKiRet(strm.ReadLine(pInst->pStrm, ppCStr, 0, 0, NULL, -1, NULL));
		++nProcessed;
		if(pbHadFileData != NULL)
			*pbHadFileData = 1; /* this is just a flag, so set it and forget it */
		CHKiRet(enqLine(pInst, *ppCStr)); /* process line */
		rsCStrDestruct(ppCStr); /* discard string (must be done by us!) */

		if(pInst->iPersistStateInterval > 0 && ++pInst->nRecords >= pInst->iPersistStateInterval) {
			persistStrmState(pInst);
			pInst->nRecords = 0;
		}
	}

finalize_it:
	multiSubmitFlush(&pInst->multiSub);

	if(*ppCStr != NULL) {
		rsCStrDestruct(ppCStr);
	}
}

/* poll a file, need to check file rollover etc. open file if not open */
static void pollFile(instanceConf_t *pInst, int *pbHadFileData)
{
	cstr_t *pCStr = NULL;
	/* Note: we must do pthread_cleanup_push() immediately, because the POSIX macros
	 * otherwise do not work if I include the _cleanup_pop() inside an if... -- rgerhards, 2008-08-14
	 */
	pthread_cleanup_push(pollFileCancelCleanup, &pCStr);
	pollFileReal(pInst, pbHadFileData, &pCStr);
	pthread_cleanup_pop(0);
}


/* create input instance, set default parameters, and
 * add it to the list of instances.
 */
static rsRetVal ATTR_NONNULL(1) createInstance(instanceConf_t **const pinst)
{
	instanceConf_t *inst;
	DEFiRet;
	CHKmalloc(inst = malloc(sizeof(instanceConf_t)));
	inst->next = NULL;
	inst->pBindRuleset = NULL;
	inst->ratelimiter = NULL;
	inst->pStrm = NULL;
	inst->multiSub.ppMsgs = NULL;

	inst->pszBindRuleset = NULL;
	inst->pszUlogBaseName = NULL;
	inst->pszCurrFName = NULL;
	inst->pszTag = NULL;
	inst->pszStateFile = NULL;
	inst->nMultiSub = NUM_MULTISUB;
	inst->iSeverity = 5;
	inst->iFacility = 128;
	inst->maxLinesAtOnce = 0;
	inst->iPersistStateInterval = 0;
	inst->nRecords = 0;

	*pinst = inst;
finalize_it:
	RETiRet;
}

/* This adds a new listener object to the bottom of the list, but
 * it does NOT initialize any data members except for the list
 * pointers themselves.
 */
static rsRetVal ATTR_NONNULL() lstnAdd(instanceConf_t *pInst)
{
	DEFiRet;
	CHKiRet(ratelimitNew(&pInst->ratelimiter, "imtuxedoulog", (char*)pInst->pszUlogBaseName));
	CHKmalloc(pInst->multiSub.ppMsgs = malloc(pInst->nMultiSub * sizeof(smsg_t *)));
	pInst->multiSub.maxElem = pInst->nMultiSub;
	pInst->multiSub.nElem = 0;

	/* insert it at the begin of the list */
	pInst->prev = NULL;
	pInst->next = confRoot;

	if (confRoot != NULL)
		confRoot->prev = pInst;

	confRoot = pInst;

finalize_it:
	RETiRet;
}

/* delete a listener object */
static void ATTR_NONNULL(1) lstnDel(instanceConf_t *pInst)
{
	DBGPRINTF("lstnDel called for %s\n", pInst->pszUlogBaseName);
	if(pInst->pStrm != NULL) { /* stream open? */
		persistStrmState(pInst);
		strm.Destruct(&(pInst->pStrm));
	}
	if (pInst->ratelimiter != NULL)
		ratelimitDestruct(pInst->ratelimiter);
	if (pInst->multiSub.ppMsgs != NULL)
		free(pInst->multiSub.ppMsgs);

	free(pInst->pszUlogBaseName);
	if (pInst->pszCurrFName != NULL)
		free(pInst->pszCurrFName);
	if (pInst->pszTag)
		free(pInst->pszTag);
	if (pInst->pszStateFile)
		free(pInst->pszStateFile);
	if (pInst->pszBindRuleset != NULL)
		free(pInst->pszBindRuleset);
	free(pInst);
}

/* Monitor files in traditional polling mode.
 */
static void do_polling(void)
{
	int bHadFileData; /* were there at least one file with data during this run? */
	struct stat sb;
	while(glbl.GetGlobalInputTermState() == 0) {
		do {
			instanceConf_t *pInst;
			bHadFileData = 0;
			for(pInst = confRoot ; pInst != NULL ; pInst = pInst->next) {
				uchar *temp = mkFileNameWithTime(pInst);

				DBGPRINTF("imtuxedoulog: do_polling start '%s' / '%s'\n", pInst->pszUlogBaseName, temp);
				/*
				 * Is the file name is different : a rotation time is reached
				 * If so, then it the new file exists ? and is a file ?
				 */
				if (temp && stat((const char*)temp, &sb) == 0 && S_ISREG(sb.st_mode) &&
						(pInst->pszCurrFName == NULL ||
						strcmp((char*)temp,(char*)pInst->pszCurrFName) != 0))
				{
					DBGPRINTF("imtuxedoulog: timed file : rotation reach "
							"switching form '%s' to '%s' !",
							(char*)pInst->pszUlogBaseName, temp );

					/* first of all change the listener datas */
					if (pInst->pszCurrFName != NULL) {
						free(pInst->pszCurrFName);
						strm.Destruct(&pInst->pStrm);
					}
					pInst->pszCurrFName = temp;
					temp = NULL;

					/* And finish by destroy the stream object, so the next polling will recreate
					 * it based on new data.
					 */
					if(glbl.GetGlobalInputTermState() == 1)
						break; /* terminate input! */
				}
				if (temp) free(temp);

				/* let's poll the file */
				if (pInst->pszCurrFName != NULL)
					pollFile(pInst, &bHadFileData);

				DBGPRINTF("imtuxedoulog: do_polling end for '%s'\n", pInst->pszUlogBaseName);
				if (pInst->iPersistStateInterval == -1)
					persistStrmState(pInst);
			}
		} while(bHadFileData == 1 && glbl.GetGlobalInputTermState() == 0);

		/* Note: the additional 10ns wait is vitally important. It guards rsyslog
		 * against totally hogging the CPU if the users selects a polling interval
		 * of 0 seconds. It doesn't hurt any other valid scenario. So do not remove.
		 * rgerhards, 2008-02-14
		 */
		if(glbl.GetGlobalInputTermState() == 0)
			srSleep(iPollInterval, 10);
	}
}

BEGINnewInpInst
	struct cnfparamvals *pvals;
	instanceConf_t *inst;
	int i;
CODESTARTnewInpInst
	DBGPRINTF("newInpInst (imtuxedoulog)\n");

	pvals = nvlstGetParams(lst, &inppblk, NULL);
	if(pvals == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if(Debug) {
		DBGPRINTF("input param blk in imtuxedoulog:\n");
		cnfparamsPrint(&inppblk, pvals);
	}

	CHKiRet(createInstance(&inst));

	for(i = 0 ; i < inppblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(inppblk.descr[i].name, "ulogbase")) {
			inst->pszUlogBaseName = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(inppblk.descr[i].name, "tag")) {
			inst->pszTag = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
			inst->lenTag = es_strlen(pvals[i].val.d.estr);
		} else if(!strcmp(inppblk.descr[i].name, "ruleset")) {
			inst->pszBindRuleset = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(inppblk.descr[i].name, "severity")) {
			inst->iSeverity = pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "facility")) {
			inst->iFacility = pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "maxlinesatonce")) {
			inst->maxLinesAtOnce = pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "persiststateinterval")) {
			inst->iPersistStateInterval = pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "maxsubmitatonce")) {
			inst->nMultiSub = pvals[i].val.d.n;
		} else {
			DBGPRINTF("program error, non-handled "
				"param '%s'\n", inppblk.descr[i].name);
		}
	}
	if(inst->pszUlogBaseName == NULL) {
		lstnDel(inst);
		LogError(0, RS_RET_FILE_NOT_SPECIFIED,
			"ulogbase is not configured - no input will be gathered");
		ABORT_FINALIZE(RS_RET_FILE_NOT_SPECIFIED);
	}

	if ((iRet = lstnAdd(inst)) != RS_RET_OK) {
		LogError(0, iRet,
			"add input %s to list failed", inst->pszUlogBaseName);
		lstnDel(inst);
		ABORT_FINALIZE(iRet);
	}

finalize_it:
CODE_STD_FINALIZERnewInpInst
	cnfparamvalsDestruct(pvals, &inppblk);
ENDnewInpInst

BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
ENDbeginCnfLoad

BEGINendCnfLoad
CODESTARTendCnfLoad
ENDendCnfLoad

BEGINcheckCnf
instanceConf_t *inst;
CODESTARTcheckCnf
	for(inst = confRoot ; inst != NULL ; inst = inst->next) {
		std_checkRuleset(pModConf , inst);
	}
ENDcheckCnf

BEGINactivateCnf
CODESTARTactivateCnf
	runModConf = pModConf;
ENDactivateCnf

BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf

BEGINrunInput
CODESTARTrunInput
	do_polling();
	DBGPRINTF("terminating upon request of rsyslog core\n");
ENDrunInput

BEGINwillRun
CODESTARTwillRun
	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.Construct(&pInputName));
	CHKiRet(prop.SetString(pInputName, UCHAR_CONSTANT("imtuxedoulog"), sizeof("imtuxedoulog") - 1));
	CHKiRet(prop.ConstructFinalize(pInputName));
finalize_it:
ENDwillRun

BEGINafterRun
CODESTARTafterRun
	while(confRoot != NULL) {
		instanceConf_t *inst = confRoot;
		confRoot = confRoot->next;
		lstnDel(inst);
	}

	if(pInputName != NULL)
		prop.Destruct(&pInputName);
ENDafterRun

BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature

BEGINmodExit
CODESTARTmodExit
	/* release objects we used */
	objRelease(strm, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
	objRelease(ruleset, CORE_COMPONENT);
ENDmodExit

BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
CODEqueryEtryPt_STD_CONF2_IMOD_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt

static inline void
std_checkRuleset_genErrMsg(__attribute__((unused)) modConfData_t *modConf, instanceConf_t *inst)
{
	LogError(0, NO_ERRCODE, "imtuxedoulog: ruleset '%s' for ULOG base %s not found - "
			"using default ruleset instead", inst->pszBindRuleset,
			inst->pszUlogBaseName);
}

BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(strm, CORE_COMPONENT));
	CHKiRet(objUse(ruleset, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));
ENDmodInit
