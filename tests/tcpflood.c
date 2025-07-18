/* Opens a large number of tcp connections and sends
 * messages over them. This is used for stress-testing.
 *
 * NOTE: the following part is actually the SPEC (or call it man page).
 * It's not random comments. So if the code behavior does not match what
 * is written here, it should be considered a bug.
 *
 * Params
 * -h	hostname to use inside message
 * -t	target address (default 127.0.0.1)
 * -p	target port(s) (default 13514), multiple via port1:port2:port3...
 * -n	number of target ports (all target ports must be given in -p!)
 *      Note -c must also be set to at LEAST the number of -n!
 * -c	number of connections (default 1), use negative number
 *      to set a "soft limit": if tcpflood cannot open the
 *      requested number of connections, gracefully degrade to
 *      whatever number could be opened. This is useful in environments
 *      where system config constraints cannot be overriden (e.g.
 *      vservers, non-admin users, ...)
 * -m	number of messages to send (connection is random)
 * -i	initial message number (optional)
 * -P	PRI to be used for generated messages (default is 167).
 *      Specify the plain number without leading zeros
 * -d   amount of extra data to add to message. If present, the
 *      number itself will be added as third field, and the data
 *      bytes as forth. Add -r to randomize the amount of extra
 *      data included in the range 1..(value of -d).
 * -r	randomize amount of extra data added (-d must be > 0)
 * -s	(silent) do not show progress indicator (never done on non-tty)
 * -f	support for testing dynafiles. If given, include a dynafile ID
 *      in the range 0..(f-1) as the SECOND field, shifting all field values
 *      one field to the right. Zero (default) disables this functionality.
 * -M   the message to be sent. Disables all message format options, as
 *      only that exact same message is sent.
 * -I   read specified input file, do NOT generate own test data. The test
 *      completes when eof is reached.
 * -B   The specified file (-I) is binary. No data processing is done by
 *      tcpflood. If multiple connections are specified, data is read in
 *      chunks and spread across the connections without taking any record
 *      delimiters into account.
 * -C	when input from a file is read, this file is transmitted -C times
 *      (C like cycle, running out of meaningful option switches ;))
 * -D	randomly drop and re-establish connections. Useful for stress-testing
 *      the TCP receiver.
 * -F	USASCII value for frame delimiter (in octet-stuffing mode), default LF
 * -R	number of times the test shall be run (very useful for gathering performance
 *      data and other repetitive things). Default: 1
 * -S   number of seconds to sleep between different runs (-R) Default: 30
 * -X   generate sTats data records. Default: off
 * -e   encode output in CSV (not yet everywhere supported)
 *      for performance data:
 *      each inidividual line has the runtime of one test
 *      the last line has 0 in field 1, followed by numberRuns,TotalRuntime,
 *      Average,min,max
 * -T   transport to use. Currently supported: "udp", "tcp" (default), "tls" (tcp+tls), relp-plain, relp-tls
 *      Note: UDP supports a single target port, only
 * -u	Set RELP TLS Library to gnutls or openssl
 * -W	wait time between sending batches of messages, in microseconds (Default: 0)
 * -b   number of messages within a batch (default: 100,000,000 millions)
 * -Y	use multiple threads, one per connection (which means 1 if one only connection
 *  	is configured!)
 * -y   use RFC5424 style test message
 * -x	CA Cert File for verification (TLS Mode / OpenSSL only)
 * -z	private key file for TLS mode
 * -Z	cert (public key) file for TLS mode
 * -a	Authentication Mode for relp-tls
 * -A	do NOT abort if an error occured during sending messages
 * -E	Permitted Peer for relp-tls
 * -L	loglevel to use for GnuTLS troubleshooting (0-off to 10-all, 0 default)
 * -j	format message in json, parameter is JSON cookie
 * -o   number of threads to use for connection establishment (default: 25)
 * -O	Use octet-count framing
 * -v   verbose output, possibly useful for troubleshooting. Most importantly,
 *      this gives insight into librelp actions (if relp is selected as protocol).
 * -k	Custom Configuration string passwed through the TLS library.
 *	Currently only OpenSSL is supported, possible configuration commands and values can be found here:
 *	https://www.openssl.org/docs/man1.0.2/man3/SSL_CONF_cmd.html
 *	Sample: -k"Protocol=ALL,-SSLv2,-SSLv3,-TLSv1,-TLSv1.1"
 *	Works for LIBRELP now as well!
 *
 * Part of the testbench for rsyslog.
 *
 * Copyright 2009-2025 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <pthread.h>
#ifdef ENABLE_RELP
#include <librelp.h>
#endif
#include <sys/resource.h>
#include <sys/time.h>
#include <errno.h>
#ifdef ENABLE_GNUTLS
#	include <gnutls/gnutls.h>
#	if GNUTLS_VERSION_NUMBER <= 0x020b00
#		include <gcrypt.h>
	GCRY_THREAD_OPTION_PTHREAD_IMPL;
#	endif
#endif
#ifdef ENABLE_OPENSSL
	#include <openssl/ssl.h>
	#include <openssl/x509v3.h>
	#include <openssl/err.h>
#	ifndef OPENSSL_NO_ENGINE
#		include <openssl/engine.h>
#	endif

	/* OpenSSL API differences */
	#if OPENSSL_VERSION_NUMBER >= 0x10100000L
		#define RSYSLOG_X509_NAME_oneline(X509CERT) X509_get_subject_name(X509CERT)
		#define RSYSLOG_BIO_method_name(SSLBIO) BIO_method_name(SSLBIO)
		#define RSYSLOG_BIO_number_read(SSLBIO) BIO_number_read(SSLBIO)
		#define RSYSLOG_BIO_number_written(SSLBIO) BIO_number_written(SSLBIO)
	#else
		#define RSYSLOG_X509_NAME_oneline(X509CERT) (X509CERT != NULL ? X509CERT->cert_info->subject : NULL)
		#define RSYSLOG_BIO_method_name(SSLBIO) SSLBIO->method->name
		#define RSYSLOG_BIO_number_read(SSLBIO) SSLBIO->num
		#define RSYSLOG_BIO_number_written(SSLBIO) SSLBIO->num
	#endif

#endif

char *test_rs_strerror_r(int errnum, char *buf, size_t buflen) {
#ifndef HAVE_STRERROR_R
	char *pszErr;
	pszErr = strerror(errnum);
	snprintf(buf, buflen, "%s", pszErr);
#else
#	ifdef STRERROR_R_CHAR_P
	char *p = strerror_r(errnum, buf, buflen);
	if (p != buf) {
		strncpy(buf, p, buflen);
		buf[buflen - 1] = '\0';
	}
#	else
	strerror_r(errnum, buf, buflen);
#	endif
#endif /* #ifdef __hpux */
	return buf;
}

#define INVALID_SOCKET -1
/* Name of input file, must match $IncludeConfig in test suite .conf files */
#define NETTEST_INPUT_CONF_FILE "nettest.input.conf"
/* name of input file, must match $IncludeConfig in .conf files */

#define MAX_EXTRADATA_LEN 512*1024
#define MAX_SENDBUF 2 * MAX_EXTRADATA_LEN
#define MAX_RCVBUF 16 * 1024 + 1/* TLS RFC 8449: max size of buffer for message reception */

static int nThreadsConnOpen = 25;	/* Number for threads for openeing the connections */
static char *targetIP = "127.0.0.1";
static char *msgPRI = "167";
static int targetPort[5] = {13514};
static int numTargetPorts = 1;
static int verbose = 0;
static int dynFileIDs = 0;
static int extraDataLen = 0; /* amount of extra data to add to message */
static int useRFC5424Format = 0; /* should the test message be in RFC5424 format? */
static int bRandomizeExtraData = 0; /* randomize amount of extra data added */
static int numMsgsToSend = 1; /* number of messages to send */
static int numConnections = 1; /* number of connections to create */
static int softLimitConnections  = 0; /* soft connection limit, see -c option description */
static int *sockArray;  /* array of sockets to use */
#ifdef ENABLE_RELP
static relpClt_t **relpCltArray;  /* array of sockets to use */
#endif
static int msgNum = 0;	/* initial message number to start with */
static int bShowProgress = 1; /* show progress messages */
static int bSilent = 0; /* completely silent operation */
static int bRandConnDrop = 0; /* randomly drop connections? */
static double dbRandConnDrop = 0.95; /* random drop probability */
static char *MsgToSend = NULL; /* if non-null, this is the actual message to send */
static char *hostname = "172.20.245.8"; /* this is the "tratditional" default, as bad is it is... */
static int bBinaryFile = 0;	/* is -I file binary */
static char *dataFile = NULL;	/* name of data file, if NULL, generate own data */
static int numFileIterations = 1;/* how often is file data to be sent? */
static char frameDelim = '\n';	/* default frame delimiter */
FILE *dataFP = NULL;		/* file pointer for data file, if used */
static long nConnDrops = 0;	/* counter: number of time connection was dropped (-D option) */
static int numRuns = 1;		/* number of times the test shall be run */
static int sleepBetweenRuns = 30; /* number of seconds to sleep between runs */
static int bStatsRecords = 0;	/* generate stats records */
static int bCSVoutput = 0;	/* generate output in CSV (where applicable) */
static long long batchsize = 100000000ll;
static int waittime = 0;
static int runMultithreaded = 0; /* run tests in multithreaded mode */
static int numThrds = 1;	/* number of threads to use */
static int abortOnSendFail = 1;	/* abort run if sending fails? */
static char *tlsCAFile = NULL;
static char *tlsCertFile = NULL;
static char *tlsKeyFile = NULL;
static char *relpAuthMode = NULL;
static char *relpPermittedPeer = NULL;
#if defined(HAVE_RELPENGINESETTLSLIBBYNAME)
static char *relpTlsLib = NULL;
#endif

static int tlsLogLevel = 0;
static char *jsonCookie = NULL; /* if non-NULL, use JSON format with this cookie */
static int octateCountFramed = 0;
static char *customConfig = NULL; /* Stores a string with custom configuration passed through the TLS driver */

#ifdef ENABLE_GNUTLS
static gnutls_session_t *sessArray;	/* array of TLS sessions to use */
static gnutls_certificate_credentials_t tlscred;
#endif

#ifdef ENABLE_OPENSSL
/* Main OpenSSL CTX pointer */
static SSL_CTX *ctx;
static SSL **sslArray;
static struct sockaddr_in dtls_client_addr;	/* socket address sender for receiving DTLS data */
static int udpsockin;				/* socket for receiving messages in DTLS mode */
#endif

/* variables for managing multi-threaded operations */
int runningThreads;		/* number of threads currently running */
int doRun;			/* shall sender thread begin to run? */
pthread_mutex_t thrdMgmt;	/* mutex for controling startup/shutdown */
pthread_cond_t condStarted;
pthread_cond_t condDoRun;

/* the following struct provides information for a generator instance (thread) */
struct instdata {
	/* lower and upper bounds for the thread in question */
	unsigned long long lower;
	unsigned long long numMsgs; /* number of messages to send */
	unsigned long long numSent; /* number of messages already sent */
	unsigned idx;	/**< index of fd to be used for sending */
	pthread_t thread; /**< thread processing this instance */
} *instarray = NULL;

/* the following structure is used to gather performance data */
struct runstats {
	unsigned long long totalRuntime;
	unsigned long minRuntime;
	unsigned long maxRuntime;
	int numRuns;
};

static int udpsockout;			/* socket for sending in UDP mode */
static struct sockaddr_in udpRcvr;	/* remote receiver in UDP mode */

static enum { TP_UDP, TP_TCP, TP_TLS, TP_RELP_PLAIN, TP_RELP_TLS, TP_DTLS } transport = TP_TCP;

/* forward definitions */
static void initTLSSess(int);
static int sendTLS(int i, char *buf, size_t lenBuf);
static void closeTLSSess(int __attribute__((unused)) i);

static void initDTLSSess(void);
static int sendDTLS(char *buf, size_t lenBuf);
static void closeDTLSSess(void);

#ifdef ENABLE_RELP
/* RELP subsystem */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
static void relp_dbgprintf(char __attribute__((unused)) *fmt, ...) {
	printf(fmt);
}
#pragma GCC diagnostic pop

static relpEngine_t *pRelpEngine;
#define CHKRELP(f) if(f != RELP_RET_OK) { fprintf(stderr, "%s\n", #f); exit(1); }

static void
onErr(void *pUsr, char *objinfo, char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	fprintf(stderr, "tcpflood: onErr '%s'\n", errmesg);
}

static void
onGenericErr(char *objinfo, char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	fprintf(stderr, "tcpflood: onGenericErr '%s'\n", errmesg);
}

static void
onAuthErr(void *pUsr, char *authinfo, char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	fprintf(stderr, "tcpflood: onAuthErr '%s' peer '%s'\n", errmesg, authinfo);
}

static void
initRELP_PLAIN(void)
{
	CHKRELP(relpEngineConstruct(&pRelpEngine));
	CHKRELP(relpEngineSetDbgprint(pRelpEngine,
		verbose ? relp_dbgprintf : NULL));
	CHKRELP(relpEngineSetEnableCmd(pRelpEngine, (unsigned char*)"syslog",
		eRelpCmdState_Required));
	/* Error output support */
	CHKRELP(relpEngineSetOnErr(pRelpEngine, onErr));
	CHKRELP(relpEngineSetOnGenericErr(pRelpEngine, onGenericErr));
	CHKRELP(relpEngineSetOnAuthErr(pRelpEngine, onAuthErr));

}
#endif /* #ifdef ENABLE_RELP */

/* prepare send subsystem for UDP send */
static int
setupUDP(void)
{
	if((udpsockout = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		return 1;

	memset((char *) &udpRcvr, 0, sizeof(udpRcvr));
	udpRcvr.sin_family = AF_INET;
	udpRcvr.sin_port = htons(targetPort[0]);
	if(inet_aton(targetIP, &udpRcvr.sin_addr)==0) {
		fprintf(stderr, "inet_aton() failed\n");
		return(1);
	}

	return 0;
}

#if defined(ENABLE_OPENSSL)
static int
setupDTLS(void)
{
	// Setup receiving Socket for DTLS
	if((udpsockin = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return 1;

	memset(&dtls_client_addr, 0, sizeof(dtls_client_addr));
	dtls_client_addr.sin_family = AF_INET;
	dtls_client_addr.sin_port = htons(0);
	dtls_client_addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(udpsockin, (struct sockaddr*)&dtls_client_addr, sizeof(dtls_client_addr)) < 0) {
		perror("bind()");
		fprintf(stderr, "Unable to bind DTLS CLient socket\n");
		return(1);
	}

	memset((char *) &udpRcvr, 0, sizeof(udpRcvr));
	udpRcvr.sin_family = AF_INET;
	udpRcvr.sin_port = htons(targetPort[0]);
	if(inet_aton(targetIP, &udpRcvr.sin_addr)==0) {
		fprintf(stderr, "inet_aton() failed\n");
		return(1);
	}

	// Init Socket Connection (Which technically does not connect but prepares socket for DTLS)
	printf("[DEBUG] Init Session to %s:%d ...\n", targetIP, targetPort[0]);
	udpsockout = socket(AF_INET, SOCK_DGRAM, 0);
	// Connect the UDP socket to the server's address
	if (connect(udpsockout, (const struct sockaddr *) &udpRcvr, sizeof(udpRcvr)) < 0) {
		perror("connect()");
		fprintf(stderr, "connect to %s:%d failed\n", targetIP, targetPort[0]);
		return(1);
	}
	sockArray[0] = -1;

	return 0;
}
#endif

/* open a single tcp connection
 */
int openConn(const int connIdx)
{
	int sock;
	struct sockaddr_in addr;
	int port;
	int retries = 0;
	int rnd;

	/* randomize port if required */
	if(numTargetPorts > 1) {
		rnd = rand(); /* easier if we need value for debug messages ;) */
		port = targetPort[(rnd % numTargetPorts)];
	} else {
		port = targetPort[0];
	}
	if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS) {
		#ifdef ENABLE_RELP
		relpRetVal relp_r;
		relpClt_t *relpClt;
		char relpPort[16];
		snprintf(relpPort, sizeof(relpPort), "%d", port);
		CHKRELP(relpEngineCltConstruct(pRelpEngine, &relpClt));
		if(transport == TP_RELP_TLS) {
			#if defined(HAVE_RELPENGINESETTLSLIBBYNAME)
			if(relpTlsLib != NULL && relpEngineSetTLSLibByName(pRelpEngine, relpTlsLib) != RELP_RET_OK) {
				fprintf(stderr, "relpTlsLib not accepted by librelp, using default\n");
			}
			#endif
			if(relpCltEnableTLS(relpClt) != RELP_RET_OK) {
				fprintf(stderr, "error while enabling TLS for relp\n");
				exit(1);
			}
			if(relpAuthMode != NULL && relpCltSetAuthMode(relpClt, relpAuthMode) != RELP_RET_OK) {
				fprintf(stderr, "could not set Relp Authentication mode: %s\n", relpAuthMode);
				exit(1);
			}
			if(tlsCAFile != NULL && relpCltSetCACert(relpClt, tlsCAFile) != RELP_RET_OK) {
				fprintf(stderr, "could not set CA File: %s\n", tlsCAFile);
				exit(1);
			}
			if(tlsCertFile != NULL && relpCltSetOwnCert(relpClt, tlsCertFile) != RELP_RET_OK) {
				fprintf(stderr, "could not set Cert File: %s\n", tlsCertFile);
				exit(1);
			}
			if(tlsKeyFile != NULL && relpCltSetPrivKey(relpClt, tlsKeyFile) != RELP_RET_OK) {
				fprintf(stderr, "could not set Key File: %s\n", tlsKeyFile);
				exit(1);
			}
			if(relpPermittedPeer != NULL && relpCltAddPermittedPeer(relpClt, relpPermittedPeer)
					!= RELP_RET_OK) {
				fprintf(stderr, "could not set Permitted Peer: %s\n", relpPermittedPeer);
				exit(1);
			}
#if defined(HAVE_RELPENGINESETTLSCFGCMD)
			/* Check for Custom Config string */
			if(customConfig != NULL && relpCltSetTlsConfigCmd(relpClt, customConfig)
					!= RELP_RET_OK) {
				fprintf(stderr, "could not set custom tls command: %s\n", customConfig);
				exit(1);
			}
#endif
		}
		relpCltArray[connIdx] = relpClt;
		relp_r = relpCltConnect(relpCltArray[connIdx], 2,
			(unsigned char*)relpPort, (unsigned char*)targetIP);
		if(relp_r != RELP_RET_OK) {
			fprintf(stderr, "relp connect failed with return %d\n", relp_r);
			return(1);
		}
		sockArray[connIdx] = 1; /* mimic "all ok" state TODO: this looks invalid! */
		#endif
	} else { /* TCP, with or without TLS */
		if((sock=socket(AF_INET, SOCK_STREAM, 0))==-1) {
			return(1);
		}
		memset((char *) &addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if(inet_aton(targetIP, &addr.sin_addr)==0) {
			fprintf(stderr, "inet_aton() failed\n");
			return(1);
		}
		while(1) { /* loop broken inside */
			if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
				break;
			} else {
				fprintf(stderr, "warning: connect failed, retrying... %s\n", strerror(errno));
				if(retries++ == 50) {
					perror("connect()");
					fprintf(stderr, "connect(%d) failed\n", port);
					return(1);
				} else {
					usleep(100000); /* ms = 1000 us! */
				}
			}
		}

		sockArray[connIdx] = sock;

		if(transport == TP_TLS) {
			initTLSSess(connIdx);
		}
	}
	return 0;
}

static int progressCounter = 0;
static pthread_mutex_t counterLock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
	int startIdx;
	int endIdx;
} ThreadArgsOpen;

/* Thread function to handle a range of connections */
void *connectionWorker(void *arg)
{
	ThreadArgsOpen *args = (ThreadArgsOpen *)arg;
	int i;
	static int reportedConnOpenErr = 0; // TODO: mutex!

	for(i = args->startIdx; i <= args->endIdx; i++) {
		if(openConn(i) != 0) {
			if(reportedConnOpenErr++ == 0) {
				fprintf(stderr, "Error opening connection %d; %s\n", i, strerror(errno));
			}

			pthread_exit(NULL);
		}

		/* Update progress counter */
		pthread_mutex_lock(&counterLock);
		const int ctr_fixed = progressCounter++;
		pthread_mutex_unlock(&counterLock);
		if (bShowProgress && i % 10 == 0) {
			printf("\r%5.5d", ctr_fixed);
		}
	}

	pthread_exit(NULL);
}


/* open all requested tcp connections
 * this includes allocating the connection array
 */
int openConnections(void)
{
	int i;
	char msgBuf[128];
	size_t lenMsg;
	pthread_t threads[nThreadsConnOpen];
	ThreadArgsOpen threadArgs[nThreadsConnOpen];
	int threadCount = nThreadsConnOpen;

	if(transport == TP_UDP)
		return setupUDP();

#	if defined(ENABLE_OPENSSL)
	sslArray = calloc(numConnections, sizeof(SSL *));
#	elif defined(ENABLE_GNUTLS)
	sessArray = calloc(numConnections, sizeof(gnutls_session_t));
#	endif
	sockArray = calloc(numConnections, sizeof(int));

#	if defined(ENABLE_OPENSSL)
	// Use setupDTLS on DTLS
	if(transport == TP_DTLS)
		return setupDTLS();
#	endif

	#ifdef ENABLE_RELP
	if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS)
		relpCltArray = calloc(numConnections, sizeof(relpClt_t*));
	#endif

	// Adjust the number of threads if there are fewer connections
	if (numConnections < threadCount) {
		threadCount = numConnections;
	}

	if(bShowProgress)
		if(write(1, "      open connections", sizeof("      open connections")-1)){}

	// Divide connections among threads
	int connectionsPerThread = numConnections / threadCount;
	int remainder = numConnections % threadCount;
	int startIdx = 0;

	for (i = 0; i < threadCount; i++) {
		int endIdx = startIdx + connectionsPerThread - 1;
		endIdx += remainder;
		remainder = 0;

		threadArgs[i].startIdx = startIdx;
		threadArgs[i].endIdx = endIdx;

		if (pthread_create(&threads[i], NULL, connectionWorker, &threadArgs[i]) != 0) {
			fprintf(stderr, "Error creating thread: %s\n", strerror(errno));
			return 1;
		}

		startIdx = endIdx + 1;
	}

	/* Wait for all connection open threads to finish */
	for (i = 0; i < threadCount; i++) {
		pthread_join(threads[i], NULL);
	}

	return 0;
}


/* we also close all connections because otherwise we may get very bad
 * timing for the syslogd - it may not be able to process all incoming
 * messages fast enough if we immediately shut down.
 * TODO: it may be an interesting excercise to handle that situation
 * at the syslogd level, too
 * rgerhards, 2009-04-14
 */
void closeConnections(void)
{
	int i;
	size_t lenMsg;
	struct linger ling;
	char msgBuf[128];

	if(transport == TP_UDP) {
		return;
	}
#	if defined(ENABLE_OPENSSL)
	else if(transport == TP_DTLS) {
		closeDTLSSess();
		return;
	}
#	endif

	if(bShowProgress)
		if(write(1, "      close connections", sizeof("      close connections")-1)){}
	for(i = 0 ; i < numConnections ; ++i) {
		if(i % 10 == 0 && bShowProgress) {
			lenMsg = sprintf(msgBuf, "\r%5.5d", i);
			if(write(1, msgBuf, lenMsg)){}
		}
		if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS) {
			#ifdef ENABLE_RELP
			relpRetVal relpr;
			if(sockArray[i] != -1) {
				relpr = relpEngineCltDestruct(pRelpEngine, relpCltArray+i);
				if(relpr != RELP_RET_OK) {
					fprintf(stderr, "relp error %d on close\n", relpr);
				}
				sockArray[i] = -1;
			}
			#endif
		} else { /* TCP and TLS modes */
			if(sockArray[i] != -1) {
				/* we try to not overrun the receiver by trying to flush buffers
				 * *during* close(). -- rgerhards, 2010-08-10
				 */
				ling.l_onoff = 1;
				ling.l_linger = 1;
				setsockopt(sockArray[i], SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
				if(transport == TP_TLS) {
					closeTLSSess(i);
				}
				close(sockArray[i]);
			}
		}
	}
	if(bShowProgress) {
		lenMsg = sprintf(msgBuf, "\r%5.5d close connections\n", i);
		if(write(1, msgBuf, lenMsg)){}
	}

}


/* generate the message to be sent according to program command line parameters.
 * this has been moved to its own function as we now have various different ways
 * of constructing test messages. -- rgerhards, 2010-03-31
 */
static void
genMsg(char *buf, size_t maxBuf, size_t *pLenBuf, struct instdata *inst)
{
	int edLen; /* actual extra data length to use */
	char extraData[MAX_EXTRADATA_LEN + 1];
	char dynFileIDBuf[128] = "";
	int done;
	char payloadLen[32];
	int payloadStringLen;

	if(dataFP != NULL) {
		/* get message from file */
		do {
			done = 1;
			*pLenBuf = fread(buf, 1, MAX_EXTRADATA_LEN + 1024, dataFP);
			if(*pLenBuf == 0) {
				if(--numFileIterations > 0)  {
					rewind(dataFP);
					done = 0; /* need new iteration */
				} else {
					*pLenBuf = 0;
					goto finalize_it;
				}
			}
		} while(!done); /* Attention: do..while()! */
	} else if(jsonCookie != NULL) {
		if(useRFC5424Format) {
			*pLenBuf = snprintf(buf, maxBuf, "<%s>1 2003-03-01T01:00:00.000Z mymachine.example.com "
						"tcpflood - tag [tcpflood@32473 MSGNUM"
						"=\"%8.8d\"] %s{\"msgnum\":%d}%c", msgPRI, msgNum,
						jsonCookie, msgNum, frameDelim);
		} else {
			*pLenBuf = snprintf(buf, maxBuf, "<%s>Mar  1 01:00:00 %s tag %s{\"msgnum\":%d}%c",
					       msgPRI, hostname, jsonCookie, msgNum, frameDelim);
		}
	} else if(MsgToSend == NULL) {
		if(dynFileIDs > 0) {
			snprintf(dynFileIDBuf, sizeof(dynFileIDBuf), "%d:", rand() % dynFileIDs);
		}
		if(extraDataLen == 0) {
			if(useRFC5424Format) {
				*pLenBuf = snprintf(buf, maxBuf, "<%s>1 2003-03-01T01:00:00.000Z "
						"mymachine.example.com tcpflood - tag [tcpflood@32473 "
						"MSGNUM=\"%8.8d\"] msgnum:%s%8.8d:%c",
						msgPRI, msgNum, dynFileIDBuf, msgNum, frameDelim);
			} else {
				*pLenBuf = snprintf(buf, maxBuf, "<%s>Mar  1 01:00:00 %s tag "
						"msgnum:%s%8.8d:%c", msgPRI, hostname, dynFileIDBuf,
						msgNum, frameDelim);
			}
		} else {
			if(bRandomizeExtraData)
				edLen = ((unsigned long) rand() + extraDataLen) % extraDataLen + 1;
			else
				edLen = extraDataLen;
			memset(extraData, 'X', edLen);
			extraData[edLen] = '\0';
			if(useRFC5424Format) {
				*pLenBuf = snprintf(buf, maxBuf, "<%s>1 2003-03-01T01:00:00.000Z "
						"mymachine.example.com tcpflood - tag [tcpflood@32473 "
						"MSGNUM=\"%8.8d\"] msgnum:%s%8.8d:%c",
						msgPRI, msgNum, dynFileIDBuf, msgNum, frameDelim);
			} else {
				*pLenBuf = snprintf(buf, maxBuf, "<%s>Mar  1 01:00:00 %s tag msgnum"
						":%s%8.8d:%d:%s%c", msgPRI, hostname, dynFileIDBuf, msgNum, edLen,
						extraData, frameDelim);
			}
		}
	} else {
		/* use fixed message format from command line */
		*pLenBuf = snprintf(buf, maxBuf, "%s%c", MsgToSend, frameDelim);
	}
	if (octateCountFramed == 1) {
		snprintf(payloadLen, sizeof(payloadLen), "%zd ", *pLenBuf);
		payloadStringLen = strlen(payloadLen);
		memmove(buf + payloadStringLen, buf, *pLenBuf);
		memcpy(buf, payloadLen, payloadStringLen);
		*pLenBuf += payloadStringLen;
	}
	++inst->numSent;

finalize_it: /*EMPTY to keep the compiler happy */;
}


static int
sendPlainTCP(const int socknum, const char *const buf, const size_t lenBuf, int *const ret_errno)
{
	size_t lenSent;
	int r;

	lenSent = 0;
	while(lenSent != lenBuf) {
		r = send(sockArray[socknum], buf + lenSent, lenBuf - lenSent, 0);
		if(r > 0) {
			lenSent += r;
		} else {
			*ret_errno = errno;
			goto finalize_it;
		}
	}

finalize_it:
	return lenSent;
}


/* send messages to the tcp connections we keep open. We use
 * a very basic format that helps identify the message
 * (via msgnum:<number>: e.g. msgnum:00000001:). This format is suitable
 * for extracton to field-based properties.
 * The first numConnection messages are sent sequentially, as are the
 * last. All messages in between are sent over random connections.
 * Note that message numbers start at 0.
 */
int sendMessages(struct instdata *inst)
{
	unsigned i = 0;
	int socknum;
	size_t lenBuf;
	size_t lenSend = 0;
	char *statusText = "";
	char buf[MAX_EXTRADATA_LEN + 1024];
	char sendBuf[MAX_SENDBUF];
	int offsSendBuf = 0;
	char errStr[1024];
	int error_number = 0;
	unsigned show_progress_interval = 100;

	if(!bSilent) {
		if(dataFile == NULL) {
			printf("Sending %llu messages.\n", inst->numMsgs);
			statusText = "messages";
			if ((inst->numMsgs / 100) > show_progress_interval) {
				show_progress_interval = inst->numMsgs / 100;
			}
		} else {
			printf("Sending file '%s' %d times.\n", dataFile,
			       numFileIterations);
			statusText = "kb";
		}
	}
	if(bShowProgress)
		printf("\r%8.8d %s sent", 0, statusText);
	while(i < inst->numMsgs) {
		if(runMultithreaded) {
			socknum = inst->idx;
		} else {
			if((int) i < numConnections)
				socknum = i;
			else if(i >= inst->numMsgs - numConnections) {
				socknum = i - (inst->numMsgs - numConnections);
			} else {
				int rnd = rand();
				socknum = rnd % numConnections;
			}
		}
		genMsg(buf, sizeof(buf), &lenBuf, inst); /* generate the message to send according to params */
		if(lenBuf == 0)
			break;	/* terminate when no message could be generated */
		if(transport == TP_TCP) {
			if(sockArray[socknum] == -1) {
				/* connection was dropped, need to re-establish */
				if(openConn(socknum) != 0) {
					printf("error in trying to re-open connection %d\n", socknum);
					exit(1);
				}
			}
			lenSend = sendPlainTCP(socknum, buf, lenBuf, &error_number);
		} else if(transport == TP_UDP) {
			lenSend = sendto(udpsockout, buf, lenBuf, 0, &udpRcvr, sizeof(udpRcvr));
			error_number = errno;
		} else if(transport == TP_TLS) {
			if(sockArray[socknum] == -1) {
				/* connection was dropped, need to re-establish */
				if(openConn(socknum) != 0) {
					printf("error in trying to re-open connection %d\n", socknum);
					exit(1);
				}
			}
			if(offsSendBuf + lenBuf < MAX_SENDBUF) {
				memcpy(sendBuf+offsSendBuf, buf, lenBuf);
				offsSendBuf += lenBuf;
				lenSend = lenBuf; /* simulate "good" call */
			} else {
				lenSend = sendTLS(socknum, sendBuf, offsSendBuf);
				lenSend = (lenSend == offsSendBuf) ? lenBuf : -1;
				memcpy(sendBuf, buf, lenBuf);
				offsSendBuf = lenBuf;
			}
#	if defined(ENABLE_OPENSSL)
		} else if(transport == TP_DTLS) {
			if(sockArray[0] == -1) {
				// Init DTLS Session (Bind local listener)
				initDTLSSess();
			}
			lenSend = sendDTLS(buf, lenBuf);
#	endif
		} else if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS) {
			#ifdef ENABLE_RELP
			relpRetVal relp_ret;
			if(sockArray[socknum] == -1) {
				/* connection was dropped, need to re-establish */
				if(openConn(socknum) != 0) {
					printf("error in trying to re-open connection %d\n", socknum);
					exit(1);
				}
			}
			relp_ret = relpCltSendSyslog(relpCltArray[socknum],
					(unsigned char*)buf, lenBuf);
			if (relp_ret == RELP_RET_OK) {
				lenSend = lenBuf; /* mimic ok */
			} else {
				lenSend = 0; /* mimic fail */
				printf("\nrelpCltSendSyslog() failed with relp error code %d\n",
					   relp_ret);
			}
			#endif
		}
		if(lenSend != lenBuf) {
			printf("\r%5.5u\n", i);
			fflush(stdout);
			test_rs_strerror_r(error_number, errStr, sizeof(errStr));
			if(lenSend == 0) {
				printf("tcpflood: socket %d, index %u, msgNum %lld CLOSED REMOTELY (%s)\n",
					sockArray[socknum], i, inst->numSent, errStr);
			} else {
				printf("tcpflood: send() failed \"%s\" at socket %d, index %u, "
						"msgNum %lld, lenSend %zd, lenBuf %zd\n",
						errStr, sockArray[socknum], i, inst->numSent, lenSend,
						lenBuf);
				}
			fflush(stderr);

			if(abortOnSendFail) {
				printf("tcpflood terminates due to send failure\n");
				return(1);
			}
		}
		if(i % show_progress_interval == 0) {
			if(bShowProgress)
				printf("\r%8.8u", i);
		}
		if(!runMultithreaded && bRandConnDrop) {
			/* if we need to randomly drop connections, see if we
			 * are a victim
			 */
			if(rand() > (int) (RAND_MAX * dbRandConnDrop)) {
				if(transport == TP_TLS && offsSendBuf != 0) {
					/* send remaining buffer */
					lenSend = sendTLS(socknum, sendBuf, offsSendBuf);
					if(lenSend != offsSendBuf) {
						fprintf(stderr, "tcpflood: error in send function for conn %d "
							"causes potential data loss lenSend %zd, "
							"offsSendBuf %d\n",
							sockArray[socknum], lenSend, offsSendBuf);
					}
					offsSendBuf = 0;
				}
				++nConnDrops;
				close(sockArray[socknum]);
				sockArray[socknum] = -1;
			}
		}
		if(inst->numSent % batchsize == 0) {
			usleep(waittime);
		}
		++msgNum;
		++i;
	}
	if(transport == TP_TLS && offsSendBuf != 0) {
		/* send remaining buffer */
		lenSend = sendTLS(socknum, sendBuf, offsSendBuf);
	}
	if(!bSilent)
		printf("\r%8.8u %s sent\n", i, statusText);

	return 0;
}


/* this is the thread that starts a generator
 */
static void *
thrdStarter(void *arg)
{
	struct instdata *inst = (struct instdata*) arg;
	pthread_mutex_lock(&thrdMgmt);
	runningThreads++;
	pthread_cond_signal(&condStarted);
	while(doRun == 0) {
		pthread_cond_wait(&condDoRun, &thrdMgmt);
	}
	pthread_mutex_unlock(&thrdMgmt);
	if(sendMessages(inst) != 0) {
		printf("error sending messages\n");
	}
	return NULL;
}


/* This function initializes the actual traffic generators. The function sets up all required
 * parameter blocks and starts threads. It returns when all threads are ready to run
 * and the main task must just enable them.
 */
static void
prepareGenerators()
{
	int i;
	long long msgsThrd;
	long long starting = 0;
	pthread_attr_t thrd_attr;

	if(runMultithreaded) {
		bSilent = 1;
		numThrds = numConnections;
	} else {
		numThrds = 1;
	}

	pthread_attr_init(&thrd_attr);
	pthread_attr_setstacksize(&thrd_attr, 4096*1024);

	runningThreads = 0;
	doRun = 0;
	pthread_mutex_init(&thrdMgmt, NULL);
	pthread_cond_init(&condStarted, NULL);
	pthread_cond_init(&condDoRun, NULL);

	if(instarray != NULL) {
		free(instarray);
	}
	instarray = calloc(numThrds, sizeof(struct instdata));
	msgsThrd = numMsgsToSend / numThrds;

	for(i = 0 ; i < numThrds ; ++i)  {
		instarray[i].lower = starting;
		instarray[i].numMsgs = msgsThrd;
		instarray[i].numSent = 0;
		instarray[i].idx = i;
		pthread_create(&(instarray[i].thread), &thrd_attr, thrdStarter, instarray + i);
		/*printf("started thread %x\n", (unsigned) instarray[i].thread);*/
		starting += msgsThrd;
	}
}

/* Let all generators run. Threads must have been started. Here we wait until
 * all threads are initialized and then broadcast that they can begin to run.
 */
static void
runGenerators()
{
	pthread_mutex_lock(&thrdMgmt);
	while(runningThreads != numThrds){
		pthread_cond_wait(&condStarted, &thrdMgmt);
	}
	doRun = 1;
	pthread_cond_broadcast(&condDoRun);
	pthread_mutex_unlock(&thrdMgmt);
}


/* Wait for all traffic generators to stop.
 */
static void
waitGenerators()
{
	int i;
	for(i = 0 ; i < numThrds ; ++i)  {
		pthread_join(instarray[i].thread, NULL);
		/*printf("thread %x stopped\n", (unsigned) instarray[i].thread);*/
	}
	pthread_mutex_destroy(&thrdMgmt);
	pthread_cond_destroy(&condStarted);
	pthread_cond_destroy(&condDoRun);
}

/* functions related to computing statistics on the runtime of a test. This is
 * a separate function primarily not to mess up the test driver.
 * rgerhards, 2010-12-08
 */
static void
endTiming(struct timeval *tvStart, struct runstats *stats)
{
	long sec, usec;
	unsigned long runtime;
	struct timeval tvEnd;

	gettimeofday(&tvEnd, NULL);
	if(tvStart->tv_usec > tvEnd.tv_usec) {
		tvEnd.tv_sec--;
		tvEnd.tv_usec += 1000000;
	}

	sec = tvEnd.tv_sec - tvStart->tv_sec;
	usec = tvEnd.tv_usec - tvStart->tv_usec;

	runtime = sec * 1000 + (usec / 1000);
	stats->totalRuntime += runtime;
	if(runtime < stats->minRuntime)
		stats->minRuntime = runtime;
	if(runtime > stats->maxRuntime)
		stats->maxRuntime = runtime;

	if(!bSilent || bStatsRecords) {
		if(bCSVoutput) {
			printf("%lu.%3.3ld\n", runtime / 1000, runtime % 1000);
		} else {
			printf("runtime: %lu.%3.3ld\n", runtime / 1000, runtime % 1000);
		}
	}
}


/* generate stats summary record at end of run
 */
static void
genStats(struct runstats *stats)
{
	long unsigned avg;
	avg = stats->totalRuntime / stats->numRuns;

	if(bCSVoutput) {
		printf("#numRuns,TotalRuntime,AvgRuntime,MinRuntime,MaxRuntime\n");
		printf("%d,%llu.%3.3d,%lu.%3.3lu,%lu.%3.3lu,%lu.%3.3lu\n",
			stats->numRuns,
		        stats->totalRuntime / 1000, (int) stats->totalRuntime % 1000,
		        avg / 1000, avg % 1000,
		        stats->minRuntime / 1000, stats->minRuntime % 1000,
		        stats->maxRuntime / 1000, stats->maxRuntime % 1000);
	} else {
		printf("Runs:     %d\n",   stats->numRuns);
		printf("Runtime:\n");
		printf("  total:  %llu.%3.3d\n", stats->totalRuntime / 1000,
						 (int) stats->totalRuntime % 1000);
		printf("  avg:    %lu.%3.3lu\n",  avg / 1000, avg % 1000);
		printf("  min:    %lu.%3.3lu\n",  stats->minRuntime / 1000, stats->minRuntime % 1000);
		printf("  max:    %lu.%3.3lu\n",  stats->maxRuntime / 1000, stats->maxRuntime % 1000);
		printf("All times are wallclock time.\n");
	}
}


/* Run the actual test. This function handles various meta-parameters, like
 * a specified number of iterations, performance measurement and so on...
 * rgerhards, 2010-12-08
 */
static int
runTests(void)
{
	struct timeval tvStart;
	struct runstats stats;
	int run;

	stats.totalRuntime = 0;
	stats.minRuntime = 0xffffffffllu;
	stats.maxRuntime = 0;
	stats.numRuns = numRuns;
	run = 1;
	while(1) { /* loop broken inside */
		if(!bSilent)
			printf("starting run %d\n", run);
		prepareGenerators();
		gettimeofday(&tvStart, NULL);
		runGenerators();
		waitGenerators();
		endTiming(&tvStart, &stats);
		if(run == numRuns)
			break;
		if(!bSilent)
			printf("sleeping %d seconds before next run\n", sleepBetweenRuns);
		sleep(sleepBetweenRuns);
		++run;
	}

	if(bStatsRecords) {
		genStats(&stats);
	}

	return 0;
}

#	if defined(ENABLE_OPENSSL)
/* OpenSSL implementation of TLS funtions.
 * alorbach, 2018-06-11
 */


#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
long BIO_debug_callback_ex(BIO *bio, int cmd, const char __attribute__((unused)) *argp,
			size_t __attribute__((unused)) len, int argi, long __attribute__((unused)) argl,
			int ret, size_t __attribute__((unused)) *processed)
#else
long BIO_debug_callback(BIO *bio, int cmd, const char __attribute__((unused)) *argp,
			int argi, long __attribute__((unused)) argl, long ret)
#endif
{
	long r = 1;

	if (BIO_CB_RETURN & cmd) {
		r = ret;
	}

	printf("tcpflood: openssl debugmsg: BIO[%p]: ", (void *)bio);

	switch (cmd) {
	case BIO_CB_FREE:
		printf("Free - %s\n", RSYSLOG_BIO_method_name(bio));
		break;
/* Disabled due API changes for OpenSSL 1.1.0+ */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	case BIO_CB_READ:
		if (bio->method->type & BIO_TYPE_DESCRIPTOR) {
			printf("read(%d,%lu) - %s fd=%d\n",
				RSYSLOG_BIO_number_read(bio), (unsigned long)argi,
				RSYSLOG_BIO_method_name(bio), RSYSLOG_BIO_number_read(bio));
		} else {
			printf("read(%d,%lu) - %s\n",
				RSYSLOG_BIO_number_read(bio), (unsigned long)argi, RSYSLOG_BIO_method_name(bio));
		}
		break;
	case BIO_CB_WRITE:
		if (bio->method->type & BIO_TYPE_DESCRIPTOR) {
			printf("write(%d,%lu) - %s fd=%d\n",
				RSYSLOG_BIO_number_written(bio), (unsigned long)argi,
				RSYSLOG_BIO_method_name(bio), RSYSLOG_BIO_number_written(bio));
		} else {
			printf("write(%d,%lu) - %s\n",
				RSYSLOG_BIO_number_written(bio), (unsigned long)argi, RSYSLOG_BIO_method_name(bio));
		}
		break;
#else
	case BIO_CB_READ:
		printf("read %s\n", RSYSLOG_BIO_method_name(bio));
		break;
	case BIO_CB_WRITE:
		printf("write %s\n", RSYSLOG_BIO_method_name(bio));
		break;
#endif
	case BIO_CB_PUTS:
		printf("puts() - %s\n", RSYSLOG_BIO_method_name(bio));
		break;
	case BIO_CB_GETS:
		printf("gets(%lu) - %s\n", (unsigned long)argi,
			RSYSLOG_BIO_method_name(bio));
		break;
	case BIO_CB_CTRL:
		printf("ctrl(%lu) - %s\n", (unsigned long)argi,
			RSYSLOG_BIO_method_name(bio));
		break;
	case BIO_CB_RETURN | BIO_CB_READ:
		printf("read return %d\n", (int)ret);
		break;
	case BIO_CB_RETURN | BIO_CB_WRITE:
		printf("write return %d\n", (int)ret);
		break;
	case BIO_CB_RETURN | BIO_CB_GETS:
		printf("gets return %d\n", (int)ret);
		break;
	case BIO_CB_RETURN | BIO_CB_PUTS:
		printf("puts return %d\n", (int)ret);
		break;
	case BIO_CB_RETURN | BIO_CB_CTRL:
		printf("ctrl return %d\n", (int)ret);
		break;
	default:
		printf("bio callback - unknown type (%d)\n", cmd);
		break;
	}

	return r;
}

void osslLastSSLErrorMsg(int ret, SSL *ssl, const char* pszCallSource)
{
	unsigned long un_error = 0;
	char psz[256];

	if (ssl == NULL) {
		/* Output Error Info*/
		printf("tcpflood: Error in '%s' with ret=%d\n", pszCallSource, ret);
	} else {
		long iMyRet = SSL_get_error(ssl, ret);

		/* Check which kind of error we have */
		printf("tcpflood: openssl error '%s' with error code=%ld\n", pszCallSource, iMyRet);
		if(iMyRet == SSL_ERROR_SYSCALL){
			iMyRet = ERR_get_error();
			if(ret == 0) {
				iMyRet = SSL_get_error(ssl, iMyRet);
				if(iMyRet == 0) {
					*psz = '\0';
				} else {
					ERR_error_string_n(iMyRet, psz, 256);
				}
				printf("tcpflood: Errno %d, SysErr: %s\n", errno, psz);
			}
		} else {
			printf("tcpflood: Unknown SSL Error in '%s' (%d), SSL_get_error: %ld\n",
				pszCallSource, ret, iMyRet);
		}
	}

	/* Loop through errors */
	while ((un_error = ERR_get_error()) > 0){
		ERR_error_string_n(un_error, psz, 256);
		printf("tcpflood: %s Errorstack: %s\n", pszCallSource, psz);
	}
}

int verify_callback(int status, X509_STORE_CTX *store)
{
	char szdbgdata1[256];
	char szdbgdata2[256];

	if(status == 0) {
		printf("tcpflood: verify_callback certificate validation failed!\n");

		X509 *cert = X509_STORE_CTX_get_current_cert(store);
		int depth = X509_STORE_CTX_get_error_depth(store);
		int err = X509_STORE_CTX_get_error(store);
		X509_NAME_oneline(X509_get_issuer_name(cert), szdbgdata1, sizeof(szdbgdata1));
		X509_NAME_oneline(RSYSLOG_X509_NAME_oneline(cert), szdbgdata2, sizeof(szdbgdata2));

		/* Log Warning only on EXPIRED */
		if (err == X509_V_OK || err == X509_V_ERR_CERT_HAS_EXPIRED) {
			printf(
				"tcpflood: Certificate warning at depth: %d \n\t"
				"issuer  = %s\n\t"
				"subject = %s\n\t"
				"err %d:%s\n",
				depth, szdbgdata1, szdbgdata2, err, X509_verify_cert_error_string(err));

			/* Set Status to OK*/
			status = 1;
		} else {
			printf(
				"tcpflood: Certificate error at depth: %d \n\t"
				"issuer  = %s\n\t"
				"subject = %s\n\t"
				"err %d:%s\n",
				depth, szdbgdata1, szdbgdata2, err, X509_verify_cert_error_string(err));
			exit(1);
		}
	}
	return status;
}


/* global init OpenSSL
 */
static void
initTLS(const SSL_METHOD *method)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	/* Setup OpenSSL library  < 1.1.0 */
	if( !SSL_library_init()) {
#else
	/* Setup OpenSSL library >= 1.1.0 with system default settings */
	if( OPENSSL_init_ssl(0, NULL) == 0) {
#endif
		printf("tcpflood: error openSSL initialization failed!\n");
		exit(1);
	}

	/* Load readable error strings */
	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
	/*
	* ERR_load_*(), ERR_func_error_string(), ERR_get_error_line(), ERR_get_error_line_data(), ERR_get_state()
	* OpenSSL now loads error strings automatically so these functions are not needed.
	* SEE FOR MORE:
	*	https://www.openssl.org/docs/manmaster/man7/migration_guide.html
	*
	*/
#else
	/* Load error strings into mem*/
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
#endif


	// Create OpenSSL Context
	ctx = SSL_CTX_new(method);

	if(tlsCAFile != NULL && SSL_CTX_load_verify_locations(ctx, tlsCAFile, NULL) != 1) {
		printf("tcpflood: Error, Failed loading CA certificate"
				" Is the file at the right path? And do we have the permissions?");
		exit(1);
	}
	SSL_CTX_set_ecdh_auto(ctx, 1);
	if(SSL_CTX_use_certificate_chain_file(ctx, tlsCertFile) != 1) {
		printf("tcpflood: error cert file could not be accessed -- have you mixed up key and certificate?\n");
		printf("If in doubt, try swapping the files in -z/-Z\n");
		printf("Certifcate is: '%s'\n", tlsCertFile);
		printf("Key        is: '%s'\n", tlsKeyFile);
		exit(1);
	}
	if(SSL_CTX_use_PrivateKey_file(ctx, tlsKeyFile, SSL_FILETYPE_PEM) != 1) {
		printf("tcpflood: error key file could not be accessed -- have you mixed up key and certificate?\n");
		printf("If in doubt, try swapping the files in -z/-Z\n");
		printf("Certifcate is: '%s'\n", tlsCertFile);
		printf("Key        is: '%s'\n", tlsKeyFile);
		exit(1);
	}

	/* Set CTX Options */
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);		/* Disable insecure SSLv2 Protocol */
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);		/* Disable insecure SSLv3 Protocol */
	SSL_CTX_sess_set_cache_size(ctx,1024);

	/* Check for Custom Config string */
	if (customConfig != NULL){
#if OPENSSL_VERSION_NUMBER >= 0x10002000L && !defined(LIBRESSL_VERSION_NUMBER)
	char *pCurrentPos;
	char *pNextPos;
	char *pszCmd;
	char *pszValue;
	int iConfErr;

	printf("tcpflood: custom config set to '%s'\n", customConfig);

	/* Set working pointer */
	pCurrentPos = (char*) customConfig;
	if (strlen(pCurrentPos) > 0) {
		pNextPos = index(pCurrentPos, '=');
		if (pNextPos != NULL) {
			pszCmd = strndup(pCurrentPos, pNextPos-pCurrentPos);
			pszValue = strdup(++pNextPos);

			// Create CTX Config Helper
			SSL_CONF_CTX *cctx;
			cctx = SSL_CONF_CTX_new();
			SSL_CONF_CTX_set_flags(cctx, SSL_CONF_FLAG_CLIENT);
			SSL_CONF_CTX_set_flags(cctx, SSL_CONF_FLAG_FILE);
			SSL_CONF_CTX_set_flags(cctx, SSL_CONF_FLAG_SHOW_ERRORS);
			SSL_CONF_CTX_set_ssl_ctx(cctx, ctx);

			/* Add SSL Conf Command */
			iConfErr = SSL_CONF_cmd(cctx, pszCmd, pszValue);
			if (iConfErr > 0) {
				printf("tcpflood: Successfully added Command %s:%s\n",
					pszCmd, pszValue);
			}
			else {
				printf("tcpflood: error, adding Command: %s:%s "
					"in SSL_CONF_cmd with error '%d'\n",
					pszCmd, pszValue, iConfErr);
				osslLastSSLErrorMsg(0, NULL, "initTLS");
			}

			/* Finalize SSL Conf */
			iConfErr = SSL_CONF_CTX_finish(cctx);
			if (!iConfErr) {
				printf("tcpflood: error, setting openssl command parameters: %s\n",
					customConfig);
			}

			free(pszCmd);
			free(pszValue);
		} else {
			printf("tcpflood: error, invalid value for -k: %s\n", customConfig);
		}
	} else {
		printf("tcpflood: error, invalid value for -k: %s\n", customConfig);
	}
#else
	printf("tcpflood: TLS library does not support SSL_CONF_cmd API (maybe it is too old?).");
#endif
	}


	/* DO ONLY SUPPORT DEFAULT CIPHERS YET
	* SSL_CTX_set_cipher_list(ctx,"ALL");			 Support all ciphers */

/*	// Create Extra Length DH!
	pDH = DH_new();
	if ( !DH_generate_parameters_ex(pDH, 768, DH_GENERATOR_2, NULL) )
	{
		if(pDH)
			DH_free(pDH);

		fprintf(stderr, "Failed to generated dynamic DH\n");
		exit(1);
	}
	else
	{
		int iErrCheck = 0;
		if ( !DH_check( pDH, &iErrCheck) )
		{
			fprintf(stderr, "Failed to generated dynamic DH - iErrCheck=%d\n", iErrCheck);
			exit(1);
		}
	}
*/
	/* Set default VERIFY Options for OpenSSL CTX - and CALLBACK */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, verify_callback);

	SSL_CTX_set_timeout(ctx, 30);	/* Default Session Timeout, TODO: Make configureable */
	SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
}

static void
exitTLS(void)
{
	SSL_CTX_free(ctx);
#ifndef OPENSSL_NO_ENGINE
	ENGINE_cleanup();
#endif
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
static void
initTLSSess(const int i)
{
	int res;
	BIO *bio_client;
	SSL* pNewSsl = SSL_new(ctx);

	sslArray[i] = pNewSsl;

	if(!sslArray[i]) {
		osslLastSSLErrorMsg(0, sslArray[i], "initTLSSess1");
	}

	SSL_set_verify(sslArray[i], SSL_VERIFY_NONE, verify_callback);

	/* Create BIO from socket array! */
	bio_client = BIO_new_socket(sockArray[i], BIO_CLOSE /*BIO_NOCLOSE*/);
	if (bio_client == NULL) {
		osslLastSSLErrorMsg(0, sslArray[i], "initTLSSess2");
		exit(1);
	} else {
	//	printf("initTLSSess: Init client BIO[%p] done\n", (void *)bio_client);
	}

	if(tlsLogLevel > 0) {
		/* Set debug Callback for client BIO as well! */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
		BIO_set_callback_ex(bio_client, BIO_debug_callback_ex);
#else
		BIO_set_callback(bio_client, BIO_debug_callback);
#endif // OPENSSL_VERSION_NUMBER >= 0x10100000L
	}

	/* Blocking socket */
	BIO_set_nbio( bio_client, 0 );
	SSL_set_bio(sslArray[i], bio_client, bio_client);
	SSL_set_connect_state(sslArray[i]); /*sets ssl to work in client mode.*/

	/* Perform the TLS handshake */
	if((res = SSL_do_handshake(sslArray[i])) <= 0) {
		osslLastSSLErrorMsg(res, sslArray[i], "initTLSSess3");
		exit(1);
	}
}
#pragma GCC diagnostic pop


static int
sendTLS(int i, char *buf, size_t lenBuf)
{
	size_t lenSent;
	int r, err;

	lenSent = 0;
	while(lenSent != lenBuf) {
		r = SSL_write(sslArray[i], buf + lenSent, lenBuf - lenSent);
		if(r > 0) {
			lenSent += r;
		} else {
			err = SSL_get_error(sslArray[i], r);
			if(err != SSL_ERROR_ZERO_RETURN && err != SSL_ERROR_WANT_READ &&
				err != SSL_ERROR_WANT_WRITE) {
				/*SSL_ERROR_ZERO_RETURN: TLS connection has been closed. This
				 * result code is returned only if a closure alert has occurred
				 * in the protocol, i.e. if the connection has been closed cleanly.
				 *SSL_ERROR_WANT_READ/WRITE: The operation did not complete, try
				 * again later. */
				printf("Error while sending data: [%d] %s", err, ERR_error_string(err, NULL));
				printf("Error is: %s", ERR_reason_error_string(err));
			} else {
				/* Check for SSL Shutdown */
				if (SSL_get_shutdown(sslArray[i]) == SSL_RECEIVED_SHUTDOWN) {
					printf("received SSL_RECEIVED_SHUTDOWN!\n");
				} else {
					printf("[ERROR] while sending data: [%d] %s", err, ERR_error_string(err, NULL));
					printf("[ERROR] Reason: %s", ERR_reason_error_string(err));
				}
			}
			exit(1);
		}
	}

	return lenSent;
}

static void
closeTLSSess(int i)
{
	int r;
	r = SSL_shutdown(sslArray[i]);
	if (r <= 0){
		/* Shutdown not finished, call SSL_read to do a bidirectional shutdown, see doc for more:
		*	https://www.openssl.org/docs/man1.1.1/man3/SSL_shutdown.html
		*/
		char rcvBuf[MAX_RCVBUF];
		SSL_read(sslArray[i], rcvBuf, MAX_RCVBUF);

	}
	SSL_free(sslArray[i]);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
static void
initDTLSSess()
{
	int res;
	BIO *bio_client;

	// Create new SSL
	SSL* pNewSsl = SSL_new(ctx);

	// set to array variables
	sslArray[0] = pNewSsl;
	sockArray[0] = udpsockout;

	if(!sslArray[0]) {
		fprintf(stderr, "Unable to create SSL\n");
		osslLastSSLErrorMsg(0, sslArray[0], "initDTLSSess1");
		exit(1);
	}

	SSL_set_verify(sslArray[0], SSL_VERIFY_NONE, verify_callback);

	/* Create BIO from socket array! */
	bio_client = BIO_new_dgram(udpsockout, BIO_NOCLOSE);
	if (!bio_client) {
		fprintf(stderr, "Unable to create BIO\n");
		osslLastSSLErrorMsg(0, sslArray[0], "initDTLSSess2");
		exit(1);
	}
	BIO_ctrl(bio_client, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &dtls_client_addr);
	SSL_set_bio(sslArray[0], bio_client, bio_client);


	if(tlsLogLevel > 0) {
		/* Set debug Callback for client BIO as well! */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined(LIBRESSL_VERSION_NUMBER)
		BIO_set_callback_ex(bio_client, BIO_debug_callback_ex);
#else
		BIO_set_callback(bio_client, BIO_debug_callback);
#endif // OPENSSL_VERSION_NUMBER >= 0x10100000L
	}

	/* Blocking socket */
//	BIO_set_nbio( bio_client, 0 );
//	SSL_set_bio(sslArray[0], bio_client, bio_client);
//	SSL_set_connect_state(sslArray[0]); /*sets ssl to work in client mode.*/

	printf("[DEBUG] Starting DTLS session ...\n");
	/* Perform handshake */
	if (SSL_connect(sslArray[0]) <= 0) {
		fprintf(stderr, "SSL_connect failed\n");
		osslLastSSLErrorMsg(0, sslArray[0], "initDTLSSess3");
		exit(1);
	}

	// Print Cipher info
	const SSL_CIPHER *cipher = SSL_get_current_cipher(sslArray[0]);
	if(tlsLogLevel > 0) {
		printf("[DEBUG] Cipher used: %s\n", SSL_CIPHER_get_name(cipher));
	}

	// Print Peer Certificate info
	if(tlsLogLevel > 0) {
		X509 *cert = SSL_get_peer_certificate(sslArray[0]);
		if (cert != NULL) {
			char *line;
			line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
			printf("[DEBUG] Subject: %s\n", line);
			OPENSSL_free(line);

			line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
			printf("[DEBUG] Issuer: %s\n", line);
			OPENSSL_free(line);
			X509_free(cert);
		} else {
			printf("[DEBUG] No certificates.\n");
		}
	}

	/* Set and activate timeouts */
	struct timeval timeout;
	timeout.tv_sec = 3;
	timeout.tv_usec = 0;
	BIO_ctrl(bio_client, BIO_CTRL_DGRAM_SET_RECV_TIMEOUT, 0, &timeout);
}
#pragma GCC diagnostic pop


static int
sendDTLS(char *buf, size_t lenBuf)
{
	size_t lenSent;
	int r, err;

	lenSent = 0;
	r = SSL_write(sslArray[0], buf + lenSent, lenBuf - lenSent);
	if(r > 0) {
		lenSent += r;
	} else {
		err = SSL_get_error(sslArray[0], r);
		switch(err) {
			case SSL_ERROR_SYSCALL:
				printf("[ERROR] SSL_write (SSL_ERROR_SYSCALL): %s\n", strerror(errno));
				break;
			default:
				printf("[ERROR] while sending data: [%d] %s", err, ERR_error_string(err, NULL));
				printf("[ERROR] Reason: %s", ERR_reason_error_string(err));
		}
		exit(1);
	}
	return lenSent;
}

static void
closeDTLSSess()
{
	printf("closeDTLSSess ENTER\n");

	int r;
	r = SSL_shutdown(sslArray[0]);
	if (r <= 0){
		/* Shutdown not finished, call SSL_read to do a bidirectional shutdown, see doc for more:
		*	https://www.openssl.org/docs/man1.1.1/man3/SSL_shutdown.html
		*/
		char rcvBuf[MAX_RCVBUF];
		SSL_read(sslArray[0], rcvBuf, MAX_RCVBUF);

	}
	SSL_free(sslArray[0]);
	close(udpsockout);
	close(udpsockin);

	printf("closeDTLSSess EXIT\n");
}

#	elif defined(ENABLE_GNUTLS)
/* This defines a log function to be provided to GnuTLS. It hopefully
 * helps us track down hard to find problems.
 * rgerhards, 2008-06-20
 */
static void tlsLogFunction(int level, const char *msg)
{
	printf("GnuTLS (level %d): %s", level, msg);
}

static void
exitTLS(void)
{
}

/* global init GnuTLS
 */
static void
initTLS(void)
{
	int r;

	/* order of gcry_control and gnutls_global_init matters! */
	#if GNUTLS_VERSION_NUMBER <= 0x020b00
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	#endif
	gnutls_global_init();
	/* set debug mode, if so required by the options */
	if(tlsLogLevel > 0) {
		gnutls_global_set_log_function(tlsLogFunction);
		gnutls_global_set_log_level(tlsLogLevel);
	}

	r = gnutls_certificate_allocate_credentials(&tlscred);
	if(r != GNUTLS_E_SUCCESS) {
		printf("error allocating credentials\n");
		gnutls_perror(r);
		exit(1);
	}
	r = gnutls_certificate_set_x509_key_file(tlscred, tlsCertFile, tlsKeyFile, GNUTLS_X509_FMT_PEM);
	if(r != GNUTLS_E_SUCCESS) {
		printf("error setting certificate files -- have you mixed up key and certificate?\n");
		printf("If in doubt, try swapping the files in -z/-Z\n");
		printf("Certifcate is: '%s'\n", tlsCertFile);
		printf("Key        is: '%s'\n", tlsKeyFile);
		gnutls_perror(r);
		r = gnutls_certificate_set_x509_key_file(tlscred, tlsKeyFile, tlsCertFile,
							 GNUTLS_X509_FMT_PEM);
		if(r == GNUTLS_E_SUCCESS) {
			printf("Tried swapping files, this seems to work "
			       "(but results may be unpredictable!)\n");
		} else {
			exit(1);
		}
	}
}


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
static void
initTLSSess(const int i)
{
	int r;
	gnutls_init(sessArray + i, GNUTLS_CLIENT);

	/* Use default priorities */
	gnutls_set_default_priority(sessArray[i]);

	/* put our credentials to the current session */
	r = gnutls_credentials_set(sessArray[i], GNUTLS_CRD_CERTIFICATE, tlscred);
	if(r != GNUTLS_E_SUCCESS) {
		fprintf (stderr, "Setting credentials failed\n");
		gnutls_perror(r);
		exit(1);
	}

	/* NOTE: the following statement generates a cast warning, but there seems to
	 * be no way around it with current GnuTLS. Do NOT try to "fix" the situation!
	 */
	gnutls_transport_set_ptr(sessArray[i], (gnutls_transport_ptr_t) sockArray[i]);

	/* Perform the TLS handshake */
	r = gnutls_handshake(sessArray[i]);
	if(r < 0) {
		fprintf (stderr, "TLS Handshake failed\n");
		gnutls_perror(r);
		exit(1);
	}
}
#pragma GCC diagnostic pop


static int
sendTLS(int i, char *buf, size_t lenBuf)
{
	int lenSent;
	int r;

	lenSent = 0;
	while(lenSent != lenBuf) {
		r = gnutls_record_send(sessArray[i], buf + lenSent, lenBuf - lenSent);
		if(r < 0)
			break;
		lenSent += r;
	}

	return lenSent;
}

static void
closeTLSSess(int i)
{
	gnutls_bye(sessArray[i], GNUTLS_SHUT_RDWR);
	gnutls_deinit(sessArray[i]);
}
#	else	/* NO TLS available */
static void initTLS(void) {}
static void exitTLS(void) {}
static void initTLSSess(int __attribute__((unused)) i) {}
static int sendTLS(int __attribute__((unused)) i, char __attribute__((unused)) *buf,
	size_t __attribute__((unused)) lenBuf) { return 0; }
static void closeTLSSess(int __attribute__((unused)) i) {}

static void initDTLSSess(void) {}
static int sendDTLS(char *buf, size_t lenBuf) {}
static void closeDTLSSess(void) {}
#	endif

static void
setTargetPorts(const char *const port_arg)
{
	int i = 0;

	char *saveptr;
	char *ports = strdup(port_arg);
	char *port = strtok_r(ports, ":", &saveptr);
	while(port != NULL) {
		if(i == sizeof(targetPort)/sizeof(int)) {
			fprintf(stderr, "too many ports specified, max %d\n",
				(int) (sizeof(targetPort)/sizeof(int)));
			exit(1);
		}
		targetPort[i] = atoi(port);
		i++;
		port = strtok_r(NULL, ":", &saveptr);
	}
	free(ports);
}


/* Run the test.
 * rgerhards, 2009-04-03
 */
int main(int argc, char *argv[])
{
	int ret = 0;
	int opt;
	struct sigaction sigAct;
	struct rlimit maxFiles;
	static char buf[1024];
	struct rlimit limit;
	int os_max_fds;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
		perror("Failed to get RLIMIT_NOFILE");
		exit(1);
	}

	os_max_fds = limit.rlim_cur; // Soft limit
	srand(time(NULL));	/* seed is good enough for our needs */

	/* on Solaris, we do not HAVE MSG_NOSIGNAL, so for this reason
	 * we block SIGPIPE (not an issue for this program)
	 */
	memset(&sigAct, 0, sizeof(sigAct));
	sigemptyset(&sigAct.sa_mask);
	sigAct.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sigAct, NULL);

	setvbuf(stdout, buf, _IONBF, 48);

	while((opt = getopt(argc, argv, "a:ABb:c:C:d:DeE:f:F:h:i:I:j:k:l:L:m:M:n:o:OP:p:rR:"
				        "sS:t:T:u:vW:x:XyYz:Z:")) != -1) {
		switch (opt) {
		case 'b':	batchsize = atoll(optarg);
				break;
		case 't':	targetIP = optarg;
				break;
		case 'p':	setTargetPorts(optarg);
				break;
		case 'n':	numTargetPorts = atoi(optarg);
				break;
		case 'c':	numConnections = atoi(optarg);
				if(numConnections < 0) {
					numConnections *= -1;
					softLimitConnections = 1;
				}
				break;
		case 'C':	numFileIterations = atoi(optarg);
				break;
		case 'm':	numMsgsToSend = atoi(optarg);
				break;
		case 'i':	msgNum = atoi(optarg);
				break;
		case 'P':	msgPRI = optarg;
				break;
		case 'j':	jsonCookie = optarg;
				break;
		case 'd':	extraDataLen = atoi(optarg);
				if(extraDataLen > MAX_EXTRADATA_LEN) {
					fprintf(stderr, "-d max is %d!\n",
						MAX_EXTRADATA_LEN);
					exit(1);
				}
				break;
		case 'D':	bRandConnDrop = 1;
				break;
		case 'l':	dbRandConnDrop = atof(optarg);
				printf("RandConnDrop Level: '%lf' \n", dbRandConnDrop);
				break;
		case 'r':	bRandomizeExtraData = 1;
				break;
		case 'f':	dynFileIDs = atoi(optarg);
				break;
		case 'F':	frameDelim = atoi(optarg);
				break;
		case 'h':	hostname = optarg;
				break;
		case 'L':	tlsLogLevel = atoi(optarg);
				break;
		case 'M':	MsgToSend = optarg;
				break;
		case 'I':	dataFile = optarg;
				/* in this mode, we do not know the num messages to send, so
				 * we set a (high) number to keep the code happy.
				 */
				numMsgsToSend = 1000000;
				break;
		case 's':	bSilent = 1;
				break;
		case 'B':	bBinaryFile = 1;
				break;
		case 'R':	numRuns = atoi(optarg);
				break;
		case 'S':	sleepBetweenRuns = atoi(optarg);
				break;
		case 'X':	bStatsRecords = 1;
				break;
		case 'e':	bCSVoutput = 1;
				break;
		case 'T':	if(!strcmp(optarg, "udp")) {
					transport = TP_UDP;
				} else if(!strcmp(optarg, "tcp")) {
					transport = TP_TCP;
				} else if(!strcmp(optarg, "tls")) {
#					if defined(ENABLE_OPENSSL)
						transport = TP_TLS;
#					elif defined(ENABLE_GNUTLS)
						transport = TP_TLS;
#					else
						fprintf(stderr, "compiled without gnutls/openssl TLS support: "
							"\"-Ttls\" not supported!\n");
						exit(1);
#					endif
				} else if(!strcmp(optarg, "relp-plain")) {
#					if defined(ENABLE_RELP)
						transport = TP_RELP_PLAIN;
#					else
						fprintf(stderr, "compiled without RELP support: "
							"\"-Trelp-plain\" not supported!\n"
							"(add --enable-relp to ./configure options "
							"if desired)\n");
						exit(1);
#					endif
				} else if(!strcmp(optarg, "relp-tls")) {
#					if defined(ENABLE_RELP)
						transport = TP_RELP_TLS;
#					else
						fprintf(stderr, "compiled without RELP support: "
							"\"-Trelp-tls\" not supported!\n"
							"(add --enable-relp to ./configure options "
							"if desired)\n");
						exit(1);
#					endif
				} else if(!strcmp(optarg, "dtls")) {
#					if defined(ENABLE_OPENSSL)
						transport = TP_DTLS;
#					else
						fprintf(stderr, "compiled without openssl TLS support: "
							"\"-Tdtls\" not supported!\n");
						exit(1);
#					endif
				} else {
					fprintf(stderr, "unknown transport '%s'\n", optarg);
					exit(1);
				}
				break;
		case 'a':	relpAuthMode = optarg;
				break;
		case 'A':	abortOnSendFail = 0;
				break;
		case 'E':	relpPermittedPeer = optarg;
				break;
		case 'u':
#if defined(HAVE_RELPENGINESETTLSLIBBYNAME)
			relpTlsLib = optarg;
#endif
				break;
		case 'W':	waittime = atoi(optarg);
				break;
		case 'Y':	runMultithreaded = 1;
				break;
		case 'y':	useRFC5424Format = 1;
				break;
		case 'x':	tlsCAFile = optarg;
				break;
		case 'z':	tlsKeyFile = optarg;
				break;
		case 'Z':	tlsCertFile = optarg;
				break;
		case 'o':	nThreadsConnOpen = atoi(optarg);
				break;
		case 'O':	octateCountFramed = 1;
				break;
		case 'v':	verbose = 1;
				break;
		case 'k':	customConfig = optarg;
				break;
		default:	printf("invalid option '%c' or value missing - terminating...\n", opt);
				exit (1);
				break;
		}
	}

	const char *const ci_env = getenv("CI");
	if(ci_env != NULL && !strcmp(ci_env, "true")) {
		bSilent = 1;	/* auto-apply silent option during CI runs */
	}

	if(numConnections >= (os_max_fds - 20)) {
		fprintf(stderr, "We are asked to use %d connections, but the OS permits only %d "
			"open file descriptors.\n", numConnections, os_max_fds);
		if(softLimitConnections) {
			numConnections = os_max_fds - 20;
			fprintf(stderr, "We reduced the actual number of connections to %d. "
				"This leaves some room for opening files.\n",
				numConnections);
		} else {
			fprintf(stderr, "Connection count is hard requirement, so we "
				"error-terminate\n");
			exit(1);
		}

	}

	if(tlsCAFile != NULL && transport != TP_RELP_TLS) {
		#if !defined(ENABLE_OPENSSL)
			fprintf(stderr, "-x CAFile not supported in GnuTLS mode - ignored.\n"
				"Note: we do NOT VERIFY the remote peer when compiled for GnuTLS.\n"
				"When compiled for OpenSSL, we do.\n");
		#endif
	}

	if(bStatsRecords && waittime) {
		fprintf(stderr, "warning: generating performance stats and using a waittime "
				"is somewhat contradictory!\n");
	}

	if(!isatty(1) || bSilent)
		bShowProgress = 0;

	if(numConnections > 20) {
		/* if we use many (whatever this means, 20 is randomly picked)
		 * connections, we need to make sure we have a high enough
		 * limit. -- rgerhards, 2010-03-25
		 */
		maxFiles.rlim_cur = numConnections + 20;
		maxFiles.rlim_max = numConnections + 20;
		if(setrlimit(RLIMIT_NOFILE, &maxFiles) < 0) {
			perror("setrlimit to increase file handles failed");
			fprintf(stderr,
			        "could not set sufficiently large number of "
			        "open files for required connection count!\n");
			if(!softLimitConnections) {
				exit(1);
			}
		}
	}

	if(dataFile != NULL) {
		if((dataFP = fopen(dataFile, "r")) == NULL) {
			perror(dataFile);
			exit(1);
		}
	}

	if(tlsKeyFile != NULL || tlsCertFile != NULL) {
		if(	transport != TP_TLS &&
			transport != TP_DTLS &&
			transport != TP_RELP_TLS) {
			printf("error: TLS certificates were specified, but TLS is NOT enabled: "
					"To enable TLS use parameter -Ttls\n");
			exit(1);
		}
	}

	if(transport == TP_TLS) {
		if(tlsKeyFile == NULL || tlsCertFile == NULL) {
			printf("error: transport TLS is specified (-Ttls), -z and -Z must also "
				"be specified\n");
			exit(1);
		}
		/* Create main CTX Object. Use SSLv23_method for < Openssl 1.1.0 and TLS_method for newer versions! */
#if defined(ENABLE_OPENSSL)
#	if OPENSSL_VERSION_NUMBER < 0x10100000L
		initTLS(SSLv23_method());
#	else
		initTLS(TLS_method());
#	endif
#else
		initTLS();
#endif
	} else if(transport == TP_DTLS) {
		if(tlsKeyFile == NULL || tlsCertFile == NULL) {
			printf("error: transport DTLS is specified (-Tdtls), -z and -Z must also "
				"be specified\n");
			exit(1);
		}
#if defined(ENABLE_OPENSSL)
		initTLS(DTLS_client_method());
#else
		printf("error: transport DTLS is specified (-Tdtls) but not supported in GnuTLS driver\n");
		exit(1);
#endif
	} else if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS) {
		#ifdef ENABLE_RELP
		initRELP_PLAIN();
		#endif
	}

	if(openConnections() != 0) {
		printf("error opening connections\n");
		exit(1);
	}

	if(runTests() != 0) {
		printf("error running tests\n");
		exit(1);
	}

	closeConnections(); /* this is important so that we do not finish too early! */

	#ifdef ENABLE_RELP
	if(transport == TP_RELP_PLAIN || transport == TP_RELP_TLS) {
		CHKRELP(relpEngineDestruct(&pRelpEngine));
	}
	#endif

	if(nConnDrops > 0 && !bSilent)
		printf("-D option initiated %ld connection closures\n", nConnDrops);

	if(!bSilent)
		printf("End of tcpflood Run\n");

	if(transport == TP_TLS) {
		exitTLS();
	}

	exit(ret);
}
